// feed/fyers_decoder.hpp -- FYERS v3 JSON market-data message -> normalized
// Tick/DepthUpdate.  No socket, credentials, allocation or clock is used.
#pragma once

#include <feed/tick.hpp>
#include <instruments/contract_spec.hpp>

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

namespace altair {

enum class FyersDecodeError : std::uint8_t {
    Malformed,
    MissingIdentity,
    MissingPrice,
    InvalidPrice,
    OutputFull
};

enum class FyersMessageType : std::uint8_t {
    SymbolUpdate,
    IndexUpdate,
    DepthUpdate
};

struct FyersDecodeResult {
    std::size_t ticks{};
    std::size_t depths{};
    std::size_t unknown_token{};
    std::size_t blocked{};
    std::size_t invalid_field{};
};

namespace fyers_detail {

[[nodiscard]] inline std::string_view value(std::string_view json,
                                             std::string_view key) noexcept {
    std::size_t p = 0;
    for (;;) {
        p = json.find('"', p);
        if (p == std::string_view::npos) return {};
        const std::size_t begin = p + 1;
        if (begin + key.size() < json.size()
            && json.substr(begin, key.size()) == key
            && json[begin + key.size()] == '"') {
            p = json.find(':', begin + key.size() + 1);
            if (p == std::string_view::npos) return {};
            ++p;
            while (p < json.size()
                   && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r'
                       || json[p] == '\n')) ++p;
            if (p >= json.size()) return {};
            if (json[p] == '"') {
                const std::size_t s = ++p;
                bool escaped = false;
                for (; p < json.size(); ++p) {
                    if (escaped) { escaped = false; continue; }
                    if (json[p] == '\\') { escaped = true; continue; }
                    if (json[p] == '"') return json.substr(s, p - s);
                }
                return {};
            }
            const std::size_t s = p;
            while (p < json.size() && json[p] != ',' && json[p] != '}'
                   && json[p] != ']' && json[p] != ' ' && json[p] != '\n'
                   && json[p] != '\r' && json[p] != '\t') ++p;
            return json.substr(s, p - s);
        }
        ++p;
    }
}

[[nodiscard]] inline bool parse_u32(std::string_view text,
                                    std::uint32_t& out) noexcept {
    if (text.empty()) return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

[[nodiscard]] inline bool parse_i64(std::string_view text,
                                    std::int64_t& out) noexcept {
    if (text.empty()) return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

/// Convert a decimal rupee JSON value into exact paise. FYERS prices are
/// decimal values in websocket JSON; binary floating point is never used.
[[nodiscard]] inline bool parse_paise(std::string_view text,
                                      Price& out) noexcept {
    if (text.empty() || text.size() > 40) return false;
    bool negative = false;
    if (text.front() == '-') { negative = true; text.remove_prefix(1); }
    if (text.empty()) return false;
    std::uint64_t whole = 0, fraction = 0;
    unsigned digits = 0;
    bool dot = false;
    for (char c : text) {
        if (c == '.' && !dot) { dot = true; continue; }
        if (c < '0' || c > '9' || (dot && digits == 2)) return false;
        if (dot) { fraction = fraction * 10u + static_cast<unsigned>(c - '0'); ++digits; }
        else {
            const auto d = static_cast<std::uint64_t>(c - '0');
            if (whole > (std::numeric_limits<std::uint64_t>::max() - d) / 10u)
                return false;
            whole = whole * 10u + d;
        }
    }
    if (digits == 1) fraction *= 10u;
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / 100u)
        return false;
    const std::uint64_t magnitude = whole * 100u + fraction;
    if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                    + static_cast<std::uint64_t>(negative)) return false;
    if (negative && magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u)
        out = Price{std::numeric_limits<std::int64_t>::min()};
    else
        out = Price{negative ? -static_cast<std::int64_t>(magnitude)
                             : static_cast<std::int64_t>(magnitude)};
    return true;
}

[[nodiscard]] inline bool price(std::string_view json, std::string_view key,
                                Price& out) noexcept {
    return parse_paise(value(json, key), out);
}

[[nodiscard]] inline bool qty(std::string_view json, std::string_view key,
                              Qty& out) noexcept {
    std::int64_t n = 0;
    if (!parse_i64(value(json, key), n) || n < 0) return false;
    out = Qty{n};
    return true;
}

} // namespace fyers_detail

/// Decode one FYERS v3 data-socket message. The official message types are
/// `sf`/`if` for symbol/index updates and `dp` for five-level depth. A
/// `fyToken` is preferred for identity; the canonical symbol is a safe
/// fallback for fixtures and adapters that do not receive the token field.
[[nodiscard]] inline std::expected<FyersDecodeResult, FyersDecodeError>
decode_fyers_message(std::string_view json, const SpecStore& store,
                      Timestamp recv_ts, std::uint32_t& seq,
                      Tick* ticks, std::size_t tick_cap,
                      DepthUpdate* depths, std::size_t depth_cap) noexcept {
    FyersDecodeResult result{};
    if (json.empty()) return std::unexpected(FyersDecodeError::Malformed);
    const auto type = fyers_detail::value(json, "type");
    if (type.empty()) return std::unexpected(FyersDecodeError::Malformed);
    FyersMessageType message_type{};
    if (type == "sf") message_type = FyersMessageType::SymbolUpdate;
    else if (type == "if") message_type = FyersMessageType::IndexUpdate;
    else if (type == "dp") message_type = FyersMessageType::DepthUpdate;
    else return std::unexpected(FyersDecodeError::Malformed);

    std::uint32_t token = 0;
    auto token_text = fyers_detail::value(json, "fyToken");
    bool have_token = fyers_detail::parse_u32(token_text, token);
    InstrumentId id = InstrumentId::Invalid;
    if (have_token) {
        const auto found = store.id_of(FeedSource::Fyers, token);
        if (!found) { ++result.unknown_token; return result; }
        id = *found;
    } else {
        const auto symbol = fyers_detail::value(json, "symbol");
        if (symbol.empty() || symbol.size() > kMaxSymbolLen)
            return std::unexpected(FyersDecodeError::MissingIdentity);
        char name[kMaxSymbolLen + 1]{};
        for (std::size_t i = 0; i < symbol.size(); ++i) name[i] = symbol[i];
        const auto found = store.id_of_symbol(name);
        if (!found) { ++result.unknown_token; return result; }
        id = *found;
    }
    if (store.is_blocked(id)) { ++result.blocked; return result; }

    const bool is_depth = message_type == FyersMessageType::DepthUpdate;
    if (!is_depth) {
        if (ticks == nullptr || tick_cap == 0)
            return std::unexpected(FyersDecodeError::OutputFull);
        Price last{};
        const auto ltp = fyers_detail::value(json, "ltp");
        if (ltp.empty())
            return std::unexpected(FyersDecodeError::MissingPrice);
        if (!fyers_detail::parse_paise(ltp, last) || last.raw() < 0)
            return std::unexpected(FyersDecodeError::InvalidPrice);
        Tick& t = ticks[0];
        t = Tick{};
        t.id = id;
        t.seq = seq++;
        t.recv_ts = recv_ts;
        t.exchange_ts = recv_ts;
        t.last = last;
        t.source = FeedSource::Fyers;
        t.flags = set_flag(t.flags, TickFlag::NoExchangeTs);
        (void)fyers_detail::qty(json, "last_traded_qty", t.last_qty);
        (void)fyers_detail::qty(json, "vol_traded_today", t.volume);
        std::int64_t oi = 0;
        if (fyers_detail::parse_i64(fyers_detail::value(json, "oi"), oi)) t.oi = oi;
        std::int64_t seconds = 0;
        constexpr std::int64_t kMaxExchangeSeconds =
            std::numeric_limits<std::int64_t>::max() / 1'000'000'000LL;
        if (fyers_detail::parse_i64(fyers_detail::value(json, "last_traded_time"), seconds)
            && seconds > 0 && seconds <= kMaxExchangeSeconds) {
            t.exchange_ts = Timestamp{seconds * 1'000'000'000LL};
            t.flags = static_cast<std::uint16_t>(
                t.flags & ~static_cast<std::uint16_t>(TickFlag::NoExchangeTs));
        }
        result.ticks = 1;
        return result;
    }

    if (depths == nullptr || depth_cap == 0)
        return std::unexpected(FyersDecodeError::OutputFull);
    DepthUpdate& d = depths[0];
    d = DepthUpdate{};
    d.id = id;
    d.seq = seq++;
    d.exchange_ts = recv_ts;
    d.recv_ts = recv_ts;
    d.source = FeedSource::Fyers;
    for (std::size_t i = 0; i < kDepthLevels; ++i) {
        char bid_px[16]{}, ask_px[16]{}, bid_qty[16]{}, ask_qty[16]{};
        const int n = static_cast<int>(i + 1);
        std::snprintf(bid_px, sizeof(bid_px), "bid_price%d", n);
        std::snprintf(ask_px, sizeof(ask_px), "ask_price%d", n);
        std::snprintf(bid_qty, sizeof(bid_qty), "bid_size%d", n);
        std::snprintf(ask_qty, sizeof(ask_qty), "ask_size%d", n);
        if (!fyers_detail::price(json, bid_px, d.bid[i].px)
            || !fyers_detail::price(json, ask_px, d.ask[i].px)
            || !fyers_detail::qty(json, bid_qty, d.bid[i].qty)
            || !fyers_detail::qty(json, ask_qty, d.ask[i].qty)) {
            if (i == 0) return std::unexpected(FyersDecodeError::Malformed);
            break;
        }
        if (d.bid[i].px.raw() < 0 || d.ask[i].px.raw() < 0)
            return std::unexpected(FyersDecodeError::InvalidPrice);
        std::snprintf(bid_qty, sizeof(bid_qty), "bid_order%d", n);
        std::snprintf(ask_qty, sizeof(ask_qty), "ask_order%d", n);
        std::uint32_t orders = 0;
        if (fyers_detail::parse_u32(fyers_detail::value(json, bid_qty), orders))
            d.bid[i].orders = orders;
        orders = 0;
        if (fyers_detail::parse_u32(fyers_detail::value(json, ask_qty), orders))
            d.ask[i].orders = orders;
        ++d.bid_levels;
        ++d.ask_levels;
    }
    if (d.bid_levels == 0 || d.ask_levels == 0)
        return std::unexpected(FyersDecodeError::Malformed);
    result.depths = 1;
    return result;
}

} // namespace altair
