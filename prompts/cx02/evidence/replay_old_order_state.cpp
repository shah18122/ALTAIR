// prompts/cx02/evidence/replay_old_order_state.cpp -- planted-violation evidence
// for CX02-B6.
//
// Compiled against the PRE-CX02 oms/order_state.hpp (git show HEAD:...), put
// FIRST on the include path so `#include <oms/order_state.hpp>` resolves to
// the old table. Uses only events that existed before CX02, so the same
// source compiles against both headers. Exit 0 means every trigger reproduced
// on the old header; the new tests in oms/tests/test_order_state.cpp assert
// the opposite outcome on the same sequences.
//
// Not part of the build. Run by hand; output recorded in CHECKPOINT.md.

#include <oms/order_state.hpp>

#include <cstdint>
#include <cstdio>

using namespace altair;

namespace {
Timestamp at(std::int64_t ms) { return Timestamp{ms * 1'000'000}; }
FillReport fill(std::int64_t cum, std::int64_t ms) {
    return FillReport{Qty{cum}, Price{2'408'000}, at(ms)};
}
int reproduced = 0;
void saw(bool bad, const char* what) {
    std::printf("  %-12s %s\n", bad ? "REPRODUCED" : "not seen", what);
    if (bad) { ++reproduced; }
}
} // namespace

int main() {
    std::printf("replay against the header on the include path\n");

    {   // C13-001
        Order o = *open_order(Qty{150}, at(0));
        (void)apply(o, OrderEvent::Ack, at(10));
        (void)apply(o, OrderEvent::CancelSent, at(20));
        (void)apply_fill(o, fill(75, 25));
        const auto r = apply(o, OrderEvent::CancelAck, at(30));
        saw(!r && is_live(o.state),
            "C13-001 partial fill under PendingCancel, then CancelAck refused;"
            " order still live");
    }
    {   // C13-002
        Order o = *open_order(Qty{150}, at(0));
        (void)apply(o, OrderEvent::Ack, at(10));
        (void)apply(o, OrderEvent::CancelSent, at(20));
        (void)apply(o, OrderEvent::Reject, at(30));   // the refused CANCEL
        const auto f = apply_fill(o, fill(150, 40));
        saw(!f && o.cum_qty.raw() == 0,
            "C13-002 refused cancel made the order Rejected; the real fill was"
            " refused and cum_qty stayed 0");
    }
    {   // C13-001 related: late ack after a fill
        Order o = *open_order(Qty{150}, at(0));
        (void)apply_fill(o, fill(150, 5));
        const auto a = apply(o, OrderEvent::Ack, at(10));
        saw(!a && o.refused == 1,
            "late Ack after the order filled was counted as a refusal");
    }
    {   // C13-001 related: crossed fill reopened a cancelled order
        Order o = *open_order(Qty{150}, at(0));
        (void)apply(o, OrderEvent::Ack, at(10));
        (void)apply(o, OrderEvent::CancelSent, at(20));
        (void)apply(o, OrderEvent::CancelAck, at(30));
        (void)apply_fill(o, fill(75, 40));
        saw(is_live(o.state),
            "crossed fill after CancelAck reopened the order as live");
    }

    std::printf("reproduced %d of 4\n", reproduced);
    return reproduced == 4 ? 0 : 1;
}
