// Forecast training runs off the GUI thread; admitted trade updates use a cached model.
//
// DATA: FYERS ONLY. Bars come from data/store/fyers/spot/<inst>/<tf>/ (the
// FYERS candles ops/linux/fetch.py history and the nightly keep) and, on
// "Fetch recent + train", from FYERS alone; dataset/ (TradingView exports)
// and Kite are never read here. Five-level depth comes from ALTAIR's own
// recorder (data/store/depth_bars/fut/<inst>/1m/, written by altair_store
// decode): FYERS serves no historical depth, so depth history starts on the
// first recorded day.
#pragma once

#include <algorithm>
#include <cmath>
#include "live_forecast.hpp"
#include "live_market.hpp"
#include "price_client.hpp"
#include <models/depth_features.hpp>
#include <models/stream_forecast.hpp>
#include <QCheckBox>
#include <QMap>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <map>
#include <memory>
#include <optional>

namespace altair::ui {
struct ForecastTrainingResult {
    std::optional<StreamForecastModel> fitted;
    /// The same-session returns through the last completed bar: the live
    /// page continues this window, so the first live bar needs no warm-up.
    ReturnWindow window;
    /// The last bar's depth row (depth models only; empty when it had none).
    std::optional<depth::DepthBar> last_depth;
    QString error;
    QString provider;
    QString bars_dir;
    std::size_t loaded = 0, excluded = 0, older = 0;
    /// Stored bars the broker's fresh fetch REVISED (a different close at the
    /// same bar end), and the largest revision in basis points. The fetched
    /// bar wins: a bar captured mid-session differs from the settled print by
    /// a few paise, and refusing the whole training run over that was the
    /// "Fetched bar conflicts with stored history" error.
    std::size_t revised = 0;
    double max_revision_bp = 0.0;
    /// Duplicate stamps inside the stored files with different closes; the
    /// later row is kept.
    std::size_t duplicate_stamps = 0;
    /// Five-level depth: recorded minutes read, recorded days, bars with a reading.
    std::size_t depth_minutes = 0, depth_days = 0, bars_with_depth = 0;
    QString depth_first, depth_last;
};

/// Tracks only connection epochs and post-reset bar continuity. No broker or
/// socket dependency, so reconnect warm-up rules are testable in isolation.
class ForecastStreamContinuity {
public:
    static constexpr std::size_t kRequiredFreshIntervals = 20;

    explicit ForecastStreamContinuity(bool connected = false) noexcept
        : connected_(connected) {}

    /// Returns true only for a real connected/disconnected transition.
    bool observe_connection(bool connected) noexcept {
        if (connected_ == connected) return false;
        connected_ = connected;
        needs_reanchor_ = true;
        fresh_intervals_ = 0;
        return true;
    }

    /// First admitted market-time tick after a transition must seed a new
    /// rolling window instead of reusing bars from the previous epoch.
    bool consume_reanchor() noexcept {
        if (!needs_reanchor_) return false;
        needs_reanchor_ = false;
        fresh_intervals_ = 0;
        return true;
    }

    void reset_warmup() noexcept { fresh_intervals_ = 0; }
    void completed_interval() noexcept {
        if (fresh_intervals_ < kRequiredFreshIntervals) ++fresh_intervals_;
    }
    [[nodiscard]] std::size_t fresh_intervals() const noexcept { return fresh_intervals_; }
    [[nodiscard]] bool ready() const noexcept {
        return fresh_intervals_ >= kRequiredFreshIntervals;
    }

private:
    bool connected_ = false;
    bool needs_reanchor_ = false;
    std::size_t fresh_intervals_ = 0;
};

/// Recorded depth minutes under `dir` (one CSV a day), in time order. A
/// repeated minute keeps the later file's row. False with `error` on a
/// malformed file: a silently skipped row would be a silent hole.
inline bool load_depth_minutes(const QString& dir, std::vector<depth::DepthMinute>& out, std::size_t& days,
                               QString& error) {
    out.clear();
    days = 0;
    const QDir d(dir);
    if (!d.exists()) return true;
    std::map<std::int64_t, depth::DepthMinute> by_end;
    for (const QString& name : d.entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name)) {
        if (QThread::currentThread()->isInterruptionRequested()) { error = QStringLiteral("Training cancelled"); return false; }
        QFile f(d.filePath(name));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { error = QStringLiteral("Cannot read %1").arg(f.fileName()); return false; }
        QTextStream in(&f);
        if (in.readLine().trimmed() != QLatin1String(depth::kCsvHeader.data(), static_cast<qsizetype>(depth::kCsvHeader.size()))) {
            error = QStringLiteral("Not a depth_1m file (header): %1").arg(f.fileName());
            return false;
        }
        std::size_t rows = 0;
        while (!in.atEnd()) {
            const QByteArray line = in.readLine().trimmed().toUtf8();
            if (line.isEmpty()) continue;
            const auto m = depth::parse_csv_row(std::string_view(line.constData(), static_cast<std::size_t>(line.size())));
            if (!m) { error = QStringLiteral("Malformed depth row in %1").arg(f.fileName()); return false; }
            by_end[m->end_ns] = *m;
            ++rows;
        }
        if (rows > 0) ++days;
    }
    out.reserve(by_end.size());
    for (const auto& [end, m] : by_end) out.push_back(m);
    return true;
}

/// Cold worker input: copied strings, no widgets. Bar ends are known at fit time.
/// `root` is the repository root (data/store/... lives under it).
inline ForecastTrainingResult train_forecast_page(const QString& root, const QString& symbol,
                                                  const QString& interval, bool fetch,
                                                  std::size_t horizon_bars = 1, bool use_depth = false) {
    constexpr int kTrainingBars = 12000;
    ForecastTrainingResult result;
    const auto width = interval_ns_for(interval);
    const bool daily = width >= kStreamDayNs;
    const auto now = QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
    const QTimeZone ist(19800);
    QMap<std::int64_t, double> bars;
    std::size_t valid_rows = 0;
    if (use_depth && daily) {
        result.error = QStringLiteral("Five-level depth needs an intraday interval (1m to 60m).");
        return result;
    }
    if (use_depth && symbol != QLatin1String("nifty") && symbol != QLatin1String("banknifty")) {
        result.error = QStringLiteral("%1 has no futures order book; five-level depth is NIFTY and BANKNIFTY only.").arg(symbol);
        return result;
    }
    const QDir directory(root + QStringLiteral("/data/store/fyers/spot/%1/%2").arg(symbol, interval));
    result.bars_dir = directory.absolutePath();
    QStringList files = directory.exists("all.csv") ? QStringList{"all.csv"}
        : directory.entryList({"*.csv"}, QDir::Files, QDir::Name);
    if (files.isEmpty() && !fetch) {
        result.error = QStringLiteral("No FYERS candles in %1. Run python3 ops/linux/fetch.py history --go once (needs today's FYERS "
                                      "login); the nightly then adds each day. dataset/ is not used: FYERS data only.")
                           .arg(result.bars_dir);
        return result;
    }
    for (const QString& name : files) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            result.error = QStringLiteral("Training cancelled"); return result;
        }
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            result.error = QStringLiteral("Cannot read %1").arg(name); return result;
        }
        QTextStream input(&file);
        const QStringList header = input.readLine().split(',');
        int timestamp_column = -1, close_column = -1;
        for (int i = 0; i < header.size(); ++i) {
            const QString field = header[i].trimmed().remove('"').toLower();
            if (field == "timestamp" || field == "date" || field == "time") timestamp_column = i;
            if (field == "close") close_column = i;
        }
        if (timestamp_column < 0 || close_column < 0) {
            result.error = QStringLiteral("CSV needs timestamp/date and close columns: %1").arg(name); return result;
        }
        std::int64_t prior_end = 0;
        std::size_t rows_read = 0;
        while (!input.atEnd()) {
            if ((rows_read++ & 0x0fffU) == 0 && QThread::currentThread()->isInterruptionRequested()) {
                result.error = QStringLiteral("Training cancelled"); return result;
            }
            const QString line = input.readLine();
            if (line.trimmed().isEmpty()) continue;
            const auto columns = line.split(',');
            if (columns.size() <= qMax(timestamp_column, close_column) || line.contains('"')) {
                result.error = QStringLiteral("Malformed or quoted CSV row in %1").arg(name); return result;
            }
            bool ok = false;
            const double price = columns[close_column].trimmed().toDouble(&ok);
            const QString timestamp = columns[timestamp_column].trimmed();
            QDateTime stamp = QDateTime::fromString(timestamp, Qt::ISODate);
            if (timestamp.size() == 10)
                stamp = QDateTime(QDate::fromString(timestamp, Qt::ISODate), QTime(0, 0), ist);
            if (!ok || !std::isfinite(price) || price <= 0.0 || !stamp.isValid()) {
                result.error = QStringLiteral("Invalid price/timestamp in %1").arg(name); return result;
            }
            stamp = stamp.toTimeZone(ist);
            const auto end = daily
                ? QDateTime(stamp.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL
                : stamp.toMSecsSinceEpoch() * 1'000'000LL + width;
            const auto close = QDateTime(stamp.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL;
            if (end > now || (!daily && (stamp.time() < QTime(9, 15) || end > close))) {
                ++result.excluded; continue;
            }
            if (end < prior_end) {
                result.error = QStringLiteral("Timestamps go backwards within %1").arg(name); return result;
            }
            prior_end = end;
            ++valid_rows;
            if (bars.contains(end) && bars.value(end) != price) {
                // A repeated stamp with a different close: the later row is
                // the newer capture, so it replaces the earlier one, counted.
                bars.insert(end, price);
                ++result.duplicate_stamps;
                continue;
            }
            if (!bars.contains(end) && bars.size() >= kTrainingBars && end <= bars.firstKey()) {
                ++result.older;
                continue;
            }
            bars.insert(end, price);
            if (bars.size() > kTrainingBars) { bars.erase(bars.begin()); ++result.older; }
        }
    }
    if (fetch) {
        // FYERS only: a failed FYERS fetch is reported, never filled from Kite.
        const auto fresh = fetch_recent(symbol, quant_symbol_by_dir(symbol).kite_token, interval,
                                        daily ? 120 : 20, /*fyers_only=*/true);
        if (!fresh.ran) { result.error = QStringLiteral("FYERS fetch failed: %1").arg(fresh.error); return result; }
        result.provider = fresh.provider;
        for (std::size_t i = 0; i < fresh.bars.closes.size(); ++i) {
            const auto start = fresh.bars.stamps_ns[i];
            const auto date = QDateTime::fromMSecsSinceEpoch(start / 1'000'000LL, ist);
            const auto session_end = QDateTime(date.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL;
            const auto end = daily ? session_end : start + width;
            if (end > now || (!daily && (date.time() < QTime(9, 15) || end > session_end))) {
                ++result.excluded; continue;
            }
            if (bars.contains(end) && bars.value(end) != fresh.bars.closes[i]) {
                // THE BROKER'S BAR WINS. The stored one was captured earlier
                // (often mid-bar); the fetch is the settled print. Counted and
                // sized, never silent.
                const double before = bars.value(end);
                const double bp = before > 0.0 ? std::fabs(fresh.bars.closes[i] / before - 1.0) * 1e4 : 0.0;
                result.max_revision_bp = std::max(result.max_revision_bp, bp);
                ++result.revised;
                bars.insert(end, fresh.bars.closes[i]);
                continue;
            }
            if (!bars.contains(end) && bars.size() >= kTrainingBars && end <= bars.firstKey()) {
                ++result.older;
                continue;
            }
            bars.insert(end, fresh.bars.closes[i]);
            ++valid_rows;
            if (bars.size() > kTrainingBars) { bars.erase(bars.begin()); ++result.older; }
        }
    }
    result.loaded = valid_rows;
    std::vector<double> prices;
    std::vector<std::int64_t> ends;
    for (auto it = bars.cbegin(); it != bars.cend(); ++it) { ends.push_back(it.key()); prices.push_back(it.value()); }

    std::vector<depth::DepthBar> depth_rows;
    if (use_depth) {
        const QString depth_dir = root + QStringLiteral("/data/store/depth_bars/fut/%1/1m").arg(symbol);
        std::vector<depth::DepthMinute> minutes;
        QString error;
        if (!load_depth_minutes(depth_dir, minutes, result.depth_days, error)) { result.error = error; return result; }
        result.depth_minutes = minutes.size();
        if (minutes.empty()) {
            result.error = QStringLiteral("No recorded five-level depth in %1. FYERS serves no past depth, so depth history "
                                          "exists only from ALTAIR's own recorder: run the recorder and the nightly decode, "
                                          "then train again.").arg(depth_dir);
            return result;
        }
        const auto stamp = [&](std::int64_t ns) {
            return QDateTime::fromMSecsSinceEpoch(ns / 1'000'000LL, ist).toString("yyyy-MM-dd");
        };
        result.depth_first = stamp(minutes.front().end_ns);
        result.depth_last = stamp(minutes.back().end_ns);
        depth_rows = depth::depth_for_bars(minutes, ends, width);
        if (depth_rows.size() != ends.size()) {
            result.error = QStringLiteral("Depth minutes are out of order in %1").arg(depth_dir);
            return result;
        }
        for (const auto& r : depth_rows) if (std::isfinite(r[0])) ++result.bars_with_depth;
    }
    auto fitted = fit_stream_forecast(prices, ends, width, horizon_bars, depth_rows);
    if (!fitted) {
        if (use_depth && fitted.error() == StreamForecastError::InsufficientHistory)
            result.error = QStringLiteral("Not enough recorded depth yet: %1 of %2 bars have a five-level reading "
                                          "(%3 recorded day(s), %4 to %5). The fit needs 500 usable rows with depth; "
                                          "keep the recorder running and train again.")
                               .arg(result.bars_with_depth).arg(ends.size()).arg(result.depth_days)
                               .arg(result.depth_first, result.depth_last);
        else
            result.error = QStringLiteral("Fit refused: need 600 valid FYERS bars and 500 usable target windows "
                                          "(%1 bars in %2; error %3)")
                               .arg(prices.size()).arg(result.bars_dir).arg(static_cast<int>(fitted.error()));
        return result;
    }
    result.window = ReturnWindow(width);
    for (std::size_t i = 0; i < prices.size(); ++i) (void)result.window.push(prices[i], ends[i]);
    if (use_depth && std::isfinite(depth_rows.back()[0])) result.last_depth = depth_rows.back();
    result.fitted = std::move(*fitted);
    return result;
}

/// Bars in the chosen horizon: 0 seconds means the next bar. Daily stays one
/// session, and a horizon shorter than the bar rounds up to one bar.
inline std::size_t horizon_bars_for(const QString& interval, int seconds) {
    const auto width = interval_ns_for(interval);
    if (seconds <= 0 || width <= 0 || width >= 86'400'000'000'000LL) return 1;
    const auto span = static_cast<std::int64_t>(seconds) * 1'000'000'000LL;
    return static_cast<std::size_t>(std::max<std::int64_t>(1, (span + width - 1) / width));
}

class StreamForecastPage final : public QWidget {
public:
    /// `root`: the repository root (FYERS candles, recorded depth and the
    /// live universe are under its data/).
    explicit StreamForecastPage(PriceClient* client, QString root, QWidget* parent = nullptr)
        : QWidget(parent), client_(client), continuity_(client->connected()), root_(std::move(root)) {
        setObjectName("streamForecastPage");
        auto* layout = new QVBoxLayout(this);
        auto* controls = new QHBoxLayout;
        symbols_ = new QComboBox(this);
        std::size_t count = 0;
        const auto* symbols = quant_symbols(count);
        for (std::size_t i = 0; i < count; ++i) symbols_->addItem(symbols[i].label, symbols[i].dir);
        interval_ = new QComboBox(this);
        for (const QString& value : {"1m", "5m", "15m", "60m", "1d"}) interval_->addItem(value, value);
        interval_->setCurrentIndex(interval_->findData(QStringLiteral("15m")));
        horizon_ = new QComboBox(this);
        horizon_->addItem("Next bar", 0);
        horizon_->addItem("Next 1 hour", 3600);
        horizon_->setCurrentIndex(1);
        depth_ = new QCheckBox("5-level depth", this);
        depth_->setToolTip("Adds touch OBI, 5-level OBI (plain and weighted), microprice offset, spread and 5-level OFI "
                           "from the near-month future's book. Depth history exists only from ALTAIR's recorder.");
        train_ = new QPushButton("Train from history", this);
        fetch_ = new QPushButton("Fetch recent + train", this);
        stream_ = new QPushButton("Connect price stream", this);
        controls->addWidget(symbols_); controls->addWidget(interval_); controls->addWidget(horizon_);
        controls->addWidget(depth_);
        controls->addWidget(train_); controls->addWidget(fetch_); controls->addWidget(stream_);
        layout->addLayout(controls);
        status_ = new QLabel("Select a timeframe and train. Future targets and current price are shown separately.", this);
        status_->setWordWrap(true); layout->addWidget(status_);
        output_ = new QPlainTextEdit(this); output_->setReadOnly(true);
        output_->setStyleSheet("QPlainTextEdit{background:#0B0F14;color:#C9D1D9;font-family:Consolas;font-size:13px;border:0;}");
        layout->addWidget(output_, 1);
        connect(train_, &QPushButton::clicked, this, [this] { begin_training(false); });
        connect(fetch_, &QPushButton::clicked, this, [this] { begin_training(true); });
        connect(stream_, &QPushButton::clicked, this, [this] {
            if (client_->connected()) client_->stop();
            else client_->start();
        });
        const auto invalidate = [this] {
            ++generation_; fitted_.reset(); latest_.reset(); pending_ = false; live_ = false;
            stop_depth_watch();
            continuity_.reset_warmup();
            if (worker_) worker_->requestInterruption();
            output_->clear(); status_->setText("Selection changed. Train this instrument/timeframe.");
        };
        connect(symbols_, &QComboBox::currentIndexChanged, this, invalidate);
        connect(interval_, &QComboBox::currentIndexChanged, this, invalidate);
        connect(horizon_, &QComboBox::currentIndexChanged, this, invalidate);
        connect(depth_, &QCheckBox::toggled, this, invalidate);
        const auto enable_choices = [this] {
            const bool intraday = interval_->currentData().toString() != QStringLiteral("1d");
            const QString symbol = symbols_->currentData().toString();
            horizon_->setEnabled(intraday);
            depth_->setEnabled(intraday && (symbol == QLatin1String("nifty") || symbol == QLatin1String("banknifty")));
        };
        connect(interval_, &QComboBox::currentIndexChanged, this, enable_choices);
        connect(symbols_, &QComboBox::currentIndexChanged, this, enable_choices);
        enable_choices();
        connect(client_, &PriceClient::tradeUpdated, this, [this](unsigned token) { on_trade(token); });
        connect(client_, &PriceClient::frameApplied, this, [this](unsigned token, int kind, qint64 engine_ns) {
            on_book(token, kind, engine_ns);
        });
        connect(client_, &PriceClient::statusChanged, this, [this] {
            const bool connected = client_->connected();
            stream_->setText(connected ? "Disconnect price stream" : "Connect price stream");
            if (continuity_.observe_connection(connected)) {
                invalidate_live_epoch(connected
                    ? QStringLiteral("Stream reconnected — waiting for a fresh trade")
                    : QStringLiteral("Disconnected — no current live forecast"));
            }
        });
        auto* paint = new QTimer(this);
        paint->setInterval(100);
        connect(paint, &QTimer::timeout, this, [this] {
            if (latest_ && live_ && QDateTime::currentMSecsSinceEpoch() * 1'000'000LL - latest_->issued_ns > 5'000'000'000LL) {
                latest_.reset(); live_status_ = "Stale — waiting for a fresh trade"; dirty_ = true;
            }
            if (dirty_) { render(); dirty_ = false; }
        });
        paint->start();
    }
    ~StreamForecastPage() override {
        if (worker_) { worker_->requestInterruption(); worker_->wait(); }
        stop_depth_watch();
    }
private:
    void begin_training(bool fetch) {
        if (worker_) return;
        const auto generation = ++generation_;
        fitted_.reset(); latest_.reset(); output_->clear(); pending_ = false; live_ = false;
        stop_depth_watch();
        const QString symbol = symbols_->currentData().toString(), interval = interval_->currentData().toString();
        token_ = quant_symbol_by_dir(symbol).kite_token;
        const auto horizon = horizon_bars_for(interval, horizon_->currentData().toInt());
        const bool use_depth = depth_->isEnabled() && depth_->isChecked();
        train_->setEnabled(false); fetch_->setEnabled(false);
        status_->setText("Training in background: chronological fit / selection / calibration, purged and embargoed. "
                         "Navigation remains available.");
        auto result = std::make_shared<ForecastTrainingResult>();
        worker_ = QThread::create([result, root = root_, symbol, interval, fetch, horizon, use_depth] {
            *result = train_forecast_page(root, symbol, interval, fetch, horizon, use_depth);
        });
        auto* thread = worker_.data();
        connect(thread, &QThread::finished, this,
                [this, result, generation, symbol, daily = interval == "1d"] {
            worker_ = nullptr; train_->setEnabled(true); fetch_->setEnabled(true);
            if (generation != generation_) return;
            if (!result->fitted) { status_->setText(result->error); return; }
            fitted_ = std::move(result->fitted);
            window_ = result->window;
            last_tick_ = 0; pending_ = false; tainted_end_ = 0;
            continuity_.reset_warmup();
            missed_ = client_->missed_trades(); received_ = 0;
            const auto& f = *fitted_;
            const QTimeZone ist(19800);
            const auto day_of = [&](std::int64_t ns) {
                return QDateTime::fromMSecsSinceEpoch(ns / 1'000'000LL, ist).toString("yyyy-MM-dd");
            };
            metrics_ = QString("Target: close %1 bar(s) ahead, inside one session\n"
                               "Data: FYERS candles in %2\n"
                               "Rows %3 to %4: fit %5 | tree-depth selection %6 | calibration %7 | depth %8 (searched 2-5)\n"
                               "Leakage guards: %9 overlapping rows purged, %10 rows embargoed at the partition boundaries\n"
                               "Calibration RMSE: model %11 bps / random walk %12 bps\n"
                               "Loaded %13 bars; older bars outside 12,000-bar window %14; excluded partial/session bars %15; "
                               "rows skipped (target crosses a session break or gap, or < 20 returns in 14 days) %16\n")
                .arg(f.horizon_bars).arg(result->bars_dir)
                .arg(day_of(f.first_row_ns), day_of(f.last_row_ns))
                .arg(f.training_rows).arg(f.validation_rows).arg(f.scored_rows).arg(f.chosen_depth)
                .arg(f.purged_rows).arg(f.embargoed_rows)
                .arg(f.rmse_bps, 0, 'f', 3).arg(f.naive_rmse_bps, 0, 'f', 3)
                .arg(result->loaded).arg(result->older).arg(result->excluded).arg(f.skipped_windows);
            if (f.depth_features > 0) {
                const bool helps = std::isfinite(f.returns_only_rmse_bps) && f.rmse_bps < f.returns_only_rmse_bps;
                metrics_ += QString("Five-level depth (near future, recorded %1 day(s), %2 to %3): %4 of the bars have a reading; "
                                    "%5 rows dropped for no reading\n"
                                    "Same calibration rows: with depth %6 bps vs returns only %7 bps — depth %8\n")
                    .arg(result->depth_days).arg(result->depth_first, result->depth_last)
                    .arg(result->bars_with_depth).arg(f.depth_missing_rows)
                    .arg(f.rmse_bps, 0, 'f', 3).arg(f.returns_only_rmse_bps, 0, 'f', 3)
                    .arg(helps ? QStringLiteral("lowers the error on this sample (not yet proof of an edge)")
                               : QStringLiteral("does NOT help on this sample"));
                start_depth_watch(symbol);
            }
            if (result->revised > 0 || result->duplicate_stamps > 0)
                metrics_ += QString("FYERS fetch revised %1 stored bar(s), largest change %2 bp; %3 repeated stamp(s) in the files kept their later row\n")
                    .arg(result->revised).arg(result->max_revision_bp, 0, 'f', 2).arg(result->duplicate_stamps);
            std::array<double, kStreamReturnLags> returns{};
            window_.copy(returns);
            std::span<const double> depth_row;
            if (result->last_depth) depth_row = *result->last_depth;
            const auto estimate = (f.depth_features == 0 || result->last_depth)
                ? f.predict(returns, window_.last_close(), f.observed_through_ns, depth_row)
                : std::expected<ForwardEstimate, StreamForecastError>{std::unexpected(StreamForecastError::BadInput)};
            if (estimate) latest_ = *estimate;
            live_status_ = estimate ? QStringLiteral("Historical forecast from last completed bar — waiting for live trades")
                                    : QStringLiteral("The last completed bar has no depth reading — waiting for live trades");
            const QString source = result->provider.isEmpty()
                ? QString{} : QStringLiteral(" Recent history: %1.").arg(result->provider);
            status_->setText((daily
                ? QStringLiteral("Daily model fitted from completed bars. Live ticks do not advance a daily-session forecast.")
                : QStringLiteral("Model fitted. Each admitted trade updates inference; display refreshes at 10 Hz."))
                + source);
            dirty_ = true;
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    /// Watches the near-month future's book for live depth features.
    void start_depth_watch(const QString& symbol) {
        stop_depth_watch();
        const QString under = symbol == QLatin1String("nifty") ? QStringLiteral("NIFTY") : QStringLiteral("BANKNIFTY");
        const qint64 today = QDate(1970, 1, 1).daysTo(QDateTime::currentDateTime(QTimeZone(19800)).date());
        const LiveRow* best = nullptr;
        const auto rows = load_live_universe(root_ + QStringLiteral("/data/live/universe.csv"));
        for (const auto& r : rows)
            if (r.kind == QLatin1String("future") && r.underlying == under && r.fyers.startsWith(QLatin1String("NSE:")) &&
                r.expiry_day >= today && (best == nullptr || r.expiry_day < best->expiry_day))
                best = &r;
        if (best == nullptr) {
            depth_note_ = QStringLiteral("No %1 future in data/live/universe.csv: live depth forecasts are unavailable "
                                         "until the price service streams it.").arg(under);
            return;
        }
        depth_token_ = best->token;
        depth_note_ = QStringLiteral("Live depth from %1 (5 levels).").arg(best->fyers);
        client_->watch_frames(depth_token_, true);
    }
    void stop_depth_watch() {
        if (depth_token_ != 0) client_->watch_frames(depth_token_, false);
        depth_token_ = 0;
        depth_builder_ = depth::DepthMinuteBuilder{};
        depth_done_.clear();
        depth_note_.clear();
    }

    void on_book(unsigned token, int kind, qint64 engine_ns) {
        if (depth_token_ == 0 || token != depth_token_ || kind != PriceClient::AppliedBook || !fitted_) return;
        const auto* p = client_->price(token);
        if (p == nullptr || p->replay || p->simulated || p->levels == 0) return;
        // RULE 11: deliberate truncation -- the features are defined on the
        // top five levels; a 50-level book is read as its top five.
        const std::size_t n = std::min<std::size_t>(p->levels, depth::kLevels);
        std::array<depth::BookRung, depth::kLevels> bids{}, asks{};
        for (std::size_t k = 0; k < n; ++k) {
            bids[k] = {p->bids[k].price_paise, p->bids[k].qty};
            asks[k] = {p->asks[k].price_paise, p->asks[k].qty};
        }
        if (!depth_builder_.on_book(bids.data(), n, asks.data(), n, engine_ns, depth_done_)) return;
        // Only minutes that can still fall inside a live bar are kept.
        const std::int64_t keep_after = engine_ns - fitted_->interval_ns - depth::kMinuteNs;
        depth_done_.erase(std::remove_if(depth_done_.begin(), depth_done_.end(),
                                         [keep_after](const depth::DepthMinute& m) { return m.end_ns <= keep_after; }),
                          depth_done_.end());
    }

    /// The bar in progress's depth row: the minutes inside it so far,
    /// including the one in progress (as the price features use the
    /// provisional bar). Empty when no book arrived in this bar.
    [[nodiscard]] std::optional<depth::DepthBar> live_depth(std::int64_t bar_end) const {
        const std::int64_t start = bar_end - fitted_->interval_ns;
        depth::BarSums s;
        for (const auto& m : depth_done_)
            if (m.end_ns > start && m.end_ns <= bar_end) s.add(m);
        if (const auto c = depth_builder_.current(); c && c->end_ns > start && c->end_ns <= bar_end) s.add(*c);
        const auto row = s.row(1);
        if (!std::isfinite(row[0])) return std::nullopt;
        return row;
    }

    void on_trade(unsigned token) {
        if (!fitted_ || token != token_) return;
        const auto* price = client_->price(token);
        if (!price) return;
        const auto now = QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const auto ts = price->exchange_ts_ns, width = fitted_->interval_ns;
        const auto reach = width * static_cast<std::int64_t>(fitted_->horizon_bars);
        if (width >= kStreamDayNs) {
            const QString message = "Daily projection is anchored to the last completed daily bar; a session calendar is needed to resolve its next close.";
            if (live_status_ != message) { live_status_ = message; dirty_ = true; }
            return;
        }
        if (price->replay || ts <= 0 || ts < fitted_->observed_through_ns || ts < last_tick_ ||
            ts > now + 1'000'000'000LL || now - ts > 5'000'000'000LL || price->last_paise <= 0) {
            latest_.reset(); live_status_ = "Input refused: replay, stale/out-of-order trade or stream gap. Refit after a gap."; dirty_ = true; return;
        }
        const QTimeZone ist(19800);
        const auto at = QDateTime::fromMSecsSinceEpoch(ts / 1'000'000LL, ist);
        const auto open = QDateTime(at.date(), QTime(9, 15), ist).toMSecsSinceEpoch() * 1'000'000LL;
        const auto close = QDateTime(at.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL;
        if (ts < open || ts >= close) {
            latest_.reset(); live_status_ = "Outside the 09:15-15:30 session: no intraday forecast."; dirty_ = true; return;
        }
        const double now_price = static_cast<double>(price->last_paise) / 100.0;
        const auto bucket_end = open + ((ts - open) / width + 1) * width;
        // A new stream epoch or lost trades: the bar in progress may be
        // missing its true last print, so its close is never a return.
        bool gap = continuity_.consume_reanchor();
        if (client_->missed_trades() != missed_) { missed_ = client_->missed_trades(); gap = true; }
        if (gap) {
            window_.break_chain();
            if (pending_) tainted_end_ = pending_end_;
        }
        // A later bar began: the pending one is complete.
        if (pending_ && bucket_end != pending_end_) {
            if (pending_end_ == tainted_end_ || !window_.push(pending_price_, pending_end_)) window_.break_chain();
            pending_ = false;
        }
        last_tick_ = ts;
        if (bucket_end > close) {
            // The 60m bar from 15:15 ends after the close: training leaves it
            // out, so it is neither a bar nor a forecast here.
            latest_.reset(); live_status_ = "Last partial bar of the session: no bar and no forecast."; dirty_ = true; return;
        }
        if (bucket_end <= window_.last_end()) {
            latest_.reset(); live_status_ = "This trade is inside the fitted history; waiting for the next bar."; dirty_ = true; return;
        }
        pending_ = true; pending_end_ = bucket_end; pending_price_ = now_price;
        ++received_; live_ = true; dirty_ = true;
        if (ts + reach > close) {
            latest_.reset();
            live_status_ = "No intraday target inside this session (the horizon would end after 15:30). Bars keep updating for the next session.";
            return;
        }
        if (!window_.ready(ts)) {
            latest_.reset();
            live_status_ = "Waiting: fewer than 20 same-session returns in the last 14 days. Use \"Fetch recent + train\".";
            return;
        }
        std::array<double, kStreamReturnLags> observed{};
        const bool provisional = bucket_end != tainted_end_ && window_.observe(now_price, bucket_end, observed);
        if (!provisional) window_.copy(observed);
        std::optional<depth::DepthBar> depth_row;
        if (fitted_->depth_features > 0) {
            depth_row = live_depth(bucket_end);
            if (!depth_row) {
                latest_.reset();
                live_status_ = depth_token_ == 0 ? depth_note_
                    : QStringLiteral("Waiting for the near future's five-level book in this bar.");
                return;
            }
        }
        std::span<const double> depth_span;
        if (depth_row) depth_span = *depth_row;
        const auto estimate = fitted_->predict(observed, now_price, ts, depth_span);
        latest_ = estimate ? std::optional<ForwardEstimate>(*estimate) : std::nullopt;
        live_status_ = !estimate ? QStringLiteral("Inference refused invalid output")
            : provisional ? QStringLiteral("Live projection — current tick + selected duration")
                          : QStringLiteral("Live projection — first bar of the session or after a gap: completed returns only");
    }
    void invalidate_live_epoch(const QString& reason) {
        // The bar in progress may have lost prints: it never becomes a return,
        // and the next bar starts a new chain. Completed returns are kept.
        window_.break_chain();
        if (pending_) tainted_end_ = pending_end_;
        depth_builder_.break_stream();
        last_tick_ = 0;
        pending_ = false;
        latest_.reset();
        live_ = false;
        missed_ = client_->missed_trades();
        live_status_ = reason;
        dirty_ = true;
    }
    void render() {
        QString text = live_status_ + "\n\n";
        if (latest_) {
            const QTimeZone ist(19800);
            const auto stamp = [&](std::int64_t ns) {
                return QDateTime::fromMSecsSinceEpoch(ns / 1'000'000LL, ist).toString("yyyy-MM-dd HH:mm:ss.zzz 'IST'");
            };
            const QString future_target = interval_->currentData().toString() == "1d"
                ? QStringLiteral("next trading-session close (date unresolved)")
                : stamp(latest_->target_ns);
            text += QString("Issued at       %1\nFuture target   %2\n\nCurrent observed price  %3\nPredicted future price %4\nPredicted move          %5 bps\nEmpirical residual band %6 — %7\n\n")
                .arg(stamp(latest_->issued_ns), future_target)
                .arg(latest_->current, 0, 'f', 2).arg(latest_->predicted, 0, 'f', 2)
                .arg(latest_->move_bps, 0, 'f', 3).arg(latest_->low, 0, 'f', 2).arg(latest_->high, 0, 'f', 2);
        }
        text += metrics_;
        if (!depth_note_.isEmpty()) text += depth_note_ + "\n";
        text += QString("\nLive trade updates evaluated: %1\n").arg(received_);
        text += "\nInputs: the last 20 same-session bar returns (a return only between bars one interval apart, so the overnight gap is never an input; the window may reach back up to 14 days).\n"
                "Targets are scaled by each row's own trailing volatility for fitting, then restored to basis points and price.\n"
                "No leakage: features end at the anchor bar, labels start there; splits are chronological, purged and embargoed; bin edges are fitted on the training rows only.\n"
                "The band is a held-out completed-bar 90th-percentile residual, not a guaranteed coverage rate.\n"
                "Intraday live inference uses the current unfinished bar and targets issue time + horizon; this tick-phase horizon has not been calibrated on historical tick snapshots.\n"
                "A horizon that would end after 15:30 gives no live forecast; labels never straddle the session break.\n"
                "Daily labels use the next available completed session; its target date is unresolved without an exchange calendar.\n"
                "A forecast is an estimate. Accuracy and trading usefulness require forward evaluation.\n";
        output_->setPlainText(text);
    }
    PriceClient* client_;
    ForecastStreamContinuity continuity_;
    QString root_, metrics_, live_status_, depth_note_;
    QComboBox *symbols_, *interval_, *horizon_;
    QCheckBox* depth_;
    QPushButton *train_, *fetch_, *stream_;
    QLabel* status_;
    QPlainTextEdit* output_;
    QPointer<QThread> worker_;
    std::optional<StreamForecastModel> fitted_;
    std::optional<ForwardEstimate> latest_;
    ReturnWindow window_;
    depth::DepthMinuteBuilder depth_builder_;
    std::vector<depth::DepthMinute> depth_done_;
    std::uint64_t generation_ = 0, missed_ = 0, received_ = 0;
    std::uint32_t token_ = 0, depth_token_ = 0;
    std::int64_t last_tick_ = 0, pending_end_ = 0, tainted_end_ = 0;
    double pending_price_ = 0.0;
    bool pending_ = false, dirty_ = false, live_ = false;
};
} // namespace altair::ui
