// oms/kite_adapter.hpp -- Kite's vocabulary, translated.
//
// P4-05. Turns an Altair order intent into the form Kite's REST API wants,
// and turns Kite's order-status strings back into P4-04 state transitions.
//
// It builds requests and interprets responses. It does NOT send them: the
// transport is `broker/https_client.hpp`, and keeping the translation separate
// means every mapping below is testable without a network, a credential or a
// live session -- which is why this card has a hundred assertions and no
// broker.
//
// AN UNKNOWN STATUS IS AN ERROR, NEVER A DEFAULT.
//
// Kite publishes about a dozen order statuses and adds to them. The tempting
// mapping is a switch with a default arm sending anything unrecognised to
// something harmless-looking -- Open, usually, or the previous state. Both are
// the phantom-default defect in its most expensive form: a status Altair does
// not understand becomes an order Altair believes is resting, and the position
// underneath it is real.
//
// So `map_status` returns `std::expected` and an unrecognised string is
// refused. A refused status blocks the symbol and raises a flag (rule 9). It
// is better to stop trading one instrument than to hold a position whose state
// is a guess.
//
// PRICES CROSS THE WIRE AS RUPEES AND LIVE HERE AS PAISE.
//
// Kite quotes and accepts decimal rupees; the ledger is integer paise (rule 3).
// The conversion happens at this boundary and nowhere else, and it is exact for
// every price NSE can quote because the tick is 5 paise. Anything that does not
// land on a whole paisa is REFUSED rather than rounded -- a rounded order price
// is an order at a price nobody chose.

#pragma once

#include <broker/kite_api.hpp>
#include <oms/order_state.hpp>
#include <risk/cost.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>

namespace altair {

// The P2-11 constants live in `altair::kite`. Pulled in by name rather than
// with a blanket using-directive, so it stays visible which strings are
// Zerodha's vocabulary and which are Altair's.
using kite::kExchangeNfo;
using kite::kExchangeNse;
using kite::kExchangeBse;
using kite::kProductMis;
using kite::kProductNrml;
using kite::kProductCnc;
using kite::kOrderTypeMarket;
using kite::kOrderTypeLimit;
using kite::kOrderTypeSl;
using kite::kOrderTypeSlm;
using kite::kValidityDay;
using kite::kValidityIoc;
using kite::kVarietyRegular;
using kite::kTransactionBuy;
using kite::kTransactionSell;

enum class AdapterError : std::uint8_t {
    /// A Kite status string this build does not know. Blocks the symbol.
    UnknownStatus,
    /// A price that is not a whole number of paise, or is negative.
    UnrepresentablePrice,
    /// Quantity, symbol or exchange missing or nonsensical.
    MalformedIntent,
    /// The response did not carry the field the contract says it must.
    MalformedResponse,
    /// The request would not fit the caller's buffer.
    BufferTooSmall
};

/// Everything needed to place one order.
struct OrderIntentKite {
    std::string_view exchange;        ///< kExchangeNfo etc.
    std::string_view tradingsymbol;   ///< exactly as the instrument dump has it
    Side side = Side::Buy;
    Qty quantity{0};                  ///< POSITIVE; direction is `side`
    /// Limit price in PAISE. Zero for a market order.
    Price limit_price{0};
    /// Trigger price in PAISE, for SL / SL-M. Zero when not used.
    Price trigger_price{0};
    std::string_view product = kProductMis;
    std::string_view order_type = kOrderTypeLimit;
    std::string_view validity = kValidityDay;
    std::string_view variety = kVarietyRegular;
    /// Free-form tag, echoed back on every update. Altair puts its own order
    /// id here so a postback can be matched without a lookup table that could
    /// go stale across a restart.
    std::string_view tag;
};

namespace detail {

/// Paise to the decimal rupee string Kite expects.
///
/// Exact, and refuses what it cannot represent. Two decimals is all Kite
/// accepts for equity and F&O; currency derivatives quote four, which this
/// build does not place orders in and which would need its own path rather
/// than a wider format string.
[[nodiscard]] inline std::expected<std::string, AdapterError>
paise_to_rupee_string(Price p) noexcept {
    if (p.raw() < 0) {
        return std::unexpected(AdapterError::UnrepresentablePrice);
    }
    char buf[32];
    const std::int64_t rupees = p.raw() / 100;
    const std::int64_t paise = p.raw() % 100;
    const int n = std::snprintf(buf, sizeof buf, "%lld.%02lld",
                                static_cast<long long>(rupees),
                                static_cast<long long>(paise));
    if (n <= 0 || n >= static_cast<int>(sizeof buf)) {
        return std::unexpected(AdapterError::UnrepresentablePrice);
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

/// The inverse. Parses Kite's decimal rupees into integer paise.
///
/// REFUSES a third decimal rather than rounding it. A price Kite sends that
/// Altair cannot represent exactly is a disagreement about the ledger, and
/// rounding it would put a number in the book that the broker never quoted.
[[nodiscard]] inline std::expected<Price, AdapterError>
rupee_string_to_paise(std::string_view s) noexcept {
    if (s.empty()) { return std::unexpected(AdapterError::UnrepresentablePrice); }
    bool neg = false;
    std::size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    std::int64_t whole = 0;
    bool any = false;
    for (; i < s.size() && s[i] != '.'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return std::unexpected(AdapterError::UnrepresentablePrice);
        }
        whole = whole * 10 + (s[i] - '0');
        any = true;
    }
    if (!any) { return std::unexpected(AdapterError::UnrepresentablePrice); }
    std::int64_t frac = 0;
    int digits = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') {
                return std::unexpected(AdapterError::UnrepresentablePrice);
            }
            if (digits < 2) { frac = frac * 10 + (s[i] - '0'); ++digits; }
            else if (s[i] != '0') {
                // A third decimal that is not zero cannot be represented.
                return std::unexpected(AdapterError::UnrepresentablePrice);
            }
        }
    }
    while (digits < 2) { frac *= 10; ++digits; }
    const std::int64_t total = whole * 100 + frac;
    return Price{neg ? -total : total};
}

/// Percent-encode a value for a form body. Trading symbols carry '&' and '%'
/// on some instruments, and an unencoded one silently truncates the request.
inline void append_encoded(std::string& out, std::string_view v) {
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : v) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '-' || c == '_'
                       || c == '.' || c == '~';
        if (safe) { out += static_cast<char>(c); }
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
}

inline void append_field(std::string& out, std::string_view k,
                         std::string_view v) {
    if (!out.empty()) { out += '&'; }
    append_encoded(out, k);
    out += '=';
    append_encoded(out, v);
}

} // namespace detail

/// Build the form body for POST /orders/{variety}.
[[nodiscard]] inline std::expected<std::string, AdapterError>
build_place_order(const OrderIntentKite& o) {
    if (o.quantity.raw() <= 0 || o.tradingsymbol.empty()
        || o.exchange.empty()) {
        return std::unexpected(AdapterError::MalformedIntent);
    }
    // A LIMIT order with no price is a market order by accident. Refused:
    // the two differ by an unbounded amount of slippage.
    if (o.order_type == std::string_view{kOrderTypeLimit}
        && o.limit_price.raw() <= 0) {
        return std::unexpected(AdapterError::MalformedIntent);
    }
    // SL and SL-M need a trigger. Without one Kite rejects the order, but
    // catching it here costs nothing and saves a round trip.
    if ((o.order_type == std::string_view{kOrderTypeSl}
         || o.order_type == std::string_view{kOrderTypeSlm})
        && o.trigger_price.raw() <= 0) {
        return std::unexpected(AdapterError::MalformedIntent);
    }

    std::string body;
    body.reserve(256);
    detail::append_field(body, "exchange", o.exchange);
    detail::append_field(body, "tradingsymbol", o.tradingsymbol);
    detail::append_field(body, "transaction_type",
                         o.side == Side::Buy ? kTransactionBuy
                                             : kTransactionSell);
    char qty[24];
    std::snprintf(qty, sizeof qty, "%lld",
                  static_cast<long long>(o.quantity.raw()));
    detail::append_field(body, "quantity", qty);
    detail::append_field(body, "product", o.product);
    detail::append_field(body, "order_type", o.order_type);
    detail::append_field(body, "validity", o.validity);

    if (o.limit_price.raw() > 0) {
        const auto p = detail::paise_to_rupee_string(o.limit_price);
        if (!p) { return std::unexpected(p.error()); }
        detail::append_field(body, "price", *p);
    }
    if (o.trigger_price.raw() > 0) {
        const auto p = detail::paise_to_rupee_string(o.trigger_price);
        if (!p) { return std::unexpected(p.error()); }
        detail::append_field(body, "trigger_price", *p);
    }
    if (!o.tag.empty()) { detail::append_field(body, "tag", o.tag); }
    return body;
}

/// Kite's order status, mapped to a P4-04 state.
///
/// EVERY known status is listed explicitly and there is NO default arm. A
/// status this build has never seen returns UnknownStatus, which blocks the
/// symbol -- better than deciding an order is resting when nobody knows.
[[nodiscard]] inline std::expected<OrderState, AdapterError>
map_status(std::string_view s) noexcept {
    // Terminal.
    if (s == "COMPLETE")  { return OrderState::Filled; }
    if (s == "CANCELLED") { return OrderState::Cancelled; }
    if (s == "REJECTED")  { return OrderState::Rejected; }

    // Resting at the exchange.
    if (s == "OPEN")            { return OrderState::Open; }
    // TRIGGER PENDING is a stop-loss order accepted and WAITING for its
    // trigger. It is live at the exchange and will fill without further
    // action, so it is Open -- treating it as pending would leave Altair
    // thinking it can still walk away from a resting stop.
    if (s == "TRIGGER PENDING") { return OrderState::Open; }

    // In flight at the broker: sent, not yet confirmed by the exchange.
    if (s == "PUT ORDER REQ RECEIVED")     { return OrderState::PendingNew; }
    if (s == "VALIDATION PENDING")         { return OrderState::PendingNew; }
    if (s == "OPEN PENDING")               { return OrderState::PendingNew; }
    if (s == "MODIFY VALIDATION PENDING")  { return OrderState::PendingReplace; }
    if (s == "MODIFY PENDING")             { return OrderState::PendingReplace; }
    if (s == "CANCEL PENDING")             { return OrderState::PendingCancel; }

    // Kite reports a partial fill as OPEN with a non-zero filled_quantity
    // rather than a distinct status, so PartiallyFilled is derived from the
    // quantities in `map_update` and never from the status string alone.
    return std::unexpected(AdapterError::UnknownStatus);
}

/// One order update from Kite -- a poll of /orders or a postback.
struct KiteOrderUpdate {
    std::string_view status;
    /// CUMULATIVE filled quantity, which is what P4-04 wants.
    Qty filled_quantity{0};
    Qty pending_quantity{0};
    /// Average fill price, as Kite's decimal-rupee string.
    std::string_view average_price;
    /// The tag Altair set when placing, echoed back.
    std::string_view tag;
};

/// What an update means for a P4-04 order.
struct MappedUpdate {
    OrderState state = OrderState::Unset;
    Qty cum_qty{0};
    Price avg_price{0};
    /// True when the status said OPEN but a quantity has already filled --
    /// Kite has no distinct partial-fill status.
    bool derived_partial = false;
};

[[nodiscard]] inline std::expected<MappedUpdate, AdapterError>
map_update(const KiteOrderUpdate& u) noexcept {
    const auto st = map_status(u.status);
    if (!st) { return std::unexpected(st.error()); }
    if (u.filled_quantity.raw() < 0 || u.pending_quantity.raw() < 0) {
        return std::unexpected(AdapterError::MalformedResponse);
    }

    MappedUpdate m{};
    m.state = *st;
    m.cum_qty = u.filled_quantity;
    if (!u.average_price.empty()) {
        const auto p = detail::rupee_string_to_paise(u.average_price);
        if (!p) { return std::unexpected(p.error()); }
        m.avg_price = *p;
    }
    // Kite reports a partial as OPEN with filled_quantity > 0. Deriving it
    // from the QUANTITIES rather than the string is the only way to see it.
    if (m.state == OrderState::Open && u.filled_quantity.raw() > 0) {
        m.state = OrderState::PartiallyFilled;
        m.derived_partial = true;
    }
    return m;
}

/// The order id Kite returned from a place/modify/cancel.
[[nodiscard]] inline std::expected<std::string, AdapterError>
parse_order_id(std::string_view body) {
    const std::string_view key = "\"order_id\"";
    const std::size_t p = body.find(key);
    if (p == std::string_view::npos) {
        return std::unexpected(AdapterError::MalformedResponse);
    }
    std::size_t q = body.find(':', p + key.size());
    if (q == std::string_view::npos) {
        return std::unexpected(AdapterError::MalformedResponse);
    }
    ++q;
    while (q < body.size() && (body[q] == ' ' || body[q] == '"')) { ++q; }
    std::string out;
    while (q < body.size() && body[q] != '"' && body[q] != ','
           && body[q] != '}') {
        out += body[q];
        ++q;
    }
    if (out.empty()) {
        return std::unexpected(AdapterError::MalformedResponse);
    }
    return out;
}

} // namespace altair
