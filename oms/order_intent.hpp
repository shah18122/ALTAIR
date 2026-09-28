// oms/order_intent.hpp -- the queue the UI writes and oms/ drains.
//
// P25-03.
//
// WHY THIS EXISTS, AND WHAT IT DELIBERATELY IS NOT.
//
// CLAUDE.md is unconditional: `oms/` is the only thing that can place an order,
// and the in-process decision gave up blast radius in exchange for exactly one
// property kept by construction -- THE UI CANNOT TRADE. Smit asked for buy and
// sell buttons. This is how both are true at once.
//
// The UI writes an INTENT. An intent is a request, and it is weaker than an
// order in four specific ways, each of which is the point:
//
//   1. IT CARRIES LOTS, NOT QUANTITY. The UI never multiplies by a lot size.
//      `oms/` does that, from the point-in-time spec store, at drain time --
//      so a stale lot size in a UI that has been open since Tuesday cannot
//      size an order. Rule 1.
//
//   2. IT CARRIES NO ORDER ID AND NO BROKER REFERENCE. `oms/` assigns those.
//      An intent has no identity in the order state machine until oms/ gives
//      it one, so nothing in the UI can refer to a live order.
//
//   3. IT IS NOT VALIDATED BY THE WRITER. The UI may show a lot size and a
//      tick, and it may refuse obvious nonsense to save a round trip, but
//      `oms/` re-derives every field and may refuse. A UI-side check is a
//      convenience, never a gate.
//
//   4. AN UNDRAINED INTENT IS NOT AN ORDER. It is PENDING. The UI must say so
//      in those words, because the failure mode here is a person believing
//      they are flat when a request is sitting in a file.
//
// A FILE, FOR THE SAME REASON THE KILL SWITCH IS A FILE.
//
// `desktop/kill_switch.hpp`: "A file survives the process, which is the exact
// case this has to work across." The in-process decision means a paint bug can
// take down the process holding live positions. An intent written before that
// happens must still be there afterwards -- either to be drained, or to be
// found and cancelled by a person who needs to know it existed.
//
// Append-only JSONL. The writer never rewrites and never deletes; `oms/` keeps
// its own byte offset. A truncating writer and a reader holding an offset is a
// silent-corruption machine.
//
// NO SHARED HEADER WITH THE UI. CONFORMANCE VECTORS INSTEAD.
//
// `desktop/` does not include this file and does not link `altair_oms`. It has
// its own emitter, and both sides test against `oms/tests/vectors/intents.jsonl`
// -- the pattern CLAUDE.md already sets for `server/` and a remote client:
// "Shared test vectors, not shared code -- if the implementations drift, the
// vectors fail rather than the dashboard quietly showing a wrong number."
//
// CX02-B1. THE PARSER WAS NOT THE STRICT SCHEMA IT CLAIMED TO BE.
//
// It found each key with `line.find("\"lots\":")` -- the FIRST occurrence
// anywhere in the line -- and the only structural check was that the line
// started with `{`. Three findings followed from that one design:
//
//   C13-005  A writer interrupted mid-append, followed by the next append,
//            leaves ONE line holding the front of record A and all of record
//            B. First-occurrence lookup read A's side and lots with B's price
//            and product, and it PARSED: a SELL of 3 NIFTY 50 lots at NIFTY
//            BANK's limit.
//   C13-006  Digits were accumulated with no overflow check (signed overflow
//            is UB), and a token above 2^32 was cast to uint32 -- 4295223561
//            became 256265, a different instrument that parsed as complete.
//   C13-007  First-wins duplicate keys, keys matched inside nested objects,
//            1.5 read as 1, "1" read as 1, any backslash escape half-decoded,
//            and any string accepted for product, validity and exchange.
//
// So this is now a real grammar for the one shape the schema allows: exactly
// one flat object; each of the thirteen keys exactly once; strings with only
// the two escapes the emitter writes; JSON integers checked against overflow;
// and every value inside a stated bound (rule 11 -- refuse, never clamp).
// Anything else is refused with the reason, and the drain counts it.

#pragma once

#include <core/types/units.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace altair::oms {

/// Schema version. A change here is a change both sides must make, and the
/// parser refuses anything it does not know rather than reading a v2 field
/// list as a v1 record.
inline constexpr int kIntentSchemaVersion = 1;

inline constexpr const char* kIntentFile = "data/order_intents.jsonl";

// ── bounds (rule 11: each one REFUSES) ───────────────────────────────────

/// Longest line accepted. The vectors are ~230 bytes; thirteen fields at their
/// caps fit well inside this.
inline constexpr std::size_t kMaxIntentLineBytes = 1024;
/// Longest decoded string value (id, by, symbol, ...).
inline constexpr std::size_t kMaxIntentText = 64;
/// Most lots one intent may ask for. The ticket's own control stops at 100;
/// risk limits in oms/ are tighter again. This is a sanity bound on the file,
/// not a position limit.
inline constexpr std::int64_t kMaxIntentLots = 1000;
/// Rs 1,000,000,000.00. No listed contract trades within four orders of
/// magnitude of it.
inline constexpr std::int64_t kMaxIntentPricePaise = 100'000'000'000LL;

enum class IntentError : std::uint8_t {
    /// Not JSON, or not the object shape this schema defines.
    Malformed,
    /// `v` absent or not `kIntentSchemaVersion`.
    UnknownVersion,
    /// A field the schema requires is absent.
    MissingField,
    /// Present but nonsensical: zero lots, a negative price, an unknown side,
    /// a string where an integer belongs, an impossible date.
    BadValue,
    /// A limit order with no price, or a market order with one. Both are a
    /// different order from the one somebody meant.
    PriceContradictsType,
    // CX02-B1. Appended so the ordinals above keep their meaning.
    /// The same key twice. First-wins and last-wins readers disagree about
    /// which order this is, so it is neither.
    DuplicateField,
    /// A key the schema does not define -- including a nested object's.
    UnknownField,
    /// The line, or one string in it, exceeds its bound.
    TooLong,
    /// A number outside its bound: an int64 overflow, a token above 2^32-1,
    /// lots above kMaxIntentLots, a price above kMaxIntentPricePaise.
    OutOfRange
};

enum class IntentSide : std::uint8_t { Buy, Sell };
enum class IntentType : std::uint8_t { Market, Limit };

/// One request, as parsed. Nothing here is trusted; see the header.
struct OrderIntent {
    int version = 0;
    std::string id;              ///< client-generated, opaque to oms/
    std::string at;              ///< ISO-8601 with offset, as written
    std::string by;              ///< the signed-in user
    std::uint32_t token = 0;     ///< instrument token, from the master
    std::string symbol;          ///< tradingsymbol, for the audit trail
    std::string exchange;
    IntentSide side = IntentSide::Buy;
    /// LOTS. Not quantity. oms/ multiplies by the spec store's lot size.
    std::int64_t lots = 0;
    IntentType order_type = IntentType::Limit;
    /// Limit price in integer PAISE. Zero for a market order, and the parser
    /// refuses the two combinations that contradict each other.
    std::int64_t limit_paise = 0;
    std::string product;
    std::string validity;
    /// `at`, as UTC nanoseconds since the epoch. CX02-B1: the queue ages an
    /// intent by this, so it is parsed and range-checked rather than carried
    /// as an opaque string.
    std::int64_t at_ns = 0;

    /// A drained intent is still only a request. This says whether oms/ has
    /// looked at it, never whether an order exists.
    [[nodiscard]] bool complete() const noexcept {
        return version == kIntentSchemaVersion && !id.empty() && token != 0
            && lots > 0 && !symbol.empty() && !exchange.empty();
    }
};

namespace detail {

inline constexpr std::size_t kIntentKeyCount = 13;

enum IntentField : std::uint8_t {
    kFieldV, kFieldId, kFieldAt, kFieldBy, kFieldToken, kFieldSymbol,
    kFieldExchange, kFieldSide, kFieldLots, kFieldOrderType, kFieldLimitPaise,
    kFieldProduct, kFieldValidity
};

inline constexpr std::array<std::string_view, kIntentKeyCount> kFieldNames{
    "v", "id", "at", "by", "token", "symbol", "exchange", "side", "lots",
    "order_type", "limit_paise", "product", "validity"};

/// True where the value is a JSON integer, false where it is a JSON string.
inline constexpr std::array<bool, kIntentKeyCount> kFieldIsInt{
    true, false, false, false, true, false, false, false, true, false, true,
    false, false};

/// The object, syntactically valid, before any value is judged.
struct RawIntent {
    std::array<bool, kIntentKeyCount> seen{};
    std::array<std::int64_t, kIntentKeyCount> num{};
    std::array<std::string, kIntentKeyCount> text{};
};

[[nodiscard]] constexpr bool is_json_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

class IntentReader {
public:
    explicit IntentReader(std::string_view s) noexcept : s_(s) {}

    void skip_ws() noexcept {
        while (i_ < s_.size() && is_json_ws(s_[i_])) { ++i_; }
    }
    [[nodiscard]] bool at_end() const noexcept { return i_ >= s_.size(); }
    [[nodiscard]] bool next_is(char c) const noexcept {
        return i_ < s_.size() && s_[i_] == c;
    }
    [[nodiscard]] bool eat(char c) noexcept {
        if (!next_is(c)) { return false; }
        ++i_;
        return true;
    }

    /// A JSON string admitting only the `\"` and `\\` escapes -- the two the
    /// emitter writes. Any other escape, and any raw control character, is
    /// refused rather than half-decoded. `cap` bounds the decoded length.
    [[nodiscard]] std::expected<void, IntentError>
    string(std::string& out, std::size_t cap) {
        out.clear();
        if (!eat('"')) { return std::unexpected(IntentError::Malformed); }
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') { return {}; }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::unexpected(IntentError::Malformed);
            }
            char v = c;
            if (c == '\\') {
                if (i_ >= s_.size()) {
                    return std::unexpected(IntentError::Malformed);
                }
                const char e = s_[i_++];
                if (e != '"' && e != '\\') {
                    return std::unexpected(IntentError::Malformed);
                }
                v = e;
            }
            if (out.size() >= cap) {
                return std::unexpected(IntentError::TooLong);
            }
            out.push_back(v);
        }
        return std::unexpected(IntentError::Malformed);      // unterminated
    }

    /// A JSON integer: optional `-`, no `+`, no leading zero, digits only.
    /// A fraction or exponent is left unread, and the object grammar then
    /// refuses the `.` or `e` it finds where a `,` belongs.
    [[nodiscard]] std::expected<std::int64_t, IntentError> integer() noexcept {
        const bool neg = eat('-');
        if (at_end() || s_[i_] < '0' || s_[i_] > '9') {
            return std::unexpected(IntentError::Malformed);
        }
        if (s_[i_] == '0' && i_ + 1 < s_.size() && s_[i_ + 1] >= '0'
            && s_[i_ + 1] <= '9') {
            return std::unexpected(IntentError::Malformed);
        }
        std::int64_t v = 0;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
            const std::int64_t d = s_[i_] - '0';
            if (v > (std::numeric_limits<std::int64_t>::max() - d) / 10) {
                return std::unexpected(IntentError::OutOfRange);
            }
            v = v * 10 + d;
            ++i_;
        }
        return neg ? -v : v;
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;
};

/// Exactly one flat object with known keys, each at most once, then only
/// whitespace to the end of the line.
[[nodiscard]] inline std::expected<RawIntent, IntentError>
parse_object(std::string_view line) {
    if (line.size() > kMaxIntentLineBytes) {
        return std::unexpected(IntentError::TooLong);
    }
    IntentReader r(line);
    RawIntent out;
    r.skip_ws();
    if (!r.eat('{')) { return std::unexpected(IntentError::Malformed); }
    r.skip_ws();
    if (!r.eat('}')) {
        std::string key;
        for (;;) {
            r.skip_ws();
            // The longest key is 11 bytes; a key longer than 16 is unknown by
            // definition, and is reported as that.
            if (const auto k = r.string(key, 16); !k) {
                return std::unexpected(k.error() == IntentError::TooLong
                                           ? IntentError::UnknownField
                                           : k.error());
            }
            std::size_t f = kIntentKeyCount;
            for (std::size_t j = 0; j < kIntentKeyCount; ++j) {
                if (kFieldNames[j] == key) {
                    f = j;
                    break;
                }
            }
            if (f == kIntentKeyCount) {
                return std::unexpected(IntentError::UnknownField);
            }
            if (out.seen[f]) {
                return std::unexpected(IntentError::DuplicateField);
            }
            out.seen[f] = true;
            r.skip_ws();
            if (!r.eat(':')) { return std::unexpected(IntentError::Malformed); }
            r.skip_ws();
            if (kFieldIsInt[f]) {
                // "lots":"1" is a string that looks like a number, and a
                // reader that accepts it will accept "1e3" next.
                if (r.next_is('"')) {
                    return std::unexpected(IntentError::BadValue);
                }
                const auto n = r.integer();
                if (!n) { return std::unexpected(n.error()); }
                out.num[f] = *n;
            } else {
                // A number, object, array or literal where a string belongs.
                if (!r.next_is('"')) {
                    return std::unexpected(IntentError::BadValue);
                }
                if (const auto s = r.string(out.text[f], kMaxIntentText); !s) {
                    return std::unexpected(s.error());
                }
            }
            r.skip_ws();
            if (r.eat(',')) { continue; }
            if (r.eat('}')) { break; }
            return std::unexpected(IntentError::Malformed);
        }
    }
    r.skip_ws();
    if (!r.at_end()) { return std::unexpected(IntentError::Malformed); }
    return out;
}

/// Days from 1970-01-01 to a proleptic Gregorian date (Hinnant's algorithm).
[[nodiscard]] constexpr std::int64_t
days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = (m > 2) ? m - 3 : m + 9;
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] constexpr bool
two_digits(std::string_view s, std::size_t at, unsigned& out) noexcept {
    const char a = s[at];
    const char b = s[at + 1];
    if (a < '0' || a > '9' || b < '0' || b > '9') { return false; }
    out = static_cast<unsigned>((a - '0') * 10 + (b - '0'));
    return true;
}

/// `YYYY-MM-DDTHH:MM:SS` followed by `Z` or `+HH:MM` / `-HH:MM`, naming a real
/// calendar instant between 2000 and 2200, as UTC nanoseconds.
///
/// An offset is REQUIRED. A local time with no offset is an instant only on
/// the machine that wrote it.
[[nodiscard]] constexpr bool
parse_intent_time(std::string_view s, std::int64_t& ns) noexcept {
    if (s.size() != 20 && s.size() != 25) { return false; }
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':'
        || s[16] != ':') {
        return false;
    }
    unsigned cc = 0, yy = 0, mo = 0, dd = 0, hh = 0, mi = 0, se = 0;
    if (!two_digits(s, 0, cc) || !two_digits(s, 2, yy) || !two_digits(s, 5, mo)
        || !two_digits(s, 8, dd) || !two_digits(s, 11, hh)
        || !two_digits(s, 14, mi) || !two_digits(s, 17, se)) {
        return false;
    }
    const unsigned year = cc * 100 + yy;
    if (year < 2000 || year > 2200 || mo < 1 || mo > 12 || hh > 23 || mi > 59
        || se > 59) {
        return false;
    }
    constexpr unsigned kDaysIn[12] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const unsigned dim = kDaysIn[mo - 1] + ((mo == 2 && leap) ? 1u : 0u);
    if (dd < 1 || dd > dim) { return false; }

    std::int64_t offset_s = 0;
    if (s.size() == 20) {
        if (s[19] != 'Z') { return false; }
    } else {
        const char sign = s[19];
        unsigned oh = 0, om = 0;
        if ((sign != '+' && sign != '-') || s[22] != ':'
            || !two_digits(s, 20, oh) || !two_digits(s, 23, om) || oh > 14
            || om > 59 || (oh == 14 && om != 0)) {
            return false;
        }
        offset_s = (static_cast<std::int64_t>(oh) * 3600
                    + static_cast<std::int64_t>(om) * 60)
                   * (sign == '-' ? -1 : 1);
    }
    const std::int64_t days = days_from_civil(year, mo, dd);
    const std::int64_t secs = days * 86400 + static_cast<std::int64_t>(hh) * 3600
                            + static_cast<std::int64_t>(mi) * 60
                            + static_cast<std::int64_t>(se) - offset_s;
    ns = secs * 1'000'000'000LL;
    return true;
}

/// UTC instants representable by the parser's 2000--2200 local-date range,
/// allowing the full legal ±14:00 offsets. Persisted dedupe entries and the
/// drainer clock must stay inside this interval so TTL differences fit int64.
[[nodiscard]] constexpr bool
valid_intent_timestamp(std::int64_t ns) noexcept {
    constexpr std::int64_t kMinSeconds =
        days_from_civil(1999, 12, 31) * 86400 + 10 * 3600;
    constexpr std::int64_t kMaxSecondsExclusive =
        days_from_civil(2201, 1, 1) * 86400 + 14 * 3600;
    return ns >= kMinSeconds * 1'000'000'000LL
        && ns < kMaxSecondsExclusive * 1'000'000'000LL;
}

[[nodiscard]] constexpr bool id_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
[[nodiscard]] constexpr bool user_char(char c) noexcept {
    return id_char(c) || c == '.' || c == '@';
}
[[nodiscard]] constexpr bool upper_char(char c) noexcept {
    return c >= 'A' && c <= 'Z';
}
/// Printable ASCII, excluding the two characters the emitter would have had to
/// escape. A tradingsymbol containing either is not one this build trades.
[[nodiscard]] constexpr bool symbol_char(char c) noexcept {
    return c >= 0x20 && c <= 0x7E && c != '"' && c != '\\';
}

[[nodiscard]] inline bool
all_chars(const std::string& s, std::size_t lo, std::size_t hi,
          bool (*ok)(char) noexcept) noexcept {
    if (s.size() < lo || s.size() > hi) { return false; }
    for (const char c : s) {
        if (!ok(c)) { return false; }
    }
    return true;
}

} // namespace detail

/// Parse one line of the queue.
///
/// Order of judgement: grammar (Malformed / DuplicateField / UnknownField /
/// TooLong / OutOfRange for an overflowing integer), then presence
/// (MissingField), then version (UnknownVersion), then each value (BadValue /
/// OutOfRange), then the price against the type (PriceContradictsType).
[[nodiscard]] inline std::expected<OrderIntent, IntentError>
parse_intent(std::string_view line) {
    using detail::IntentField;
    const auto raw = detail::parse_object(line);
    if (!raw) { return std::unexpected(raw.error()); }
    const detail::RawIntent& f = *raw;
    for (std::size_t j = 0; j < detail::kIntentKeyCount; ++j) {
        if (!f.seen[j]) { return std::unexpected(IntentError::MissingField); }
    }
    if (f.num[detail::kFieldV] != kIntentSchemaVersion) {
        return std::unexpected(IntentError::UnknownVersion);
    }

    OrderIntent in;
    in.version = kIntentSchemaVersion;
    in.id = f.text[detail::kFieldId];
    in.at = f.text[detail::kFieldAt];
    in.by = f.text[detail::kFieldBy];
    in.symbol = f.text[detail::kFieldSymbol];
    in.exchange = f.text[detail::kFieldExchange];
    in.product = f.text[detail::kFieldProduct];
    in.validity = f.text[detail::kFieldValidity];

    if (!detail::all_chars(in.id, 1, kMaxIntentText, detail::id_char)
        || !detail::all_chars(in.by, 1, kMaxIntentText, detail::user_char)
        || !detail::all_chars(in.symbol, 1, kMaxIntentText, detail::symbol_char)
        || in.symbol.front() == ' ' || in.symbol.back() == ' '
        || !detail::all_chars(in.exchange, 2, 8, detail::upper_char)) {
        return std::unexpected(IntentError::BadValue);
    }
    if (!detail::parse_intent_time(in.at, in.at_ns)) {
        return std::unexpected(IntentError::BadValue);
    }

    // The token is checked in int64 BEFORE it is narrowed: 4295223561 used to
    // become 256265, a different instrument.
    const std::int64_t token = f.num[detail::kFieldToken];
    if (token <= 0) { return std::unexpected(IntentError::BadValue); }
    if (token > static_cast<std::int64_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected(IntentError::OutOfRange);
    }
    in.token = static_cast<std::uint32_t>(token);

    in.lots = f.num[detail::kFieldLots];
    if (in.lots <= 0) { return std::unexpected(IntentError::BadValue); }
    if (in.lots > kMaxIntentLots) {
        return std::unexpected(IntentError::OutOfRange);
    }

    const std::string& side = f.text[detail::kFieldSide];
    if (side == "BUY") { in.side = IntentSide::Buy; }
    else if (side == "SELL") { in.side = IntentSide::Sell; }
    else { return std::unexpected(IntentError::BadValue); }

    const std::string& type = f.text[detail::kFieldOrderType];
    if (type == "MARKET") { in.order_type = IntentType::Market; }
    else if (type == "LIMIT") { in.order_type = IntentType::Limit; }
    else { return std::unexpected(IntentError::BadValue); }

    in.limit_paise = f.num[detail::kFieldLimitPaise];
    if (in.limit_paise < 0) { return std::unexpected(IntentError::BadValue); }
    if (in.limit_paise > kMaxIntentPricePaise) {
        return std::unexpected(IntentError::OutOfRange);
    }

    if (in.product != "NRML" && in.product != "MIS" && in.product != "CNC") {
        return std::unexpected(IntentError::BadValue);
    }
    if (in.validity != "DAY" && in.validity != "IOC") {
        return std::unexpected(IntentError::BadValue);
    }

    // A LIMIT with no price is a market order by accident; a MARKET with one
    // is a limit somebody will not see. `kite_adapter.hpp` refuses the first
    // at the wire; refusing both here means the queue never carries a record
    // whose meaning depends on which module read it.
    if (in.order_type == IntentType::Limit && in.limit_paise == 0) {
        return std::unexpected(IntentError::PriceContradictsType);
    }
    if (in.order_type == IntentType::Market && in.limit_paise != 0) {
        return std::unexpected(IntentError::PriceContradictsType);
    }
    return in;
}

// DRAINING lives in oms/intent_queue.hpp (CX02-B2). P25-03's
// `drain_intents(path, offset)` was removed rather than kept beside it: it
// returned an empty batch for a missing file, trusted an offset into a file
// that could have been replaced, and never expired an intent (C13-008), and a
// second drain API with those defects is one somebody would call.

} // namespace altair::oms
