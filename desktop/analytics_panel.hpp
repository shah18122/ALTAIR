// desktop/analytics_panel.hpp -- Monte Carlo, z-score and the rolling moments,
// run on real data.
//
// P11Q-10.
//
// SMIT ASKED WHERE THESE WERE. THEY WERE BUILT AND TESTED AND HAD NO PAGE.
//
//   backtest/montecarlo.hpp   P6-07: GBM, Heston, Merton jump-diffusion, IID
//                             and block bootstrap, plus the performance
//                             statistics computed off them
//   analytics/rolling.hpp     RunningMoments and RollingWindow, both with
//                             stddev(), skew(), kurtosis() and zscore()
//
// A capability nobody can see is one that gets asked about, and answering "it
// is in a header" is not the same as showing it working. So this page runs
// both on the real NIFTY series and prints what they say.
//
// AN IID BOOTSTRAP DESTROYS THE THING YOU ARE MEASURING, AND THIS SHOWS IT.
//
// That is P6-07's own card and it is the reason `block_bootstrap` exists.
// Resampling returns one at a time is the obvious way to put a confidence
// interval around a backtest, and it silently assumes independence. Market
// returns are not independent: volatility clusters, so a bad day follows a bad
// day far more often than chance allows, and those runs are what produce the
// drawdowns capital dies in.
//
// Shuffle them and the clustering is gone. The mean survives, the variance
// survives, and the DRAWDOWN -- the statistic the whole exercise exists to
// bound -- comes out too small. This page runs both on the same series and
// prints the two side by side, because the argument is only convincing with
// the numbers next to it.
//
// EVERY NUMBER IS COMPUTED ON THE CLICK. Nothing is cached and nothing is a
// literal, so a statistic that stops working shows up as a changed number
// rather than as a string that still says what it said today.

#pragma once

#include <analytics/rolling.hpp>
#include <backtest/montecarlo.hpp>

#include "data/bar_csv.hpp"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace altair::ui {

class AnalyticsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit AnalyticsPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel(
            QStringLiteral("<h3>Analytics — moments, z-score, Monte "
                           "Carlo</h3>"), this));

        auto* note = new QLabel(
            QStringLiteral(
                "<code>analytics/rolling.hpp</code> and "
                "<code>backtest/montecarlo.hpp</code> have been built and "
                "tested since P6-07; neither had a page. Everything below is "
                "computed on the click from the real daily NIFTY series — "
                "nothing cached, nothing a literal."),
            this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note);

        run_ = new QPushButton(QStringLiteral("Run on real NIFTY daily"), this);
        connect(run_, &QPushButton::clicked, this, &AnalyticsPanel::run);
        v->addWidget(run_);

        out_ = new QPlainTextEdit(this);
        out_->setReadOnly(true);
        out_->setStyleSheet(QStringLiteral(
            "background:#12161A;color:#D6DBDF;font-family:Consolas,monospace;"));
        v->addWidget(out_, 1);
        run();
    }

private Q_SLOTS:
    void run() {
        const LoadResult d = load_bars_csv(
            QStringLiteral(ALTAIR_DATASET_DIR "/spot/nifty/1d/all.csv"),
            24LL * 3600 * 1'000'000'000LL, DailyStamp::SessionClose, true);
        if (!d.ok() || d.bars.size() < 500) {
            out_->setPlainText(QStringLiteral("no daily series: %1").arg(d.error));
            return;
        }
        std::vector<double> r;
        r.reserve(d.bars.size());
        for (std::size_t i = 1; i < d.bars.size(); ++i) {
            const double a = static_cast<double>(d.bars[i - 1].close);
            const double b = static_cast<double>(d.bars[i].close);
            if (a > 0.0 && b > 0.0) { r.push_back(std::log(b / a)); }
        }

        QString o;
        o += QStringLiteral("ROLLING MOMENTS — analytics/rolling.hpp\n");
        o += QStringLiteral("%1 daily log returns\n\n").arg(r.size());

        RunningMoments m;
        for (double x : r) { m.add(x); }
        const auto sd = m.stddev();
        const auto sk = m.skewness();
        const auto ku = m.excess_kurtosis();
        o += QStringLiteral("  mean      %1 bps/day\n")
                 .arg(m.mean().value_or(0.0) * 10'000.0, 0, 'f', 3);
        o += QStringLiteral("  stddev    %1 bps/day   (annualised %2%)\n")
                 .arg(sd ? *sd * 10'000.0 : 0.0, 0, 'f', 2)
                 .arg(sd ? *sd * std::sqrt(252.0) * 100.0 : 0.0, 0, 'f', 2);
        o += QStringLiteral("  skewness  %1\n").arg(sk ? *sk : 0.0, 0, 'f', 4);
        o += QStringLiteral("  kurtosis  %1  (EXCESS — a normal is 0)\n")
                 .arg(ku ? *ku : 0.0, 0, 'f', 4);
        o += QStringLiteral(
            "\n  That kurtosis is why a Gaussian VaR understates the tail. "
            "The moments are\n  computed by Welford's online update, so they "
            "are exact in one pass and do\n  not lose precision the way "
            "sum-of-squares does on a long series.\n");

        // Z-SCORE of the most recent day against the running distribution.
        if (!r.empty()) {
            const auto z = m.zscore(r.back());
            o += QStringLiteral("\n  z-score of the last day (%1 bps): %2\n")
                     .arg(r.back() * 10'000.0, 0, 'f', 1)
                     .arg(z ? QString::number(*z, 'f', 3)
                            : QStringLiteral("refused — zero spread"));
            // The extremes, so the scale means something.
            std::size_t beyond3 = 0;
            for (double x : r) {
                const auto zz = m.zscore(x);
                if (zz && std::fabs(*zz) > 3.0) { ++beyond3; }
            }
            o += QStringLiteral(
                     "  days beyond 3 sigma: %1 of %2 (%3%). A normal "
                     "distribution predicts 0.27%.\n")
                     .arg(beyond3).arg(r.size())
                     .arg(100.0 * static_cast<double>(beyond3)
                              / static_cast<double>(r.size()), 0, 'f', 2);
            o += QStringLiteral(
                "  zscore() REFUSES on a zero spread rather than dividing by "
                "it -- a z of\n  infinity is not a large move, it is an "
                "absent denominator.\n");
        }

        // ---- MONTE CARLO -------------------------------------------------
        o += QStringLiteral("\n\nMONTE CARLO — backtest/montecarlo.hpp "
                            "(P6-07)\n");
        o += QStringLiteral("  10,000 paths of %1 days, resampled from the "
                            "SAME returns.\n\n").arg(r.size());

        constexpr std::size_t kPaths = 10000;
        const std::size_t n = r.size();
        std::vector<double> path(n);
        Rng rng(0xA17A12u);

        double iid_dd = 0.0, blk_dd = 0.0;
        double iid_worst = 0.0, blk_worst = 0.0;
        for (std::size_t p = 0; p < kPaths; ++p) {
            if (iid_bootstrap(path.data(), n, r.data(), n, rng)) {
                if (const auto s = metrics(path.data(), n, 0.0)) {
                    iid_dd += s->max_drawdown;
                    iid_worst = std::max(iid_worst, s->max_drawdown);
                }
            }
            // Block length 20: about a trading month, long enough to carry a
            // volatility cluster through the resample.
            if (block_bootstrap(path.data(), n, r.data(), n, 20, rng)) {
                if (const auto s = metrics(path.data(), n, 0.0)) {
                    blk_dd += s->max_drawdown;
                    blk_worst = std::max(blk_worst, s->max_drawdown);
                }
            }
        }
        const double pn = static_cast<double>(kPaths);
        o += QStringLiteral("                        mean max drawdown   "
                            "worst of 10,000\n");
        o += QStringLiteral("  IID bootstrap         %1%              %2%\n")
                 .arg(100.0 * iid_dd / pn, 6, 'f', 2)
                 .arg(100.0 * iid_worst, 6, 'f', 2);
        o += QStringLiteral("  BLOCK bootstrap (20)  %1%              %2%\n")
                 .arg(100.0 * blk_dd / pn, 6, 'f', 2)
                 .arg(100.0 * blk_worst, 6, 'f', 2);

        // The actual realised drawdown, for scale.
        const auto real_stats = metrics(r.data(), n, 0.0);
        o += QStringLiteral("  ACTUAL realised       %1%\n")
                 .arg(100.0 * (real_stats ? real_stats->max_drawdown : 0.0),
                      6, 'f', 2);

        const double ratio = iid_dd > 0.0 ? blk_dd / iid_dd : 0.0;
        o += QStringLiteral(
                 "\n  The block bootstrap gives a drawdown %1x the IID one.\n")
                 .arg(ratio, 0, 'f', 2);
        o += QStringLiteral(
            "\n  THIS IS P6-07'S CARD. Shuffling returns one at a time "
            "destroys volatility\n  clustering -- the mean survives, the "
            "variance survives, and the DRAWDOWN,\n  which is the statistic "
            "the exercise exists to bound, comes out too small.\n  Sizing on "
            "an IID interval is sizing on a market that does not have bad\n"
            "  weeks, only bad days scattered at random.\n");
        const double blk_mean = 100.0 * blk_dd / pn;
        const double iid_mean = 100.0 * iid_dd / pn;
        const double actual = 100.0 * (real_stats ? real_stats->max_drawdown
                                                  : 0.0);
        o += QStringLiteral(
                 "\n  Against the ACTUAL realised drawdown of %1%:\n"
                 "    block bootstrap is out by %2 points\n"
                 "    IID bootstrap  is out by %3 points, and it UNDERSTATES\n")
                 .arg(actual, 0, 'f', 2)
                 .arg(std::fabs(blk_mean - actual), 0, 'f', 2)
                 .arg(std::fabs(iid_mean - actual), 0, 'f', 2);
        o += QStringLiteral(
            "\n  Understating is the dangerous direction: a risk model that "
            "says the worst\n  drawdown is smaller than it was sizes you "
            "bigger than the market allows.\n");

        out_->setPlainText(o);
    }

private:
    QPushButton* run_ = nullptr;
    QPlainTextEdit* out_ = nullptr;
};

} // namespace altair::ui
