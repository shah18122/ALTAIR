// desktop/live_models.hpp -- the live models, and what they are doing with paper money.
//
// altair_live_engine runs every live model on the price service's stream and
// paper-trades their signals (app/live_engine_main.cpp). This panel is its
// window, inside the Terminal so it shares the Terminal's price stream:
//   * every model: its state (watching, in position, done today, abstaining),
//     its current reading, and -- always -- WHY it is or is not acting, from
//     data/live/engine_state.json, refreshed every second;
//   * open positions, MARKED TICK BY TICK from the stream: a long at the bid,
//     a short at the ask -- what closing it now would fetch;
//   * every round trip from data/live/paper/trades.csv with the model that
//     took it, gross, expenses, net and why it was opened and closed;
//   * P&L by model: today and all days, realised and open.
// The engine is a separate process this panel can start and stop. Expenses
// are refused while config/charges.toml is unverified, as everywhere in the
// project; "Price UNVERIFIED expenses" passes --unverified-costs and every
// figure is then marked UNVERIFIED.
//
// IT CANNOT TRADE. The engine places no orders and this panel links no broker.

#pragma once

#include "live_market.hpp"
#include "price_client.hpp"
#include "theme.hpp"

#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <vector>

namespace altair::ui {

/// One row of data/live/paper/trades.csv.
struct LivePaperRow {
    QString date, model, symbol, side, entry_time, exit_time, why_in, why_out, source, costs;
    quint32 token = 0;
    qint64 qty = 0;
    double entry = 0, exit = 0, gross = 0;
    double expenses = std::numeric_limits<double>::quiet_NaN(), net = std::numeric_limits<double>::quiet_NaN();
};

namespace live_models_detail {

/// A CSV line into fields; double quotes protect commas.
[[nodiscard]] inline QStringList split(const QString& line) {
    QStringList out;
    QString cur;
    bool quoted = false;
    for (const QChar c : line) {
        if (c == QLatin1Char('"')) { quoted = !quoted; continue; }
        if (c == QLatin1Char(',') && !quoted) { out << cur; cur.clear(); continue; }
        cur.append(c);
    }
    out << cur;
    return out;
}

[[nodiscard]] inline double num_or_nan(const QString& s) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] inline QString rupees(double v) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    // U+2060 WORD JOINER keeps the minus on the number when a cell wraps.
    return (v < 0 ? QStringLiteral("−⁠") : QString()) + QLocale(QLocale::English, QLocale::India).toString(std::fabs(v), 'f', 0);
}

[[nodiscard]] inline QColor pnl_color(double v) {
    if (!std::isfinite(v) || v == 0.0) return QColor(0xD0, 0xD6, 0xDE);
    return v > 0 ? QColor(0x7F, 0xD1, 0x7F) : QColor(0xF0, 0x7A, 0x6A);
}

} // namespace live_models_detail

[[nodiscard]] inline std::vector<LivePaperRow> load_live_trades(const QString& path) {
    std::vector<LivePaperRow> rows;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return rows;
    QTextStream in(&f);
    const QStringList head = live_models_detail::split(in.readLine());
    const auto col = [&head](const char* n) { return static_cast<int>(head.indexOf(QString::fromLatin1(n))); };
    const int c_date = col("date"), c_model = col("model"), c_sym = col("symbol"), c_tok = col("token"), c_side = col("side"),
              c_qty = col("qty"), c_et = col("entry_time"), c_e = col("entry"), c_xt = col("exit_time"), c_x = col("exit"),
              c_g = col("gross"), c_exp = col("expenses"), c_net = col("net"), c_wi = col("why_in"), c_wo = col("why_out"),
              c_src = col("source"), c_costs = col("costs");
    if (c_model < 0 || c_g < 0) return rows;
    while (!in.atEnd()) {
        const QStringList c = live_models_detail::split(in.readLine());
        if (c.size() < head.size()) continue;
        LivePaperRow r;
        r.date = c.value(c_date); r.model = c.value(c_model); r.symbol = c.value(c_sym); r.token = c.value(c_tok).toUInt();
        r.side = c.value(c_side); r.qty = c.value(c_qty).toLongLong(); r.entry_time = c.value(c_et);
        r.entry = c.value(c_e).toDouble(); r.exit_time = c.value(c_xt); r.exit = c.value(c_x).toDouble();
        r.gross = c.value(c_g).toDouble(); r.expenses = live_models_detail::num_or_nan(c.value(c_exp));
        r.net = live_models_detail::num_or_nan(c.value(c_net)); r.why_in = c.value(c_wi); r.why_out = c.value(c_wo);
        r.source = c.value(c_src); r.costs = c.value(c_costs);
        rows.push_back(r);
    }
    return rows;
}

class LiveModelsPanel final : public QWidget {
public:
    LiveModelsPanel(const PriceClient* client, QString root = {}, QWidget* parent = nullptr)
        : QWidget(parent), client_(client), root_(std::move(root)) {
#ifdef ALTAIR_SOURCE_DIR
        if (root_.isEmpty()) root_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        setObjectName(QStringLiteral("liveModels"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(6, 6, 6, 6);
        v->setSpacing(6);

        auto* bar = new QHBoxLayout;
        start_ = new QPushButton(QStringLiteral("Start models"), this);
        start_->setToolTip(QStringLiteral(
            "Run altair_live_engine: every live model on the price service's stream, paper-trading its signals. "
            "Start the feed first (Market Watch → Start live feed, or Start SIM). Places no orders."));
        stop_ = new QPushButton(QStringLiteral("Stop models"), this);
        stop_->setEnabled(false);
        unverified_ = new QCheckBox(QStringLiteral("Price UNVERIFIED expenses"), this);
        unverified_->setToolTip(QStringLiteral(
            "Pass --unverified-costs: price expenses from config/charges.toml although it is unverified. "
            "Every expense and net figure is then marked UNVERIFIED."));
        auto_demo_ = new QCheckBox(QStringLiteral("Demo trade automatically"), this);
        auto_demo_->setObjectName(QStringLiteral("autoDemo"));
        auto_demo_->setToolTip(QStringLiteral(
            "Start the models by themselves whenever the feed is streaming: every model, the cross-exchange "
            "arbitrage included, paper-trades its signals. Nothing reaches a broker. Stop models turns it off "
            "until Start models."));
        bar->addWidget(start_);
        bar->addWidget(stop_);
        bar->addWidget(auto_demo_);
        bar->addWidget(unverified_);
        bar->addStretch();
        v->addLayout(bar);

        status_ = new QLabel(this);
        status_->setTextFormat(Qt::RichText);
        status_->setWordWrap(true);
        v->addWidget(status_);

        totals_ = new QLabel(this);
        totals_->setTextFormat(Qt::RichText);
        totals_->setWordWrap(true);
        v->addWidget(totals_);

        tabs_ = new QTabWidget(this);
        models_ = make_live_table(5, {QStringLiteral("Model"), QStringLiteral("Family"), QStringLiteral("State"),
                                      QStringLiteral("Signal"), QStringLiteral("Why")}, this);
        models_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        models_->horizontalHeader()->setStretchLastSection(true);
        models_->setColumnWidth(0, 215);
        models_->setColumnWidth(1, 120);
        models_->setColumnWidth(2, 100);
        models_->setColumnWidth(3, 190);
        models_->setWordWrap(true);
        models_->setSelectionMode(QAbstractItemView::SingleSelection);
        models_->setSelectionBehavior(QAbstractItemView::SelectRows);
        fields_ = make_live_table(2, {QStringLiteral("Reading"), QStringLiteral("Value")}, this);
        auto* split = new QSplitter(Qt::Horizontal, this);
        split->addWidget(models_);
        split->addWidget(fields_);
        split->setStretchFactor(0, 78);
        split->setStretchFactor(1, 22);

        positions_ = make_live_table(10, {QStringLiteral("Model"), QStringLiteral("Symbol"), QStringLiteral("Side"),
                                          QStringLiteral("Qty"), QStringLiteral("Entry"), QStringLiteral("Mark"),
                                          QStringLiteral("Open P&L ₹"), QStringLiteral("Entry exp ₹"),
                                          QStringLiteral("Held"), QStringLiteral("Why in")}, tabs_);
        positions_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        positions_->horizontalHeader()->setStretchLastSection(true);
        positions_->setColumnWidth(0, 200);
        positions_->setColumnWidth(1, 190);
        tabs_->addTab(positions_, QStringLiteral("Open positions"));

        trades_ = make_live_table(14, {QStringLiteral("Date"), QStringLiteral("Model"), QStringLiteral("Symbol"),
                                       QStringLiteral("Side"), QStringLiteral("Qty"), QStringLiteral("In"),
                                       QStringLiteral("Entry"), QStringLiteral("Out"), QStringLiteral("Exit"),
                                       QStringLiteral("Gross ₹"), QStringLiteral("Expenses ₹"), QStringLiteral("Net ₹"),
                                       QStringLiteral("Why out"), QStringLiteral("Costs")}, tabs_);
        trades_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        trades_->setColumnWidth(1, 200);
        trades_->setColumnWidth(2, 190);
        tabs_->addTab(trades_, QStringLiteral("Paper trades"));

        by_model_ = make_live_table(7, {QStringLiteral("Model"), QStringLiteral("Trades"), QStringLiteral("Gross ₹"),
                                        QStringLiteral("Expenses ₹"), QStringLiteral("Net ₹"), QStringLiteral("Open P&L ₹"),
                                        QStringLiteral("Net today ₹")}, tabs_);
        by_model_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        by_model_->horizontalHeader()->setStretchLastSection(true);
        by_model_->setColumnWidth(0, 230);
        tabs_->addTab(by_model_, QStringLiteral("P&&L by model"));
        // Models above, money below: both always on screen.
        auto* stack = new QSplitter(Qt::Vertical, this);
        stack->addWidget(split);
        stack->addWidget(tabs_);
        stack->setStretchFactor(0, 55);
        stack->setStretchFactor(1, 45);
        v->addWidget(stack, 1);

        connect(start_, &QPushButton::clicked, this, [this] { user_stopped_ = false; start_engine(); });
        connect(stop_, &QPushButton::clicked, this, [this] { user_stopped_ = true; stop_engine(); });
        connect(models_, &QTableWidget::currentCellChanged, this, [this](int r, int, int, int) { show_fields(r); });
        file_timer_.setInterval(1000);
        connect(&file_timer_, &QTimer::timeout, this, [this] {
            reload();
            // Demo trading by default: the models start once the feed streams.
            if (auto_demo_->isChecked() && engine_ == nullptr && !user_stopped_ && client_ != nullptr && client_->connected())
                start_engine();
        });
        file_timer_.start();
        mark_timer_.setInterval(250);
        connect(&mark_timer_, &QTimer::timeout, this, [this] { if (isVisible()) mark(); });
        mark_timer_.start();
        reload();
    }
    ~LiveModelsPanel() override { stop_engine(); }
    LiveModelsPanel(const LiveModelsPanel&) = delete;
    LiveModelsPanel& operator=(const LiveModelsPanel&) = delete;

    /// For tests.
    void reload() {
        read_state();
        read_trades();
        mark();
    }
    [[nodiscard]] QTableWidget* models_table() const noexcept { return models_; }
    /// "Price UNVERIFIED expenses": the Terminal's paper orders follow it too.
    [[nodiscard]] bool price_unverified() const { return unverified_->isChecked(); }
    /// Demo trading by default (the app turns it on; tests leave it off).
    void set_auto_demo(bool on) { auto_demo_->setChecked(on); }
    [[nodiscard]] bool auto_demo() const { return auto_demo_->isChecked(); }
    [[nodiscard]] bool engine_running() const noexcept { return engine_ != nullptr; }
    [[nodiscard]] QTableWidget* positions_table() const noexcept { return positions_; }
    [[nodiscard]] QTableWidget* trades_table() const noexcept { return trades_; }
    [[nodiscard]] QTableWidget* by_model_table() const noexcept { return by_model_; }
    [[nodiscard]] QString status_text() const { return status_->text(); }

private:
    struct Pos {
        QString model, symbol, why;
        quint32 token = 0;
        int side = 0;
        qint64 qty = 0, want = 0;
        double entry = 0, entry_expenses = std::numeric_limits<double>::quiet_NaN();
        bool carry = false;
        QString state, exit_reason;   ///< opening / open / closing, and why it is closing
    };

    void read_state() {
        QFile f(root_ + QStringLiteral("/data/live/engine_state.json"));
        const QString muted = QString::fromLatin1(theme_token::kTextMuted);
        if (!f.open(QIODevice::ReadOnly)) {
            status_->setText(QStringLiteral("<span style='color:%1'>The models are not running. Start a feed on the Watch "
                                            "view, then Start models.</span>").arg(muted));
            models_->setRowCount(0);
            positions_cache_.clear();
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        const qint64 age = QDateTime::currentSecsSinceEpoch() - QFileInfo(f).lastModified().toSecsSinceEpoch();
        const bool sim = o.value(QStringLiteral("source")).toString() == QLatin1String("SIM");
        const bool stale = o.value(QStringLiteral("stale")).toBool();
        const qint64 eng = static_cast<qint64>(o.value(QStringLiteral("engine_ns")).toDouble());
        const QString tag = sim ? QStringLiteral("<span style='color:#F4C95D'>● SIM — paper trades on a simulated market</span>")
                                : QStringLiteral("<span style='color:#7FD17F'>● LIVE market data — paper trades only</span>");
        // Why entries are refused right now, from the engine's one risk check.
        const QString halt = o.value(QStringLiteral("halt")).toString();
        const QString paused = o.value(QStringLiteral("paused")).toString();
        QString gate;
        if (!halt.isEmpty()) gate += QStringLiteral(" · <b style='color:#F07A6A'>HALTED: %1</b>").arg(halt.toHtmlEscaped());
        if (!paused.isEmpty()) gate += QStringLiteral(" · <b style='color:#F4C95D'>%1</b>").arg(paused.toHtmlEscaped());
        const int working = o.value(QStringLiteral("working_orders")).toInt();
        if (working > 0) gate += QStringLiteral(" · %1 order(s) working").arg(working);
        // The capital the book ties up (an estimate, not SPAN) and how fast the engine keeps up.
        const QJsonObject mg = o.value(QStringLiteral("margin")).toObject();
        if (!mg.isEmpty()) {
            const auto r = [](const QJsonValue& v) {
                return v.isDouble() ? live_models_detail::rupees(v.toDouble()) : QStringLiteral("unknown");
            };
            gate += QStringLiteral(" · margin est. %1 of %2 (peak today %3; not SPAN)")
                        .arg(r(mg.value(QStringLiteral("estimate"))), r(mg.value(QStringLiteral("limit"))),
                             r(mg.value(QStringLiteral("peak_today"))));
        }
        const QJsonObject lat = o.value(QStringLiteral("latency")).toObject();
        if (!lat.isEmpty()) {
            const double fp = lat.value(QStringLiteral("frame_us")).toObject().value(QStringLiteral("p99")).toDouble(-1.0);
            const double dp = lat.value(QStringLiteral("decision_us")).toObject().value(QStringLiteral("p99")).toDouble(-1.0);
            if (fp >= 0.0 && dp >= 0.0)
                gate += QStringLiteral(" · p99 frame %1 µs, decision %2 µs").arg(fp, 0, 'f', 0).arg(dp, 0, 'f', 0);
        }
        status_->setText(QStringLiteral("%1 &nbsp;<span style='color:%2'>engine %3 IST%4%5 · %6</span>")
                             .arg(tag, muted, live_detail::ist(eng, false))
                             .arg((stale ? QStringLiteral(" · <b style='color:#F07A6A'>FEED STALE: no new entries</b>") : QString()) + gate)
                             .arg(age > 5 ? QStringLiteral(" · state %1 s old (engine stopped?)").arg(age) : QString())
                             .arg(o.value(QStringLiteral("note")).toString().toHtmlEscaped()));
        const QJsonArray ms = o.value(QStringLiteral("models")).toArray();
        models_->setRowCount(static_cast<int>(ms.size()));
        fields_by_row_.clear();
        for (int r = 0; r < ms.size(); ++r) {
            const QJsonObject m = ms.at(r).toObject();
            const QString state = m.value(QStringLiteral("state")).toString();
            const QColor sc = state == QLatin1String("in position") ? QColor(0xF4, 0xC9, 0x5D)
                            : state == QLatin1String("abstaining") ? QColor(0xF0, 0x7A, 0x6A) : QColor(0xD0, 0xD6, 0xDE);
            live_set(models_, r, 0, m.value(QStringLiteral("name")).toString());
            live_set(models_, r, 1, m.value(QStringLiteral("family")).toString());
            live_set(models_, r, 2, state, sc);
            live_set(models_, r, 3, m.value(QStringLiteral("signal")).toString());
            live_set(models_, r, 4, m.value(QStringLiteral("reason")).toString());
            for (int c = 0; c < 5; ++c) if (auto* it = models_->item(r, c)) it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            std::vector<std::pair<QString, QString>> fv;
            for (const auto& x : m.value(QStringLiteral("fields")).toArray()) {
                const QJsonArray kv = x.toArray();
                fv.push_back({kv.at(0).toString(), kv.at(1).toString()});
            }
            fields_by_row_.push_back(fv);
        }
        show_fields(models_->currentRow() < 0 ? 0 : models_->currentRow());
        positions_cache_.clear();
        for (const auto& x : o.value(QStringLiteral("positions")).toArray()) {
            const QJsonObject p = x.toObject();
            Pos q;
            q.model = p.value(QStringLiteral("model")).toString();
            q.symbol = p.value(QStringLiteral("symbol")).toString();
            q.token = static_cast<quint32>(p.value(QStringLiteral("token")).toDouble());
            q.side = p.value(QStringLiteral("side")).toInt();
            q.qty = static_cast<qint64>(p.value(QStringLiteral("qty")).toDouble());
            q.entry = p.value(QStringLiteral("entry")).toDouble();
            if (!p.value(QStringLiteral("entry_expenses")).isNull()) q.entry_expenses = p.value(QStringLiteral("entry_expenses")).toDouble();
            q.carry = p.value(QStringLiteral("carry")).toBool();
            q.why = p.value(QStringLiteral("why_in")).toString();
            q.want = static_cast<qint64>(p.value(QStringLiteral("want")).toDouble());
            q.state = p.value(QStringLiteral("state")).toString();
            q.exit_reason = p.value(QStringLiteral("exit_reason")).toString();
            positions_cache_.push_back(q);
        }
        engine_day_ = eng > 0 ? QDate(1970, 1, 1).addDays((eng / 1'000'000'000LL + 19800) / 86400).toString(Qt::ISODate) : QString();
    }

    void show_fields(int r) {
        if (r < 0 || r >= static_cast<int>(fields_by_row_.size())) { fields_->setRowCount(0); return; }
        const auto& fv = fields_by_row_[static_cast<std::size_t>(r)];
        fields_->setRowCount(static_cast<int>(fv.size()));
        for (int i = 0; i < static_cast<int>(fv.size()); ++i) {
            live_set(fields_, i, 0, fv[static_cast<std::size_t>(i)].first);
            live_set(fields_, i, 1, fv[static_cast<std::size_t>(i)].second);
            if (auto* it = fields_->item(i, 0)) it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    void read_trades() {
        const QString path = root_ + QStringLiteral("/data/live/paper/trades.csv");
        const QFileInfo fi(path);
        const qint64 stamp = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() + fi.size() : 0;
        if (stamp == trades_stamp_) return;
        trades_stamp_ = stamp;
        trades_cache_ = load_live_trades(path);
        const int n = static_cast<int>(trades_cache_.size());
        trades_->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            const LivePaperRow& t = trades_cache_[static_cast<std::size_t>(n - 1 - i)];   // newest first
            live_set(trades_, i, 0, t.date);
            live_set(trades_, i, 1, t.model);
            live_set(trades_, i, 2, t.symbol);
            live_set(trades_, i, 3, t.side);
            live_set(trades_, i, 4, QString::number(t.qty));
            live_set(trades_, i, 5, t.entry_time.section(QLatin1Char(' '), 1));
            live_set(trades_, i, 6, QString::number(t.entry, 'f', 2));
            live_set(trades_, i, 7, t.exit_time.section(QLatin1Char(' '), 1));
            live_set(trades_, i, 8, QString::number(t.exit, 'f', 2));
            live_set(trades_, i, 9, live_models_detail::rupees(t.gross), live_models_detail::pnl_color(t.gross));
            live_set(trades_, i, 10, live_models_detail::rupees(t.expenses));
            live_set(trades_, i, 11, live_models_detail::rupees(t.net), live_models_detail::pnl_color(t.net));
            live_set(trades_, i, 12, t.why_out);
            live_set(trades_, i, 13, t.source + QStringLiteral(" · ") + t.costs);
        }
    }

    /// Mark every open position from the stream, and total by model.
    void mark() {
        const int n = static_cast<int>(positions_cache_.size());
        positions_->setRowCount(n);
        std::map<QString, double> open_pnl;
        double open_total = 0.0;
        bool open_complete = true;
        for (int i = 0; i < n; ++i) {
            const Pos& p = positions_cache_[static_cast<std::size_t>(i)];
            const LivePrice* lp = client_ != nullptr ? client_->price(p.token) : nullptr;
            // Marked where closing would deal -- a long at the bid, a short at
            // the ask -- from a quote no older than 10 s on the feed's clock.
            // Never at the last trade: an unquoted position is unmarked.
            qint64 px = 0;
            if (lp != nullptr && lp->has_quote && lp->quote.has(kQuoteHasTop)
                && lp->quote_ns > 0 && lp->last_ns - lp->quote_ns <= 10'000'000'000LL)
                px = p.side > 0 ? lp->quote.bid : lp->quote.ask;
            const double mark_px = static_cast<double>(px) / 100.0;
            const double u = p.qty == 0 ? 0.0
                           : px > 0 ? static_cast<double>(p.side) * (mark_px - p.entry) * static_cast<double>(p.qty)
                                    : std::numeric_limits<double>::quiet_NaN();
            if (std::isfinite(u)) { open_pnl[p.model] += u; open_total += u; } else { open_complete = false; }
            live_set(positions_, i, 0, p.model);
            live_set(positions_, i, 1, p.symbol);
            live_set(positions_, i, 2, p.side > 0 ? QStringLiteral("long") : QStringLiteral("short"),
                     p.side > 0 ? QColor(0x7F, 0xB8, 0xF0) : QColor(0xF0, 0xA0, 0x7F));
            live_set(positions_, i, 3, p.want > p.qty && p.state == QLatin1String("opening")
                                           ? QStringLiteral("%1/%2").arg(p.qty).arg(p.want) : QString::number(p.qty));
            live_set(positions_, i, 4, QString::number(p.entry, 'f', 2));
            live_set(positions_, i, 5, px > 0 ? QString::number(mark_px, 'f', 2) : QStringLiteral("no fresh quote"));
            live_set(positions_, i, 6, live_models_detail::rupees(u), live_models_detail::pnl_color(u));
            live_set(positions_, i, 7, live_models_detail::rupees(p.entry_expenses));
            live_set(positions_, i, 8, (p.carry ? QStringLiteral("carried") : QStringLiteral("intraday"))
                                           + (p.state == QLatin1String("opening") ? QStringLiteral(" · entry working")
                                              : p.state == QLatin1String("closing") ? QStringLiteral(" · exit working: ") + p.exit_reason
                                                                                    : QString()));
            live_set(positions_, i, 9, p.why);
            for (int c : {0, 1, 9}) if (auto* it = positions_->item(i, c)) it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
        // P&L by model: realised from the trades file, open from the marks.
        struct Sum { int trades = 0; double gross = 0, exp = 0, net = 0, today = 0; bool unpriced = false; };
        std::map<QString, Sum> by;
        double gross = 0, exp = 0, net_today = 0;
        bool any_unpriced = false;
        for (const auto& t : trades_cache_) {
            Sum& s = by[t.model];
            ++s.trades;
            s.gross += t.gross;
            gross += t.gross;
            if (std::isfinite(t.expenses)) { s.exp += t.expenses; exp += t.expenses; } else { s.unpriced = any_unpriced = true; }
            if (std::isfinite(t.net)) s.net += t.net;
            if (t.date == engine_day_ && std::isfinite(t.net)) { s.today += t.net; net_today += t.net; }
        }
        for (const auto& [m, u] : open_pnl) by[m];
        by_model_->setRowCount(static_cast<int>(by.size()));
        int r = 0;
        for (const auto& [m, s] : by) {
            const auto it = open_pnl.find(m);
            const double o = it == open_pnl.end() ? 0.0 : it->second;
            live_set(by_model_, r, 0, m);
            if (auto* x = by_model_->item(r, 0)) x->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            live_set(by_model_, r, 1, QString::number(s.trades));
            live_set(by_model_, r, 2, live_models_detail::rupees(s.gross), live_models_detail::pnl_color(s.gross));
            live_set(by_model_, r, 3, s.unpriced ? QStringLiteral("unpriced") : live_models_detail::rupees(s.exp));
            live_set(by_model_, r, 4, s.unpriced ? QStringLiteral("—") : live_models_detail::rupees(s.net), live_models_detail::pnl_color(s.net));
            live_set(by_model_, r, 5, live_models_detail::rupees(o), live_models_detail::pnl_color(o));
            live_set(by_model_, r, 6, s.unpriced ? QStringLiteral("—") : live_models_detail::rupees(s.today), live_models_detail::pnl_color(s.today));
            ++r;
        }
        const QString muted = QString::fromLatin1(theme_token::kTextMuted);
        totals_->setText(QStringLiteral("<span style='color:%1'>All paper trades:</span> %2 round trips · gross <b>%3</b> · "
                                        "expenses <b>%4</b> · net <b style='color:%5'>%6</b> &nbsp; "
                                        "<span style='color:%1'>today net</span> <b>%7</b> &nbsp; "
                                        "<span style='color:%1'>open, marked live</span> <b style='color:%8'>%9</b>%10")
                             .arg(muted).arg(trades_cache_.size()).arg(live_models_detail::rupees(gross))
                             .arg(any_unpriced ? QStringLiteral("unpriced") : live_models_detail::rupees(exp))
                             .arg(live_models_detail::pnl_color(gross - exp).name())
                             .arg(any_unpriced ? QStringLiteral("— (expenses refused)") : live_models_detail::rupees(gross - exp))
                             .arg(any_unpriced ? QStringLiteral("—") : live_models_detail::rupees(net_today))
                             .arg(live_models_detail::pnl_color(open_total).name())
                             .arg(live_models_detail::rupees(open_total))
                             .arg(open_complete ? QString() : QStringLiteral(" <span style='color:%1'>(some positions have no fresh quote and are left out of this total)</span>").arg(muted)));
    }

    void start_engine() {
        if (engine_ != nullptr) return;
        const QString exe = live_detail::find_helper(QStringLiteral("altair_live_engine"));
        if (exe.isEmpty()) {
            status_->setText(QStringLiteral("<span style='color:#F07A6A'>altair_live_engine is not built (it needs Boost and "
                                            "tomlplusplus; build the net preset).</span>"));
            return;
        }
        engine_ = new QProcess(this);
        engine_->setWorkingDirectory(root_);
        engine_->setProcessChannelMode(QProcess::MergedChannels);
        connect(engine_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            const QString tail = QString::fromLocal8Bit(engine_->readAll()).trimmed().section(QChar('\n'), -1);
            engine_->deleteLater();
            engine_ = nullptr;
            start_->setEnabled(true);
            stop_->setEnabled(false);
            unverified_->setEnabled(true);
            status_->setText(status_->text() + QStringLiteral("<br><span style='color:%1'>engine exited %2: %3</span>")
                                                   .arg(code == 0 ? QStringLiteral("#8A93A2") : QStringLiteral("#F07A6A"))
                                                   .arg(code).arg(tail.toHtmlEscaped()));
        });
        QStringList args;
        if (unverified_->isChecked()) args << QStringLiteral("--unverified-costs");
        engine_->start(exe, args);
        start_->setEnabled(false);
        stop_->setEnabled(true);
        unverified_->setEnabled(false);
    }

    void stop_engine() {
        if (engine_ == nullptr) return;
        engine_->terminate();
        if (!engine_->waitForFinished(5000)) engine_->kill();
    }

    const PriceClient* client_;
    QString root_;
    QPushButton* start_ = nullptr;
    QPushButton* stop_ = nullptr;
    QCheckBox* unverified_ = nullptr;
    QCheckBox* auto_demo_ = nullptr;
    bool user_stopped_ = false;
    QLabel* status_ = nullptr;
    QLabel* totals_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* models_ = nullptr;
    QTableWidget* fields_ = nullptr;
    QTableWidget* positions_ = nullptr;
    QTableWidget* trades_ = nullptr;
    QTableWidget* by_model_ = nullptr;
    QProcess* engine_ = nullptr;
    QTimer file_timer_, mark_timer_;
    std::vector<std::vector<std::pair<QString, QString>>> fields_by_row_;
    std::vector<Pos> positions_cache_;
    std::vector<LivePaperRow> trades_cache_;
    QString engine_day_;
    qint64 trades_stamp_ = -1;
};

} // namespace altair::ui
