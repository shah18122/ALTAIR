// prompts/cx02/evidence/replay_old_intent_drain.cpp -- planted-violation
// evidence for CX02-B2 (finding C13-008, and the expiry half of C13-009).
//
// Compiled ONLY against the PRE-CX02 oms/order_intent.hpp (git show HEAD:...),
// which still has drain_intents(). Exit 0 means every behaviour below
// reproduced on that header. The replacement API (oms/intent_queue.hpp) is
// held to the opposite outcomes by oms/tests/test_intent_queue.cpp.
//
// Not part of the build. Run by hand ("oldonly"); output recorded in
// CHECKPOINT.md. Writes two scratch files in the working directory and
// removes them.

#include <oms/order_intent.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using namespace altair::oms;

namespace {
int reproduced = 0;
void saw(bool bad, const char* what) {
    std::printf("  %-12s %s\n", bad ? "REPRODUCED" : "not seen", what);
    if (bad) { ++reproduced; }
}
const char* kA = R"({"v":1,"id":"A1","at":"2026-09-08T18:22:31+05:30","by":"smit","token":260105,"symbol":"NIFTY BANK","exchange":"NSE","side":"BUY","lots":1,"order_type":"LIMIT","limit_paise":5711490,"product":"NRML","validity":"DAY"})";
const char* kB = R"({"v":1,"id":"B1","at":"2026-09-08T18:23:02+05:30","by":"smit","token":256265,"symbol":"NIFTY 50","exchange":"NSE","side":"SELL","lots":3,"order_type":"LIMIT","limit_paise":2415580,"product":"MIS","validity":"DAY"})";
} // namespace

int main() {
    const char* q = "cx02_replay_drain.jsonl";

    const auto missing = drain_intents("cx02_this_file_does_not_exist.jsonl", 0);
    saw(missing.intents.empty() && missing.rejected == 0 && missing.lines == 0,
        "a missing queue file drained as an EMPTY batch, with no error");

    { std::ofstream f(q, std::ios::binary | std::ios::trunc); f << kA << "\n" << kB << "\n"; }
    const auto first = drain_intents(q, 0);
    saw(first.intents.size() == 2,
        "intents stamped 2026-09-08 were accepted on any later day -- nothing expires");

    const auto again = drain_intents(q, 0);
    saw(again.intents.size() == 2 && again.intents[0].id == first.intents[0].id,
        "a re-drain from an unsaved offset returned the same ids again -- no dedupe");

    { std::ofstream f(q, std::ios::binary | std::ios::trunc); f << kA << "\n"; }
    const auto trunc = drain_intents(q, first.next_offset);
    saw(trunc.intents.empty() && trunc.rejected == 0,
        "an offset past the end of a truncated file returned nothing, and no error");

    std::remove(q);
    std::printf("reproduced %d of 4\n", reproduced);
    return reproduced == 4 ? 0 : 1;
}
