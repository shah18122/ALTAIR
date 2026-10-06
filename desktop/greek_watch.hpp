// desktop/greek_watch.hpp -- Greek Watch: the options you picked, their
// Greeks live, and the summary by underlying and expiry beneath them.
//
// Opened from the option chain: Enter or double-click a CE or PE cell and the
// strike joins the watch, every column filling from the stream -- LTP, bid,
// ask, IV inverted from the market mid with Black-76, Δ Γ vega Θ, the
// theoretical price (at your IV when you type one), and, for the units you
// hold (one lot by default; negative for short), the value columns and MtoM.
// The summary below adds the value columns up per underlying and expiry: the
// old Greek Summary, merged in.
//
// Reads the price stream; places nothing.

#pragma once

#include "live_market.hpp"

#include <QDialog>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <vector>

namespace altair::ui {

/// One watched option and what it is valued on.
struct GreekLeg {
    ChainContract c;
    QString under;
    quint32 fut = 0, spot = 0;
    qint64 fut_expiry = 0;
    double units = 0.0;                                           ///< + long, − short
    double user_iv = std::numeric_limits<double>::quiet_NaN();    ///< as a fraction; NaN: the market's
    double trade_price = std::numeric_limits<double>::quiet_NaN(); ///< rupees; NaN: no MtoM
};

/// The numbers of one leg now, in rupees (Γ per rupee, vega per vol point, Θ per day).
struct GreekRow {
    double years = 0, forward = 0, spot = 0, ltp = 0, bid = 0, ask = 0;
    double iv = std::numeric_limits<double>::quiet_NaN();
    double theo = std::numeric_limits<double>::quiet_NaN();
    double delta = std::numeric_limits<double>::quiet_NaN(), gamma = std::numeric_limits<double>::quiet_NaN(),
           vega = std::numeric_limits<double>::quiet_NaN(), theta = std::numeric_limits<double>::quiet_NaN();
    double mtm = std::numeric_limits<double>::quiet_NaN();
};

[[nodiscard]] inline GreekRow greek_row(const PriceClient* client, const GreekLeg& g, double rate) {
    GreekRow r;
    const LivePrice* p = client != nullptr ? client->price(g.c.token) : nullptr;
    const LivePrice* clock = client != nullptr ? client->price(g.spot != 0 ? g.spot : g.fut) : nullptr;
    if (clock == nullptr && p != nullptr) clock = p;
    const std::int64_t now_ns = clock != nullptr && clock->exchange_ts_ns > 0 ? clock->exchange_ts_ns
                              : QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
    r.years = g.c.expiry_day > 0
        ? static_cast<double>((g.c.expiry_day * 86400 + 36000) * 1'000'000'000LL - now_ns) / (365.0 * 86400.0 * 1e9) : 0.0;
    const ChainForward f = chain_forward(client, g.fut, g.fut_expiry, g.spot, g.c.expiry_day, r.years, rate);
    r.forward = f.paise / 100.0;
    r.spot = static_cast<double>(live_mid(client, g.spot)) / 100.0;
    if (p != nullptr) {
        r.ltp = static_cast<double>(p->last_paise) / 100.0;
        if (p->has_quote && p->quote.has(kQuoteHasTop)) {
            r.bid = static_cast<double>(p->quote.bid) / 100.0;
            r.ask = static_cast<double>(p->quote.ask) / 100.0;
        }
    }
    const LiveIv iv = live_iv(g.c.call, live_mid(client, g.c.token), f.paise, g.c.strike, r.years, rate);
    r.iv = iv.iv;
    const double vol = std::isfinite(g.user_iv) && g.user_iv > 0 ? g.user_iv : r.iv;
    if (std::isfinite(vol) && vol > 0 && f.paise > 0 && r.years > 0) {
        const auto k = altair::detail::black76_unchecked(g.c.call ? OptionRight::Call : OptionRight::Put, f.paise,
                                                         g.c.strike * 100.0, r.years, vol, rate);
        r.theo = k.price / 100.0;
        r.delta = k.delta;
        r.gamma = k.gamma * 100.0;          // per rupee of the forward
        r.vega = k.vega / 100.0 / 100.0;    // rupees per vol point
        r.theta = k.theta / 365.0 / 100.0;  // rupees per calendar day
    }
    if (std::isfinite(g.trade_price) && r.ltp > 0) r.mtm = (r.ltp - g.trade_price) * g.units;
    return r;
}

class GreekWatchWindow final : public QDialog {
public:
    enum Col { Symbol, Under, Expiry, Strike, Type, Days, Units, TradePx, Spot, Fwd, Ltp, Bid, Ask, Iv, UserIv, Theo,
               Delta, Gamma, Vega, Theta, DVal, GVal, VVal, TVal, Mtm, Cols };
    enum SumCol { SUnder, SExpiry, SLegs, SUnits, SDVal, SGVal, SVVal, STVal, SMtm, SCols };

    GreekWatchWindow(const PriceClient* client, QWidget* parent = nullptr) : QDialog(parent), client_(client) {
        setObjectName(QStringLiteral("greekWatch"));
        setWindowTitle(QStringLiteral("Greek Watch"));
        setWindowFlag(Qt::WindowMaximizeButtonHint, true);
        resize(1320, 560);
        auto* v = new QVBoxLayout(this);
        auto* head = new QLabel(QStringLiteral(
            "Options join from the option chain (Enter or double-click a CE or PE). Every column fills from the stream. "
            "<b>Units</b> (one lot by default, negative for short), <b>Trade price</b> and <b>User IV %</b> are yours to "
            "type; the value columns, MtoM and the summary follow them. Delete removes a row."), this);
        head->setWordWrap(true);
        head->setTextFormat(Qt::RichText);
        v->addWidget(head);
        auto* split = new QSplitter(Qt::Vertical, this);
        grid_ = new QTableWidget(0, Cols, split);
        grid_->setObjectName(QStringLiteral("greekWatchGrid"));
        grid_->setHorizontalHeaderLabels({QStringLiteral("Symbol"), QStringLiteral("Underlying"), QStringLiteral("Expiry"),
                                          QStringLiteral("Strike"), QStringLiteral("Type"), QStringLiteral("Days"),
                                          QStringLiteral("Units"), QStringLiteral("Trade price"), QStringLiteral("Spot"),
                                          QStringLiteral("Forward"), QStringLiteral("LTP"), QStringLiteral("Bid"),
                                          QStringLiteral("Ask"), QStringLiteral("IV %"), QStringLiteral("User IV %"),
                                          QStringLiteral("Theoretical"), QStringLiteral("Δ"), QStringLiteral("Γ"),
                                          QStringLiteral("Vega"), QStringLiteral("Θ/day"), QStringLiteral("ΔVal"),
                                          QStringLiteral("ΓVal"), QStringLiteral("VVal"), QStringLiteral("ΘVal"),
                                          QStringLiteral("MtoM")});
        grid_->verticalHeader()->hide();
        grid_->verticalHeader()->setDefaultSectionSize(21);
        grid_->horizontalHeader()->setDefaultSectionSize(76);
        grid_->setColumnWidth(Symbol, 190);
        grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
        grid_->installEventFilter(this);
        summary_ = new QTableWidget(0, SCols, split);
        summary_->setObjectName(QStringLiteral("greekSummaryGrid"));
        summary_->setHorizontalHeaderLabels({QStringLiteral("Underlying"), QStringLiteral("Expiry"), QStringLiteral("Legs"),
                                             QStringLiteral("Net units"), QStringLiteral("ΔVal"), QStringLiteral("ΓVal"),
                                             QStringLiteral("VVal"), QStringLiteral("ΘVal"), QStringLiteral("MtoM")});
        summary_->verticalHeader()->hide();
        summary_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        summary_->horizontalHeader()->setStretchLastSection(true);
        split->addWidget(grid_);
        split->addWidget(summary_);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 1);
        v->addWidget(split, 1);
        QObject::connect(grid_, &QTableWidget::cellChanged, this, [this](int r, int c) { edited(r, c); });
        timer_.setInterval(500);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
        timer_.start();
    }

    /// Add a leg (or select it when it is already watched). True when added.
    bool add(const GreekLeg& leg) {
        for (std::size_t i = 0; i < legs_.size(); ++i)
            if (legs_[i].c.token == leg.c.token) { grid_->selectRow(static_cast<int>(i)); return false; }
        legs_.push_back(leg);
        if (legs_.back().units == 0.0) legs_.back().units = static_cast<double>(std::max<qint64>(1, leg.c.lot));
        rebuild();
        grid_->selectRow(static_cast<int>(legs_.size()) - 1);
        save();
        return true;
    }
    void remove(int row) {
        if (row < 0 || row >= static_cast<int>(legs_.size())) return;
        legs_.erase(legs_.begin() + row);
        rebuild();
        save();
    }
    [[nodiscard]] const std::vector<GreekLeg>& legs() const noexcept { return legs_; }
    [[nodiscard]] QTableWidget* grid() const noexcept { return grid_; }
    [[nodiscard]] QTableWidget* summary() const noexcept { return summary_; }
    /// Where the watched tokens are kept between sessions ("" = nowhere).
    void set_store(const QString& path) { store_ = path; }
    [[nodiscard]] std::vector<std::pair<quint32, double>> stored() const {
        std::vector<std::pair<quint32, double>> out;
        QFile f(store_);
        if (store_.isEmpty() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QStringList c = in.readLine().split(QLatin1Char(','));
            if (c.size() < 2 || c[0].isEmpty() || !c[0].at(0).isDigit()) continue;
            out.emplace_back(c[0].toUInt(), c[1].toDouble());
        }
        return out;
    }

    /// The live numbers for every leg on screen, then the summary.
    void refresh() {
        const QSignalBlocker b(grid_);
        struct Sum { int legs = 0; double units = 0, d = 0, g = 0, v = 0, t = 0, m = 0; bool any_m = false; };
        std::map<std::pair<QString, qint64>, Sum> sums;
        for (int i = 0; i < static_cast<int>(legs_.size()); ++i) {
            const GreekLeg& g = legs_[static_cast<std::size_t>(i)];
            const GreekRow r = greek_row(client_, g, rate_);
            const auto num = [](double x, int dp) { return std::isfinite(x) && x != 0.0 ? QString::number(x, 'f', dp) : QStringLiteral("—"); };
            set(i, Days, num(r.years * 365.0, 2));
            set(i, Spot, num(r.spot, 2));
            set(i, Fwd, num(r.forward, 2));
            set(i, Ltp, num(r.ltp, 2));
            set(i, Bid, num(r.bid, 2));
            set(i, Ask, num(r.ask, 2));
            set(i, Iv, std::isfinite(r.iv) ? QString::number(r.iv * 100.0, 'f', 2) : QStringLiteral("—"));
            set(i, Theo, num(r.theo, 2));
            set(i, Delta, num(r.delta, 3));
            set(i, Gamma, num(r.gamma, 5));
            set(i, Vega, num(r.vega, 2));
            set(i, Theta, num(r.theta, 2));
            const double u = g.units;
            set(i, DVal, num(r.delta * u, 1));
            set(i, GVal, num(r.gamma * u, 3));
            set(i, VVal, num(r.vega * u, 1));
            set(i, TVal, num(r.theta * u, 1));
            set(i, Mtm, num(r.mtm, 2));
            Sum& s = sums[{g.under, g.c.expiry_day}];
            ++s.legs;
            s.units += u;
            if (std::isfinite(r.delta)) { s.d += r.delta * u; s.g += r.gamma * u; s.v += r.vega * u; s.t += r.theta * u; }
            if (std::isfinite(r.mtm)) { s.m += r.mtm; s.any_m = true; }
        }
        summary_->setRowCount(static_cast<int>(sums.size()));
        int row = 0;
        for (const auto& [key, s] : sums) {
            const QStringList cells{key.first, QDate(1970, 1, 1).addDays(key.second).toString(QStringLiteral("dd-MMM-yyyy")),
                                    QString::number(s.legs), QString::number(s.units, 'f', 0), QString::number(s.d, 'f', 1),
                                    QString::number(s.g, 'f', 3), QString::number(s.v, 'f', 1), QString::number(s.t, 'f', 1),
                                    s.any_m ? QString::number(s.m, 'f', 2) : QStringLiteral("—")};
            for (int c = 0; c < cells.size(); ++c) {
                auto* it = summary_->item(row, c);
                if (it == nullptr) { it = new QTableWidgetItem; summary_->setItem(row, c, it); }
                it->setText(cells[c]);
            }
            ++row;
        }
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        // While a cell is being edited its editor has the keys, not the grid.
        if (obj == grid_ && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Delete) {
            remove(grid_->currentRow());
            return true;
        }
        return QDialog::eventFilter(obj, e);
    }

private:
    void set(int r, int c, const QString& text) {
        auto* it = grid_->item(r, c);
        if (it == nullptr) {
            it = new QTableWidgetItem;
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid_->setItem(r, c, it);
        }
        if (it->text() != text) it->setText(text);
    }
    void rebuild() {
        const QSignalBlocker b(grid_);
        grid_->setRowCount(static_cast<int>(legs_.size()));
        for (int i = 0; i < static_cast<int>(legs_.size()); ++i) {
            const GreekLeg& g = legs_[static_cast<std::size_t>(i)];
            set(i, Symbol, g.c.symbol);
            set(i, Under, g.under);
            set(i, Expiry, QDate(1970, 1, 1).addDays(g.c.expiry_day).toString(QStringLiteral("dd-MMM-yy")));
            set(i, Strike, QString::number(g.c.strike, 'f', g.c.strike == std::floor(g.c.strike) ? 0 : 2));
            set(i, Type, g.c.call ? QStringLiteral("CE") : QStringLiteral("PE"));
            for (const int c : {static_cast<int>(Units), static_cast<int>(TradePx), static_cast<int>(UserIv)}) {
                auto* it = grid_->item(i, c);
                if (it == nullptr) { it = new QTableWidgetItem; grid_->setItem(i, c, it); }
                it->setFlags(it->flags() | Qt::ItemIsEditable);
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                it->setBackground(QColor(0x1C, 0x2B, 0x36));
            }
            grid_->item(i, Units)->setText(QString::number(g.units, 'f', 0));
            grid_->item(i, TradePx)->setText(std::isfinite(g.trade_price) ? QString::number(g.trade_price, 'f', 2) : QString());
            grid_->item(i, UserIv)->setText(std::isfinite(g.user_iv) ? QString::number(g.user_iv * 100.0, 'f', 2) : QString());
        }
        refresh();
    }
    void edited(int r, int c) {
        if (r < 0 || r >= static_cast<int>(legs_.size())) return;
        GreekLeg& g = legs_[static_cast<std::size_t>(r)];
        const QString t = grid_->item(r, c) != nullptr ? grid_->item(r, c)->text().trimmed() : QString();
        bool ok = false;
        const double x = t.toDouble(&ok);
        if (c == Units && ok) g.units = x;
        else if (c == TradePx) g.trade_price = ok && x > 0 ? x : std::numeric_limits<double>::quiet_NaN();
        else if (c == UserIv) g.user_iv = ok && x > 0 ? x / 100.0 : std::numeric_limits<double>::quiet_NaN();
        else return;
        save();
        refresh();
    }
    void save() const {
        if (store_.isEmpty()) return;
        QFile f(store_);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
        QTextStream out(&f);
        out << "token,units\n";
        for (const auto& g : legs_) out << g.c.token << ',' << g.units << '\n';
    }

    const PriceClient* client_;
    QTableWidget* grid_ = nullptr;
    QTableWidget* summary_ = nullptr;
    std::vector<GreekLeg> legs_;
    QTimer timer_;
    QString store_;
    double rate_ = 0.065;
};

} // namespace altair::ui
