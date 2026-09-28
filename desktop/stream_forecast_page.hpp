// Forecast training runs off the GUI thread; admitted trade updates use a cached model.
#pragma once
#include "live_forecast.hpp"
#include "price_client.hpp"
#include <models/stream_forecast.hpp>
#include <QMap>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>
#include <optional>

namespace altair::ui {
struct ForecastTrainingResult {
    std::optional<StreamForecastModel> fitted;
    std::array<double, 21> tail{};
    QString error;
    QString provider;
    std::size_t loaded = 0, excluded = 0, older = 0;
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

/// Cold worker input: copied strings, no widgets. Bar ends are known at fit time.
inline ForecastTrainingResult train_forecast_page(const QString& root, const QString& symbol,
                                                  const QString& interval, bool fetch) {
    constexpr int kTrainingBars = 12000;
    ForecastTrainingResult result;
    const auto width = interval_ns_for(interval);
    const auto now = QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const QTimeZone ist(19800);
        QMap<std::int64_t, double> bars;
        std::size_t valid_rows = 0;
    const QDir directory(root + QStringLiteral("/spot/%1/%2").arg(symbol, interval));
    QStringList files = directory.exists("all.csv") ? QStringList{"all.csv"}
        : directory.entryList({"*.csv"}, QDir::Files, QDir::Name);
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
            const auto end = width >= 86'400'000'000'000LL
                ? QDateTime(stamp.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL
                : stamp.toMSecsSinceEpoch() * 1'000'000LL + width;
            const auto close = QDateTime(stamp.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL;
            if (end > now || (width < 86'400'000'000'000LL && (stamp.time() < QTime(9, 15) || end > close))) {
                ++result.excluded; continue;
            }
            if (end < prior_end) {
                result.error = QStringLiteral("Timestamps go backwards within %1").arg(name); return result;
            }
            prior_end = end;
            ++valid_rows;
            if (bars.contains(end) && bars.value(end) != price) {
                result.error = QStringLiteral("Conflicting duplicate timestamp in %1").arg(name); return result;
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
        const auto fresh = fetch_recent(symbol, quant_symbol_by_dir(symbol).kite_token, interval,
                                        interval == "1d" ? 120 : 20);
        if (!fresh.ran) { result.error = QStringLiteral("Fetch failed: %1").arg(fresh.error); return result; }
        result.provider = fresh.provider;
        for (std::size_t i = 0; i < fresh.bars.closes.size(); ++i) {
            const auto start = fresh.bars.stamps_ns[i];
            const auto date = QDateTime::fromMSecsSinceEpoch(start / 1'000'000LL, ist);
            const auto session_end = QDateTime(date.date(), QTime(15, 30), ist).toMSecsSinceEpoch() * 1'000'000LL;
            const auto end = interval == "1d" ? session_end : start + width;
            if (end > now || (interval != "1d" && (date.time() < QTime(9, 15) || end > session_end))) {
                ++result.excluded; continue;
            }
            if (bars.contains(end) && bars.value(end) != fresh.bars.closes[i]) {
                result.error = QStringLiteral("Fetched bar conflicts with stored history"); return result;
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
    auto fitted = fit_stream_forecast(prices, ends, width);
    if (!fitted) { result.error = QStringLiteral("Fit refused: need 600 valid bars and 500 usable target windows (error %1)").arg(static_cast<int>(fitted.error())); return result; }
    result.fitted = std::move(*fitted);
    std::copy(prices.end() - 21, prices.end(), result.tail.begin());
    return result;
}

class StreamForecastPage final : public QWidget {
public:
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
        train_ = new QPushButton("Train from history", this);
        fetch_ = new QPushButton("Fetch recent + train", this);
        stream_ = new QPushButton("Connect price stream", this);
        controls->addWidget(symbols_); controls->addWidget(interval_);
        controls->addWidget(train_); controls->addWidget(fetch_); controls->addWidget(stream_);
        layout->addLayout(controls);
        status_ = new QLabel("Select a timeframe and train. Future targets and current price are shown separately.", this);
        status_->setWordWrap(true); layout->addWidget(status_);
        output_ = new QPlainTextEdit(this); output_->setReadOnly(true);
        output_->setStyleSheet("QPlainTextEdit{background:#11171C;color:#D6DBDF;font-family:Consolas;font-size:13px;border:0;}");
        layout->addWidget(output_, 1);
        connect(train_, &QPushButton::clicked, this, [this] { begin_training(false); });
        connect(fetch_, &QPushButton::clicked, this, [this] { begin_training(true); });
        connect(stream_, &QPushButton::clicked, this, [this] {
            if (client_->connected()) client_->stop();
            else client_->start();
        });
        const auto invalidate = [this] {
            ++generation_; fitted_.reset(); latest_.reset(); pending_ = false; live_ = false;
            continuity_.reset_warmup();
            if (worker_) worker_->requestInterruption();
            output_->clear(); status_->setText("Selection changed. Train this instrument/timeframe.");
        };
        connect(symbols_, &QComboBox::currentIndexChanged, this, invalidate);
        connect(interval_, &QComboBox::currentIndexChanged, this, invalidate);
        connect(client_, &PriceClient::tradeUpdated, this, [this](unsigned token) { on_trade(token); });
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
    }
private:
    void begin_training(bool fetch) {
        if (worker_) return;
        const auto generation = ++generation_;
        fitted_.reset(); latest_.reset(); output_->clear(); pending_ = false; live_ = false;
        const QString symbol = symbols_->currentData().toString(), interval = interval_->currentData().toString();
        token_ = quant_symbol_by_dir(symbol).kite_token;
        train_->setEnabled(false); fetch_->setEnabled(false);
        status_->setText("Training in background: chronological fit / selection / calibration. Navigation remains available.");
        auto result = std::make_shared<ForecastTrainingResult>();
        worker_ = QThread::create([result, root = root_, symbol, interval, fetch] {
            *result = train_forecast_page(root, symbol, interval, fetch);
        });
        auto* thread = worker_.data();
        connect(thread, &QThread::finished, this,
                [this, result, generation, daily = interval == "1d"] {
            worker_ = nullptr; train_->setEnabled(true); fetch_->setEnabled(true);
            if (generation != generation_) return;
            if (!result->fitted) { status_->setText(result->error); return; }
            fitted_ = std::move(result->fitted); completed_ = result->tail;
            end_ = fitted_->observed_through_ns; last_tick_ = 0;
            continuity_.reset_warmup();
            missed_ = client_->missed_trades(); received_ = 0;
            metrics_ = QString("Fit %1 | validation %2 | calibration %3 bars | depth %4\n"
                               "Calibration RMSE: model %5 bps / random walk %6 bps\n"
                               "Loaded %7 bars; older bars outside 12,000-bar window %8; excluded partial/session bars %9; skipped target gaps %10\n")
                .arg(fitted_->training_rows).arg(fitted_->validation_rows).arg(fitted_->scored_rows).arg(fitted_->chosen_depth)
                .arg(fitted_->rmse_bps, 0, 'f', 3).arg(fitted_->naive_rmse_bps, 0, 'f', 3)
                .arg(result->loaded).arg(result->older).arg(result->excluded).arg(fitted_->skipped_windows);
            const auto estimate = fitted_->predict(completed_, end_);
            if (estimate) latest_ = *estimate;
            live_status_ = "Historical forecast from last completed bar — waiting for live trades";
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
    void on_trade(unsigned token) {
        if (!fitted_ || token != token_) return;
        const auto* price = client_->price(token);
        if (!price) return;
        const auto now = QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const auto ts = price->exchange_ts_ns, width = fitted_->interval_ns;
        if (width >= 86'400'000'000'000LL) {
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
        if (ts < open || ts + width > close) {
            latest_.reset(); live_status_ = "No intraday target inside this session. Daily mode uses historical completed bars."; dirty_ = true; return;
        }
        const auto bucket_end = open + ((ts - open) / width + 1) * width;
        if (continuity_.consume_reanchor()) {
            missed_ = client_->missed_trades();
            reset_live_warmup(bucket_end, static_cast<double>(price->last_paise) / 100.0,
                              "Stream epoch changed; collecting 20 fresh completed intervals");
            last_tick_ = ts;
            return;
        }
        if (client_->missed_trades() != missed_) {
            missed_ = client_->missed_trades();
            reset_live_warmup(bucket_end, static_cast<double>(price->last_paise) / 100.0,
                              "Trade stream gap; collecting 20 consecutive fresh bars");
            last_tick_ = ts;
            return;
        }
        if (pending_ && bucket_end != pending_end_) {
            if (bucket_end != pending_end_ + width) {
                reset_live_warmup(bucket_end, static_cast<double>(price->last_paise) / 100.0,
                                  "Missing interval/session rollover; collecting 20 consecutive fresh bars");
                last_tick_ = ts;
                return;
            }
            std::move(completed_.begin() + 1, completed_.end(), completed_.begin());
            completed_.back() = pending_price_; end_ = pending_end_; pending_ = false;
            continuity_.completed_interval();
        }
        if (bucket_end != end_ + width) {
            reset_live_warmup(bucket_end, static_cast<double>(price->last_paise) / 100.0,
                              "History is behind this session; collecting 20 consecutive fresh bars");
            last_tick_ = ts;
            return;
        }
        pending_ = true; pending_end_ = bucket_end;
        pending_price_ = static_cast<double>(price->last_paise) / 100.0;
        std::array<double, 21> observed{};
        std::copy(completed_.begin() + 1, completed_.end(), observed.begin());
        observed.back() = pending_price_;
        last_tick_ = ts;
        if (!continuity_.ready()) {
            latest_.reset(); ++received_; live_ = true; dirty_ = true;
            live_status_ = QString("Warming live features: %1 / 20 completed intervals")
                .arg(continuity_.fresh_intervals());
            return;
        }
        const auto estimate = fitted_->predict(observed, ts);
        latest_ = estimate ? std::optional<ForwardEstimate>(*estimate) : std::nullopt;
        ++received_; live_ = true; dirty_ = true;
        live_status_ = estimate ? "Live projection — current tick + selected duration" : "Inference refused invalid output";
    }
    void reset_live_warmup(std::int64_t bucket_end, double price, const QString& reason) {
        completed_.fill(price);
        end_ = bucket_end - fitted_->interval_ns;
        pending_end_ = bucket_end;
        pending_price_ = price;
        pending_ = true;
        continuity_.reset_warmup();
        latest_.reset(); live_ = true; dirty_ = true;
        live_status_ = reason;
    }
    void invalidate_live_epoch(const QString& reason) {
        // The next valid tick re-seeds this window. Do not count the pending
        // in-progress bar or any completed prices retained before disconnect.
        completed_.fill(0.0);
        end_ = 0;
        pending_end_ = 0;
        pending_price_ = 0.0;
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
        text += metrics_ + QString("\nLive trade updates evaluated: %1\n").arg(received_);
        text += "\nTargets are scaled by trailing volatility for fitting, then restored to basis points and price; input features remain raw return bps.\n"
                "The band is a held-out completed-bar 90th-percentile residual, not a guaranteed coverage rate.\n"
                "Intraday live inference uses the current unfinished bar and targets issue time + interval; this tick-phase horizon has not been calibrated on historical tick snapshots.\n"
                "Daily labels use the next available completed session; its target date is unresolved without an exchange calendar.\n"
                "A forecast is an estimate. Accuracy and trading usefulness require forward evaluation.\n";
        output_->setPlainText(text);
    }
    PriceClient* client_;
    ForecastStreamContinuity continuity_;
    QString root_, metrics_, live_status_;
    QComboBox *symbols_, *interval_;
    QPushButton *train_, *fetch_, *stream_;
    QLabel* status_;
    QPlainTextEdit* output_;
    QPointer<QThread> worker_;
    std::optional<StreamForecastModel> fitted_;
    std::optional<ForwardEstimate> latest_;
    std::array<double, 21> completed_{};
    std::uint64_t generation_ = 0, missed_ = 0, received_ = 0;
    std::uint32_t token_ = 0;
    std::int64_t end_ = 0, last_tick_ = 0, pending_end_ = 0;
    double pending_price_ = 0.0;
    bool pending_ = false, dirty_ = false, live_ = false;
};
} // namespace altair::ui
