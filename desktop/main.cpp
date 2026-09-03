// desktop/main.cpp -- the UI entry point.
//
// P11Q-01.
//
// WHAT THIS IS: the first Qt card. A window, a live grid, and a replayer
// driving it. It exists to prove the toolchain and to put the P11-03 identity
// discipline into a real QAbstractTableModel, not to be a dashboard.
//
// WHAT THE DATA IS: SYNTHETIC, GENERATED HERE, AND NOT THE ENGINE'S.
//
// `app/session_file.hpp` has the engine's own `generate_session`, and this file
// deliberately does NOT include it. `app/` is the `altair` binary, not a
// library, and reaching into it from the UI would be exactly the kind of
// convenience header-crossing CLAUDE.md's layout rule exists to prevent.
//
// So the walk below is local, obvious, and labelled. It is a stand-in until a
// live or replayed feed is wired into this window, and it must not be mistaken
// for engine behaviour: it is a bounded random walk with a fixed LCG, nothing
// more. CLAUDE.md's reality check applies -- synthetic data exists because
// there is no captured tick data yet, not because it is a substitute for it.

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <feed/replay.hpp>

#include "main_window.hpp"

#include <QApplication>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace {

struct DemoInstrument {
    std::uint32_t token;
    const char* symbol;
    std::int64_t open_paise;
};

/// Four instruments so the grid has rows to sort and compare, at prices that
/// span two orders of magnitude -- which is also what makes the formatter's
/// Indian digit grouping visible.
constexpr std::array<DemoInstrument, 4> kInstruments{{
    {256265, "NIFTY 50",       2'450'000},   // Rs 24,500.00
    {260105, "BANKNIFTY",      5'210'000},   // Rs 52,100.00
    {738561, "RELIANCE",         291'500},   // Rs  2,915.00
    {2953217, "NIFTY26SEP24500CE",  12'000}, // Rs    120.00
}};

/// A deterministic interleaved session. One global sequence and a strictly
/// increasing timestamp, because `Replayer` refuses a tape that moves either
/// backwards -- and refusing is the right behaviour, so the generator has to
/// respect it rather than the replayer being loosened.
std::vector<altair::ReplayTick> demo_session(std::size_t count,
                                             altair::Timestamp start,
                                             std::uint64_t seed) {
    std::vector<altair::ReplayTick> out;
    out.reserve(count);

    std::uint64_t rng = seed * 6364136223846793005ull + 1442695040888963407ull;
    const auto next = [&rng]() noexcept -> std::uint64_t {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return rng >> 33;
    };

    std::array<std::int64_t, kInstruments.size()> px{};
    for (std::size_t i = 0; i < kInstruments.size(); ++i) {
        px[i] = kInstruments[i].open_paise;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t which = i % kInstruments.size();
        // A step proportional to the price, so the option ticks in paise and
        // the index ticks in rupees, as they actually do.
        const std::int64_t scale = px[which] / 20'000 + 1;
        const std::int64_t step =
            (static_cast<std::int64_t>(next() % 201) - 100) * scale / 100;
        px[which] += step;
        if (px[which] < 1) {
            px[which] = 1;   // a price never reaches zero
        }

        altair::ReplayTick t{};
        t.ts = start + altair::duration::millis(static_cast<std::int64_t>(i));
        t.seqno = static_cast<std::uint64_t>(i) + 1;
        t.token = kInstruments[which].token;
        t.reserved = 0;
        t.last = altair::Price{px[which]};
        t.qty = altair::Qty{static_cast<std::int64_t>(next() % 50) + 1};
        out.push_back(t);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Altair"));
    QApplication::setOrganizationName(QStringLiteral("Altair"));

    // 2026-09-03T09:15:00Z -- a plausible session open, so the engine clock in
    // the status bar reads like a trading session rather than like 1970.
    const altair::Timestamp open{1'788'428'100'000'000'000LL};
    const auto ticks = demo_session(40'000, open, 42);

    altair::ui::MainWindow window(ticks.data(), ticks.size());
    for (const auto& ins : kInstruments) {
        window.add_instrument(ins.token, QString::fromUtf8(ins.symbol));
    }
    window.show();

    return QApplication::exec();
}
