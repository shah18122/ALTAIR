// broker/kite_quote.hpp -- the /quote response, parsed.
//
// P20-03.
//
// THIS IS WHAT THE WATCHLIST'S EMPTY BID AND ASK COLUMNS HAVE BEEN WAITING
// FOR SINCE P11Q-11.
//
// That card added the columns and left them blank, deliberately: the
// instrument master is REFERENCE data and has no prices, and a watchlist with
// no bid column looks complete while one with an empty bid column says what is
// missing. `/quote` is the endpoint that fills them, and it is the one call in
// the Kite surface that returns a BID AND AN ASK together -- which is also why
// it is the only thing that can unblock P17-04's VRP sleeve.
//
// A CANDLE IS NOT A QUOTE, AND THAT IS THE WHOLE POINT.
//
// P11Q-05d and P11Q-06 both landed on this: a historical bar carries a CLOSE,
// which is one trade at one instant on whichever side happened to lift. A
// `Touch` is a bid AND an ask, and you buy the wings at the ask and sell the
// body at the bid, so a spread priced from closes is wrong by the spread --
// P11Q-06 measured 37.8% of the range vanishing when bar closes went through a
// tick pipeline. No amount of history fixes it: a million closes contain zero
// bid-ask pairs. Only this endpoint, or a live subscription, does.
//
// FIVE LEVELS, AND THE ZEROED ONES ARE NOT LIQUIDITY.
//
// Kite always returns five depth entries per side. On a thin book the tail is
// zero-filled, and `feed/tick.hpp` D6 already says an unpopulated level is
// zeroed rather than absent -- P11Q-03's depth ladder was built around exactly
// this hazard, because drawing a zeroed slot as a price at zero shows infinite
// liquidity at the best possible price. `DepthLevel::populated` carries the
// distinction so the caller cannot lose it.
//
// PRICES ARRIVE AS DOUBLES AND LEAVE AS PAISE.
//
// The wire format is JSON floating point. Rule 3 says money is integer paise,
// so the conversion happens HERE, once, at the boundary, and rounds to the
// nearest paisa rather than truncating -- truncation biases every price down
// by up to a paisa, which on a bid-ask spread biases the spread itself.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace altair::kite {

enum class QuoteError : std::uint8_t {
    /// The body was not the shape a quote response has.
    Malformed,
    /// Kite reported an error status in the envelope.
    ApiError,
    /// The requested instrument was absent from the response. NOT an empty
    /// quote -- the difference between "no data for this symbol" and "this
    /// symbol has a bid of zero".
    NotPresent
};

[[nodiscard]] inline const char* quote_error_text(QuoteError e) noexcept {
    switch (e) {
    case QuoteError::Malformed:  return "not a quote response";
    case QuoteError::ApiError:   return "Kite returned an error";
    case QuoteError::NotPresent: return "instrument absent from the response";
    }
    return "unknown";
}

/// One depth level. `populated` is the field that matters.
struct DepthLevel {
    Price price{0};
    std::int64_t quantity = 0;
    std::int64_t orders = 0;
    /// FALSE for a zero-filled tail slot. Kite always sends five; a thin book
    /// simply has fewer real ones, and a zeroed slot drawn as a price is
    /// infinite liquidity at the best possible price.
    bool populated = false;
};

struct Quote {
    std::uint32_t instrument_token = 0;
    Price last_price{0};
    std::int64_t last_quantity = 0;
    Price average_price{0};
    std::int64_t volume = 0;
    /// Total pending buy / sell quantity across the whole book, not just the
    /// five visible levels.
    std::int64_t buy_quantity = 0;
    std::int64_t sell_quantity = 0;
    Price open{0}, high{0}, low{0}, close{0};
    double net_change = 0.0;
    /// Open interest. Zero AND meaningless for cash instruments -- see
    /// `has_oi`, because a zero here is not the same as a derivative with no
    /// open interest.
    std::int64_t oi = 0;
    bool has_oi = false;
    Price lower_circuit{0}, upper_circuit{0};
    DepthLevel buy[5]{};
    DepthLevel sell[5]{};
    /// Exchange timestamp, as sent. Kept as the raw string because the two
    /// Kite time formats have different semantics (P0's reference note) and
    /// re-parsing it here would be a third place that has to know.
    std::string timestamp;

    /// Best bid and ask, or absent. THE accessor the watchlist wants.
    [[nodiscard]] bool has_touch() const noexcept {
        return buy[0].populated && sell[0].populated;
    }
    /// Spread in paise. PRECONDITION: has_touch(). A spread computed from an
    /// unpopulated level is the whole price, not a spread.
    [[nodiscard]] std::int64_t spread_paise() const noexcept {
        return sell[0].price.raw() - buy[0].price.raw();
    }
    /// Microprice: the size-weighted touch. Closer to the next trade than the
    /// mid is when the book is lopsided, which is most of the time.
    [[nodiscard]] double microprice() const noexcept {
        const double bq = static_cast<double>(buy[0].quantity);
        const double sq = static_cast<double>(sell[0].quantity);
        if (!(bq + sq > 0.0)) { return 0.0; }
        return (static_cast<double>(buy[0].price.raw()) * sq
                + static_cast<double>(sell[0].price.raw()) * bq) / (bq + sq);
    }
};

namespace detail {

/// Rupees to paise, rounded half away from zero.
///
/// NOT truncated. Truncation biases every price down by up to a paisa, and on
/// a bid-ask pair that biases the SPREAD -- the bid rounds down and the ask
/// rounds down, so the spread is understated whenever the ask's fraction is
/// larger. A cost model fed understated spreads reports edges that are not
/// there.
[[nodiscard]] inline Price to_paise(double rupees) noexcept {
    const double p = rupees * 100.0;
    return Price{static_cast<std::int64_t>(p < 0.0 ? p - 0.5 : p + 0.5)};
}

/// Minimal scanner for the one shape this file needs. A full JSON parser is
/// not warranted for a response with a fixed schema, and the failure mode of a
/// hand-rolled one -- silently returning zero for a key it could not find --
/// is exactly what `found` guards against.
[[nodiscard]] inline bool find_number(std::string_view body, std::size_t from,
                                      std::string_view key, double& out,
                                      std::size_t* at = nullptr) noexcept {
    const std::string pat = std::string("\"") + std::string(key) + "\":";
    const auto k = body.find(pat, from);
    if (k == std::string_view::npos) { return false; }
    std::size_t i = k + pat.size();
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) { ++i; }
    if (i >= body.size()) { return false; }
    if (body.compare(i, 4, "null") == 0) { return false; }
    const std::size_t start = i;
    if (body[i] == '-' || body[i] == '+') { ++i; }
    bool digits = false;
    while (i < body.size()
           && ((body[i] >= '0' && body[i] <= '9') || body[i] == '.'
               || body[i] == 'e' || body[i] == 'E'
               || ((body[i] == '-' || body[i] == '+')
                   && (body[i - 1] == 'e' || body[i - 1] == 'E')))) {
        if (body[i] >= '0' && body[i] <= '9') { digits = true; }
        ++i;
    }
    if (!digits) { return false; }
    out = std::atof(std::string(body.substr(start, i - start)).c_str());
    if (at != nullptr) { *at = i; }
    return true;
}

} // namespace detail

/// Parse one instrument out of a `/quote` response body.
///
/// `key` is the `exchange:tradingsymbol` Kite echoes back, e.g.
/// "NSE:NIFTY 50" or "NFO:NIFTY26SEPFUT".
[[nodiscard]] inline std::expected<Quote, QuoteError>
parse_quote(std::string_view body, std::string_view key) {
    if (body.find("\"status\"") == std::string_view::npos) {
        return std::unexpected(QuoteError::Malformed);
    }
    if (body.find("\"status\":\"success\"") == std::string_view::npos
        && body.find("\"status\": \"success\"") == std::string_view::npos) {
        return std::unexpected(QuoteError::ApiError);
    }
    const std::string quoted = std::string("\"") + std::string(key) + "\"";
    const auto at = body.find(quoted);
    if (at == std::string_view::npos) {
        // ABSENT, not empty. A symbol Kite did not return is a symbol we know
        // nothing about, and returning a zero-filled Quote would put a bid of
        // 0.00 on a watchlist row.
        return std::unexpected(QuoteError::NotPresent);
    }
    // Bound the scan to this instrument's object so a later symbol's fields
    // cannot be read as this one's.
    const auto depth_at = body.find("\"depth\"", at);
    const auto end = depth_at == std::string_view::npos
        ? body.size()
        : body.find('}', body.find(']', body.find("\"sell\"", depth_at)));
    const std::string_view obj =
        body.substr(at, (end == std::string_view::npos ? body.size() : end) - at);

    Quote q;
    double v = 0.0;
    if (detail::find_number(obj, 0, "instrument_token", v)) {
        q.instrument_token = static_cast<std::uint32_t>(v);
    }
    if (detail::find_number(obj, 0, "last_price", v)) {
        q.last_price = detail::to_paise(v);
    }
    if (detail::find_number(obj, 0, "last_quantity", v)) {
        q.last_quantity = static_cast<std::int64_t>(v);
    }
    if (detail::find_number(obj, 0, "average_price", v)) {
        q.average_price = detail::to_paise(v);
    }
    if (detail::find_number(obj, 0, "volume", v)) {
        q.volume = static_cast<std::int64_t>(v);
    }
    if (detail::find_number(obj, 0, "buy_quantity", v)) {
        q.buy_quantity = static_cast<std::int64_t>(v);
    }
    if (detail::find_number(obj, 0, "sell_quantity", v)) {
        q.sell_quantity = static_cast<std::int64_t>(v);
    }
    if (detail::find_number(obj, 0, "net_change", v)) { q.net_change = v; }
    // OI: PRESENCE is the signal, not the value. A cash instrument has no open
    // interest at all, which is different from a future with none outstanding.
    if (detail::find_number(obj, 0, "oi", v)) {
        q.oi = static_cast<std::int64_t>(v);
        q.has_oi = true;
    }
    if (detail::find_number(obj, 0, "lower_circuit_limit", v)) {
        q.lower_circuit = detail::to_paise(v);
    }
    if (detail::find_number(obj, 0, "upper_circuit_limit", v)) {
        q.upper_circuit = detail::to_paise(v);
    }
    const auto ohlc = obj.find("\"ohlc\"");
    if (ohlc != std::string_view::npos) {
        if (detail::find_number(obj, ohlc, "open", v)) { q.open = detail::to_paise(v); }
        if (detail::find_number(obj, ohlc, "high", v)) { q.high = detail::to_paise(v); }
        if (detail::find_number(obj, ohlc, "low", v)) { q.low = detail::to_paise(v); }
        if (detail::find_number(obj, ohlc, "close", v)) { q.close = detail::to_paise(v); }
    }

    // ---- depth, and the zeroed tail -----------------------------------
    auto side = [&](std::string_view name, DepthLevel* out) {
        const auto s = obj.find(std::string("\"") + std::string(name) + "\":[");
        if (s == std::string_view::npos) { return; }
        std::size_t cur = s;
        for (int i = 0; i < 5; ++i) {
            double px = 0.0, qty = 0.0, ord = 0.0;
            std::size_t after = cur;
            if (!detail::find_number(obj, cur, "price", px, &after)) { break; }
            if (!detail::find_number(obj, after, "quantity", qty, &after)) { break; }
            (void)detail::find_number(obj, after, "orders", ord, &after);
            out[i].price = detail::to_paise(px);
            out[i].quantity = static_cast<std::int64_t>(qty);
            out[i].orders = static_cast<std::int64_t>(ord);
            // POPULATED means a real level. Kite zero-fills the tail on a thin
            // book, and a zeroed slot drawn as a price is infinite liquidity
            // at the best possible price (P11Q-03).
            out[i].populated = px > 0.0 && qty > 0.0;
            cur = after;
        }
    };
    const auto depth = obj.find("\"depth\"");
    if (depth != std::string_view::npos) {
        side("buy", q.buy);
        side("sell", q.sell);
    }
    return q;
}

/// Build the `/quote?i=...&i=...` target for a set of `exchange:symbol` keys.
[[nodiscard]] inline std::string quote_target(
    const std::vector<std::string>& keys) {
    std::string t = "/quote";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        t += (i == 0 ? "?i=" : "&i=");
        // Percent-encode the characters that actually occur in an NSE symbol:
        // space (NIFTY 50) and colon (the exchange separator is already in the
        // value). Everything else Kite uses is unreserved.
        for (const char c : keys[i]) {
            if (c == ' ') { t += "%20"; }
            else if (c == ':') { t += "%3A"; }
            else { t += c; }
        }
    }
    return t;
}

} // namespace altair::kite
