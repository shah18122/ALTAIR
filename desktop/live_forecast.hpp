// desktop/live_forecast.hpp -- forecast the next candle, from the last real
// one, and say whether the forecaster is any good.
//
// P33-01. Smit's question, and it is a different one from the Spot Forecast
// page's:
//
//     "I just want NIFTY forecasted price from that particular moment. Use
//      live data from Kite -- if the button is clicked now it should check
//      the previous candle. Train on previous data, forecast, check against
//      actual price in past data, and make a decision maker: is the model
//      doing good or bad?"
//
// THREE THINGS, AND THEY ARE ON THIS PAGE IN THAT ORDER.
//
// 1. THE ANCHOR IS THE LAST *COMPLETED* CANDLE, PULLED LIVE.
//
//    "Live market is the previous candle" is exactly right and it is the part
//    that is easy to get wrong. While the market is open, TODAY's daily
//    candle exists and is a partial bar: its close is wherever the price
//    happens to be, not where the session ended. Anchoring on it forecasts
//    the next bar from a number that is still moving, and every later
//    comparison against "the actual close" is then comparing two different
//    things.
//
//    This project has already paid for that once. India VIX's 2026-09-04 bar
//    sat in dataset/ with low 10.71 and close 10.80 when the settled values
//    are 10.57 and 10.68 -- a bar captured mid-session and frozen. So the
//    last bar is DROPPED unless it has closed, and the page says which bar it
//    used and at what time.
//
// 2. THE BENCHMARK IS THE RANDOM WALK, NOT BUY-AND-HOLD.
//
//    The Spot Forecast page scores a trading rule and its control is
//    buy-and-hold, because a long-biased rule on a drifting index collects
//    the drift for free. This page scores a FORECAST, and the null for "what
//    will the price be" is "the same as it is now". Anybody can produce that,
//    it costs nothing, and on a financial series it is very hard to beat.
//
// 3. THE DECISION MAKER SAYS ONE OF FOUR THINGS, AND ONE OF THEM IS "I DO
//    NOT KNOW".
//
//    Better, worse, no better, and not enough evidence. The fourth is not a
//    hedge: a t-statistic on forty forecasts is a number and not evidence,
//    and a model deployed off one good month is the ordinary way this goes
//    wrong.
//
// THE UI STILL HAS NO NETWORK. The fetch is a subprocess, the same boundary
// Link Kite, the quote refresh, the account fetch and the tick feed all use.

#pragma once

#include "account_widgets.hpp"
#include "quant_pages.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTimeZone>

#include <models/calibration.hpp>
#include <models/forecast_scorecard.hpp>

namespace altair::ui {

/// Is the bar starting at `start_ns` finished?
///
/// A DAILY BAR IS NOT FINISHED AT MIDNIGHT. It is stamped 00:00 and covers
/// 09:15 to 15:30 IST on that date, so "start + one day <= now" would call
/// today's partial bar complete for the whole afternoon -- which is the exact
/// window in which somebody would click this button.
[[nodiscard]] inline bool bar_is_complete(std::int64_t start_ns,
                                          std::int64_t interval_ns,
                                          std::int64_t now_ns) {
    if (interval_ns <= 0) { return false; }
    if (interval_ns >= 86'400'000'000'000LL) {
        const QDateTime d = QDateTime::fromMSecsSinceEpoch(
            start_ns / 1'000'000LL, QTimeZone(5 * 3600 + 30 * 60));
        // 15:30 IST on that date, which is when the session settles.
        const QDateTime close(d.date(), QTime(15, 30),
                              QTimeZone(5 * 3600 + 30 * 60));
        return now_ns >= close.toMSecsSinceEpoch() * 1'000'000LL;
    }
    return start_ns + interval_ns <= now_ns;
}

/// Kite's interval name for a dataset directory name.
[[nodiscard]] inline const char* kite_interval_for(const QString& iv) {
    if (iv == QStringLiteral("1m"))  { return "minute"; }
    if (iv == QStringLiteral("5m"))  { return "5minute"; }
    if (iv == QStringLiteral("15m")) { return "15minute"; }
    if (iv == QStringLiteral("60m")) { return "60minute"; }
    return "day";
}

[[nodiscard]] inline std::int64_t interval_ns_for(const QString& iv) {
    if (iv == QStringLiteral("1m"))  { return 60'000'000'000LL; }
    if (iv == QStringLiteral("5m"))  { return 300'000'000'000LL; }
    if (iv == QStringLiteral("15m")) { return 900'000'000'000LL; }
    if (iv == QStringLiteral("60m")) { return 3'600'000'000'000LL; }
    return 86'400'000'000'000LL;
}

/// Run altair_kite_fetch for a short recent window, into a scratch directory.
///
/// SCRATCH, NOT dataset/. The UI does not write the training set. A window
/// that appended to dataset/ every time somebody pressed a button would make
/// the training data a function of how often the button was pressed, and
/// rule 10 wants a decision reproducible from a config and a data version --
/// not from a click history.
struct FetchResult {
    bool ran = false;
    QString error;
    UiStamped bars;
};

[[nodiscard]] inline FetchResult
fetch_recent(std::uint32_t token, const QString& interval_dir, int days) {
    FetchResult out;
    QString exe =
#if defined(_WIN32)
        QStringLiteral("altair_kite_fetch.exe");
#else
        QStringLiteral("altair_kite_fetch");
#endif
    QStringList tried;
    tried << QCoreApplication::applicationDirPath()
                 + QStringLiteral("/../app/") + exe;
    tried << QCoreApplication::applicationDirPath()
                 + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
    tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
    QString found;
    for (const QString& c : tried) {
        if (QFileInfo(c).isFile()) {
            found = QFileInfo(c).canonicalFilePath();
            break;
        }
    }
    if (found.isEmpty()) {
        out.error = QStringLiteral(
            "altair_kite_fetch is not in this build. It needs the `net` "
            "preset: build.bat net");
        return out;
    }

#ifdef ALTAIR_SOURCE_DIR
    const QString scratch =
        QStringLiteral(ALTAIR_SOURCE_DIR "/data/live_forecast_scratch");
#else
    const QString scratch = QStringLiteral("data/live_forecast_scratch");
#endif
    QDir().mkpath(scratch);
    // Cleared first: a stale month file from a previous instrument would be
    // merged in as if it belonged to this one.
    for (const QString& f :
         QDir(scratch).entryList(QStringList{QStringLiteral("*.csv")},
                                 QDir::Files)) {
        QFile::remove(scratch + QLatin1Char('/') + f);
    }

    const QDate today = QDateTime::currentDateTime(
                            QTimeZone(5 * 3600 + 30 * 60)).date();
    QProcess proc;
    proc.setProgram(found);
    proc.setArguments({QStringLiteral("--token"), QString::number(token),
                       QStringLiteral("--interval"),
                       QString::fromLatin1(kite_interval_for(interval_dir)),
                       QStringLiteral("--from"),
                       today.addDays(-days).toString(Qt::ISODate),
                       QStringLiteral("--to"), today.toString(Qt::ISODate),
                       QStringLiteral("--out"), scratch,
                       QStringLiteral("--volume-absent"),
                       QStringLiteral("--force"),
                       QStringLiteral("--go")});
    proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef ALTAIR_SOURCE_DIR
    proc.setWorkingDirectory(QStringLiteral(ALTAIR_SOURCE_DIR));
#endif
    proc.start();
    if (!proc.waitForStarted(5000) || !proc.waitForFinished(60000)) {
        proc.kill();
        out.error = QStringLiteral("the fetcher did not finish");
        return out;
    }
    if (proc.exitCode() != 0) {
        out.error = QString::fromUtf8(proc.readAll()).trimmed()
                        .section(QChar('\n'), -3);
        return out;
    }
    out.ran = true;
    for (const QString& f :
         QDir(scratch).entryList(QStringList{QStringLiteral("*.csv")},
                                 QDir::Files, QDir::Name)) {
        const UiStamped part =
            ui_load_stamped(scratch + QLatin1Char('/') + f);
        out.bars.closes.insert(out.bars.closes.end(), part.closes.begin(),
                               part.closes.end());
        out.bars.stamps_ns.insert(out.bars.stamps_ns.end(),
                                  part.stamps_ns.begin(),
                                  part.stamps_ns.end());
    }
    return out;
}

/// The page.
///
/// `live` decides whether Kite is asked. Everything else is identical, so the
/// offline path is the same code and not a second implementation.
[[nodiscard]] inline QString
live_forecast_report(const QString& dataset_root, const QString& sym,
                     const QString& interval, bool live) {
    const QuantSymbol q = quant_symbol_by_dir(sym);
    const std::int64_t iv_ns = interval_ns_for(interval);

    QString s = QStringLiteral(
        "LIVE FORECAST — the next candle, and whether to believe it\n"
        "P33-01\n\n");
    s += QStringLiteral("  INSTRUMENT: %1   INTERVAL: %2\n\n")
             .arg(QLatin1String(q.label), interval);

    // ---- 1. the history -------------------------------------------------
    UiStamped hist =
        ui_load_stamped(spot_path(dataset_root, sym, interval.toUtf8()
                                                         .constData()));
    if (hist.closes.size() < 2000) {
        // The intraday partitions are monthly files, not one all.csv.
        hist = UiStamped{};
        for (int y = 2015; y <= 2026; ++y) {
            for (int m = 1; m <= 12; ++m) {
                const UiStamped part = ui_load_stamped(
                    QStringLiteral("%1/spot/%2/%3/%4-%5.csv")
                        .arg(dataset_root, sym, interval)
                        .arg(y, 4, 10, QLatin1Char('0'))
                        .arg(m, 2, 10, QLatin1Char('0')));
                hist.closes.insert(hist.closes.end(), part.closes.begin(),
                                   part.closes.end());
                hist.stamps_ns.insert(hist.stamps_ns.end(),
                                      part.stamps_ns.begin(),
                                      part.stamps_ns.end());
            }
        }
    }
    if (hist.closes.size() < 2000) {
        return s + QStringLiteral(
            "  No usable history under dataset/spot/%1/%2/.\n").arg(sym,
                                                                    interval);
    }

    // ---- 2. the live tail ------------------------------------------------
    QString live_note;
    if (live) {
        const FetchResult fr = fetch_recent(q.kite_token, interval,
                                            interval == QStringLiteral("1d")
                                                ? 120 : 20);
        if (!fr.ran) {
            live_note = QStringLiteral(
                "  LIVE FETCH DID NOT RUN: %1\n"
                "  Falling back to the history on disk, which may be days\n"
                "  old. The anchor below says exactly which bar was used.\n\n")
                            .arg(fr.error);
        } else {
            // MERGE ON TIMESTAMP, FRESH WINS. Kite's settled value for a bar
            // supersedes whatever is in dataset/ -- which is the fix for the
            // frozen partial bar, not merely a refresh.
            // A MATERIAL difference is a partial bar. A tiny one is the
            // two sources disagreeing about decimal places, and replacing the
            // finer value with the coarser one would be a small loss reported
            // as a fix. One basis point: Kite's index dailies round to one
            // decimal, which on a 23,000 index is at most 0.02 bps, while the
            // India VIX partial bar this guard exists for was off by 110.
            constexpr double kMaterialBps = 1.0;
            std::size_t added = 0, replaced = 0, rounding = 0;
            for (std::size_t i = 0; i < fr.bars.closes.size(); ++i) {
                const std::int64_t ts = fr.bars.stamps_ns[i];
                bool found = false;
                for (std::size_t j = hist.stamps_ns.size(); j-- > 0;) {
                    if (hist.stamps_ns[j] == ts) {
                        const double a = hist.closes[j];
                        const double b = fr.bars.closes[i];
                        const double d =
                            (a > 0.0 && b > 0.0)
                                ? std::fabs(10'000.0 * std::log(b / a)) : 0.0;
                        if (d > kMaterialBps) {
                            hist.closes[j] = b;
                            ++replaced;
                        } else if (d > 0.0) {
                            ++rounding;   // left alone
                        }
                        found = true;
                        break;
                    }
                    // The tail is short; stop once we are older than it.
                    if (hist.stamps_ns[j] < ts - 400LL * iv_ns) { break; }
                }
                if (!found) {
                    hist.stamps_ns.push_back(ts);
                    hist.closes.push_back(fr.bars.closes[i]);
                    ++added;
                }
            }
            live_note = QStringLiteral(
                "  LIVE: %1 recent bars from Kite. %2 new; %3 stored bar(s)\n"
                "  differed MATERIALLY and were replaced — that is the\n"
                "  signature of a bar captured mid-session and frozen. A\n"
                "  further %4 differed by less than a basis point and were\n"
                "  LEFT ALONE: dataset/ carries two decimals and Kite's index\n"
                "  dailies carry one, and overwriting the finer value with\n"
                "  the coarser one is a small loss, not a correction.\n\n")
                            .arg(fr.bars.closes.size()).arg(added)
                            .arg(replaced).arg(rounding);
        }
    }
    s += live_note;

    // ---- 3. drop the unfinished bar --------------------------------------
    const std::int64_t now_ns =
        static_cast<std::int64_t>(QDateTime::currentMSecsSinceEpoch())
        * 1'000'000LL;
    QString dropped;
    while (!hist.stamps_ns.empty()
           && !bar_is_complete(hist.stamps_ns.back(), iv_ns, now_ns)) {
        dropped = ui_ist(hist.stamps_ns.back(), iv_ns < 86'400'000'000'000LL);
        hist.stamps_ns.pop_back();
        hist.closes.pop_back();
    }
    if (!dropped.isEmpty()) {
        s += QStringLiteral(
            "  DROPPED AN UNFINISHED BAR: %1.\n"
            "  While the session is open that bar's close is wherever the\n"
            "  price happens to be, not where the bar ended. Anchoring on it\n"
            "  forecasts from a number that is still moving.\n\n").arg(dropped);
    }
    if (hist.closes.size() < 2000) {
        return s + QStringLiteral("  Too little history after cleaning.\n");
    }

    // ---- 4. the forecast --------------------------------------------------
    SpotSpec spec;
    spec.horizon = 1;
    spec.lags = 8;
    spec.folds = 5;
    spec.cost_bps = 0.0;      // a FORECAST is not a trade

    // ---- 5. the backtest, which also supplies the band --------------------
    //
    // THE BAND COMES FROM THE BACKTEST, NOT FROM A TRAINING RESIDUAL. The
    // first pass runs with a nominal band purely to obtain the realised RMSE;
    // the second re-scores with the band the model has actually earned, so
    // the coverage number below is a check on a band somebody could publish.
    BacktestSpec bt;
    bt.warmup = 1500;
    bt.refit_every = interval == QStringLiteral("1d") ? 100 : 400;
    bt.max_points = 1500;

    const auto probe =
        backtest_forecasts(hist.closes, hist.stamps_ns, spec, bt, 1.0, false);
    if (!probe) {
        return s + QStringLiteral("  The walk-forward could not run.\n");
    }
    const auto probe_score = score_forecasts(*probe);
    if (!probe_score) {
        return s + QStringLiteral("  Nothing scored.\n");
    }
    const double band = probe_score->rmse_model_bps;

    const auto pts =
        backtest_forecasts(hist.closes, hist.stamps_ns, spec, bt, band, false);
    const auto sc = pts ? score_forecasts(*pts)
                        : std::unexpected(ForecastScoreError::TooFewBars);
    if (!sc) { return s + QStringLiteral("  Nothing scored.\n"); }

    const auto fc = forecast_next(hist.closes, spec, band, false,
                                  hist.stamps_ns);
    if (!fc) { return s + QStringLiteral("  The forecast could not run.\n"); }

    // ---- 5b. CALIBRATE THE BAND (P35) -------------------------------------
    //
    // Everything above publishes a band of one realised RMSE, which claims
    // 68.27% coverage on the assumption the errors are normal. They are not.
    // The conformal wrapper replaces that assumption with the record: a
    // weighted quantile of how wrong this forecaster has actually been, taken
    // separately above and below because the two tails are not the same size.
    //
    // The uncalibrated band is scored too, and both are shown. The comparison
    // is the point -- a calibrated band with no "before" beside it is a claim
    // rather than a result.
    CalibrationSpec cal;
    cal.conformal.alpha = 1.0 - 0.6827;   // match what the RMSE band claims,
    cal.conformal.mode = ConformalMode::TimeWeighted;  // so the two compare
    cal.warmup = 250;
    cal.vol_window = 20;

    const auto before_cal = score_calibration(*pts, cal.conformal.alpha,
                                              cal.vol_window);
    const auto calibrated = calibrate_band(*pts, cal);

    // The band that gets published is the CALIBRATED one when there is
    // enough record to earn it, and the raw RMSE band when there is not --
    // labelled either way, because a reader cannot tell them apart by looking.
    double pub_lo = fc->lo, pub_hi = fc->hi;
    QString band_label = QStringLiteral("band (1 realised RMSE, UNCALIBRATED)");
    if (calibrated && calibrated->second.next_ready) {
        pub_lo = fc->price - calibrated->second.next_lo;
        pub_hi = fc->price + calibrated->second.next_up;
        band_label = QStringLiteral("band (CONFORMAL, %1% target)")
                         .arg(100.0 * (1.0 - cal.conformal.alpha), 0, 'f', 1);
    }

    s += QStringLiteral("――― THE NEXT CANDLE ―――\n\n");
    s += QStringLiteral(
        "  Anchor  — last COMPLETED bar   %1\n"
        "            its close                        %2\n"
        "  Forecast is for                %3\n\n"
        "    forecast price               %4\n"
        "    %8\n"
        "                                 %5  ..  %6\n"
        "    implied move                 %7 bps\n\n")
             .arg(fc->stamped()
                      ? ui_ist(fc->last_ts_ns, iv_ns < 86'400'000'000'000LL)
                      : QStringLiteral("unstamped"), -22)
             .arg(fc->last_price, 12, 'f', 2)
             .arg(fc->stamped()
                      ? ui_ist(fc->for_ts_ns, iv_ns < 86'400'000'000'000LL)
                      : QStringLiteral("—"), -22)
             .arg(fc->price, 12, 'f', 2)
             .arg(pub_lo, 0, 'f', 2).arg(pub_hi, 0, 'f', 2)
             .arg(fc->move_bps, 0, 'f', 3)
             .arg(band_label);

    // ---- 5c. IS THE BAND HONEST? -----------------------------------------
    if (before_cal && calibrated) {
        const auto& b4 = *before_cal;
        const auto& af = calibrated->second;
        s += QStringLiteral(
            "――― IS THE BAND HONEST? ―――\n\n"
            "  A band is a claim about how often the truth lands inside it.\n"
            "  Both are scored against the same %1 forecasts, so the only\n"
            "  difference between the rows is the band itself.\n\n"
            "                     covered   target    per-regime error\n"
            "    RMSE band        %2    %3      %4 pp\n"
            "    conformal band   %5    %6      %7 pp\n\n")
                 .arg(b4.n)
                 .arg(b4.coverage, 7, 'f', 4)
                 .arg(1.0 - b4.target, 6, 'f', 4)
                 .arg(b4.reg_mae_pp, 6, 'f', 2)
                 .arg(af.coverage, 7, 'f', 4)
                 .arg(1.0 - af.target, 6, 'f', 4)
                 .arg(af.reg_mae_pp, 6, 'f', 2);

        // THE BREAKDOWN, because the average is the number that lies. A band
        // covering 95% in quiet markets and 85% in loud ones averages to a
        // figure that describes neither, and the loud one is when it matters.
        s += QStringLiteral(
            "  BY VOLATILITY QUINTILE — an average can hide a band that is\n"
            "  right on quiet days and wrong on the days that cost money.\n\n"
            "              realised vol      RMSE band    conformal\n");
        // `qi`, not `q` -- the QuantSymbol at the top of this function is
        // already called q, and shadowing it here is a warning at /W4 and a
        // trap for whoever edits this loop next.
        for (std::size_t qi = 0; qi < kVolBuckets; ++qi) {
            if (b4.by_vol[qi].n == 0) { continue; }
            s += QStringLiteral("    q%1  %2 bps        %3       %4\n")
                     .arg(qi)
                     .arg(b4.by_vol[qi].mean_vol_bps, 8, 'f', 1)
                     .arg(1.0 - b4.by_vol[qi].exceedance, 8, 'f', 4)
                     .arg(1.0 - af.by_vol[qi].exceedance, 8, 'f', 4);
        }
        s += QStringLiteral(
            "\n"
            "  effective memory %1 bars, effective sample %2 of the record.\n"
            "  %3 forecast(s) passed through uncalibrated during warmup.\n\n")
                 .arg(af.mean_memory, 0, 'f', 0)
                 .arg(af.mean_n_eff, 0, 'f', 0)
                 .arg(af.uncalibrated);
        if (af.fallbacks > 0) {
            s += QStringLiteral(
                "  %1 step(s) fell back to time-only weights because the\n"
                "  regime was unlike anything in the buffer.\n\n")
                     .arg(af.fallbacks);
        }
    }

    // ---- 6. the record ----------------------------------------------------
    s += QStringLiteral(
        "――― HOW THIS FORECASTER HAS ACTUALLY DONE ―――\n\n"
        "  %1 forecasts, every one made by a model fitted ONLY on bars\n"
        "  before it, refitted every %2 bars, and compared against the price\n"
        "  that actually printed.\n\n"
        "  The benchmark is the RANDOM WALK — \"next price = last price\".\n"
        "  Not buy-and-hold: that is the control for a trading rule, and this\n"
        "  is a forecast. Anybody can produce the random walk for free, and\n"
        "  on a price series it is very hard to beat.\n\n"
        "                            model        naive\n"
        "    mean abs error   %3 %4  bps\n"
        "    RMSE             %5 %6  bps\n"
        "    skill score      %7            (1 - model/naive; 0 = no better)\n"
        "    direction right  %8            (%9 sigma vs a coin flip)\n"
        "    always-up scores %13            <- the honest directional null\n"
        "                                        (%14 sigma vs THAT)\n"
        "    band coverage    %10            (the band claims %11)\n"
        "    mean signed err  %12  bps\n\n"
        "  A COIN FLIP IS THE WRONG DIRECTIONAL CONTROL ON A DRIFTING\n"
        "  SERIES. NIFTY rises on more bars than it falls, so a forecaster\n"
        "  that always says \"up\" already scores the always-up figure above\n"
        "  without knowing anything. The line under it is the model against\n"
        "  THAT, which is the number that means something -- the same\n"
        "  correction P21-03 made when a zero benchmark became\n"
        "  buy-and-hold.\n\n")
             .arg(sc->n).arg(bt.refit_every)
             .arg(sc->mae_model_bps, 12, 'f', 2)
             .arg(sc->mae_naive_bps, 12, 'f', 2)
             .arg(sc->rmse_model_bps, 12, 'f', 2)
             .arg(sc->rmse_naive_bps, 12, 'f', 2)
             .arg(sc->skill, 12, 'f', 4)
             .arg(sc->direction_hit, 12, 'f', 4)
             .arg(sc->direction_sigma, 0, 'f', 2)
             .arg(sc->coverage, 12, 'f', 4)
             .arg(sc->nominal_coverage, 0, 'f', 4)
             .arg(sc->mean_error_bps, 12, 'f', 3)
             .arg(sc->best_constant_direction, 12, 'f', 4)
             .arg(sc->direction_vs_drift, 0, 'f', 2);

    // ---- 7. the decision --------------------------------------------------
    const ForecastVerdict v = judge_forecast(*sc);
    s += QStringLiteral(
        "――― THE DECISION ―――\n\n"
        "    VERDICT:  %1\n\n"
        "  Taken from a PAIRED test: the per-bar difference of squared\n"
        "  errors, model against naive, so the fact that both faced the same\n"
        "  easy weeks and the same gap days is differenced away rather than\n"
        "  left in as variance.\n\n"
        "    paired mean   %2  bps^2 per bar  (positive = model closer)\n"
        "    standard err  %3\n"
        "    t             %4        (needs |t| > 2 over at least 200 bars)\n\n")
             .arg(QLatin1String(verdict_text(v)))
             .arg(sc->paired_mean, 0, 'f', 2)
             .arg(sc->paired_se, 0, 'f', 2)
             .arg(sc->t_stat, 0, 'f', 2);

    switch (v) {
    case ForecastVerdict::BetterThanNaive:
        s += QStringLiteral(
            "  The model is measurably closer to the realised price than\n"
            "  assuming no change. That is a real finding and the next\n"
            "  question is whether it survives cost — which is the Spot\n"
            "  Forecast page, not this one.\n");
        break;
    case ForecastVerdict::WorseThanNaive:
        s += QStringLiteral(
            "  The model is measurably FURTHER from the realised price than\n"
            "  doing nothing. The point forecast above is worse than simply\n"
            "  repeating the anchor, and should not be traded or sized on.\n");
        break;
    case ForecastVerdict::NoBetterThanNaive:
        s += QStringLiteral(
            "  Not distinguishable from the random walk. That is NOT the\n"
            "  same as 'worse' — it is 'no evidence either way', and it is\n"
            "  what an honest forecast record on a liquid index usually\n"
            "  says. The point forecast above is a number; the band around\n"
            "  it is the honest part.\n");
        break;
    case ForecastVerdict::NotEnoughEvidence:
        s += QStringLiteral(
            "  Too few forecasts to say anything. A t-statistic on a short\n"
            "  record is a number, not evidence.\n");
        break;
    }

    // DIRECTION AND MAGNITUDE ARE DIFFERENT SKILLS AND THIS MODEL HAS AT
    // MOST ONE OF THEM. Saying so is the point of the page: a reader who sees
    // "no better than naive" and a 3-sigma direction figure three lines apart
    // has to reconcile them, and most will pick whichever they preferred.
    if (sc->direction_vs_drift > 2.0
        && v != ForecastVerdict::BetterThanNaive) {
        s += QStringLiteral(
            "\n  BUT NOTE THE SPLIT. The model calls the DIRECTION better\n"
            "  than a constant-direction forecaster does (%1 sigma), and is\n"
            "  still no closer to the price. Those are different skills:\n"
            "  being right about the sign more often than chance and being\n"
            "  right about the SIZE are separate, and only the second makes\n"
            "  a price forecast useful. Position size is a function of\n"
            "  magnitude.\n\n"
            "  A directional edge with no magnitude edge is a signal for a\n"
            "  fixed-size rule, not a price forecast -- and whether it\n"
            "  survives cost is the Spot Forecast page's question.\n")
                 .arg(sc->direction_vs_drift, 0, 'f', 2);
    } else if (sc->direction_vs_drift <= 0.0) {
        s += QStringLiteral(
            "\n  And the direction is no better than always predicting the\n"
            "  way the series drifts (%1 sigma). There is no edge of either\n"
            "  kind here.\n")
                 .arg(sc->direction_vs_drift, 0, 'f', 2);
    }

    if (sc->coverage < sc->nominal_coverage - 0.10) {
        s += QStringLiteral(
            "\n  AND THE BAND IS TOO NARROW. It covers %1 of outcomes\n"
            "  against the %2 it claims, so it understates the error — the\n"
            "  direction that costs money, because a size derived from it is\n"
            "  too large.\n")
                 .arg(sc->coverage, 0, 'f', 3)
                 .arg(sc->nominal_coverage, 0, 'f', 3);
    } else if (sc->coverage > sc->nominal_coverage + 0.10) {
        s += QStringLiteral(
            "\n  The band is WIDER than it needs to be: it covers %1 against\n"
            "  a claimed %2. Conservative rather than dangerous, but it is\n"
            "  still a mis-stated interval.\n")
                 .arg(sc->coverage, 0, 'f', 3)
                 .arg(sc->nominal_coverage, 0, 'f', 3);
    }
    return s;
}

} // namespace altair::ui
