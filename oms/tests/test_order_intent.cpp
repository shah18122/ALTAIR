// P25-03 acceptance tests for oms/order_intent.hpp, extended by CX02-B1.
//
// THE VECTORS ARE THE CONTRACT.
//
// `desktop/` writes this queue and does not include the header that reads it.
// The two sides are kept honest by `oms/tests/vectors/intents.jsonl`, which
// this test parses and the UI test reproduces byte-for-byte. If either side
// drifts, the vectors fail rather than an order silently meaning something
// different at the two ends of a file.
//
// Test 1 parses the vectors and checks every field, because a parser that
// returns a value for a line is not the same as a parser that returns the
// RIGHT value.
//
// Test 2 is the refusals. Every one of them is a record whose meaning would
// otherwise depend on which module read it.
//
// Test 3 is the drain: offsets, a partial final line, and the fact that a
// malformed record is COUNTED rather than skipped.
//
// Test 4 is the property that matters most and is easiest to lose: an intent
// carries LOTS, never quantity, so nothing in the queue can be sent to a
// broker without oms/ having consulted the spec store.
//
// Tests 5-8 are CX02-B1 (findings C13-005, C13-006, C13-007). Test 5 carries
// its own copy of the reader it replaces, and shows that reader PARSING the
// torn line the new grammar refuses -- a refusal check that the old code
// would also have passed would prove nothing.
//
// No check description here may contain the substring FAIL.

#include <oms/order_intent.hpp>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef ALTAIR_INTENT_VECTORS
#  define ALTAIR_INTENT_VECTORS "oms/tests/vectors/intents.jsonl"
#endif

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

std::vector<std::string> read_vectors()
{
    std::vector<std::string> out;
    std::ifstream f(ALTAIR_INTENT_VECTORS);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) { out.push_back(line); }
    }
    return out;
}

/// `base` with the first `from` replaced by `to`. Every variant below is one
/// edit away from a vector, so a refusal is about exactly that edit.
std::string with(const std::string& base, const std::string& from,
                  const std::string& to)
{
    std::string s = base;
    const auto p = s.find(from);
    if (p != std::string::npos) { s.replace(p, from.size(), to); }
    return s;
}

/// The P25-03 reader's key lookup, verbatim in behaviour: the FIRST
/// occurrence of `"key":` anywhere in the line, then digits. Present only so
/// test 5 can show what the old parser did with a torn line.
bool old_first_match_int(const std::string& line, const char* key,
                         long long& out)
{
    const std::string needle = std::string("\"") + key + "\":";
    const auto p = line.find(needle);
    if (p == std::string::npos) { return false; }
    std::size_t i = p + needle.size();
    const std::size_t start = i;
    long long v = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = v * 10 + (line[i] - '0');
        ++i;
    }
    out = v;
    return i > start;
}

bool refused_as(const std::string& line, altair::oms::IntentError want)
{
    const auto r = altair::oms::parse_intent(line);
    if (r.has_value()) {
        std::printf("        PARSED (should not): %s\n", line.c_str());
        return false;
    }
    if (r.error() != want) {
        std::printf("        refused, but as error %d rather than %d\n",
                    static_cast<int>(r.error()), static_cast<int>(want));
        return false;
    }
    return true;
}

} // namespace

using namespace altair;
using namespace altair::oms;

int main()
{
    std::printf("P25-03 / CX02-B1 -- the order intent queue\n");
    std::printf("vectors: %s\n", ALTAIR_INTENT_VECTORS);

    // -----------------------------------------------------------------------
    // 1. The vectors parse, field for field.
    // -----------------------------------------------------------------------
    std::printf("\n[1] the conformance vectors\n");
    const auto lines = read_vectors();
    std::printf("        %zu vector lines\n", lines.size());
    check(lines.size() >= 3, "the vector file was found and has records");
    if (lines.size() < 3) { return 1; }

    const auto a = parse_intent(lines[0]);
    check(a.has_value(), "vector 1 parses");
    if (a) {
        check(a->version == kIntentSchemaVersion, "  version is the schema's");
        check(a->token == 260105u, "  token 260105 (NIFTY BANK)");
        check(a->symbol == "NIFTY BANK", "  tradingsymbol carried verbatim");
        check(a->exchange == "NSE", "  exchange NSE");
        check(a->side == IntentSide::Buy, "  side BUY");
        check(a->lots == 1, "  lots 1");
        check(a->order_type == IntentType::Limit, "  LIMIT");
        check(a->limit_paise == 5711490, "  limit 5,711,490 paise exactly");
        check(a->product == "NRML" && a->validity == "DAY",
              "  product and validity");
        check(a->complete(), "  and the record is complete");
        // 2026-09-08T18:22:31+05:30 is 12:52:31 UTC: day 20704 since the
        // epoch, so 20704*86400 + 46351 seconds.
        check(a->at_ns == 1'788'871'951'000'000'000LL,
              "  `at` is parsed to the UTC instant it names");
    }

    const auto b = parse_intent(lines[1]);
    check(b.has_value() && b->side == IntentSide::Sell && b->lots == 3,
          "vector 2 is a 3-lot SELL");
    const auto c = parse_intent(lines[2]);
    check(c.has_value() && c->order_type == IntentType::Market
              && c->limit_paise == 0,
          "vector 3 is a MARKET order with no price");

    // -----------------------------------------------------------------------
    // 2. What must be refused.
    // -----------------------------------------------------------------------
    std::printf("\n[2] records whose meaning would depend on the reader\n");
    struct Bad { const char* line; IntentError want; const char* why; };
    const Bad bad[] = {
        {R"({"v":2,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::UnknownVersion,
         "a future schema version is refused, not read as v1"},
        {R"({"v":1,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"LIMIT","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::PriceContradictsType,
         "a LIMIT with no price is refused: it is a market order by accident"},
        {R"({"v":1,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","limit_paise":100,"product":"MIS","validity":"DAY"})",
         IntentError::PriceContradictsType,
         "a MARKET with a price is refused: it is a limit nobody will see"},
        {R"({"v":1,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":0,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::BadValue, "zero lots is refused"},
        {R"({"v":1,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"HOLD","lots":1,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::BadValue, "an unknown side is refused, never defaulted"},
        {R"({"v":1,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":0,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::BadValue, "token 0 is refused"},
        {R"({"v":1,"id":"x","at":"2026-09-08T10:00:00+05:30","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","product":"MIS","validity":"DAY"})",
         IntentError::MissingField, "a missing price field is refused"},
        {"not json at all", IntentError::Malformed, "a non-JSON line is refused"},
    };
    for (const Bad& t : bad) {
        check(refused_as(t.line, t.want), t.why);
    }

    // -----------------------------------------------------------------------
    // 3. The drain moved to oms/intent_queue.hpp (CX02-B2), and its tests --
    //    offsets, partial tails, counted refusals, and the C13-008 cases the
    //    old drain got wrong -- are in tests/test_intent_queue.cpp.
    // -----------------------------------------------------------------------
    std::printf("\n[3] draining: see test_intent_queue.cpp\n");
    check(refused_as(R"({"v":1,"broken":true})", IntentError::UnknownField),
          "a malformed record the drain counts is refused here with a reason");

    // -----------------------------------------------------------------------
    // 4. The property that must not be lost.
    // -----------------------------------------------------------------------
    std::printf("\n[4] an intent carries LOTS, never quantity\n");
    std::printf("        There is no quantity field in the schema, so nothing\n"
                "        in this queue can reach a broker without oms/ having\n"
                "        multiplied by a lot size from the spec store.\n");
    const bool has_qty =
        lines[0].find("\"quantity\"") != std::string::npos
        || lines[0].find("\"qty\"") != std::string::npos;
    check(!has_qty, "no vector carries a quantity field");
    if (a) {
        check(a->lots > 0, "and lots is what it does carry");
    }
    // An intent is not an order: it has no id the state machine knows.
    const bool has_order_id =
        lines[0].find("\"order_id\"") != std::string::npos
        || lines[0].find("\"broker") != std::string::npos;
    check(!has_order_id,
          "and no broker or order id, so it has no identity oms/ did not give");
    check(refused_as(with(lines[0], "{", R"({"quantity":75,)"),
                     IntentError::UnknownField),
          "and a line that ADDS a quantity field is refused, not ignored");

    // -----------------------------------------------------------------------
    // 5. C13-005: a torn append followed by the next append.
    // -----------------------------------------------------------------------
    std::printf("\n[5] torn_append_then_next_append_is_refused\n");
    // The writer died after `"lots":3` of vector 2. The next append wrote
    // vector 1 whole, onto the same line.
    const std::string torn =
        lines[1].substr(0, lines[1].find("\"lots\":3") + 8) + lines[0];
    long long old_lots = 0, old_price = 0;
    const bool old_read =
        old_first_match_int(torn, "lots", old_lots)
        && old_first_match_int(torn, "limit_paise", old_price);
    std::printf("        the replaced reader saw lots %lld at limit %lld paise"
                " -- vector 2's size at vector 1's price\n",
                old_lots, old_price);
    check(old_read && old_lots == 3 && old_price == 5711490,
          "(the P25-03 first-occurrence reader read a mixed record from it)");
    check(refused_as(torn, IntentError::Malformed),
          "the strict grammar refuses the torn line outright");

    const std::string torn_in_string =
        lines[1].substr(0, lines[1].find("NIFTY 50") + 4) + lines[0];
    check(refused_as(torn_in_string, IntentError::Malformed),
          "and a tear inside a string value is refused too");
    check(refused_as(lines[0] + "x", IntentError::Malformed),
          "anything after the closing brace is refused");
    check(parse_intent(lines[0] + "  \r").has_value(),
          "while trailing whitespace is not an error");
    check(parse_intent(with(lines[0], "+05:30", "+14:00")).has_value(),
          "the maximum ISO-8601 offset +14:00 remains valid");
    check(!parse_intent(with(lines[0], "+05:30", "+14:01")),
          "an ISO-8601 offset beyond +14:00 is refused");
    check(!parse_intent(with(lines[0], "+05:30", "-14:01")),
          "the corresponding negative offset beyond -14:00 is refused");

    // -----------------------------------------------------------------------
    // 6. C13-006: overflow and narrowing.
    // -----------------------------------------------------------------------
    std::printf("\n[6] token_and_lots_are_range_checked_before_narrowing\n");
    check(refused_as(with(lines[1], "\"token\":256265", "\"token\":4295223561"),
                     IntentError::OutOfRange),
          "token 4295223561 is refused -- it used to narrow to 256265, a"
          " different instrument");
    check(refused_as(with(lines[1], "\"token\":256265", "\"token\":4294967296"),
                     IntentError::OutOfRange),
          "token 2^32 is refused -- it used to narrow to 0 and still parse");
    check(refused_as(with(lines[1], "\"lots\":3",
                          "\"lots\":18446744073709551617"),
                     IntentError::OutOfRange),
          "lots 2^64+1 is refused -- it used to be signed-overflow UB");
    check(refused_as(with(lines[1], "\"lots\":3", "\"lots\":1001"),
                     IntentError::OutOfRange),
          "lots above kMaxIntentLots is refused, not clamped");
    check(refused_as(with(lines[0], "\"limit_paise\":5711490",
                          "\"limit_paise\":100000000001"),
                     IntentError::OutOfRange),
          "a limit above kMaxIntentPricePaise is refused");
    const auto top = parse_intent(
        with(lines[1], "\"token\":256265", "\"token\":4294967295"));
    check(top.has_value() && top->token == 4294967295u,
          "the largest uint32 token is accepted exactly");

    // -----------------------------------------------------------------------
    // 7. C13-007: the grammar.
    // -----------------------------------------------------------------------
    std::printf("\n[7] the schema is a grammar, not a substring search\n");
    const std::string dup = with(lines[0], "\"validity\":\"DAY\"}",
                                 "\"validity\":\"DAY\",\"lots\":40}");
    check(refused_as(dup, IntentError::DuplicateField),
          "a duplicate key is refused -- first-wins read lots 1, last-wins 40");
    check(refused_as(with(lines[0], "{", R"({"meta":{"lots":40},)"),
                     IntentError::UnknownField),
          "a nested object carrying a key is refused -- it used to set lots 40");
    check(refused_as(with(lines[0], "\"lots\":1", "\"lots\":1.5"),
                     IntentError::Malformed),
          "1.5 lots is refused, not read as 1");
    check(refused_as(with(lines[0], "\"lots\":1", "\"lots\":1e0"),
                     IntentError::Malformed),
          "an exponent is refused");
    check(refused_as(with(lines[0], "\"lots\":1", "\"lots\":\"1\""),
                     IntentError::BadValue),
          "a quoted integer is refused");
    check(refused_as(with(lines[0], "\"lots\":1", "\"lots\":01"),
                     IntentError::Malformed),
          "a leading zero is refused");
    check(refused_as(with(lines[0], "\"lots\":1", "\"lots\":+1"),
                     IntentError::Malformed),
          "a leading plus is refused");
    check(refused_as(with(lines[0], "\"symbol\":\"NIFTY BANK\"",
                          "\"symbol\":\"NIFTY\\u0020BANK\""),
                     IntentError::Malformed),
          "an escape other than \\\" and \\\\ is refused, not half-decoded");
    check(refused_as(with(lines[0], "NIFTY BANK", "NIFTY\tBANK"),
                     IntentError::Malformed),
          "a raw control character inside a string is refused");
    check(refused_as(with(lines[0], "\"symbol\":\"NIFTY BANK\"",
                          "\"symbol\":7"),
                     IntentError::BadValue),
          "a number where a string belongs is refused");
    check(refused_as(with(lines[0], "\"product\":\"NRML\"",
                          "\"product\":\"BO\""),
                     IntentError::BadValue),
          "a product outside NRML/MIS/CNC is refused");
    check(refused_as(with(lines[0], "\"validity\":\"DAY\"",
                          "\"validity\":\"GTT\""),
                     IntentError::BadValue),
          "a validity outside DAY/IOC is refused");
    check(refused_as(with(lines[0], "\"exchange\":\"NSE\"",
                          "\"exchange\":\"\""),
                     IntentError::BadValue),
          "an empty exchange is refused");
    check(refused_as(with(lines[0], "\"exchange\":\"NSE\"",
                          "\"exchange\":\"nse\""),
                     IntentError::BadValue),
          "a lower-case exchange is refused");
    check(refused_as(with(lines[0], "01J8XA0000000000000000BUY1",
                          "01J8XA 0000BUY1"),
                     IntentError::BadValue),
          "an id with a space is refused");
    check(refused_as(with(lines[0], "01J8XA0000000000000000BUY1",
                          std::string(65, 'A')),
                     IntentError::TooLong),
          "an id longer than kMaxIntentText is refused");
    check(refused_as(with(lines[0], "{", "{" + std::string(1100, ' ')),
                     IntentError::TooLong),
          "a line longer than kMaxIntentLineBytes is refused");

    // -----------------------------------------------------------------------
    // 8. `at` names a real instant, with an offset.
    // -----------------------------------------------------------------------
    std::printf("\n[8] at_is_a_real_instant_with_an_offset\n");
    const std::string at = "2026-09-08T18:22:31+05:30";
    check(refused_as(with(lines[0], at, "2026-02-30T10:00:00+05:30"),
                     IntentError::BadValue),
          "30 February is refused");
    check(refused_as(with(lines[0], at, "2026-09-08T18:22:31"),
                     IntentError::BadValue),
          "a local time with no offset is refused -- it is an instant only on"
          " the machine that wrote it");
    check(refused_as(with(lines[0], at, "2026-09-08T24:00:00Z"),
                     IntentError::BadValue),
          "hour 24 is refused");
    const auto utc = parse_intent(with(lines[0], at, "2026-09-08T12:52:31Z"));
    check(utc.has_value() && a && utc->at_ns == a->at_ns,
          "12:52:31Z and 18:22:31+05:30 are the same instant");
    const auto leap = parse_intent(with(lines[0], at, "2028-02-29T09:15:00+05:30"));
    check(leap.has_value(), "29 February of a leap year is accepted");

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
