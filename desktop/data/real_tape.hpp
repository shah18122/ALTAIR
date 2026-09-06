// desktop/data/real_tape.hpp -- the live grid, on real instruments.
//
// P11Q-09.
//
// THE GRID SHOWED SYNTH-A THROUGH SYNTH-D BECAUSE THERE WAS NOTHING ELSE.
//
// P11Q-01 put four synthetic instruments on a bounded random walk, and an
// earlier version of that list used REAL tickers -- RELIANCE among them --
// against fabricated prices. That was the worst thing in the window and it was
// the first thing on screen, so the names were changed to SYNTH-* and the
// prices to round numbers nothing trades at.
//
// P2-12 removed the reason. dataset/ now holds 2.8 million real bars across
// NIFTY, India VIX and the near NIFTY future, so the grid can show instruments
// that exist, under their own names, at prices they actually traded.
//
// A BAR CLOSE IS A REAL TRADE. A RECONSTRUCTED CANDLE IS NOT.
//
// P11Q-06 measured that pushing bar closes through a tick pipeline and
// re-bucketing them loses 37.8% of the range, and that finding stands -- for
// CANDLES. It does not apply here and the difference is worth stating,
// because the same operation looks identical from outside.
//
// A candle rebuilt from closes is wrong: the high and the low are gone, so the
// reconstructed range is the spread of the closes rather than the true extent.
// But the grid does not draw candles. It shows a last traded price, a change
// and a volume, and every close in dataset/ IS a last traded price at a real
// instant. Replaying them is a faithful, sparse tape -- one print a minute
// instead of every print -- and the status bar says which.
//
// So: real prices, real names, no fabrication. What is lost is DENSITY, and
// that is named on screen rather than implied away.
//
// THE THREE INSTRUMENTS ARE NOT INTERCHANGEABLE AND THE TAPE SAYS SO.
//
// NIFTY 50 and India VIX are INDICES: they do not trade, so their volume field
// is empty in dataset/ and is carried through as absent rather than as zero.
// The future does trade and carries real volume. A grid that showed 0 for the
// index would be claiming no trading in a minute the market was open -- the
// same confusion P11Q-06 unpicked and P2-12d refused to write.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <feed/replay.hpp>

#include "bar_csv.hpp"

#include <QString>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace altair::ui {

/// One instrument the grid can show.
struct TapeInstrument {
    std::uint32_t token = 0;
    QString symbol;
    /// Where its bars live, relative to dataset/.
    QString dir;
    /// False for an index: it does not trade, so its volume is ABSENT and
    /// must not be rendered as zero.
    bool trades = false;
};

struct RealTape {
    std::vector<ReplayTick> ticks;
    std::vector<TapeInstrument> instruments;
    /// Sessions the tape covers, and bars per instrument, for the status line.
    std::size_t sessions = 0;
    /// First and last bar in the tape, ns since epoch. Zero when empty.
    std::int64_t first_ns = 0;
    std::int64_t last_ns = 0;
    QString error;
    [[nodiscard]] bool ok() const noexcept { return !ticks.empty(); }
};

/// The instruments the grid shows, in Smit's stated priority order:
/// NIFTY spot, then the NIFTY future, then India VIX.
///
/// Tokens are the ones looked up from the Kite master in P2-12e, not typed
/// from memory -- and they are here rather than in a literal at the call site
/// so there is one place to correct if a contract rolls.
[[nodiscard]] inline std::vector<TapeInstrument> tape_instruments() {
    return {
        {256265, QStringLiteral("NIFTY 50"),
         QStringLiteral("spot/nifty/1m"), false},
        {17512194, QStringLiteral("NIFTY26SEPFUT"),
         QStringLiteral("fut/nifty/1m"), true},
        {264969, QStringLiteral("INDIA VIX"),
         QStringLiteral("spot/indiavix/1m"), false},
    };
}

/// Build one time-ordered tape from the last `sessions` trading days.
///
/// Bounded on purpose. The full 1-minute set is 2.2 million bars across three
/// instruments and the grid is a live view, not an archive -- loading all of
/// it costs seconds at startup to show a tape nobody scrolls back through.
/// The chart is where history belongs and it reads the same files.
[[nodiscard]] inline RealTape load_real_tape(const QString& dataset_root,
                                             std::size_t sessions) {
    RealTape out;
    out.instruments = tape_instruments();

    constexpr std::int64_t kMinuteNs = 60'000'000'000LL;
    struct Row { std::int64_t ts; std::size_t which; Candle c; };
    std::vector<Row> rows;

    for (std::size_t i = 0; i < out.instruments.size(); ++i) {
        const TapeInstrument& ins = out.instruments[i];
        // Roughly 20 trading days a month, plus one file of slack because a
        // request landing early in a month would otherwise straddle the
        // boundary and come up short. Deliberately generous: the cost of one
        // extra monthly file is milliseconds, and the cost of being one file
        // short is a grid that silently shows fewer sessions than the status
        // bar claims.
        const std::size_t want_files = sessions / 20 + 2;
        const LoadResult r = load_bars_dir(
            dataset_root + QStringLiteral("/") + ins.dir, kMinuteNs,
            DailyStamp::SessionClose,
            // An index reports no volume, so a zero from that source means
            // "not reported". For the future a zero would be a real zero --
            // but the fetched files carry real turnover, so it never arises.
            !ins.trades, want_files);
        if (!r.ok()) {
            out.error = r.error;
            continue;
        }
        for (const Candle& c : r.bars) {
            rows.push_back(Row{c.start_ns, i, c});
        }
    }
    if (rows.empty()) {
        if (out.error.isEmpty()) {
            out.error = QStringLiteral("no bars under %1").arg(dataset_root);
        }
        return out;
    }

    // Keep only the last `sessions` distinct days, then sort by time. Sorting
    // AFTER the cut, because sorting 2.2 million rows to throw most away is
    // the expensive order.
    std::vector<std::int64_t> days;
    days.reserve(rows.size());
    for (const Row& r : rows) { days.push_back(r.ts / 86'400'000'000'000LL); }
    std::sort(days.begin(), days.end());
    days.erase(std::unique(days.begin(), days.end()), days.end());
    const std::int64_t cutoff =
        days.size() > sessions
            ? days[days.size() - sessions] * 86'400'000'000'000LL
            : 0;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [cutoff](const Row& r) { return r.ts < cutoff; }),
               rows.end());
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.ts < b.ts; });

    out.sessions = std::min(sessions, days.size());
    // THE RANGE, NOT JUST THE COUNT.
    //
    // "last 5 sessions" is true of a tape ending today and of one ending three
    // weeks ago, and they look identical on screen. The dataset is filled by a
    // manual fetch that needs a daily Kite login, so a stale tape is the
    // NORMAL state rather than an edge case, and the first and last bar dates
    // are the only thing that distinguishes them.
    out.first_ns = rows.empty() ? 0 : rows.front().ts;
    out.last_ns = rows.empty() ? 0 : rows.back().ts;
    out.ticks.reserve(rows.size());
    std::uint64_t seq = 0;
    for (const Row& r : rows) {
        ReplayTick t{};
        // The bar's CLOSE, stamped at the bar's END. A close is the last
        // price of that minute, so stamping it at the minute's start would
        // put a price on screen before it happened -- rule 7, and the kind of
        // one-bar look-ahead that is invisible in a grid.
        t.ts = Timestamp{r.c.end_ns};
        t.seqno = ++seq;
        t.token = out.instruments[r.which].token;
        t.reserved = 0;
        t.last = Price{r.c.close};
        // Qty carries the bar's volume where the instrument trades. For an
        // index the field is left at zero and `trades` is false, so the grid
        // knows not to render it as a traded quantity.
        t.qty = Qty{out.instruments[r.which].trades ? r.c.volume : 0};
        out.ticks.push_back(t);
    }
    return out;
}

} // namespace altair::ui
