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
#include <server/quote_payload.hpp>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QVariant>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>

namespace altair::ui {

/// One printed trade, for time and sales.
struct LiveTapePrint {
    std::int64_t ts_ns = 0;
    std::int64_t price_paise = 0;
    std::int64_t qty = 0;
};

/// How many trades each instrument keeps for time and sales.
inline constexpr std::size_t kLiveTapeDepth = 300;

/// The newest state of one instrument, as the wire delivered it.
struct LivePrice {
    std::int64_t last_paise = 0;
    std::int64_t last_qty = 0;
    std::int64_t volume = 0;
    std::int64_t oi = 0;
    std::int64_t exchange_ts_ns = 0;
    bool has_volume = false;
    bool has_oi = false;
    bool replay = false;
    bool simulated = false;   ///< altair_price_service --sim: never shown as live
    std::uint64_t updates = 0;
    std::uint64_t trades = 0;
    /// Feed time (the frame's engine time) of the last quote and the last
    /// book: what a paper fill is judged fresh against. 0 = never.
    std::int64_t quote_ns = 0;
    std::int64_t book_ns = 0;
    std::int64_t last_ns = 0;   ///< feed time of the newest frame for this instrument

    /// The quote topic (server/quote_payload.hpp): OHLC, previous close, top
    /// of book, ATP, totals, circuits. `quote.flags` says which are present.
    QuotePayload quote{};
    bool has_quote = false;

    /// The latest trades, newest at the back. RULE 11: truncates visibly --
    /// the tape is a window of the last kLiveTapeDepth prints, and `trades`
    /// counts every one, so the window never passes for the whole day.
    std::deque<LiveTapePrint> tape;

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
            // A new connection is a new stream: its first frames are the
            // bus's baseline for this client, at whatever sequence the
            // service is at now (and a restarted service starts again at 1).
            seq_.clear();
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
    [[nodiscard]] QAbstractSocket::SocketState state() const { return sock_->state(); }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
    [[nodiscard]] std::uint64_t missed() const noexcept { return missed_; }
    [[nodiscard]] std::uint64_t missed_trades() const noexcept { return trade_missed_; }
    [[nodiscard]] std::uint64_t undecodable() const noexcept { return bad_; }
    [[nodiscard]] std::uint64_t unknown_topics() const noexcept { return unknown_topic_; }

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
    void tradeUpdated(unsigned token);

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

            // DISPATCH BY TOPIC. A quote frame is not a price frame, and
            // decoding one as the other would read a previous close as a
            // last price.
            if (h->topic == kTopicQuote) {
                const auto q = decode_quote(raw + kFrameHeaderBytes, h->payload_len);
                if (q) { apply_quote(*h, *q); } else { ++bad_; }
            } else if (h->topic == kTopicTrades || h->topic == kTopicBook) {
                const auto p = decode_price(raw + kFrameHeaderBytes,
                                            h->payload_len);
                if (p) {
                    apply(*h, *p);
                } else {
                    ++bad_;
                }
            } else {
                ++unknown_topic_;   // a newer service; skipped by topic, never misread
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

    /// Per-topic sequence. A Snapshot (a late joiner's baseline, sent to this
    /// client alone) carries the sequence the stream is AT; a Delta must be
    /// exactly one past the last. False for a duplicate Delta (skip it).
    bool sequence(const FrameHeader& h) {
        auto& seen = seq_[h.topic];
        if (h.kind == FrameKind::Snapshot) {
            if (seen != 0 && h.seq > seen) {
                ++gaps_;
                missed_ += h.seq - seen;
                if (h.topic == kTopicTrades) trade_missed_ += h.seq - seen;
            }
            seen = h.seq;
            return true;
        }
        if (seen != 0 && h.seq <= seen) return false;
        if (seen != 0 && h.seq > seen + 1) {
            ++gaps_;
            missed_ += h.seq - seen - 1;
            if (h.topic == kTopicTrades) trade_missed_ += h.seq - seen - 1;
        }
        seen = h.seq;
        return true;
    }

    void apply(const FrameHeader& h, const DecodedPrice& d) {
        ++frames_;

        // Sequence is per topic. A gap on trades does not invalidate the book,
        // which is exactly why protocol.hpp gives every topic its own counter.
        if (!sequence(h)) return;
        const bool snapshot = h.kind == FrameKind::Snapshot;

        LivePrice& lp = last_[d.payload.token];
        ++lp.updates;
        lp.replay = d.payload.has(kPriceReplay);
        lp.simulated = d.payload.has(kPriceSimulated);
        lp.exchange_ts_ns = d.payload.exchange_ts_ns;
        lp.last_ns = std::max(lp.last_ns, h.engine_time_ns);
        if (h.topic == kTopicTrades) {
            lp.last_paise = d.payload.last_paise;
            lp.last_qty = d.payload.last_qty;
            // A snapshot is the last trade as it stood, not a new print: it
            // sets the price but is neither counted nor put on the tape.
            if (!snapshot) {
                ++lp.trades;
                lp.tape.push_back(LiveTapePrint{d.payload.exchange_ts_ns, d.payload.last_paise, d.payload.last_qty});
                if (lp.tape.size() > kLiveTapeDepth) { lp.tape.pop_front(); }
            }
            // ABSENCE IS NOT ZERO, so the presence bit is carried through
            // rather than collapsed into the value. A grid that draws 0 for an
            // index's volume is claiming a measurement nobody made.
            lp.has_volume = d.payload.has(kPriceHasVolume);
            if (lp.has_volume) { lp.volume = d.payload.volume; }
            lp.has_oi = d.payload.has(kPriceHasOi);
            if (lp.has_oi) { lp.oi = d.payload.oi; }
        } else if (h.topic == kTopicBook) {
            lp.levels = d.payload.depth_levels;
            lp.book_ns = h.engine_time_ns;
            for (std::size_t i = 0; i < kMaxDepthLevels; ++i) {
                lp.bids[i] = d.bids[i];
                lp.asks[i] = d.asks[i];
            }
        }
        Q_EMIT priceUpdated(d.payload.token);
        if (h.topic == kTopicTrades && !snapshot) Q_EMIT tradeUpdated(d.payload.token);
    }

    void apply_quote(const FrameHeader& h, const QuotePayload& q) {
        ++frames_;
        if (!sequence(h)) return;
        LivePrice& lp = last_[q.token];
        ++lp.updates;
        lp.quote = q;
        lp.has_quote = true;
        if (q.has(kQuoteHasTop)) lp.quote_ns = h.engine_time_ns;
        lp.last_ns = std::max(lp.last_ns, h.engine_time_ns);
        lp.replay = q.has(kQuoteReplay);
        lp.simulated = q.has(kQuoteSimulated);
        Q_EMIT priceUpdated(q.token);
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
    std::uint64_t trade_missed_ = 0;
    std::uint64_t bad_ = 0;
    std::uint64_t unknown_topic_ = 0;
};

}  // namespace altair::ui
