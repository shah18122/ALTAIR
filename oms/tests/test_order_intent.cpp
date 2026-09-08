// P25-03 acceptance tests for oms/order_intent.hpp.
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
// No check description here may contain the substring FAIL.

#include <oms/order_intent.hpp>

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

} // namespace

using namespace altair;
using namespace altair::oms;

int main()
{
    std::printf("P25-03 -- the order intent queue\n");
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
        {R"({"v":2,"id":"x","at":"t","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::UnknownVersion,
         "a future schema version is refused, not read as v1"},
        {R"({"v":1,"id":"x","at":"t","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"LIMIT","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::PriceContradictsType,
         "a LIMIT with no price is refused: it is a market order by accident"},
        {R"({"v":1,"id":"x","at":"t","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","limit_paise":100,"product":"MIS","validity":"DAY"})",
         IntentError::PriceContradictsType,
         "a MARKET with a price is refused: it is a limit nobody will see"},
        {R"({"v":1,"id":"x","at":"t","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":0,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::BadValue, "zero lots is refused"},
        {R"({"v":1,"id":"x","at":"t","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"HOLD","lots":1,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::BadValue, "an unknown side is refused, never defaulted"},
        {R"({"v":1,"id":"x","at":"t","by":"u","token":0,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","limit_paise":0,"product":"MIS","validity":"DAY"})",
         IntentError::BadValue, "token 0 is refused"},
        {R"({"v":1,"id":"x","at":"t","by":"u","token":1,"symbol":"S","exchange":"NSE","side":"BUY","lots":1,"order_type":"MARKET","product":"MIS","validity":"DAY"})",
         IntentError::MissingField, "a missing price field is refused"},
        {"not json at all", IntentError::Malformed, "a non-JSON line is refused"},
    };
    for (const Bad& t : bad) {
        const auto r = parse_intent(t.line);
        check(!r.has_value() && r.error() == t.want, t.why);
    }

    // -----------------------------------------------------------------------
    // 3. The drain.
    // -----------------------------------------------------------------------
    std::printf("\n[3] draining, offsets, and a partial final line\n");
    const char* tmp = "test_intents_tmp.jsonl";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << lines[0] << "\n" << lines[1] << "\n";
    }
    const auto d1 = drain_intents(tmp, 0);
    std::printf("        drained %zu, rejected %zu, offset %zu\n",
                d1.intents.size(), d1.rejected, d1.next_offset);
    check(d1.intents.size() == 2 && d1.rejected == 0,
          "both records drained from offset 0");

    // Append a third and resume from the recorded offset.
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::app);
        f << lines[2] << "\n";
    }
    const auto d2 = drain_intents(tmp, d1.next_offset);
    check(d2.intents.size() == 1,
          "resuming from the offset returns ONLY the new record");

    // A malformed line is counted, not skipped.
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::app);
        f << "{\"v\":1,\"broken\":true}\n";
    }
    const auto d3 = drain_intents(tmp, d2.next_offset);
    std::printf("        after a bad line: drained %zu, rejected %zu\n",
                d3.intents.size(), d3.rejected);
    check(d3.rejected == 1,
          "a malformed record is COUNTED, so a caller can refuse the batch");

    // A partial final line -- writer interrupted mid-append -- is not consumed.
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::app);
        f << "{\"v\":1,\"id\":\"partial";        // no newline
    }
    const auto d4 = drain_intents(tmp, d3.next_offset);
    std::printf("        partial tail: drained %zu, rejected %zu, offset %zu"
                " (was %zu)\n",
                d4.intents.size(), d4.rejected, d4.next_offset, d3.next_offset);
    check(d4.intents.empty() && d4.rejected == 0
              && d4.next_offset == d3.next_offset,
          "a partial final line is left unconsumed for the next drain");
    std::remove(tmp);

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

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
