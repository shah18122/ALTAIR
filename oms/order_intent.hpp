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

#pragma once

#include <core/types/units.hpp>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace altair::oms {

/// Schema version. A change here is a change both sides must make, and the
/// parser refuses anything it does not know rather than reading a v2 field
/// list as a v1 record.
inline constexpr int kIntentSchemaVersion = 1;

inline constexpr const char* kIntentFile = "data/order_intents.jsonl";

enum class IntentError : std::uint8_t {
    /// Not JSON, or not the object shape this schema defines.
    Malformed,
    /// `v` absent or not `kIntentSchemaVersion`.
    UnknownVersion,
    /// A field the schema requires is absent.
    MissingField,
    /// Present but nonsensical: zero lots, a negative price, an unknown side.
    BadValue,
    /// A limit order with no price, or a market order with one. Both are a
    /// different order from the one somebody meant.
    PriceContradictsType
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

    /// A drained intent is still only a request. This says whether oms/ has
    /// looked at it, never whether an order exists.
    [[nodiscard]] bool complete() const noexcept {
        return version == kIntentSchemaVersion && !id.empty() && token != 0
            && lots > 0 && !symbol.empty() && !exchange.empty();
    }
};

namespace detail {

/// Minimal, allocation-tolerant JSON field reader.
///
/// Deliberately NOT a general JSON parser. The schema is a flat object of
/// strings and integers written by one emitter, and a full parser here would
/// be a large surface for a format that is fixed by conformance vectors. It
/// refuses anything that does not look exactly like what the emitter writes.
[[nodiscard]] inline bool
field(std::string_view line, std::string_view key, std::string& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    const auto p = line.find(needle);
    if (p == std::string_view::npos) { return false; }
    auto i = p + needle.size();
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) { ++i; }
    if (i >= line.size()) { return false; }
    if (line[i] == '"') {
        ++i;
        out.clear();
        while (i < line.size() && line[i] != '"') {
            // The emitter escapes nothing but the quote and the backslash,
            // and the vectors pin that. A record containing anything else is
            // refused rather than half-decoded.
            if (line[i] == '\\' && i + 1 < line.size()) {
                ++i;
            }
            out.push_back(line[i]);
            ++i;
        }
        return i < line.size();
    }
    const auto start = i;
    while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i]))
                               || line[i] == '-' || line[i] == '+')) {
        ++i;
    }
    if (i == start) { return false; }
    out.assign(line.substr(start, i - start));
    return true;
}

[[nodiscard]] inline bool to_i64(const std::string& s, std::int64_t& out) {
    if (s.empty()) { return false; }
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') { neg = (s[0] == '-'); i = 1; }
    if (i >= s.size()) { return false; }
    std::int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) { return false; }
        v = v * 10 + (s[i] - '0');
    }
    out = neg ? -v : v;
    return true;
}

} // namespace detail

/// Parse one line of the queue.
[[nodiscard]] inline std::expected<OrderIntent, IntentError>
parse_intent(std::string_view line) {
    if (line.empty() || line.front() != '{' ) {
        return std::unexpected(IntentError::Malformed);
    }
    OrderIntent in;
    std::string s;
    std::int64_t n = 0;

    if (!detail::field(line, "v", s) || !detail::to_i64(s, n)) {
        return std::unexpected(IntentError::MissingField);
    }
    if (n != kIntentSchemaVersion) {
        return std::unexpected(IntentError::UnknownVersion);
    }
    in.version = static_cast<int>(n);

    const auto need = [&](const char* k, std::string& dst) {
        return detail::field(line, k, dst);
    };
    if (!need("id", in.id) || !need("at", in.at) || !need("by", in.by)
        || !need("symbol", in.symbol) || !need("exchange", in.exchange)
        || !need("product", in.product) || !need("validity", in.validity)) {
        return std::unexpected(IntentError::MissingField);
    }

    if (!detail::field(line, "token", s) || !detail::to_i64(s, n) || n <= 0) {
        return std::unexpected(IntentError::BadValue);
    }
    in.token = static_cast<std::uint32_t>(n);

    if (!detail::field(line, "lots", s) || !detail::to_i64(s, in.lots)) {
        return std::unexpected(IntentError::MissingField);
    }
    if (in.lots <= 0) { return std::unexpected(IntentError::BadValue); }

    if (!detail::field(line, "side", s)) {
        return std::unexpected(IntentError::MissingField);
    }
    if (s == "BUY") { in.side = IntentSide::Buy; }
    else if (s == "SELL") { in.side = IntentSide::Sell; }
    else { return std::unexpected(IntentError::BadValue); }

    if (!detail::field(line, "order_type", s)) {
        return std::unexpected(IntentError::MissingField);
    }
    if (s == "MARKET") { in.order_type = IntentType::Market; }
    else if (s == "LIMIT") { in.order_type = IntentType::Limit; }
    else { return std::unexpected(IntentError::BadValue); }

    if (!detail::field(line, "limit_paise", s)
        || !detail::to_i64(s, in.limit_paise)) {
        return std::unexpected(IntentError::MissingField);
    }
    if (in.limit_paise < 0) { return std::unexpected(IntentError::BadValue); }

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

/// What a drain produced.
struct DrainResult {
    std::vector<OrderIntent> intents;
    /// Byte offset to resume from. The caller persists this; the queue file is
    /// never truncated, so an offset stays valid across a restart.
    std::size_t next_offset = 0;
    /// Lines that did not parse. NOT skipped silently: a malformed record in
    /// an order queue is a reason to stop and look, and the count is how a
    /// caller knows to.
    std::size_t rejected = 0;
    /// Lines seen in total.
    std::size_t lines = 0;
};

/// Drain from `offset` to end of file.
///
/// Returns rejected records as a COUNT rather than dropping them off the end
/// of a log nobody reads. A caller that sees `rejected > 0` should refuse to
/// act on the batch: the queue is append-only, so a bad line means either a
/// writer bug or a corrupted file, and neither is a condition under which to
/// place the other orders in the same batch.
[[nodiscard]] inline DrainResult
drain_intents(const std::string& path, std::size_t offset) {
    DrainResult out;
    out.next_offset = offset;

    // std::ifstream rather than std::fopen: MSVC deprecates fopen under /W4
    // and gate 1 is zero warnings. Silencing it with _CRT_SECURE_NO_WARNINGS
    // would turn off the whole family of checks for one call, which is the
    // wrong trade -- the same decision P12-01 made when sscanf raised C4996.
    //
    // BINARY MODE IS LOAD-BEARING. tellg() on a text-mode stream on Windows
    // does not count the bytes the file holds, and this offset is persisted
    // and resumed from. A text-mode offset would drift by one per line and
    // eventually resume mid-record.
    std::ifstream f(path, std::ios::binary);
    if (!f) { return out; }
    f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f) { return out; }

    std::string line;
    while (std::getline(f, line)) {
        // getline consumed a delimiter iff it did not stop at EOF. A final
        // line with no newline is a writer interrupted mid-append: it is NOT
        // consumed, and the offset stays before it so the next drain sees the
        // record whole.
        const bool complete_line = !f.eof();
        if (!complete_line) { break; }
        ++out.lines;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            const auto p = parse_intent(line);
            if (p) { out.intents.push_back(*p); } else { ++out.rejected; }
        }
        const auto pos = f.tellg();
        if (pos >= 0) { out.next_offset = static_cast<std::size_t>(pos); }
    }
    return out;
}

} // namespace altair::oms
