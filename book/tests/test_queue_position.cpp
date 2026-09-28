#include <book/queue_position.hpp>

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
}

int main() {
    using namespace altair;
    QueuePosition queue{40};
    check(queue.apply({10, QueueEventKind::Snapshot, 100}).value_or(1) == 0,
          "snapshot establishes quantity ahead without filling us");
    check(queue.apply({11, QueueEventKind::CancelAhead, 30}).value_or(1) == 0
              && queue.state().ahead == 70,
          "an identified cancellation ahead advances queue position");
    check(queue.apply({12, QueueEventKind::AddAhead, 10}).value_or(1) == 0
              && queue.state().ahead == 80,
          "an identified add ahead moves queue position backwards");
    check(queue.apply({13, QueueEventKind::ExecuteAtLevel, 90}).value_or(0) == 10
              && queue.state().ahead == 0 && queue.state().own_remaining == 30,
          "trades consume quantity ahead before filling the resting order");
    check(queue.apply({14, QueueEventKind::ExecuteAtLevel, 50}).value_or(0) == 30
              && queue.state().own_filled == 40,
          "fills are capped by our remaining quantity");

    QueuePosition broken{10};
    (void)broken.apply({100, QueueEventKind::Snapshot, 5});
    const auto gap = broken.apply({102, QueueEventKind::ExecuteAtLevel, 10});
    check(!gap && gap.error() == QueuePositionError::SequenceGap && !broken.state().valid,
          "a feed sequence gap invalidates queue state instead of guessing");
    std::printf("Queue position: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
