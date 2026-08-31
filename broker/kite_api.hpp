// broker/kite_api.hpp — the Kite Connect API surface, transcribed not guessed.
//
// P2-11. Every string in this file is copied from gokiteconnect **v4.4.2**
// (verified: `git describe` reports v4.4.2 at 028ce8b) — `connect.go` for the
// constants and endpoints, `orders.go` for the order parameters, `errors.go`
// for the error taxonomy. Nothing here was inferred from documentation prose.
//
// It exists so that Phase 4's OMS cannot invent a product code or misspell a
// parameter. A wrong constant does not fail to compile: it reaches the
// exchange and is either rejected with a generic message or, worse, accepted
// as a different kind of order than the one intended.
//
// ─────────────────────────────────────────────────────────────────────────
// THE ASYMMETRY THAT MATTERS MOST
//
//   The WebSocket sends prices as INTEGER PAISE.
//   The REST order API takes prices as DECIMAL RUPEES.
//
// `OrderParams.Price` is a Go `float64` serialised straight to the query
// string by `query.Values` with no scaling at all. So a price that arrives on
// the wire as 250005 must be sent back as "2500.05".
//
// Getting this backwards is a 100x error in either direction, and it does not
// look like a bug: an order at 100x the intended price is rejected by the
// exchange's price band, and one at 1/100th sits in the book looking patient.
// `kite_paise_to_rupees` below is the only sanctioned conversion, and it never
// forms a double.
// ─────────────────────────────────────────────────────────────────────────

#pragma once

#include <core/types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair::kite {

// ── transport ────────────────────────────────────────────────────────────
inline constexpr const char* kBaseUri = "https://api.kite.trade";
inline constexpr const char* kLoginBaseUri = "https://kite.zerodha.com";
inline constexpr const char* kVersionHeader = "X-Kite-Version";
inline constexpr const char* kVersion = "3";
/// gokiteconnect's own default. Not a guess, and not a number to raise
/// casually: a request still in flight at 09:15 is a decision made late.
inline constexpr std::int64_t kRequestTimeoutMs = 7000;

// ── varieties (the {variety} path segment on /orders) ────────────────────
inline constexpr const char* kVarietyRegular = "regular";
inline constexpr const char* kVarietyAmo     = "amo";
inline constexpr const char* kVarietyBo      = "bo";
inline constexpr const char* kVarietyCo      = "co";
inline constexpr const char* kVarietyIceberg = "iceberg";
inline constexpr const char* kVarietyAuction = "auction";

// ── products ─────────────────────────────────────────────────────────────
// MIS is intraday and auto-squares off; CNC is delivery; NRML is F&O carry.
// Sending CNC where MIS was meant takes delivery and changes the margin, the
// STT treatment and the holding period all at once.
inline constexpr const char* kProductBo   = "BO";
inline constexpr const char* kProductCo   = "CO";
inline constexpr const char* kProductMis  = "MIS";
inline constexpr const char* kProductCnc  = "CNC";
inline constexpr const char* kProductNrml = "NRML";
inline constexpr const char* kProductMtf  = "MTF";

// ── order types ──────────────────────────────────────────────────────────
inline constexpr const char* kOrderTypeMarket = "MARKET";
inline constexpr const char* kOrderTypeLimit  = "LIMIT";
inline constexpr const char* kOrderTypeSl     = "SL";
inline constexpr const char* kOrderTypeSlm    = "SL-M";   // note the hyphen

// ── validity ─────────────────────────────────────────────────────────────
inline constexpr const char* kValidityDay = "DAY";
inline constexpr const char* kValidityIoc = "IOC";
inline constexpr const char* kValidityTtl = "TTL";        // needs validity_ttl

// ── transaction type ─────────────────────────────────────────────────────
inline constexpr const char* kTransactionBuy  = "BUY";
inline constexpr const char* kTransactionSell = "SELL";

// ── exchanges ────────────────────────────────────────────────────────────
// NFO/BFO are the derivative segments; CDS/BCD are currency, which P2-02
// refuses because their wire scale is finer than a paisa.
inline constexpr const char* kExchangeNse = "NSE";
inline constexpr const char* kExchangeBse = "BSE";
inline constexpr const char* kExchangeMcx = "MCX";
inline constexpr const char* kExchangeNfo = "NFO";
inline constexpr const char* kExchangeBfo = "BFO";
inline constexpr const char* kExchangeCds = "CDS";
inline constexpr const char* kExchangeBcd = "BCD";

// ── order status ─────────────────────────────────────────────────────────
inline constexpr const char* kStatusComplete  = "COMPLETE";
inline constexpr const char* kStatusRejected  = "REJECTED";
inline constexpr const char* kStatusCancelled = "CANCELLED";

// ── margins ──────────────────────────────────────────────────────────────
inline constexpr const char* kMarginsEquity    = "equity";
inline constexpr const char* kMarginsCommodity = "commodity";

/// Market protection: -1 asks Kite to pick the cap itself.
inline constexpr int kMarketProtectionAuto = -1;

// ── endpoints. %s / %d are the substitutions gokiteconnect documents. ─────
inline constexpr const char* kUriSession        = "/session/token";
inline constexpr const char* kUriSessionRenew   = "/session/refresh_token";
inline constexpr const char* kUriUserProfile    = "/user/profile";
inline constexpr const char* kUriUserMargins    = "/user/margins";
inline constexpr const char* kUriUserMarginsSeg = "/user/margins/%s";      // {segment}

inline constexpr const char* kUriOrders         = "/orders";
inline constexpr const char* kUriTrades         = "/trades";
inline constexpr const char* kUriOrderHistory   = "/orders/%s";            // {order_id}
inline constexpr const char* kUriOrderTrades    = "/orders/%s/trades";     // {order_id}
inline constexpr const char* kUriPlaceOrder     = "/orders/%s";            // {variety}
inline constexpr const char* kUriModifyOrder    = "/orders/%s/%s";         // {variety}/{order_id}
inline constexpr const char* kUriCancelOrder    = "/orders/%s/%s";         // {variety}/{order_id}

inline constexpr const char* kUriPositions      = "/portfolio/positions";
inline constexpr const char* kUriHoldings       = "/portfolio/holdings";
inline constexpr const char* kUriConvertPos     = "/portfolio/positions";

inline constexpr const char* kUriOrderMargins   = "/margins/orders";
inline constexpr const char* kUriBasketMargins  = "/margins/basket";
inline constexpr const char* kUriOrderCharges   = "/charges/orders";

inline constexpr const char* kUriInstruments    = "/instruments";
inline constexpr const char* kUriInstrumentsEx  = "/instruments/%s";       // {exchange}

// ── order parameter names, from OrderParams' `url:` tags ─────────────────
// These are the exact wire names. A misspelling here is silently dropped by
// `omitempty` on the far side rather than rejected, so an order goes out
// missing a field it appeared to set.
inline constexpr const char* kParamExchange        = "exchange";
inline constexpr const char* kParamTradingsymbol   = "tradingsymbol";
inline constexpr const char* kParamValidity        = "validity";
inline constexpr const char* kParamValidityTtl     = "validity_ttl";
inline constexpr const char* kParamProduct         = "product";
inline constexpr const char* kParamOrderType       = "order_type";
inline constexpr const char* kParamTransactionType = "transaction_type";
inline constexpr const char* kParamQuantity        = "quantity";
inline constexpr const char* kParamDisclosedQty    = "disclosed_quantity";
inline constexpr const char* kParamPrice           = "price";
inline constexpr const char* kParamTriggerPrice    = "trigger_price";
inline constexpr const char* kParamSquareoff       = "squareoff";
inline constexpr const char* kParamStoploss        = "stoploss";
inline constexpr const char* kParamTrailingSl      = "trailing_stoploss";
inline constexpr const char* kParamIcebergLegs     = "iceberg_legs";
inline constexpr const char* kParamIcebergQty      = "iceberg_quantity";
inline constexpr const char* kParamAuctionNumber   = "auction_number";
inline constexpr const char* kParamMarketProtect   = "market_protection";
inline constexpr const char* kParamAutoslice       = "autoslice";
inline constexpr const char* kParamAlgoId          = "algo_id";
inline constexpr const char* kParamTag             = "tag";

// ── error taxonomy, from errors.go ───────────────────────────────────────
inline constexpr const char* kErrGeneral    = "GeneralException";
inline constexpr const char* kErrToken      = "TokenException";   // session died
inline constexpr const char* kErrPermission = "PermissionError";
inline constexpr const char* kErrUser       = "UserException";
inline constexpr const char* kErrTwoFA      = "TwoFAException";
inline constexpr const char* kErrOrder      = "OrderException";
inline constexpr const char* kErrInput      = "InputException";
inline constexpr const char* kErrData       = "DataException";
inline constexpr const char* kErrNetwork    = "NetworkException";

enum class RupeeFormatError : std::uint8_t {
    BufferTooSmall
};

/// Integer paise -> the decimal-rupee string the REST API expects.
/// UNIT: in paise, out a NUL-terminated decimal string of rupees.
///
/// The EXACT INVERSE of P1-04's `parse_rupees_to_paise`, and the only
/// sanctioned way to put a price into an order. It never forms a double:
/// `250005 / 100.0` is representable, but the general case is not, and a price
/// that drifts by one unit is an order at the wrong tick.
///
/// Always two decimal places, because that is what the exchange prices in and
/// a bare integer would be ambiguous to read in a log.
/// `out` needs 24 bytes to hold any int64 paise with sign and point.
[[nodiscard]] inline std::expected<std::size_t, RupeeFormatError>
kite_paise_to_rupees(Price p, char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap < 24) {
        return std::unexpected(RupeeFormatError::BufferTooSmall);
    }
    std::int64_t v = p.raw();
    const bool neg = v < 0;

    // Negate in UNSIGNED space: -INT64_MIN is not representable, and forming
    // it is UB rather than a large number.
    std::uint64_t a = neg ? (~static_cast<std::uint64_t>(v) + 1u)
                          : static_cast<std::uint64_t>(v);

    const std::uint64_t rupees = a / 100u;
    const unsigned paise = static_cast<unsigned>(a % 100u);

    char digits[24];
    std::size_t n = 0;
    if (rupees == 0) {
        digits[n++] = '0';
    } else {
        std::uint64_t r = rupees;
        while (r > 0) {
            digits[n++] = static_cast<char>('0' + (r % 10u));
            r /= 10u;
        }
    }

    std::size_t o = 0;
    if (neg) {
        out[o++] = '-';
    }
    while (n > 0) {
        out[o++] = digits[--n];
    }
    out[o++] = '.';
    out[o++] = static_cast<char>('0' + paise / 10u);
    out[o++] = static_cast<char>('0' + paise % 10u);
    out[o] = '\0';
    return o;
}

} // namespace altair::kite
