// feed/fyers_tbt.hpp -- the FYERS 50-level book ("TBT" socket), decoded.
//
// FYERS sends the 50-level book over a second WebSocket
// (GET https://api-t1.fyers.in/indus/home/tbtws gives its URL; the default is
// wss://rtsocket-api.fyers.in/versova). The client sends JSON text: a
// subscription per channel ({"type":1,"data":{"subs":1,"symbols":[...],
// "mode":"depth","channel":"1"}}), then which channels to resume
// ({"type":2,"data":{"resumeChannels":[...],"pauseChannels":[]}}), and "ping"
// every ten seconds. The server answers in protobuf (the official SDK's
// msg.proto, proto3, no package):
//
//   SocketMessage { MessageType type = 1; map<string, MarketFeed> feeds = 2;
//                   bool snapshot = 3; string msg = 4; bool error = 5; }
//   MarketFeed    { ...; Depth depth = 5; UInt64Value feed_time = 6;
//                   UInt64Value send_time = 7; string token = 8;
//                   uint64 sequence_no = 9; bool snapshot = 10; string ticker = 11; ... }
//   Depth         { UInt64Value tbq = 1; UInt64Value tsq = 2;
//                   repeated MarketLevel asks = 3; repeated MarketLevel bids = 4; }
//   MarketLevel   { Int64Value price = 1; UInt32Value qty = 2;
//                   UInt32Value nord = 3; UInt32Value num = 4; }
//
// Prices are paise. An update carries only what changed: a field is changed
// when its wrapper is PRESENT -- an empty wrapper is a change to zero -- and a
// level says where it goes in `num` (0..49). A diff lists only the levels that
// moved, so `num` is the position, NOT the index in the repeated field: the
// official SDK (fyers-apiv3 up to 3.1.18) writes by that index and corrupts
// every level a sparse diff skips -- the "wrong prices after level 5" this
// code had while it copied the SDK. A level without `num` (the SDK's own test
// vectors) keeps its index. A snapshot replaces the book. This file decodes with no protobuf library: the format
// is small, fixed by the SDK, and pinned by vectors the SDK itself encoded
// (feed/tests/vectors/fyers_tbt.txt).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace altair::fyers_tbt {

inline constexpr std::size_t kTbtLevels = 50;
inline constexpr const char* kTbtUrlHost = "api-t1.fyers.in";
inline constexpr const char* kTbtUrlPath = "/indus/home/tbtws";
inline constexpr const char* kTbtDefaultHost = "rtsocket-api.fyers.in";
inline constexpr const char* kTbtDefaultPath = "/versova";
inline constexpr int kTbtPingSeconds = 10;
/// Channels the server numbers 1..50.
inline constexpr int kTbtMaxChannels = 50;
/// Symbols put on one channel. Unknown to be a server limit; small channels
/// mean a refusal costs a few symbols, not all of them.
inline constexpr std::size_t kTbtSymbolsPerChannel = 5;

/// One instrument's 50-level book, as the updates have built it.
struct TbtBook {
    std::array<std::int64_t, kTbtLevels> bid_px{}, ask_px{};
    std::array<std::int64_t, kTbtLevels> bid_qty{}, ask_qty{};
    std::array<std::uint32_t, kTbtLevels> bid_orders{}, ask_orders{};
    std::uint64_t total_bid_qty = 0, total_ask_qty = 0;
    std::uint64_t feed_time = 0;      ///< as FYERS sends it
    std::uint64_t sequence = 0;
    bool seen_snapshot = false;

    /// Levels from the top with a price on BOTH sides counted separately;
    /// the deeper of the two, so a frame carries both sides to that depth.
    [[nodiscard]] std::size_t depth() const noexcept {
        std::size_t b = 0, a = 0;
        while (b < kTbtLevels && bid_px[b] > 0) ++b;
        while (a < kTbtLevels && ask_px[a] > 0) ++a;
        return b > a ? b : a;
    }
};

/// What one SocketMessage said.
struct TbtMessage {
    bool error = false;
    std::string text;                        ///< `msg`: the server's reason on an error
    std::vector<std::string> updated;        ///< tickers whose book changed, in message order
};

namespace detail {

class PbReader {
public:
    PbReader(const std::uint8_t* p, std::size_t n) : p_(p), end_(p + n) {}
    [[nodiscard]] bool done() const noexcept { return p_ >= end_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

    bool varint(std::uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p_ >= end_) return fail();
            const std::uint8_t b = *p_++;
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return true;
        }
        return fail();   // RULE 11: a varint longer than ten bytes is refused
    }
    bool tag(std::uint32_t& field, std::uint32_t& wire) {
        std::uint64_t t = 0;
        if (!varint(t)) return false;
        field = static_cast<std::uint32_t>(t >> 3);
        wire = static_cast<std::uint32_t>(t & 7);
        return field != 0 || fail();
    }
    bool bytes(const std::uint8_t*& at, std::size_t& n) {
        std::uint64_t len = 0;
        if (!varint(len)) return false;
        if (len > static_cast<std::uint64_t>(end_ - p_)) return fail();
        at = p_;
        n = static_cast<std::size_t>(len);
        p_ += n;
        return true;
    }
    bool skip(std::uint32_t wire) {
        std::uint64_t v = 0;
        const std::uint8_t* at = nullptr;
        std::size_t n = 0;
        switch (wire) {
        case 0: return varint(v);
        case 1: if (end_ - p_ < 8) return fail(); p_ += 8; return true;
        case 2: return bytes(at, n);
        case 5: if (end_ - p_ < 4) return fail(); p_ += 4; return true;
        default: return fail();
        }
    }

private:
    bool fail() { ok_ = false; p_ = end_; return false; }
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

/// A google.protobuf wrapper (Int64Value, UInt32Value, UInt64Value): field 1,
/// absent when zero.
[[nodiscard]] inline bool wrapper(const std::uint8_t* p, std::size_t n, std::uint64_t& v) {
    v = 0;
    PbReader r(p, n);
    while (!r.done()) {
        std::uint32_t f = 0, w = 0;
        if (!r.tag(f, w)) return false;
        if (f == 1 && w == 0) { if (!r.varint(v)) return false; }
        else if (!r.skip(w)) return false;
    }
    return r.ok();
}

struct LevelDelta {
    std::optional<std::int64_t> price;
    std::optional<std::int64_t> qty;
    std::optional<std::uint32_t> orders;
    std::optional<std::uint32_t> num;    ///< the level's position, 0..49
};

[[nodiscard]] inline bool level(const std::uint8_t* p, std::size_t n, LevelDelta& out) {
    PbReader r(p, n);
    while (!r.done()) {
        std::uint32_t f = 0, w = 0;
        if (!r.tag(f, w)) return false;
        if (w == 2 && f >= 1 && f <= 4) {
            const std::uint8_t* at = nullptr;
            std::size_t len = 0;
            std::uint64_t v = 0;
            if (!r.bytes(at, len) || !wrapper(at, len, v)) return false;
            if (f == 1) out.price = static_cast<std::int64_t>(v);   // int64 on the wire: two's complement
            else if (f == 2) out.qty = static_cast<std::int64_t>(static_cast<std::uint32_t>(v));
            else if (f == 3) out.orders = static_cast<std::uint32_t>(v);
            else out.num = static_cast<std::uint32_t>(v);
        } else if (!r.skip(w)) {
            return false;
        }
    }
    return r.ok();
}

struct FeedDelta {
    std::string ticker;
    bool snapshot = false;
    bool has_depth = false;
    std::optional<std::uint64_t> tbq, tsq, feed_time;
    std::uint64_t sequence = 0;
    std::vector<LevelDelta> bids, asks;
};

[[nodiscard]] inline bool depth(const std::uint8_t* p, std::size_t n, FeedDelta& out) {
    PbReader r(p, n);
    while (!r.done()) {
        std::uint32_t f = 0, w = 0;
        if (!r.tag(f, w)) return false;
        if (w != 2) { if (!r.skip(w)) return false; continue; }
        const std::uint8_t* at = nullptr;
        std::size_t len = 0;
        if (!r.bytes(at, len)) return false;
        std::uint64_t v = 0;
        if (f == 1) { if (!wrapper(at, len, v)) return false; out.tbq = v; }
        else if (f == 2) { if (!wrapper(at, len, v)) return false; out.tsq = v; }
        else if (f == 3 || f == 4) {
            auto& side = f == 3 ? out.asks : out.bids;
            // RULE 11: a side longer than 50 levels is refused, not truncated.
            if (side.size() >= kTbtLevels) return false;
            LevelDelta d;
            if (!level(at, len, d)) return false;
            side.push_back(d);
        }
    }
    return r.ok();
}

[[nodiscard]] inline bool market_feed(const std::uint8_t* p, std::size_t n, FeedDelta& out) {
    PbReader r(p, n);
    while (!r.done()) {
        std::uint32_t f = 0, w = 0;
        if (!r.tag(f, w)) return false;
        if (f == 5 && w == 2) {
            const std::uint8_t* at = nullptr;
            std::size_t len = 0;
            if (!r.bytes(at, len) || !depth(at, len, out)) return false;
            out.has_depth = true;
        } else if (f == 6 && w == 2) {
            const std::uint8_t* at = nullptr;
            std::size_t len = 0;
            std::uint64_t v = 0;
            if (!r.bytes(at, len) || !wrapper(at, len, v)) return false;
            out.feed_time = v;
        } else if (f == 9 && w == 0) {
            if (!r.varint(out.sequence)) return false;
        } else if (f == 10 && w == 0) {
            std::uint64_t v = 0;
            if (!r.varint(v)) return false;
            out.snapshot = v != 0;
        } else if (f == 11 && w == 2) {
            const std::uint8_t* at = nullptr;
            std::size_t len = 0;
            if (!r.bytes(at, len)) return false;
            out.ticker.assign(reinterpret_cast<const char*>(at), len);
        } else if (!r.skip(w)) {
            return false;
        }
    }
    return r.ok();
}

inline void apply_side(const std::vector<LevelDelta>& d, std::array<std::int64_t, kTbtLevels>& px,
                       std::array<std::int64_t, kTbtLevels>& qty, std::array<std::uint32_t, kTbtLevels>& ord) {
    for (std::size_t i = 0; i < d.size(); ++i) {
        const std::size_t at = d[i].num ? *d[i].num : i;
        if (at >= kTbtLevels) continue;   // RULE 11: a position past 50 is dropped, never wrapped
        if (d[i].price) px[at] = *d[i].price;
        if (d[i].qty) qty[at] = *d[i].qty;
        if (d[i].orders) ord[at] = *d[i].orders;
    }
}

} // namespace detail

/// Every book this connection has built, by ticker.
class TbtBooks {
public:
    /// Decode one binary message and fold it in. False when the bytes are not
    /// a SocketMessage; nothing is applied then.
    [[nodiscard]] bool on_message(const std::uint8_t* p, std::size_t n, TbtMessage& out) {
        out = TbtMessage{};
        std::vector<detail::FeedDelta> feeds;
        bool message_snapshot = false;
        detail::PbReader r(p, n);
        while (!r.done()) {
            std::uint32_t f = 0, w = 0;
            if (!r.tag(f, w)) return false;
            if (f == 2 && w == 2) {
                // map entry: key = 1, value = 2
                const std::uint8_t* at = nullptr;
                std::size_t len = 0;
                if (!r.bytes(at, len)) return false;
                detail::PbReader e(at, len);
                detail::FeedDelta fd;
                std::string key;
                while (!e.done()) {
                    std::uint32_t ef = 0, ew = 0;
                    if (!e.tag(ef, ew)) return false;
                    const std::uint8_t* v = nullptr;
                    std::size_t vn = 0;
                    if (ef == 1 && ew == 2) {
                        if (!e.bytes(v, vn)) return false;
                        key.assign(reinterpret_cast<const char*>(v), vn);
                    } else if (ef == 2 && ew == 2) {
                        if (!e.bytes(v, vn) || !detail::market_feed(v, vn, fd)) return false;
                    } else if (!e.skip(ew)) {
                        return false;
                    }
                }
                if (!e.ok()) return false;
                if (fd.ticker.empty()) fd.ticker = key;
                feeds.push_back(std::move(fd));
            } else if (f == 3 && w == 0) {
                std::uint64_t v = 0;
                if (!r.varint(v)) return false;
                message_snapshot = v != 0;
            } else if (f == 4 && w == 2) {
                const std::uint8_t* at = nullptr;
                std::size_t len = 0;
                if (!r.bytes(at, len)) return false;
                out.text.assign(reinterpret_cast<const char*>(at), len);
            } else if (f == 5 && w == 0) {
                std::uint64_t v = 0;
                if (!r.varint(v)) return false;
                out.error = v != 0;
            } else if (!r.skip(w)) {
                return false;
            }
        }
        if (!r.ok()) return false;
        if (out.error) return true;
        for (const auto& fd : feeds) {
            if (!fd.has_depth || fd.ticker.empty()) continue;
            TbtBook& b = books_[fd.ticker];
            if (fd.snapshot || message_snapshot) {
                b = TbtBook{};   // a snapshot is the whole book
                b.seen_snapshot = true;
            }
            detail::apply_side(fd.bids, b.bid_px, b.bid_qty, b.bid_orders);
            detail::apply_side(fd.asks, b.ask_px, b.ask_qty, b.ask_orders);
            if (fd.tbq) b.total_bid_qty = *fd.tbq;
            if (fd.tsq) b.total_ask_qty = *fd.tsq;
            if (fd.feed_time) b.feed_time = *fd.feed_time;
            b.sequence = fd.sequence;
            out.updated.push_back(fd.ticker);
        }
        return true;
    }

    [[nodiscard]] const TbtBook* book(const std::string& ticker) const {
        const auto it = books_.find(ticker);
        return it == books_.end() ? nullptr : &it->second;
    }
    void reset() { books_.clear(); }

private:
    std::unordered_map<std::string, TbtBook> books_;
};

// ---- what the client sends ------------------------------------------------------

[[nodiscard]] inline std::string tbt_subscribe_text(const std::vector<std::string>& symbols, int channel) {
    std::string s = "{\"type\":1,\"data\":{\"subs\":1,\"symbols\":[";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i) s += ',';
        s += '"';
        for (const char c : symbols[i])
            if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) s += c;   // FYERS symbols never hold these
        s += '"';
    }
    s += "],\"mode\":\"depth\",\"channel\":\"" + std::to_string(channel) + "\"}}";
    return s;
}

[[nodiscard]] inline std::string tbt_resume_text(int channels) {
    std::string s = "{\"type\":2,\"data\":{\"resumeChannels\":[";
    for (int c = 1; c <= channels; ++c) {
        if (c > 1) s += ',';
        s += "\"" + std::to_string(c) + "\"";
    }
    s += "],\"pauseChannels\":[]}}";
    return s;
}

/// Split symbols into channels of kTbtSymbolsPerChannel, at most
/// kTbtMaxChannels of them. RULE 11: beyond 250 symbols the rest are returned
/// in `left_out`, never dropped silently.
[[nodiscard]] inline std::vector<std::vector<std::string>> tbt_channels(const std::vector<std::string>& symbols,
                                                                        std::vector<std::string>* left_out = nullptr) {
    std::vector<std::vector<std::string>> out;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (out.empty() || out.back().size() == kTbtSymbolsPerChannel) {
            if (out.size() == static_cast<std::size_t>(kTbtMaxChannels)) {
                if (left_out != nullptr) left_out->assign(symbols.begin() + static_cast<std::ptrdiff_t>(i), symbols.end());
                break;
            }
            out.emplace_back();
        }
        out.back().push_back(symbols[i]);
    }
    return out;
}

/// The socket URL from the tbtws reply ({"data":{"socket_url":"wss://host/path"}}),
/// split into host and path. nullopt when the reply has no usable URL.
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>> tbt_socket_url(std::string_view body) {
    const std::size_t k = body.find("\"socket_url\"");
    if (k == std::string_view::npos) return std::nullopt;
    const std::size_t q1 = body.find('"', body.find(':', k) + 1);
    if (q1 == std::string_view::npos) return std::nullopt;
    const std::size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return std::nullopt;
    std::string_view url = body.substr(q1 + 1, q2 - q1 - 1);
    if (url.substr(0, 6) != "wss://") return std::nullopt;
    url.remove_prefix(6);
    const std::size_t slash = url.find('/');
    std::string host(url.substr(0, slash)), path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    if (host.empty() || host.find_first_of(":@ ") != std::string::npos) return std::nullopt;
    return std::make_pair(std::move(host), std::move(path));
}

} // namespace altair::fyers_tbt
