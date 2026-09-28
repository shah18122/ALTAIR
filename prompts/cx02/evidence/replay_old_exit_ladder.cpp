// prompts/cx02/evidence/replay_old_exit_ladder.cpp -- planted-violation
// evidence for CX02-B8 (finding C13-010).
//
// Compiled against the PRE-CX02 oms/exit_ladder.hpp (git show HEAD:...), put
// first on the include path. Uses only the old API. Exit 0 means the defect
// reproduced: the old decision priced a gap exit at its stop, so the P&L it
// supported was Rs 0 on a trade that lost Rs 2,25,000.
//
// Not part of the build. Run by hand ("oldonly"); output recorded in
// CHECKPOINT.md.

#include <oms/exit_ladder.hpp>

#include <cstdint>
#include <cstdio>

using namespace altair;

int main() {
    ExitPlan p{};
    p.dir = Direction::Long;
    p.entry = Price{2'400'000};
    p.initial_stop = Price{2'380'000};
    p.tighter_stop = Price{2'400'000};
    p.target = Price{2'450'000};
    p.qty = Qty{750};

    const Price gap{2'370'000};
    const ExitDecision d = evaluate_exit(p, gap, Timestamp{}, false);
    const auto booked = exit_pnl(p.dir, p.entry, d.level, p.qty);
    const auto real = exit_pnl(p.dir, p.entry, gap, p.qty);
    std::printf("old decision: reason %d, level %lld (tick %lld)\n",
                static_cast<int>(d.reason),
                static_cast<long long>(d.level.raw()),
                static_cast<long long>(gap.raw()));
    std::printf("P&L at the decision's level: %lld paise; at the tick: %lld paise\n",
                booked ? static_cast<long long>(booked->raw()) : -1LL,
                real ? static_cast<long long>(real->raw()) : -1LL);
    const bool reproduced = booked && real && booked->raw() == 0
                            && real->raw() == -22'500'000
                            && d.level.raw() == 2'400'000;
    std::printf("%s\n", reproduced
                ? "REPRODUCED: the only price the old decision carried was the"
                  " stop, which books Rs 0 on a Rs 2,25,000 loss"
                : "not reproduced");
    return reproduced ? 0 : 1;
}
