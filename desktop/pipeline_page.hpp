// desktop/pipeline_page.hpp -- who does what: every job and every live model,
// what it reads, what it writes, and whether it has done it today.
//
// Smit (2026-11-05): "there's a lot of confusion what is happening, what model
// is performing what and where he is providing data". This page answers that
// in two tables, both read from the files each step leaves behind:
//   THE DAY'S JOBS   login, recorder feed, chain snapshots, push, nightly, the
//                    desktop's own models engine and the order router, in the
//                    order they run, each with its input, its output and the
//                    file that proves it ran (green: fresh now, blue: done
//                    today, amber: not today, red: never).
//   THE MODELS       every model altair_live_engine runs: what it reads off
//                    the feed, what it trades (always paper unless the LIVE
//                    switch names it), where its trades and reasons go, and
//                    which page shows it.
// The status-bar pills (token, recorder, models) use the same checks.
//
// Reads file times only (never a file's contents: data/fyers_session.json
// holds the token). Nothing here starts, stops or trades.
#pragma once

#include "theme.hpp"

#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QShowEvent>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <functional>
#include <utility>

namespace altair::ui {

/// The trading day's date as IST sees it (the server's clock may be UTC).
[[nodiscard]] inline QDate ist_today(const QDateTime& now_utc) {
    return now_utc.toUTC().addSecs(19800).date();
}
[[nodiscard]] inline QDate ist_date(const QDateTime& t) { return t.toUTC().addSecs(19800).date(); }

/// What a step's evidence file says.
enum class StepHealth : int { Never = 0, NotToday = 1, Today = 2, Fresh = 3 };

[[nodiscard]] inline QString health_word(StepHealth h) {
    switch (h) {
    case StepHealth::Fresh:    return QStringLiteral("running now");
    case StepHealth::Today:    return QStringLiteral("ran today");
    case StepHealth::NotToday: return QStringLiteral("not today");
    case StepHealth::Never:
    default:                   return QStringLiteral("never ran here");
    }
}
[[nodiscard]] inline QString health_colour(StepHealth h) {
    switch (h) {
    case StepHealth::Fresh:    return QStringLiteral("#3FB950");
    case StepHealth::Today:    return QStringLiteral("#58A6FF");
    case StepHealth::NotToday: return QStringLiteral("#D29922");
    case StepHealth::Never:
    default:                   return QStringLiteral("#F85149");
    }
}

/// `path` relative to `root`, with {day} as today's IST date (YYYY-MM-DD).
/// Fresh when written within `fresh_s` seconds (0: never "fresh", only today).
struct Evidence { StepHealth health = StepHealth::Never; QDateTime when; };

[[nodiscard]] inline Evidence evidence_of(const QString& root, QString path, int fresh_s,
                                          const QDateTime& now_utc) {
    path.replace(QStringLiteral("{day}"), ist_today(now_utc).toString(Qt::ISODate));
    const QFileInfo fi(root + QLatin1Char('/') + path);
    Evidence e;
    if (!fi.exists()) {
        // A {day} directory that is not there yet still says whether the step
        // ever ran: look for its parent.
        const qsizetype cut = path.lastIndexOf(QLatin1Char('/'));
        if (path.contains(ist_today(now_utc).toString(Qt::ISODate)) && cut > 0
            && QFileInfo::exists(root + QLatin1Char('/') + path.left(cut)))
            e.health = StepHealth::NotToday;
        return e;
    }
    e.when = fi.lastModified();
    const qint64 age = e.when.secsTo(now_utc);
    if (fresh_s > 0 && age >= -60 && age <= fresh_s) e.health = StepHealth::Fresh;
    else if (ist_date(e.when) == ist_today(now_utc)) e.health = StepHealth::Today;
    else e.health = StepHealth::NotToday;
    return e;
}

/// One step of the day, in the order it runs.
struct PipelineStep {
    const char* step;      ///< what it is
    const char* who;       ///< the program and how it starts
    const char* when;      ///< IST, Monday to Friday
    const char* reads;
    const char* writes;
    const char* evidence;  ///< the file that proves it ran ({day} = today)
    int fresh_s;           ///< written within this many seconds = running now
};

inline constexpr std::array<PipelineStep, 10> kPipelineSteps{{
    {"1  FYERS login", "altair-fyers-login.timer → broker/tools/fyers_auto_login.py", "08:30, backstop 08:40",
     "FYERS API: TOTP + PIN from the server vault", "today's token: data/fyers_session.json (the feed picks it up with no restart)",
     "data/fyers_session.json", 0},
    {"2  Recorder feed", "altair-recorder.timer → ops/linux/record_session.py → altair_price_service --live",
     "08:50 to 15:45 (and 90 s after a boot)",
     "FYERS sockets: HSM ticks + quotes, 5-level book, TBT 50-level book (BANKNIFTY first); AF_XDP bypass, kernel fallback",
     "the loopback bus 127.0.0.1:7421 (every reader below) + data/live/feed_status.json",
     "data/live/feed_status.json", 30},
    {"3  Tape recorder", "altair_feed_recorder (started by step 2)", "08:50 to 15:45",
     "the bus 127.0.0.1:7421", "every tick and book: data/store/tapes/<day>/", "data/store/tapes/{day}", 120},
    {"4  Models engine", "altair_live_engine, started by the desktop (Terminal → Models) once the feed streams",
     "09:15 to 15:30, only while the desktop is open",
     "the bus 127.0.0.1:7421 + history (data/store/fyers, newest snapshot) + config/model_params",
     "data/live/engine_state.json, paper trades data/live/paper/trades.csv, decisions + journal",
     "data/live/engine_state.json", 30},
    {"5  Order router", "altair_order_router (only when LIVE is armed in the Terminal)", "while LIVE is on",
     "data/order_intents.jsonl from the engine (only strategies switched on in LIVE)",
     "FYERS orders (api.fyers.in) + data/live_orders/orders.json", "data/live_orders/orders.json", 30},
    {"6  Chain snapshots", "altair-chain-open / altair-chain-close → ops/linux/chain_snapshot.py", "09:20 and 15:27",
     "FYERS quotes for every NIFTY / BANKNIFTY strike", "data/store/chain_snap/<day>/<HHMM>.csv",
     "data/store/chain_snap/{day}", 0},
    {"7  FYERS chains", "altair-fyers-chain → ops/linux/fyers_chain.py", "15:45",
     "FYERS option chains", "data/store/fyers_chain/ (what option research reads)", "data/store/fyers_chain", 0},
    {"8  Data push", "altair-push.timer → ops/linux/nightly.py --quick", "15:47 (waits for the recorder)",
     "today's tapes, chains, paper and LIVE trades", "the private altair-data repo (/srv/altair-data)",
     "data/logs/altair-{day}.log", 0},
    {"9  Nightly", "altair-nightly.timer → ops/linux/nightly.py", "20:00",
     "tapes → 1-minute bars + IV (data/live/iv), FYERS history, dataset/",
     "a snapshot at one cutoff (data/store/runs/LATEST.txt), retrained models with the leak check, push again",
     "data/store/runs/LATEST.txt", 0},
    {"10 Daily findings", "the desktop: Models & Signals → Daily findings", "after the close",
     "the day's forecasts, trades and decisions", "data/logs/findings/<day>.md", "data/logs/findings/{day}.md", 0},
}};

/// One live model: what it reads, what it trades, where its trades go.
struct PipelineModel {
    const char* model;
    const char* reads;
    const char* trades;
    const char* writes;
    const char* page;
};

inline constexpr std::array<PipelineModel, 10> kPipelineModels{{
    {"Straddle IV>=RV NIFTY / BANKNIFTY", "ATM straddle bid/ask, spot, INDIA VIX off the bus; HAR realised vol from FYERS history",
     "sells the nearest-expiry ATM straddle when forecast IV (bid) >= forecast RV; buys a leg back 50 % above its sale",
     "data/live/paper/trades.csv + decisions (why it did or did not sell)", "Terminal → Models"},
    {"Strangle (band edge) NIFTY / BANKNIFTY", "the 80 % volatility band from history, option bids off the bus",
     "sells the band's edges at 09:20, buys back at 15:20 (stop2x variant: at double)",
     "data/live/paper/trades.csv", "Terminal → Models"},
    {"Volatility band", "spot and realised vol", "nothing: the reading the strangle sells on", "engine_state.json",
     "Terminal → Models"},
    {"OBI A-D BANKNIFTY / NIFTY", "every 5- or 50-level book and trade print of the future and ATM +/-5 strikes",
     "A bid pressure, B ask pressure, C absorption, D withdrawal: the near future, or sells a put / call one strike out",
     "data/live/paper/trades.csv (each entry and exit with its OBI numbers)", "Live Market → Order Book Imbalance"},
    {"OBI option quoter", "the future's OBI5", "rests a sale two strikes out and pulls the threatened leg",
     "data/live/paper/trades.csv", "Live Market → Order Book Imbalance"},
    {"Diffusion BANKNIFTY / NIFTY", "who moves first: future, index, bank stocks, ATM +/-2 options (lead-lag every tick)",
     "sells the option still above the price the future implies, or the future behind its stocks",
     "data/live/diffusion/trades.csv + diffusion_<day>.csv every 15 min", "Terminal → Models"},
    {"Direction models", "1-minute bars and the forecast curriculum's trained models",
     "the near future at 10:15 when a forecast clears its cost", "data/live/paper/trades.csv", "Terminal → Models"},
    {"Pairs / Stat-arb", "BANKNIFTY vs NIFTY spread; NIFTY 50 residuals", "futures at 15:15, carried overnight",
     "data/live/paper/trades.csv", "Strategies & Tests → Pair Trading"},
    {"Arbitrage / OHL (threshold)", "NSE vs BSE quotes; every near future at 09:15", "can send REAL orders only with its own switch on and LIVE armed",
     "data/live/paper/trades.csv, LIVE: data/live_orders/strategy_trades.csv", "Strategies & Tests → Arbitrage / Threshold"},
    {"Desk Trader", "every model above + altair_trader's call + the vol premium's reading",
     "one consensus call at 10:30, paper only", "data/live/paper/trades.csv", "Terminal → Models"},
}};

/// The three status-bar questions: is today's token there, is the recorder
/// recording, are the models running.
struct PipelinePills {
    Evidence token, feed, engine;
};

[[nodiscard]] inline PipelinePills pipeline_pills(const QString& root, const QDateTime& now_utc) {
    return {evidence_of(root, QStringLiteral("data/fyers_session.json"), 0, now_utc),
            evidence_of(root, QStringLiteral("data/live/feed_status.json"), 30, now_utc),
            evidence_of(root, QStringLiteral("data/live/engine_state.json"), 30, now_utc)};
}

/// The token pill's text: the token is good for one trading day.
[[nodiscard]] inline QString token_pill_text(const Evidence& e) {
    if (e.health == StepHealth::Never) return QStringLiteral("● No token");
    if (e.health == StepHealth::NotToday)
        return QStringLiteral("● Token old (%1)").arg(e.when.toString(QStringLiteral("dd MMM")));
    return QStringLiteral("● Token %1").arg(e.when.toUTC().addSecs(19800).toString(QStringLiteral("HH:mm")));
}
[[nodiscard]] inline QString token_pill_colour(const Evidence& e) {
    return e.health >= StepHealth::Today ? health_colour(StepHealth::Fresh)
         : e.health == StepHealth::NotToday ? health_colour(StepHealth::NotToday) : health_colour(StepHealth::Never);
}

/// The recorder pill: systemd's word for the service plus the feed's file.
[[nodiscard]] inline QString recorder_pill_text(const QString& systemd_state, const Evidence& feed) {
    const bool streaming = feed.health == StepHealth::Fresh;
    if (systemd_state == QLatin1String("active"))
        return streaming ? QStringLiteral("● Recording") : QStringLiteral("● Recorder: feed quiet");
    if (streaming) return QStringLiteral("● Recorder off");
    if (systemd_state.isEmpty()) return QStringLiteral("● Recorder ?");
    return QStringLiteral("● Recorder %1").arg(systemd_state == QLatin1String("inactive") ? QStringLiteral("idle") : systemd_state);
}
[[nodiscard]] inline QString recorder_pill_colour(const QString& systemd_state, const Evidence& feed) {
    const bool streaming = feed.health == StepHealth::Fresh;
    if (systemd_state == QLatin1String("active")) return streaming ? health_colour(StepHealth::Fresh) : health_colour(StepHealth::NotToday);
    if (systemd_state == QLatin1String("failed")) return health_colour(StepHealth::Never);
    return streaming ? health_colour(StepHealth::Today) : QStringLiteral("#8B949E");
}

[[nodiscard]] inline QString engine_pill_text(const Evidence& e) {
    return e.health == StepHealth::Fresh ? QStringLiteral("● Models on")
                                         : QStringLiteral("● Models off");
}

class PipelinePage final : public QWidget {
public:
    std::function<void(const QString&)> open_page;

    explicit PipelinePage(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("pipelinePage"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(16, 14, 16, 12);
        v->setSpacing(8);
        auto* title = new QLabel(QStringLiteral("Who does what"), this);
        set_role(title, QStringLiteral("title"));
        v->addWidget(title);
        auto* note = new QLabel(QStringLiteral(
            "Every step of the trading day in the order it runs, then every model the engine runs: what each one reads, "
            "what it writes and whether it has done it today. FYERS is the only source of data and the only broker. "
            "The recorder runs on the server whether or not this window is open; the models run only while this "
            "desktop is open (Terminal → Models). Every model trades paper unless the LIVE switch names it."), this);
        note->setWordWrap(true);
        set_role(note, QStringLiteral("note"));
        v->addWidget(note);
        legend_ = new QLabel(this);
        legend_->setTextFormat(Qt::RichText);
        v->addWidget(legend_);

        auto* split = new QSplitter(Qt::Vertical, this);
        steps_ = make_table(split, {QStringLiteral("Step"), QStringLiteral("Status"), QStringLiteral("When (IST)"),
                                    QStringLiteral("Reads"), QStringLiteral("Writes"), QStringLiteral("Program")},
                            static_cast<int>(kPipelineSteps.size()));
        steps_->setObjectName(QStringLiteral("pipelineSteps"));
        for (int r = 0; r < static_cast<int>(kPipelineSteps.size()); ++r) {
            const auto& s = kPipelineSteps[static_cast<std::size_t>(r)];
            put(steps_, r, 0, s.step, true);
            put(steps_, r, 1, "");
            put(steps_, r, 2, s.when);
            put(steps_, r, 3, s.reads);
            put(steps_, r, 4, s.writes);
            put(steps_, r, 5, s.who);
            steps_->item(r, 1)->setToolTip(QString::fromUtf8(s.evidence));
        }
        models_ = make_table(split, {QStringLiteral("Model"), QStringLiteral("Reads"), QStringLiteral("Trades (paper)"),
                                     QStringLiteral("Trades and reasons go to"), QStringLiteral("See it on")},
                             static_cast<int>(kPipelineModels.size()));
        models_->setObjectName(QStringLiteral("pipelineModels"));
        for (int r = 0; r < static_cast<int>(kPipelineModels.size()); ++r) {
            const auto& m = kPipelineModels[static_cast<std::size_t>(r)];
            put(models_, r, 0, m.model, true);
            put(models_, r, 1, m.reads);
            put(models_, r, 2, m.trades);
            put(models_, r, 3, m.writes);
            put(models_, r, 4, m.page);
        }
        for (const auto& [t, widths] : {std::pair{steps_, std::array<int, 5>{150, 150, 170, 330, 330}},
                                        std::pair{models_, std::array<int, 5>{210, 300, 310, 280, 200}}})
            for (int c = 0; c < 5; ++c) t->setColumnWidth(c, widths[static_cast<std::size_t>(c)]);
        steps_->resizeRowsToContents();
        models_->resizeRowsToContents();
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        v->addWidget(split, 1);

        timer_ = new QTimer(this);
        timer_->setInterval(5000);
        connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
        refresh();
    }

    void refresh() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        int counts[4] = {0, 0, 0, 0};
        for (int r = 0; r < static_cast<int>(kPipelineSteps.size()); ++r) {
            const auto& s = kPipelineSteps[static_cast<std::size_t>(r)];
            const Evidence e = evidence_of(root_, QString::fromUtf8(s.evidence), s.fresh_s, now);
            ++counts[static_cast<int>(e.health)];
            auto* item = steps_->item(r, 1);
            QString text = health_word(e.health);
            if (e.when.isValid()) text += QStringLiteral("  ") + e.when.toUTC().addSecs(19800).toString(
                e.health >= StepHealth::Today ? QStringLiteral("HH:mm") : QStringLiteral("dd MMM HH:mm"));
            item->setText(text);
            item->setForeground(QColor(health_colour(e.health)));
        }
        legend_->setText(QStringLiteral(
            "<span style='color:%1'>● running now %2</span> &nbsp; <span style='color:%3'>● ran today %4</span> &nbsp; "
            "<span style='color:%5'>● not today %6</span> &nbsp; <span style='color:%7'>● never ran here %8</span>"
            " &nbsp; <span style='color:#8B949E'>(from each step's own file; hover a status for the file)</span>")
            .arg(health_colour(StepHealth::Fresh)).arg(counts[3]).arg(health_colour(StepHealth::Today)).arg(counts[2])
            .arg(health_colour(StepHealth::NotToday)).arg(counts[1]).arg(health_colour(StepHealth::Never)).arg(counts[0]));
    }

protected:
    void showEvent(QShowEvent* e) override { refresh(); timer_->start(); QWidget::showEvent(e); }
    void hideEvent(QHideEvent* e) override { timer_->stop(); QWidget::hideEvent(e); }

private:
    static QTableWidget* make_table(QWidget* parent, const QStringList& heads, int rows) {
        auto* t = new QTableWidget(rows, static_cast<int>(heads.size()), parent);
        t->setHorizontalHeaderLabels(heads);
        t->verticalHeader()->hide();
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setWordWrap(true);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        t->horizontalHeader()->setStretchLastSection(true);
        return t;
    }
    static void put(QTableWidget* t, int r, int c, const char* text, bool bold = false) {
        auto* item = new QTableWidgetItem(QString::fromUtf8(text));
        item->setToolTip(QString::fromUtf8(text));
        if (bold) { QFont f = item->font(); f.setBold(true); item->setFont(f); }
        t->setItem(r, c, item);
    }

    QString root_;
    QLabel* legend_ = nullptr;
    QTableWidget* steps_ = nullptr;
    QTableWidget* models_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace altair::ui
