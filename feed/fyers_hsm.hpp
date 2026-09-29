// feed/fyers_hsm.hpp -- the FYERS data-socket (HSM v1-5) wire protocol, with
// no socket in it.
//
// FYERS does not publish this binary format; its support article points
// non-SDK clients at the official SDKs. The layout here is taken from the
// official Python SDK (fyers-apiv3 3.1.18, MIT licence,
// FyersWebsocket/data_ws.py). Every frame builder and the decoder are
// checked byte-for-byte against output of that SDK in
// feed/tests/test_fyers_hsm.cpp.
//
// What this header does:
//   * pulls the `hsm_key` out of a FYERS v3 access token (a JWT);
//   * builds the auth / mode / subscribe / unsubscribe / channel / ack / ping
//     frames;
//   * turns the symbol-token API response into socket topics
//     ("sf|nse_cm|3045", "if|nse_cm|Nifty 50", "dp|nse_fo|35003");
//   * decodes server frames and emits each update as the same JSON object the
//     SDK hands to its on_message callback. feed/fyers_adapter.hpp already
//     consumes that JSON, so there is still one FYERS decoder, not two.
//
// What it does not do: open a socket, read a credential file or a clock.
// broker/fyers_data_socket.hpp owns the socket; app/fyers_ticker_main.cpp
// owns the credential.
#pragma once

#include <feed/fyers_decoder.hpp>
#include <feed/fyers_index_map.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace altair::fyers_hsm {

// The socket itself (wss://socket.fyers.in/hsm/v1-5/prod) is in
// broker/fyers_data_socket.hpp.
inline constexpr const char* kSymbolTokenHost = "api-t1.fyers.in";
inline constexpr const char* kSymbolTokenPath = "/data/symbol-token";

/// Field 4 of the auth frame identifies the client. The SDK sends its own
/// name and version; Altair identifies itself honestly and the helper lets
/// the operator override it (`--source`).
inline constexpr std::string_view kDefaultSource = "AltairCpp-1.0";
inline constexpr std::uint8_t kDefaultChannel = 11;
inline constexpr std::size_t kMaxSymbols = 5000;       // per connection
inline constexpr std::size_t kSubscribeChunk = 1500;   // topics per frame
inline constexpr int kPingSeconds = 10;
inline constexpr std::int32_t kAbsent = std::numeric_limits<std::int32_t>::min();

using Bytes = std::vector<std::uint8_t>;

enum class HsmError : std::uint8_t {
    MalformedToken,
    MissingHsmKey,
    TokenExpired,
    BadChannel,
    NoTopics,
    TopicTooLong,
    FrameTooLarge,
    SymbolTokenMalformed,
    SymbolTokenRejected,
};

[[nodiscard]] inline const char* error_text(HsmError e) noexcept {
    switch (e) {
    case HsmError::MalformedToken:       return "access token is not a FYERS v3 JWT";
    case HsmError::MissingHsmKey:        return "access token carries no hsm_key";
    case HsmError::TokenExpired:         return "access token has expired; link FYERS again";
    case HsmError::BadChannel:           return "channel must be 1..63";
    case HsmError::NoTopics:             return "no subscribable topics";
    case HsmError::TopicTooLong:         return "topic longer than 255 bytes";
    case HsmError::FrameTooLarge:        return "frame exceeds the 16-bit length field";
    case HsmError::SymbolTokenMalformed: return "symbol-token response is malformed";
    case HsmError::SymbolTokenRejected:  return "symbol-token request rejected by FYERS";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Access token -> hsm_key
// ---------------------------------------------------------------------------

namespace detail {

inline void put_u16(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}
inline void put_u32(Bytes& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}
inline void put_u64(Bytes& b, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}
inline void put_text(Bytes& b, std::string_view s) {
    b.insert(b.end(), s.begin(), s.end());
}

[[nodiscard]] inline int b64url_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

[[nodiscard]] inline std::expected<std::string, HsmError>
b64url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size() * 3 / 4 + 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = b64url_value(c);
        if (v < 0) return std::unexpected(HsmError::MalformedToken);
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

/// Value of a top-level string member. JWT claims here are plain ASCII.
[[nodiscard]] inline std::string_view json_string(std::string_view json,
                                                  std::string_view key) noexcept {
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    const auto at = json.find(needle);
    if (at == std::string_view::npos) return {};
    auto i = json.find(':', at + needle.size());
    if (i == std::string_view::npos) return {};
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size() || json[i] != '"') return {};
    const auto end = json.find('"', i + 1);
    if (end == std::string_view::npos) return {};
    return json.substr(i + 1, end - i - 1);
}

[[nodiscard]] inline bool json_int(std::string_view json, std::string_view key,
                                   std::int64_t& out) noexcept {
    std::string needle;
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    const auto at = json.find(needle);
    if (at == std::string_view::npos) return false;
    auto i = json.find(':', at + needle.size());
    if (i == std::string_view::npos) return false;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    const auto* first = json.data() + i;
    const auto* last = json.data() + json.size();
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc{} && r.ptr != first;
}

} // namespace detail

/// Strip an optional "APPID:" prefix; the socket wants the bare JWT's claim.
[[nodiscard]] inline std::string_view bare_token(std::string_view access_token) noexcept {
    const auto colon = access_token.find(':');
    return colon == std::string_view::npos ? access_token
                                           : access_token.substr(colon + 1);
}

/// The SDK's access_token_to_hsmtoken(): decode the JWT payload, refuse an
/// expired token, return `hsm_key`. `now_unix` is passed in, never read here.
[[nodiscard]] inline std::expected<std::string, HsmError>
hsm_key_from_token(std::string_view access_token, std::int64_t now_unix) {
    const auto token = bare_token(access_token);
    const auto dot1 = token.find('.');
    if (dot1 == std::string_view::npos) return std::unexpected(HsmError::MalformedToken);
    const auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos) return std::unexpected(HsmError::MalformedToken);
    const auto payload = detail::b64url_decode(token.substr(dot1 + 1, dot2 - dot1 - 1));
    if (!payload) return std::unexpected(payload.error());
    std::int64_t exp = 0;
    if (!detail::json_int(*payload, "exp", exp)) return std::unexpected(HsmError::MalformedToken);
    if (exp - now_unix < 0) return std::unexpected(HsmError::TokenExpired);
    const auto key = detail::json_string(*payload, "hsm_key");
    if (key.empty()) return std::unexpected(HsmError::MissingHsmKey);
    return std::string{key};
}

// ---------------------------------------------------------------------------
// Client -> server frames. Layouts are data_ws.py's, byte for byte.
// ---------------------------------------------------------------------------

/// Request type 1: authenticate with the hsm_key. Mode 'P' is what the SDK sends.
[[nodiscard]] inline Bytes auth_frame(std::string_view hsm_key,
                                      std::string_view source = kDefaultSource) {
    Bytes b;
    const std::size_t total = 18 + hsm_key.size() + source.size();
    b.reserve(total);
    detail::put_u16(b, static_cast<std::uint32_t>(total - 2));
    b.push_back(1);                       // request type: auth
    b.push_back(4);                       // field count
    b.push_back(1); detail::put_u16(b, static_cast<std::uint32_t>(hsm_key.size()));
    detail::put_text(b, hsm_key);
    b.push_back(2); detail::put_u16(b, 1); b.push_back('P');
    b.push_back(3); detail::put_u16(b, 1); b.push_back(1);
    b.push_back(4); detail::put_u16(b, static_cast<std::uint32_t>(source.size()));
    detail::put_text(b, source);
    return b;
}

[[nodiscard]] inline std::uint64_t channel_bits(std::uint8_t channel) noexcept {
    return (channel > 0 && channel < 64) ? (std::uint64_t{1} << channel) : 0;
}

/// Request type 12: lite (LTP only, value 76 'L') or full (value 70 'F') mode.
[[nodiscard]] inline Bytes mode_frame(bool lite, std::uint8_t channel = kDefaultChannel) {
    Bytes b;
    detail::put_u16(b, 0);
    b.push_back(12);
    b.push_back(2);
    b.push_back(1); detail::put_u16(b, 8); detail::put_u64(b, channel_bits(channel));
    b.push_back(2); detail::put_u16(b, 1); b.push_back(lite ? 76 : 70);
    return b;
}

/// Request type 8 (resume) or 7 (pause) for one channel.
[[nodiscard]] inline Bytes channel_frame(bool resume, std::uint8_t channel) {
    Bytes b;
    detail::put_u16(b, 0);
    b.push_back(resume ? 8 : 7);
    b.push_back(1);
    b.push_back(1); detail::put_u16(b, 8); detail::put_u64(b, channel_bits(channel));
    return b;
}

/// Request type 3: acknowledge `message_number` after every `ack_every` feeds.
[[nodiscard]] inline Bytes ack_frame(std::uint32_t message_number) {
    Bytes b;
    detail::put_u16(b, 11 - 2);
    b.push_back(3);
    b.push_back(1);
    b.push_back(1); detail::put_u16(b, 4); detail::put_u32(b, message_number);
    return b;
}

/// The SDK's keep-alive, sent every ten seconds.
[[nodiscard]] inline Bytes ping_frame() { return Bytes{0, 1, 11}; }

/// Request type 4 (subscribe) or 5 (unsubscribe). The SDK's leading length
/// field is `18 + scrips + len(access token) + len(source)` -- not the frame
/// length -- and is reproduced exactly because it is what the server accepts.
[[nodiscard]] inline std::expected<Bytes, HsmError>
topics_frame(bool subscribe, std::span<const std::string> topics,
             std::uint8_t channel, std::size_t access_token_len,
             std::size_t source_len = kDefaultSource.size()) {
    if (topics.empty()) return std::unexpected(HsmError::NoTopics);
    if (channel == 0 || channel >= 64) return std::unexpected(HsmError::BadChannel);
    Bytes scrips;
    detail::put_u16(scrips, static_cast<std::uint32_t>(topics.size()));
    for (const auto& t : topics) {
        if (t.size() > 255) return std::unexpected(HsmError::TopicTooLong);
        scrips.push_back(static_cast<std::uint8_t>(t.size()));
        detail::put_text(scrips, t);
    }
    const std::size_t data_len = 18 + scrips.size() + access_token_len + source_len;
    if (data_len > 0xFFFF || scrips.size() > 0xFFFF) return std::unexpected(HsmError::FrameTooLarge);
    Bytes b;
    b.reserve(scrips.size() + 12);
    detail::put_u16(b, static_cast<std::uint32_t>(data_len));
    b.push_back(subscribe ? 4 : 5);
    b.push_back(2);
    b.push_back(1); detail::put_u16(b, static_cast<std::uint32_t>(scrips.size()));
    b.insert(b.end(), scrips.begin(), scrips.end());
    b.push_back(2); detail::put_u16(b, 1); b.push_back(channel);
    return b;
}

// ---------------------------------------------------------------------------
// Symbol-token API -> socket topics
// ---------------------------------------------------------------------------

struct SymbolTokens {
    std::vector<std::pair<std::string, std::string>> valid;   // symbol, fytoken
    std::vector<std::string> invalid;
    std::string message;                                       // on rejection
};

/// Parse `{"s":"ok","validSymbol":{"NSE:SBIN-EQ":"10100000003045",...},
/// "invalidSymbol":[...]}`. Only the shapes FYERS returns are accepted.
[[nodiscard]] inline std::expected<SymbolTokens, HsmError>
parse_symbol_tokens(std::string_view body) {
    SymbolTokens out;
    const auto status = detail::json_string(body, "s");
    if (status == "error") {
        out.message = std::string{detail::json_string(body, "message")};
        return std::unexpected(HsmError::SymbolTokenRejected);
    }
    if (status != "ok") return std::unexpected(HsmError::SymbolTokenMalformed);

    const auto read_string = [&](std::size_t& i, std::string& s) -> bool {
        if (i >= body.size() || body[i] != '"') return false;
        const auto end = body.find('"', i + 1);
        if (end == std::string_view::npos) return false;
        s.assign(body.substr(i + 1, end - i - 1));
        i = end + 1;
        return true;
    };
    const auto skip_ws = [&](std::size_t& i) {
        while (i < body.size() && (body[i] == ' ' || body[i] == '\n'
               || body[i] == '\r' || body[i] == '\t')) ++i;
    };

    const auto valid_at = body.find("\"validSymbol\"");
    if (valid_at == std::string_view::npos) return std::unexpected(HsmError::SymbolTokenMalformed);
    std::size_t i = body.find(':', valid_at + 13);
    if (i == std::string_view::npos) return std::unexpected(HsmError::SymbolTokenMalformed);
    ++i; skip_ws(i);
    if (i >= body.size() || body[i] != '{') return std::unexpected(HsmError::SymbolTokenMalformed);
    ++i;
    for (;;) {
        skip_ws(i);
        if (i < body.size() && body[i] == '}') break;
        std::string symbol, token;
        if (!read_string(i, symbol)) return std::unexpected(HsmError::SymbolTokenMalformed);
        skip_ws(i);
        if (i >= body.size() || body[i] != ':') return std::unexpected(HsmError::SymbolTokenMalformed);
        ++i; skip_ws(i);
        if (!read_string(i, token)) return std::unexpected(HsmError::SymbolTokenMalformed);
        out.valid.emplace_back(std::move(symbol), std::move(token));
        skip_ws(i);
        if (i < body.size() && body[i] == ',') { ++i; continue; }
        if (i < body.size() && body[i] == '}') break;
        return std::unexpected(HsmError::SymbolTokenMalformed);
    }

    const auto invalid_at = body.find("\"invalidSymbol\"");
    if (invalid_at != std::string_view::npos) {
        std::size_t j = body.find('[', invalid_at);
        if (j != std::string_view::npos) {
            ++j;
            for (;;) {
                skip_ws(j);
                if (j >= body.size() || body[j] == ']') break;
                std::string symbol;
                if (!read_string(j, symbol)) break;
                out.invalid.push_back(std::move(symbol));
                skip_ws(j);
                if (j < body.size() && body[j] == ',') ++j;
            }
        }
    }
    return out;
}

[[nodiscard]] inline std::string_view segment_of(std::string_view fytoken) noexcept {
    if (fytoken.size() < 4) return {};
    const auto prefix = fytoken.substr(0, 4);
    for (const auto& s : kSegments)
        if (s.prefix == prefix) return s.segment;
    return {};
}

[[nodiscard]] inline std::string_view index_name(std::string_view symbol) noexcept {
    const auto it = std::lower_bound(kIndexNames.begin(), kIndexNames.end(), symbol,
        [](const IndexName& e, std::string_view s) { return e.symbol < s; });
    return (it != kIndexNames.end() && it->symbol == symbol) ? it->exchange_name
                                                             : std::string_view{};
}

enum class DataType : std::uint8_t { SymbolUpdate, DepthUpdate };

/// The SDK's symbol_to_hsmtoken() for one valid symbol. Returns an empty
/// string where the SDK skips the symbol (unknown segment, or depth on an
/// index, which has none).
[[nodiscard]] inline std::string topic_for(std::string_view symbol,
                                           std::string_view fytoken,
                                           DataType type) {
    const auto segment = segment_of(fytoken);
    if (segment.empty()) return {};
    const auto dash = symbol.rfind('-');
    const bool is_index = dash != std::string_view::npos
                          && symbol.substr(dash + 1) == "INDEX";
    std::string topic;
    if (is_index) {
        if (type == DataType::DepthUpdate) return {};
        std::string_view token = index_name(symbol);
        if (token.empty()) {
            const auto colon = symbol.find(':');
            auto rest = colon == std::string_view::npos ? symbol : symbol.substr(colon + 1);
            token = rest.substr(0, rest.find('-'));
        }
        topic = "if|";
        topic.append(segment); topic.push_back('|'); topic.append(token);
        return topic;
    }
    if (fytoken.size() <= 10) return {};
    topic = type == DataType::DepthUpdate ? "dp|" : "sf|";
    topic.append(segment); topic.push_back('|'); topic.append(fytoken.substr(10));
    return topic;
}

// ---------------------------------------------------------------------------
// Server -> client frames
// ---------------------------------------------------------------------------

enum class HsmEvent : std::uint8_t {
    None,
    AuthOk, AuthFailed,
    Subscribed, SubscribeFailed,
    Unsubscribed, UnsubscribeFailed,
    ModeOk, ModeFailed,
    ChannelPaused, ChannelResumed, ChannelFailed,
    Data,
    Unknown,
    Malformed,
};

[[nodiscard]] inline const char* event_text(HsmEvent e) noexcept {
    switch (e) {
    case HsmEvent::None:              return "none";
    case HsmEvent::AuthOk:            return "Authentication done";
    case HsmEvent::AuthFailed:        return "Authentication failed";
    case HsmEvent::Subscribed:        return "Subscribed";
    case HsmEvent::SubscribeFailed:   return "subscription failed";
    case HsmEvent::Unsubscribed:      return "Unsubscribed";
    case HsmEvent::UnsubscribeFailed: return "unsubscription failed";
    case HsmEvent::ModeOk:            return "mode set";
    case HsmEvent::ModeFailed:        return "Mode change failed";
    case HsmEvent::ChannelPaused:     return "Channel Paused";
    case HsmEvent::ChannelResumed:    return "Channel Resumed";
    case HsmEvent::ChannelFailed:     return "channel change failed";
    case HsmEvent::Data:              return "data";
    case HsmEvent::Unknown:           return "unknown frame type";
    case HsmEvent::Malformed:         return "malformed frame";
    }
    return "?";
}

struct FrameResult {
    HsmEvent event{HsmEvent::None};
    std::size_t messages{};      ///< JSON updates emitted
    std::size_t unknown_topic{}; ///< updates for a topic never mapped/snapshotted
    bool ack{};                  ///< `ack_bytes` must be sent back
    Bytes ack_bytes;
};

/// Field names in the SDK's order (map.json data_val / index_val / depthvalue).
inline constexpr std::array<std::string_view, 21> kScripFields{
    "ltp", "vol_traded_today", "last_traded_time", "exch_feed_time",
    "bid_size", "ask_size", "bid_price", "ask_price", "last_traded_qty",
    "tot_buy_qty", "tot_sell_qty", "avg_trade_price", "OI", "low_price",
    "high_price", "Yhigh", "Ylow", "lower_ckt", "upper_ckt", "open_price",
    "prev_close_price"};
inline constexpr std::array<std::string_view, 6> kIndexFields{
    "ltp", "prev_close_price", "exch_feed_time", "high_price", "low_price",
    "open_price"};
inline constexpr std::array<std::string_view, 30> kDepthFields{
    "bid_price1", "bid_price2", "bid_price3", "bid_price4", "bid_price5",
    "ask_price1", "ask_price2", "ask_price3", "ask_price4", "ask_price5",
    "bid_size1", "bid_size2", "bid_size3", "bid_size4", "bid_size5",
    "ask_size1", "ask_size2", "ask_size3", "ask_size4", "ask_size5",
    "bid_order1", "bid_order2", "bid_order3", "bid_order4", "bid_order5",
    "ask_order1", "ask_order2", "ask_order3", "ask_order4", "ask_order5"};

enum class HsmTopic : std::uint8_t { Scrip, Index, Depth };

/// One decoded update, as a view of the session's per-topic state. Values are
/// the raw wire integers in the SDK's field order (kScripFields / kIndexFields
/// / kDepthFields); price = value / (10^precision * multiplier) rupees.
/// Valid only inside the emit callback.
struct HsmUpdate {
    HsmTopic topic{};
    std::string_view symbol;        ///< FYERS symbol, e.g. "NSE:SBIN-EQ"
    std::uint32_t cookie{};         ///< caller's id from map_topic()
    const std::int32_t* value{};
    std::uint32_t present{};        ///< bit i: value[i] was sent
    std::uint16_t multiplier{1};
    std::uint8_t precision{};
    [[nodiscard]] bool has(std::size_t i) const noexcept { return (present >> i) & 1u; }
};

inline constexpr std::uint32_t kNoCookie = 0xFFFF'FFFFu;

/// Stateful decoder for one connection. Holds the last value of every field
/// per topic because FYERS sends a snapshot and then only changed fields.
class HsmSession {
public:
    explicit HsmSession(bool lite) : lite_(lite), by_id_(65536, kNoSlot) {}

    /// topic ("sf|nse_cm|3045") -> FYERS symbol ("NSE:SBIN-EQ"), plus an
    /// opaque cookie handed back with every update (the ticker passes the
    /// resolved InstrumentId, so nothing is looked up by name per update).
    /// Kept across reconnects; the snapshot state is not.
    void map_topic(std::string topic, std::string symbol,
                   std::uint32_t cookie = kNoCookie) {
        symbols_[std::move(topic)] = Mapped{std::move(symbol), cookie};
    }
    [[nodiscard]] bool lite() const noexcept { return lite_; }
    [[nodiscard]] std::size_t mapped() const noexcept { return symbols_.size(); }

    /// Forget per-connection state (topic ids, values, ack counter).
    void reset() noexcept {
        std::fill(by_id_.begin(), by_id_.end(), kNoSlot);
        topics_.clear();
        ack_every_ = 0;
        since_ack_ = 0;
    }
    [[nodiscard]] std::uint32_t ack_every() const noexcept { return ack_every_; }

    /// Decode one binary WebSocket message. `emit(const HsmUpdate&)` is called
    /// for every update the SDK would report; render_sdk_json() turns it into
    /// the SDK's on_message JSON, to_fyers_fields() into the decoder's input.
    template <typename Emit>
    [[nodiscard]] FrameResult on_frame(const std::uint8_t* p, std::size_t n, Emit&& emit) {
        FrameResult r{};
        if (p == nullptr || n < 3) { r.event = HsmEvent::Malformed; return r; }
        const std::uint8_t type = p[2];
        switch (type) {
        case 1:  r.event = auth_response(p, n); break;
        case 4:  r.event = short_ack(p, n) ? HsmEvent::Subscribed : HsmEvent::SubscribeFailed; break;
        case 5:  r.event = short_ack(p, n) ? HsmEvent::Unsubscribed : HsmEvent::UnsubscribeFailed; break;
        case 7:
        case 8: {
            const bool ok = channel_ack(p, n);
            r.event = !ok ? HsmEvent::ChannelFailed
                          : (type == 7 ? HsmEvent::ChannelPaused : HsmEvent::ChannelResumed);
            break;
        }
        case 12: r.event = mode_ack(p, n) ? HsmEvent::ModeOk : HsmEvent::ModeFailed; break;
        case 6:  r.event = HsmEvent::Data; data_feed(p, n, r, emit); break;
        default: r.event = HsmEvent::Unknown; break;
        }
        return r;
    }

private:
    using Kind = HsmTopic;
    static constexpr std::uint32_t kNoSlot = 0xFFFF'FFFFu;
    struct Mapped {
        std::string symbol;
        std::uint32_t cookie{kNoCookie};
    };
    struct Topic {
        Kind kind{};
        std::string symbol;
        std::uint32_t cookie{kNoCookie};
        std::array<std::int32_t, 32> value{};
        std::uint32_t present{};        // bit i: field i has a value
        std::uint16_t multiplier{1};
        std::uint8_t precision{};
    };

    static std::uint16_t be16(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }
    static std::uint32_t be32(const std::uint8_t* p) noexcept {
        return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16)
             | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
    }
    static std::int32_t bei32(const std::uint8_t* p) noexcept {
        return static_cast<std::int32_t>(be32(p));
    }

    HsmEvent auth_response(const std::uint8_t* p, std::size_t n) {
        // [len:2][type:1][fields:1][id:1][len:2][str][id:1][len:2][ack:4]
        if (n < 7) return HsmEvent::Malformed;
        const std::size_t len = be16(p + 5);
        if (7 + len > n) return HsmEvent::Malformed;
        const bool ok = len == 1 && p[7] == 'K';
        const std::size_t at = 7 + len + 1 + 2;
        if (at + 4 <= n) ack_every_ = be32(p + at);
        return ok ? HsmEvent::AuthOk : HsmEvent::AuthFailed;
    }
    static bool short_ack(const std::uint8_t* p, std::size_t n) noexcept {
        return n >= 8 && p[7] == 'K';
    }
    static bool channel_ack(const std::uint8_t* p, std::size_t n) noexcept {
        if (n < 8) return false;
        const std::size_t len = be16(p + 5);
        return len == 1 && p[7] == 'K';
    }
    static bool mode_ack(const std::uint8_t* p, std::size_t n) noexcept {
        if (n < 8 || p[3] < 1) return false;
        const std::size_t len = be16(p + 5);
        return len == 1 && p[7] == 'K';
    }

    template <typename Emit>
    void data_feed(const std::uint8_t* p, std::size_t n, FrameResult& r, Emit& emit) {
        if (n < 9) { r.event = HsmEvent::Malformed; return; }
        if (ack_every_ > 0) {
            ++since_ack_;
            if (since_ack_ == ack_every_) {
                r.ack = true;
                r.ack_bytes = ack_frame(be32(p + 3));
                since_ack_ = 0;
            }
        }
        const std::size_t count = be16(p + 7);
        std::size_t at = 9;
        for (std::size_t k = 0; k < count; ++k) {
            if (at >= n) { r.event = HsmEvent::Malformed; return; }
            const std::uint8_t kind = p[at];
            bool ok = false;
            if (kind == 83) ok = snapshot(p, n, at, r, emit);
            else if (kind == 85) ok = full_update(p, n, at, r, emit);
            else if (kind == 76) ok = lite_update(p, n, at, r, emit);
            if (!ok) { r.event = HsmEvent::Malformed; return; }
        }
    }

    template <typename Emit>
    bool snapshot(const std::uint8_t* p, std::size_t n, std::size_t& at,
                  FrameResult& r, Emit& emit) {
        if (at + 4 > n) return false;
        const std::uint16_t id = be16(p + at + 1);
        const std::size_t name_len = p[at + 3];
        at += 4;
        if (at + name_len + 1 > n) return false;
        const std::string name{reinterpret_cast<const char*>(p + at), name_len};
        at += name_len;
        const std::size_t fields = p[at++];
        if (at + fields * 4 + 5 > n) return false;

        Topic t{};
        const auto prefix = std::string_view{name}.substr(0, 2);
        std::size_t known = 0;
        if (prefix == "sf") { t.kind = Kind::Scrip; known = kScripFields.size(); }
        else if (prefix == "if") { t.kind = Kind::Index; known = kIndexFields.size(); }
        else if (prefix == "dp") { t.kind = Kind::Depth; known = kDepthFields.size(); }
        else {
            at += fields * 4 + 5;
            return skip_strings(p, n, at);
        }
        for (std::size_t i = 0; i < fields; ++i, at += 4) {
            const std::int32_t v = bei32(p + at);
            if (i < known && v != kAbsent) { t.value[i] = v; t.present |= 1u << i; }
        }
        at += 2;                                   // unused 16 bits
        t.multiplier = be16(p + at); at += 2;
        t.precision = p[at++];
        if (!skip_strings(p, n, at)) return false; // exchange, token, symbol

        const auto mapped = symbols_.find(name);
        if (mapped == symbols_.end()) { ++r.unknown_topic; return true; }
        t.symbol = mapped->second.symbol;
        t.cookie = mapped->second.cookie;
        std::uint32_t& slot = by_id_[id];
        if (slot == kNoSlot) {
            topics_.push_back(std::move(t));
            slot = static_cast<std::uint32_t>(topics_.size() - 1);
        } else {
            topics_[slot] = std::move(t);
        }
        publish(topics_[slot], r, emit);
        return true;
    }

    template <typename Emit>
    bool full_update(const std::uint8_t* p, std::size_t n, std::size_t& at,
                     FrameResult& r, Emit& emit) {
        if (at + 4 > n) return false;
        const std::uint16_t id = be16(p + at + 1);
        const std::size_t fields = p[at + 3];
        at += 4;
        if (at + fields * 4 > n) return false;
        const std::uint32_t slot = by_id_[id];
        if (slot == kNoSlot) { at += fields * 4; ++r.unknown_topic; return true; }
        Topic& t = topics_[slot];
        const std::size_t known = t.kind == Kind::Scrip ? kScripFields.size()
                                : t.kind == Kind::Index ? kIndexFields.size()
                                                        : kDepthFields.size();
        bool changed = false;
        for (std::size_t i = 0; i < fields; ++i, at += 4) {
            const std::int32_t v = bei32(p + at);
            if (i >= known || v == kAbsent) continue;
            const std::uint32_t bit = 1u << i;
            if (!(t.present & bit) || t.value[i] != v) {
                t.value[i] = v; t.present |= bit; changed = true;
            }
        }
        if (changed) publish(t, r, emit);
        return true;
    }

    template <typename Emit>
    bool lite_update(const std::uint8_t* p, std::size_t n, std::size_t& at,
                     FrameResult& r, Emit& emit) {
        if (at + 7 > n) return false;
        const std::uint16_t id = be16(p + at + 1);
        const std::int32_t v = bei32(p + at + 3);
        at += 7;
        const std::uint32_t slot = by_id_[id];
        if (slot == kNoSlot) { ++r.unknown_topic; return true; }
        Topic& t = topics_[slot];
        if (t.kind == Kind::Depth || v == kAbsent) return true;
        if ((t.present & 1u) && t.value[0] == v) return true;
        t.value[0] = v; t.present |= 1u;
        publish(t, r, emit);
        return true;
    }

    static bool skip_strings(const std::uint8_t* p, std::size_t n, std::size_t& at) noexcept {
        for (int s = 0; s < 3; ++s) {
            if (at >= n) return false;
            const std::size_t len = p[at++];
            if (at + len > n) return false;
            at += len;
        }
        return true;
    }

    template <typename Emit>
    void publish(const Topic& t, FrameResult& r, Emit& emit) {
        if (t.multiplier == 0) return;   // the SDK divides by it
        // Lite mode only ever reports ltp; the SDK emits nothing for depth or
        // for a topic whose ltp has not arrived.
        if (lite_ && (t.kind == Kind::Depth || !((t.present >> 0) & 1u))) return;
        HsmUpdate u;
        u.topic = t.kind;
        u.symbol = t.symbol;
        u.cookie = t.cookie;
        u.value = t.value.data();
        u.present = t.present;
        u.multiplier = t.multiplier;
        u.precision = t.precision;
        ++r.messages;
        emit(static_cast<const HsmUpdate&>(u));
    }

    bool lite_;
    std::uint32_t ack_every_{};
    std::uint32_t since_ack_{};
    std::unordered_map<std::string, Mapped> symbols_;
    std::vector<std::uint32_t> by_id_;   // topic id -> index into topics_, flat
    std::vector<Topic> topics_;
};

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

namespace detail {

inline double pow10(unsigned p) noexcept {
    double v = 1.0;
    for (unsigned i = 0; i < p; ++i) v *= 10.0;
    return v;
}
/// Python's repr() of a float for the magnitudes prices take: shortest
/// round-trip digits, fixed notation, always a decimal point.
inline void put_float(std::string& out, double v) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    std::string_view s{buf, static_cast<std::size_t>(res.ptr - buf)};
    out.append(s);
    if (s.find('.') == std::string_view::npos) out.append(".0");
}
/// Python's round(x, digits): correctly rounded decimal, back to double.
inline double py_round(double x, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, x);
    return std::strtod(buf, nullptr);
}
inline void key(std::string& out, std::string_view k) {
    if (out.size() > 1) out.push_back(',');
    out.push_back('"'); out.append(k); out.append("\":");
}
inline void put_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
}
inline void put_int(std::string& out, std::int64_t v) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, res.ptr);
}

} // namespace detail

/// The SDK's on_message JSON for one update (FyersDataSocket
/// __response_output). For --jsonl recordings and the golden tests; the
/// ticker's hot path uses to_fyers_fields() instead.
inline void render_sdk_json(const HsmUpdate& u, bool lite, std::string& out) {
    using namespace detail;
    const double scale = pow10(u.precision) * static_cast<double>(u.multiplier);
    const auto scaled = [&](std::size_t i) { return static_cast<double>(u.value[i]) / scale; };
    const char* type = u.topic == HsmTopic::Scrip ? "sf" : u.topic == HsmTopic::Index ? "if" : "dp";
    out.assign("{");
    if (lite) {
        key(out, "ltp"); put_float(out, scaled(0));
        key(out, "symbol"); put_string(out, u.symbol);
        key(out, "type"); put_string(out, type);
    } else if (u.topic == HsmTopic::Depth) {
        for (std::size_t i = 0; i < kDepthFields.size(); ++i) {
            if (!u.has(i)) continue;
            key(out, kDepthFields[i]);
            if (i < 10) put_float(out, scaled(i)); else put_int(out, u.value[i]);
        }
        key(out, "type"); put_string(out, type);
        key(out, "symbol"); put_string(out, u.symbol);
    } else if (u.topic == HsmTopic::Index) {
        bool change_done = false;
        for (std::size_t i = 0; i < kIndexFields.size(); ++i) {
            if (u.has(i)) {
                key(out, kIndexFields[i]);
                if (i == 2) put_int(out, u.value[i]); else put_float(out, scaled(i));
            }
            if (!change_done && u.has(0) && u.has(1) && i >= 1 && u.value[1] != 0) {
                const double ch = py_round(scaled(0) - scaled(1), 2);
                key(out, "ch"); put_float(out, ch);
                key(out, "chp"); put_float(out, py_round(ch / scaled(1) * 100.0, 2));
                change_done = true;
            }
        }
        key(out, "type"); put_string(out, type);
        key(out, "symbol"); put_string(out, u.symbol);
    } else {
        static constexpr std::uint32_t kScaled =
            (1u << 0) | (1u << 6) | (1u << 7) | (1u << 11) | (1u << 13)
            | (1u << 14) | (1u << 19) | (1u << 20);
        for (std::size_t i = 0; i < kScripFields.size(); ++i) {
            if (!u.has(i) || i == 12 || i == 15 || i == 16) continue; // OI, Yhigh, Ylow popped
            key(out, kScripFields[i]);
            if (i == 17 || i == 18) put_int(out, 0);                   // circuits zeroed
            else if ((kScaled >> i) & 1u) put_float(out, scaled(i));
            else put_int(out, u.value[i]);
        }
        key(out, "type"); put_string(out, type);
        key(out, "symbol"); put_string(out, u.symbol);
        if (!u.has(17)) { key(out, "lower_ckt"); put_int(out, 0); }
        if (!u.has(18)) { key(out, "upper_ckt"); put_int(out, 0); }
        if (u.has(0) && u.has(20) && u.value[20] != 0) {
            const double ch = py_round(scaled(0) - scaled(20), 4);
            key(out, "ch"); put_float(out, ch);
            key(out, "chp"); put_float(out, py_round(ch / scaled(20) * 100.0, 4));
        }
    }
    out.push_back('}');
}

/// Wire integer -> paise, exactly: value / (10^precision * multiplier) rupees.
/// False when the price is not a whole number of paise (the JSON path's
/// parse_paise refuses the same prices) or the scale is out of range.
[[nodiscard]] inline bool hsm_paise(std::int32_t value, std::uint8_t precision,
                                    std::uint16_t multiplier, Price& out) noexcept {
    if (multiplier == 0 || precision > 9) return false;
    std::int64_t denom = multiplier;
    for (unsigned i = 0; i < precision; ++i) denom *= 10;
    const std::int64_t num = static_cast<std::int64_t>(value) * 100;
    if (num % denom != 0) return false;
    out = Price{num / denom};
    return true;
}

/// The decoder's typed input for one update, without JSON. `id` is the
/// instrument the caller resolved for this topic (map_topic's cookie).
/// Same fields the JSON path reads, plus open interest, which the SDK's JSON
/// drops. Returns false for an update the decoder has nothing to take from.
[[nodiscard]] inline bool to_fyers_fields(const HsmUpdate& u, InstrumentId id,
                                          FyersFields& f) noexcept {
    f = FyersFields{};
    f.id = id;
    if (u.topic == HsmTopic::Depth) {
        f.type = FyersMessageType::DepthUpdate;
        for (std::size_t i = 0; i < kDepthLevels; ++i) {
            if (!u.has(i) || !u.has(5 + i) || !u.has(10 + i) || !u.has(15 + i)) break;
            if (!hsm_paise(u.value[i], u.precision, u.multiplier, f.bid_px[i])
                || !hsm_paise(u.value[5 + i], u.precision, u.multiplier, f.ask_px[i])
                || u.value[10 + i] < 0 || u.value[15 + i] < 0)
                break;
            f.bid_qty[i] = Qty{u.value[10 + i]};
            f.ask_qty[i] = Qty{u.value[15 + i]};
            if (u.has(20 + i) && u.value[20 + i] >= 0)
                f.bid_orders[i] = static_cast<std::uint32_t>(u.value[20 + i]);
            if (u.has(25 + i) && u.value[25 + i] >= 0)
                f.ask_orders[i] = static_cast<std::uint32_t>(u.value[25 + i]);
            ++f.levels;
        }
        return true;
    }
    f.type = u.topic == HsmTopic::Index ? FyersMessageType::IndexUpdate
                                        : FyersMessageType::SymbolUpdate;
    if (u.has(0))
        f.ltp_state = hsm_paise(u.value[0], u.precision, u.multiplier, f.ltp)
                          ? FyersFields::LtpState::Ok
                          : FyersFields::LtpState::Invalid;
    if (u.topic == HsmTopic::Scrip) {
        if (u.has(1) && u.value[1] >= 0) { f.has_volume = true; f.volume = Qty{u.value[1]}; }
        if (u.has(2)) f.last_traded_time_s = u.value[2];
        if (u.has(8) && u.value[8] >= 0) { f.has_last_qty = true; f.last_qty = Qty{u.value[8]}; }
        if (u.has(12)) { f.has_oi = true; f.oi = u.value[12]; }
    }
    return true;
}

} // namespace altair::fyers_hsm
