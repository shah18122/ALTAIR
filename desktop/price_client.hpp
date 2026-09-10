// desktop/price_client.hpp -- the window subscribes, it does not fetch.
//
// P37-04. The other half of app/price_service_main.cpp. The service owns the
// Kite connection; this connects to its loopback port and reads frames.
//
// WHAT THIS REPLACES, AND WHY IT IS BETTER THAN WHAT IT REPLACES.
//
// desktop/live_feed.hpp runs altair_kite_ticker for N seconds, waits for it to
// exit, and reads the JSON file it left behind. It is a SAMPLE, and it says so
// honestly -- but the depth ladder has been passed a null book since the day it
// was written (main_window.hpp: `ladder_->show_book(nullptr)`), because a
// ReplayTick carries no depth and nothing else ever had one. So the ladder has
// rendered "No book. The feed has never delivered depth for this instrument."
// forever, on a widget whose rendering logic is complete and tested.
//
// This is the first thing in the tree that can hand it a real one.
//
// GATE 3 IS UNAFFECTED, AND THAT IS THE POINT OF DOING IT THIS WAY.
//
// desktop/ links altair_server -- the protocol and the payload codec, which
// depend on altair_types and altair_time and nothing else. It does NOT link
// altair_broker or altair_oms, and the client vocabulary in protocol.hpp has
// no word for placing an order. The credential lives in one process and this
// is not it.
//
// A GAP IS REPORTED, NOT SMOOTHED.
//
// The bus coalesces when a reader falls behind: the newest price survives and
// the older ones are dropped, which is right for state and wrong to hide. Every
// jump in the sequence number is counted and shown, because a grid that
// silently skipped forty ticks looks exactly like a grid that received forty
// ticks in which nothing happened.

#pragma once

#include <server/price_payload.hpp>
#include <server/protocol.hpp>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QVariant>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace altair::ui {

/// The newest state of one instrument, as the wire delivered it.
struct LivePrice {
    std::int64_t last_paise = 0;
    std::int64_t volume = 0;
    std::int64_t oi = 0;
    std::int64_t exchange_ts_ns = 0;
    bool has_volume = false;
    bool has_oi = false;
    bool replay = false;
    std::uint64_t updates = 0;

    /// Five levels a side, and how many are real.
    PriceLevel bids[kMaxDepthLevels]{};
    PriceLevel asks[kMaxDepthLevels]{};
    std::uint16_t levels = 0;
};

class PriceClient final : public QObject {
    Q_OBJECT

public:
    explicit PriceClient(QObject* parent = nullptr) : QObject(parent) {
        sock_ = new QTcpSocket(this);
        connect(sock_, &QTcpSocket::readyRead, this, &PriceClient::drain);
        connect(sock_, &QTcpSocket::connected, this, [this] {
            // THE SOCKET OPTION IS APPLIED HERE, NOT IN set_read_buffer().
            // Qt can only set it once the descriptor exists, so calling it
            // before connectToHost silently does nothing -- which is exactly
            // what happened: the cap looked set, the kernel stayed unbounded,
            // and a flood at a stopped reader still coalesced nothing.
            apply_read_cap();
            Q_EMIT statusChanged();
        });
        connect(sock_, &QTcpSocket::disconnected, this,
                [this] { Q_EMIT statusChanged(); });
        // Qt 6 renamed errorOccurred; the old `error` signal is gone.
        connect(sock_, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { Q_EMIT statusChanged(); });
    }

    void start(const QString& host = QStringLiteral("127.0.0.1"),
               quint16 port = 7421) {
        host_ = host;
        port_ = port;
        sock_->abort();
        buf_.clear();
        sock_->connectToHost(host_, port_);
    }

    void stop() { sock_->abort(); Q_EMIT statusChanged(); }

    /// Bound how much this subscriber will buffer, in bytes.
    ///
    /// BOTH LAYERS, because either one alone is a fiction. setReadBufferSize
    /// caps what Qt holds; the socket option caps what the kernel holds under
    /// it. Leaving the second unbounded means a window that stops painting
    /// accumulates megabytes of stale prices in the OS and then replays them
    /// oldest-first when it resumes -- the exact opposite of what a State
    /// channel is for, and it defeats the bus's coalescing entirely because
    /// the bus never sees any backpressure.
    ///
    /// AND ON WINDOWS THE SECOND HALF LARGELY DOES NOT WORK, which is worth
    /// knowing before relying on it. A receive buffer can only be shrunk
    /// before the socket connects; Qt exposes setSocketOption only after. So
    /// the option is applied on the `connected` signal and the OS is free to
    /// ignore it, having already auto-tuned. Measured: flooding three
    /// megabytes at a client with this set to 16 KB still coalesced NOTHING,
    /// because the reader never actually applied backpressure.
    ///
    /// What DOES bind is setReadBufferSize, which caps Qt's own buffer and
    /// stops it draining the socket. Treat this as a soft hint on Windows and
    /// do not build a guarantee on it -- the bus bounds its own send buffer
    /// precisely because it cannot trust the reader to bound anything.
    ///
    /// Zero means unbounded, which is Qt's default and is what you want only
    /// if you are certain the reader never stalls.
    void set_read_buffer(qint64 bytes) {
        cap_ = bytes;
        sock_->setReadBufferSize(bytes);   // valid before connect
        apply_read_cap();                  // no-op until there is a socket
    }

    [[nodiscard]] bool connected() const {
        return sock_->state() == QAbstractSocket::ConnectedState;
    }
    [[nodiscard]] QString error_text() const { return sock_->errorString(); }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
    [[nodiscard]] std::uint64_t missed() const noexcept { return missed_; }
    [[nodiscard]] std::uint64_t undecodable() const noexcept { return bad_; }

    [[nodiscard]] const LivePrice* price(std::uint32_t token) const {
        const auto it = last_.find(token);
        return it == last_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::size_t instruments() const noexcept {
        return last_.size();
    }

Q_SIGNALS:
    void statusChanged();
    void priceUpdated(unsigned token);

private Q_SLOTS:
    void drain() {
        buf_.append(sock_->readAll());
        for (;;) {
            if (static_cast<std::size_t>(buf_.size()) < kFrameHeaderBytes) {
                break;
            }
            const auto* raw =
                reinterpret_cast<const std::uint8_t*>(buf_.constData());
            const auto h = decode_header(raw, static_cast<std::size_t>(buf_.size()));
            if (!h) {
                // THE STREAM IS NOT FRAME-ALIGNED AND CANNOT BE RECOVERED BY
                // GUESSING. Hunting for the next magic would resynchronise
                // onto a byte pattern that happened to look like one, inside a
                // price. Drop the connection and say so; the caller can
                // reconnect and get a clean stream.
                ++bad_;
                buf_.clear();
                sock_->abort();
                Q_EMIT statusChanged();
                return;
            }
            const std::size_t need = kFrameHeaderBytes + h->payload_len;
            if (static_cast<std::size_t>(buf_.size()) < need) { break; }

            const auto p = decode_price(raw + kFrameHeaderBytes,
                                        h->payload_len);
            if (p) {
                apply(*h, *p);
            } else {
                ++bad_;
            }
            buf_.remove(0, static_cast<qsizetype>(need));
        }
    }

private:
    void apply_read_cap() {
        if (cap_ > 0 && sock_->state() == QAbstractSocket::ConnectedState) {
            sock_->setSocketOption(
                QAbstractSocket::ReceiveBufferSizeSocketOption,
                QVariant::fromValue(static_cast<int>(cap_)));
        }
    }

    void apply(const FrameHeader& h, const DecodedPrice& d) {
        ++frames_;

        // Sequence is per topic. A gap on trades does not invalidate the book,
        // which is exactly why protocol.hpp gives every topic its own counter.
        auto& seen = seq_[h.topic];
        if (seen != 0 && h.seq > seen + 1) {
            ++gaps_;
            missed_ += h.seq - seen - 1;
        }
        seen = h.seq;

        LivePrice& lp = last_[d.payload.token];
        ++lp.updates;
        lp.replay = d.payload.has(kPriceReplay);
        lp.exchange_ts_ns = d.payload.exchange_ts_ns;
        if (h.topic == kTopicTrades) {
            lp.last_paise = d.payload.last_paise;
            // ABSENCE IS NOT ZERO, so the presence bit is carried through
            // rather than collapsed into the value. A grid that draws 0 for an
            // index's volume is claiming a measurement nobody made.
            lp.has_volume = d.payload.has(kPriceHasVolume);
            if (lp.has_volume) { lp.volume = d.payload.volume; }
            lp.has_oi = d.payload.has(kPriceHasOi);
            if (lp.has_oi) { lp.oi = d.payload.oi; }
        } else if (h.topic == kTopicBook) {
            lp.levels = d.payload.depth_levels;
            for (std::size_t i = 0; i < kMaxDepthLevels; ++i) {
                lp.bids[i] = d.bids[i];
                lp.asks[i] = d.asks[i];
            }
        }
        Q_EMIT priceUpdated(d.payload.token);
    }

    QTcpSocket* sock_ = nullptr;
    QByteArray buf_;
    QString host_;
    quint16 port_ = 0;
    qint64 cap_ = 0;
    std::unordered_map<std::uint32_t, LivePrice> last_;
    std::unordered_map<std::uint32_t, std::uint64_t> seq_;
    std::uint64_t frames_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t missed_ = 0;
    std::uint64_t bad_ = 0;
};

}  // namespace altair::ui
