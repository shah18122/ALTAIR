// desktop/gets_tables.hpp -- the GETS-style Terminal tables.
//
// One generic, sortable table model and a row builder per screen. Builders
// are plain functions of the typed data (desktop/gets_data.hpp) and the
// engine results (risk/option_book.hpp), so every number on screen is under
// test without a window.
//
// Column sets follow the P4-04 GETS/Greeksoft specification (in git history,
// prompts/P4-04_GETS_TABLE_SPEC.md): the Greek market watch keeps GETS'
// column order and its compact MAIN profile; columns whose meaning the
// specification could not establish (Balance, DaysATP, %DelWrtGamma, ...) are
// left out rather than invented.
//
// DISPLAY RULES (Altair's, not GETS'): an unknown value is "—", a known zero
// is "0.00"; P&L is green/red; a row that could not be valued says why in its
// Status column and tooltip.
#pragma once

#include "format.hpp"
#include "gets_data.hpp"

#include <QAbstractTableModel>
#include <QColor>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace altair::ui {

// ---- the generic model -------------------------------------------------------

struct GetsCell {
    QString text;
    QVariant sort;                    ///< numeric sort key; text when invalid
    bool numeric{};
    QColor colour;                    ///< invalid: theme default
    QString tip;
};

struct GetsColumn {
    GetsColumn(QString t, bool main = true, QString help = {}, bool edit = false)
        : title(std::move(t)), main_profile(main), tip(std::move(help)), editable(edit) {}
    QString title;
    bool main_profile{true};          ///< shown in the compact profile
    QString tip;
    bool editable{};
};

using GetsRow = QVector<GetsCell>;

class GetsTableModel final : public QAbstractTableModel {
public:
    enum Role { SortRole = Qt::UserRole + 1, KeyRole };
    using EditFn = std::function<bool(const QString& key, int column, const QVariant& value)>;

    explicit GetsTableModel(QVector<GetsColumn> columns, QObject* parent = nullptr)
        : QAbstractTableModel(parent), columns_(std::move(columns)) {}

    void set_rows(QVector<GetsRow> rows, QStringList keys = {}) {
        beginResetModel();
        rows_ = std::move(rows);
        keys_ = std::move(keys);
        endResetModel();
    }
    void set_edit(EditFn fn) { edit_ = std::move(fn); }

    [[nodiscard]] const QVector<GetsColumn>& columns() const noexcept { return columns_; }
    [[nodiscard]] QString text(int row, int column) const {
        return row >= 0 && row < rows_.size() && column >= 0 && column < rows_[row].size()
            ? rows_[row][column].text : QString();
    }
    [[nodiscard]] int column_of(const QString& title) const {
        for (int c = 0; c < columns_.size(); ++c)
            if (columns_[c].title == title) return c;
        return -1;
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(columns_.size());
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (o != Qt::Horizontal || section < 0 || section >= columns_.size()) return {};
        if (role == Qt::DisplayRole) return columns_[section].title;
        if (role == Qt::ToolTipRole && !columns_[section].tip.isEmpty()) return columns_[section].tip;
        return {};
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rows_.size()) return {};
        const GetsRow& r = rows_[index.row()];
        if (index.column() >= r.size()) return {};
        const GetsCell& c = r[index.column()];
        switch (role) {
        case Qt::DisplayRole: return c.text;
        case Qt::EditRole: return c.sort.isValid() ? c.sort : QVariant{c.text};
        case Qt::TextAlignmentRole:
            return QVariant::fromValue(Qt::Alignment(
                (c.numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter));
        case Qt::ForegroundRole: return c.colour.isValid() ? QVariant{c.colour} : QVariant{};
        case Qt::ToolTipRole: return c.tip.isEmpty() ? QVariant{} : QVariant{c.tip};
        case SortRole: return c.sort.isValid() ? c.sort : QVariant{c.text};
        case KeyRole: return index.row() < keys_.size() ? QVariant{keys_[index.row()]} : QVariant{};
        default: return {};
        }
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        if (index.isValid() && index.column() < columns_.size() && columns_[index.column()].editable
            && index.row() < keys_.size() && edit_)
            f |= Qt::ItemIsEditable;
        return f;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !index.isValid() || index.row() >= keys_.size() || !edit_)
            return false;
        return edit_(keys_[index.row()], index.column(), value);
    }

private:
    QVector<GetsColumn> columns_;
    QVector<GetsRow> rows_;
    QStringList keys_;
    EditFn edit_;
};

// ---- cells -------------------------------------------------------------------

namespace gets_cell {

inline const QColor kUp{QStringLiteral("#3FB950")};
inline const QColor kDown{QStringLiteral("#F85149")};
inline const QColor kMuted{QStringLiteral("#8B949E")};
inline const QColor kWarn{QStringLiteral("#E3A34A")};

[[nodiscard]] inline GetsCell text(const QString& t, const QString& tip = {}) {
    GetsCell c;
    c.text = t;
    c.tip = tip;
    return c;
}
[[nodiscard]] inline GetsCell blank(const QString& tip = {}) {
    GetsCell c;
    c.text = QStringLiteral("—");
    c.numeric = true;
    c.colour = kMuted;
    c.tip = tip;
    return c;
}
[[nodiscard]] inline GetsCell integer(std::int64_t v, bool sign_colour = false) {
    GetsCell c;
    c.text = QString::number(v);
    c.sort = static_cast<qlonglong>(v);
    c.numeric = true;
    if (sign_colour && v != 0) c.colour = v > 0 ? kUp : kDown;
    return c;
}
[[nodiscard]] inline GetsCell money(std::optional<std::int64_t> paise, bool pnl = false) {
    if (!paise) return blank();
    GetsCell c;
    c.text = format_paise(*paise, pnl);
    c.sort = static_cast<qlonglong>(*paise);
    c.numeric = true;
    if (pnl && *paise != 0) c.colour = *paise > 0 ? kUp : kDown;
    return c;
}
[[nodiscard]] inline GetsCell real(std::optional<double> v, int decimals, bool pnl = false,
                                   const QString& suffix = {}) {
    if (!v || !std::isfinite(*v)) return blank();
    GetsCell c;
    c.text = QString::number(*v, 'f', decimals) + suffix;
    c.sort = *v;
    c.numeric = true;
    if (pnl && std::fabs(*v) >= std::pow(10.0, -decimals)) c.colour = *v > 0 ? kUp : kDown;
    return c;
}
[[nodiscard]] inline GetsCell time(const QDateTime& t) {
    if (!t.isValid()) return blank();
    GetsCell c;
    c.text = t.toTimeZone(QTimeZone(19800)).toString(QStringLiteral("dd-MMM HH:mm:ss"));
    c.sort = t.toSecsSinceEpoch();
    return c;
}
[[nodiscard]] inline GetsCell date(std::optional<Timestamp> t) {
    if (!t) return blank(QStringLiteral("monthly contract and no instrument master: expiry unknown"));
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(t->ns_since_epoch() / 1'000'000, QTimeZone(19800));
    GetsCell c;
    c.text = d.toString(QStringLiteral("dd-MMM-yy"));
    c.sort = static_cast<qlonglong>(t->ns_since_epoch());
    return c;
}

} // namespace gets_cell

[[nodiscard]] inline QString gets_issue_text(BookLegIssue issue) {
    switch (issue) {
    case BookLegIssue::None:         return QStringLiteral("OK");
    case BookLegIssue::NoSpot:       return QStringLiteral("no spot quote");
    case BookLegIssue::NoExpiry:     return QStringLiteral("expiry unknown");
    case BookLegIssue::Expired:      return QStringLiteral("expired");
    case BookLegIssue::NoMark:       return QStringLiteral("no LTP");
    case BookLegIssue::IvUnsolved:   return QStringLiteral("IV unsolved");
    case BookLegIssue::GreeksFailed: return QStringLiteral("Greeks failed");
    }
    return QStringLiteral("?");
}

[[nodiscard]] inline QString gets_issue_tip(BookLegIssue issue) {
    switch (issue) {
    case BookLegIssue::NoSpot:
        return QStringLiteral("The underlying has no spot quote in data/fyers_quotes.json. Refresh quotes.");
    case BookLegIssue::NoExpiry:
        return QStringLiteral("A monthly FYERS ticker does not carry its expiry day. Put the Kite "
                              "instrument master at data/instruments.csv to resolve it.");
    case BookLegIssue::IvUnsolved:
        return QStringLiteral("The last price admits no volatility (at or below intrinsic, or above "
                              "the no-arbitrage bound). Refused, not clamped. Set a user IV to price it.");
    case BookLegIssue::NoMark:
        return QStringLiteral("No last traded price for this contract yet.");
    default: return {};
    }
}

// ---- Greek market watch ----------------------------------------------------------

enum GetsWatchColumn : int {
    GwSymbol, GwUserIv, GwDays, GwExpiry, GwStrike, GwType, GwUnits, GwTheo, GwIv, GwLtp,
    GwTradePrice, GwMtm, GwDelta, GwDVal, GwGamma, GwGVal, GwVega, GwVVal, GwTheta, GwTVal,
    GwExchange, GwSegment, GwUnderlying, GwSpot, GwLot, GwTradeAmt, GwTimeValue, GwStatus,
    GwColumnCount
};

[[nodiscard]] inline QVector<GetsColumn> gets_watch_columns() {
    return {
        {QStringLiteral("Symbol"), true, QStringLiteral("FYERS ticker")},
        {QStringLiteral("User IV %"), true, QStringLiteral("Your IV override. Double-click to set; empty clears. Drives Greeks and theoretical price."), true},
        {QStringLiteral("Days left"), true, QStringLiteral("Calendar days to 15:30 IST on expiry")},
        {QStringLiteral("Expiry"), true},
        {QStringLiteral("Strike"), true},
        {QStringLiteral("Type"), true},
        {QStringLiteral("Units"), true, QStringLiteral("Signed net units (not lots). 0 for a watch-only row.")},
        {QStringLiteral("Theoretical"), true, QStringLiteral("Black-Scholes on spot at the IV in use")},
        {QStringLiteral("IV %"), true, QStringLiteral("Implied volatility solved from LTP")},
        {QStringLiteral("LTP"), false},
        {QStringLiteral("Trade price"), true, QStringLiteral("Net average of the open units (broker value)")},
        {QStringLiteral("MtoM"), true, QStringLiteral("(LTP - trade price) x units, exact")},
        {QStringLiteral("Delta"), false, QStringLiteral("Per unit")},
        {QStringLiteral("DVal"), false, QStringLiteral("Position delta, in underlying units")},
        {QStringLiteral("Gamma"), false, QStringLiteral("Per unit, delta per rupee")},
        {QStringLiteral("GVal"), false, QStringLiteral("Position gamma, delta units per rupee")},
        {QStringLiteral("Vega"), false, QStringLiteral("Per unit, rupees per vol point")},
        {QStringLiteral("VVal"), false, QStringLiteral("Position vega, rupees per vol point")},
        {QStringLiteral("Theta"), false, QStringLiteral("Per unit, rupees per calendar day")},
        {QStringLiteral("TVal"), false, QStringLiteral("Position theta, rupees per calendar day")},
        {QStringLiteral("Exchange"), false},
        {QStringLiteral("Mkt seg"), false},
        {QStringLiteral("Underlying"), false},
        {QStringLiteral("Spot"), false, QStringLiteral("Underlying LTP the Greeks use")},
        {QStringLiteral("Lot size"), false, QStringLiteral("From the instrument master")},
        {QStringLiteral("Trade amt"), false, QStringLiteral("Trade price x units")},
        {QStringLiteral("Time value"), false, QStringLiteral("LTP - intrinsic at spot")},
        {QStringLiteral("Status"), true},
    };
}

[[nodiscard]] inline QVector<GetsRow> gets_watch_rows(const GetsBook& b, QStringList* keys = nullptr) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (std::size_t i = 0; i < b.legs.size(); ++i) {
        const BookLeg& leg = b.legs[i];
        const BookLegView& v = b.views[i];
        const GetsInstrument& inst = b.instruments[i];
        const bool option = leg.kind == BookLegKind::Option;
        GetsRow r(GwColumnCount);
        r[GwSymbol] = text(inst.symbol, b.watch_only[i] ? QStringLiteral("watch list") : QString());
        if (b.watch_only[i]) r[GwSymbol].colour = kMuted;
        r[GwUserIv] = leg.user_vol ? real(leg.user_vol->raw() * 100.0, 2) : blank(QStringLiteral("not set"));
        r[GwDays] = option && v.days > 0.0 ? real(v.days, 2) : blank();
        r[GwExpiry] = leg.kind == BookLegKind::Cash ? blank() : date(inst.expiry);
        r[GwStrike] = option ? money(leg.strike.raw()) : blank();
        r[GwType] = option ? text(leg.right == OptionRight::Call ? QStringLiteral("CE") : QStringLiteral("PE"))
                  : text(leg.kind == BookLegKind::Future ? QStringLiteral("FUT") : QStringLiteral("EQ"));
        r[GwUnits] = integer(leg.units.raw(), true);
        r[GwTheo] = v.unit ? money(static_cast<std::int64_t>(std::llround(v.unit->price))) : blank();
        r[GwIv] = v.implied ? real(v.implied->raw() * 100.0, 2) : blank(gets_issue_tip(v.issue));
        r[GwLtp] = leg.mark ? money(leg.mark->raw()) : blank(QStringLiteral("no LTP"));
        r[GwTradePrice] = leg.units.raw() != 0 ? money(leg.average.raw()) : blank();
        r[GwMtm] = v.mtm && leg.units.raw() != 0 ? money(v.mtm->raw(), true) : blank();
        r[GwDelta] = v.unit ? real(v.unit->delta, 4) : (leg.kind != BookLegKind::Option ? real(1.0, 4) : blank());
        r[GwDVal] = v.greeks_ok ? real(v.delta, 2, true) : blank();
        r[GwGamma] = v.unit ? real(v.unit->gamma * 100.0, 6) : blank();
        r[GwGVal] = v.greeks_ok && option ? real(v.gamma, 4) : blank();
        r[GwVega] = v.unit ? real(vega_per_vol_point(v.unit->vega) / 100.0, 2) : blank();
        r[GwVVal] = v.greeks_ok && option ? real(v.vega, 2, true) : blank();
        r[GwTheta] = v.unit ? real(theta_per_day(v.unit->theta) / 100.0, 2) : blank();
        r[GwTVal] = v.greeks_ok && option ? real(v.theta, 2, true) : blank();
        r[GwExchange] = text(inst.parsed ? QString::fromLatin1(inst.parsed->exchange.data()) : QString());
        r[GwSegment] = text(inst.segment == Segment::Opt ? QStringLiteral("OPT")
                            : inst.segment == Segment::Fut ? QStringLiteral("FUT")
                            : QStringLiteral("CASH"));
        r[GwUnderlying] = text(inst.parsed ? QString::fromLatin1(inst.parsed->underlying.data()) : QString());
        const auto& spot = b.spots[leg.underlying];
        r[GwSpot] = spot ? money(spot->raw()) : blank(QStringLiteral("no quote for %1").arg(b.underlyings[leg.underlying]));
        r[GwLot] = leg.lot ? integer(leg.lot->raw()) : blank(QStringLiteral("instrument master not loaded"));
        const auto amount = notional_of(leg.average, leg.units);
        r[GwTradeAmt] = leg.units.raw() != 0 && amount ? money(amount->raw()) : blank();
        r[GwTimeValue] = v.time_value ? money(v.time_value->raw()) : blank();
        r[GwStatus] = text(gets_issue_text(v.issue), gets_issue_tip(v.issue));
        if (v.issue != BookLegIssue::None) r[GwStatus].colour = kWarn;
        if (!inst.parsed) {
            r[GwStatus] = text(QStringLiteral("unrecognised ticker"),
                               QStringLiteral("instruments/fyers_symbol.hpp could not read this ticker"));
            r[GwStatus].colour = kWarn;
        }
        rows << r;
        if (keys) *keys << inst.symbol;
    }
    return rows;
}

// ---- portfolio Greek summary ------------------------------------------------------

[[nodiscard]] inline QVector<GetsColumn> gets_summary_columns() {
    return {
        {QStringLiteral("Underlying")},
        {QStringLiteral("Expiry"), true, QStringLiteral("ALL is the underlying's total; CASH is equity")},
        {QStringLiteral("Days left")},
        {QStringLiteral("Mkt rate"), true, QStringLiteral("Spot")},
        {QStringLiteral("MtoM"), true, QStringLiteral("Sum of leg MTM; — if any leg has no LTP")},
        {QStringLiteral("Call IV %"), true, QStringLiteral("|units|-weighted IV in use")},
        {QStringLiteral("Put IV %")},
        {QStringLiteral("Delta"), true, QStringLiteral("Underlying units")},
        {QStringLiteral("Gamma"), true, QStringLiteral("Delta units per rupee")},
        {QStringLiteral("Vega"), true, QStringLiteral("Rupees per vol point")},
        {QStringLiteral("Theta"), true, QStringLiteral("Rupees per day")},
        {QStringLiteral("Delta neutral"), true, QStringLiteral("Lots of the future to trade to flatten delta (-delta / lot)")},
        {QStringLiteral("EQ posn"), true, QStringLiteral("Cash equity units")},
        {QStringLiteral("Legs")},
        {QStringLiteral("Complete"), true, QStringLiteral("NO when any leg lacks an LTP or Greeks: the sums are then partial")},
    };
}

[[nodiscard]] inline QVector<GetsRow> gets_summary_rows(const GetsBook& b,
                                                        const std::vector<BookSummaryRow>& rows_in) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (const auto& s : rows_in) {
        GetsRow r(15);
        QString name = s.underlying < b.underlyings.size() ? b.underlyings[s.underlying] : QString();
        r[0] = text(name.isEmpty() ? QStringLiteral("(no spot)") : name);
        if (s.total) {
            r[1] = text(QStringLiteral("ALL"));
            r[0].text = QStringLiteral("Σ ") + r[0].text;
        } else {
            r[1] = s.expiry_known ? date(s.expiry) : text(QStringLiteral("CASH/undated"));
        }
        r[2] = s.days > 0.0 ? real(s.days, 2) : blank();
        r[3] = s.spot ? money(s.spot->raw()) : blank();
        r[4] = s.mtm ? money(s.mtm->raw(), true) : blank(QStringLiteral("a leg has no LTP"));
        r[5] = s.call_iv ? real(s.call_iv->raw() * 100.0, 2) : blank();
        r[6] = s.put_iv ? real(s.put_iv->raw() * 100.0, 2) : blank();
        r[7] = real(s.delta, 2, true);
        r[8] = real(s.gamma, 4);
        r[9] = real(s.vega, 2, true);
        r[10] = real(s.theta, 2, true);
        r[11] = s.hedge_lots ? real(*s.hedge_lots, 2) : blank(QStringLiteral("lot size unknown"));
        r[12] = integer(s.cash_units);
        r[13] = integer(static_cast<std::int64_t>(s.legs));
        r[14] = text(s.complete ? QStringLiteral("yes") : QStringLiteral("NO"));
        if (!s.complete) r[14].colour = kWarn;
        rows << r;
    }
    return rows;
}

// ---- what-if simulation -----------------------------------------------------------

[[nodiscard]] inline QVector<GetsColumn> gets_simulation_columns() {
    return {
        {QStringLiteral("Spot move %")},
        {QStringLiteral("Mkt rate"), true, QStringLiteral("Scenario spot")},
        {QStringLiteral("P&L now"), true, QStringLiteral("Model value at the scenario minus current LTPs (GETS Balance)")},
        {QStringLiteral("P&L at expiry"), true, QStringLiteral("Payoff at expiry minus current LTPs (GETS ExpBalance)")},
        {QStringLiteral("Delta")},
        {QStringLiteral("Gamma")},
        {QStringLiteral("Vega")},
        {QStringLiteral("Theta")},
        {QStringLiteral("Delta neutral"), true, QStringLiteral("Lots of the future to flatten delta")},
        {QStringLiteral("Complete")},
    };
}

[[nodiscard]] inline QVector<GetsRow> gets_simulation_rows(const std::vector<BookScenarioRow>& rows_in) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (const auto& s : rows_in) {
        GetsRow r(10);
        r[0] = real(s.shift.spot_pct, 2, false, QStringLiteral("%"));
        if (s.shift.spot_pct == 0.0) r[0].colour = kWarn;
        r[1] = money(s.spot.raw());
        r[2] = s.pnl ? money(s.pnl->raw(), true) : blank();
        r[3] = s.pnl_at_expiry ? money(s.pnl_at_expiry->raw(), true) : blank();
        r[4] = real(s.delta, 2, true);
        r[5] = real(s.gamma, 4);
        r[6] = real(s.vega, 2, true);
        r[7] = real(s.theta, 2, true);
        r[8] = s.hedge_lots ? real(*s.hedge_lots, 2) : blank(QStringLiteral("lot size unknown"));
        r[9] = text(s.complete ? QStringLiteral("yes") : QStringLiteral("NO"));
        if (!s.complete) r[9].colour = kWarn;
        rows << r;
    }
    return rows;
}

// ---- expenses (from the trade book) ------------------------------------------------

[[nodiscard]] inline QVector<GetsColumn> gets_expense_columns() {
    return {
        {QStringLiteral("Symbol")}, {QStringLiteral("Inst type")}, {QStringLiteral("Expiry")},
        {QStringLiteral("Strike")}, {QStringLiteral("Opt type")},
        {QStringLiteral("Buy qty")}, {QStringLiteral("Buy amt")}, {QStringLiteral("Buy avg")},
        {QStringLiteral("Sell qty")}, {QStringLiteral("Sell amt")}, {QStringLiteral("Sell avg")},
        {QStringLiteral("Orders")},
        {QStringLiteral("Total expense"), true,
         QStringLiteral("Brokerage + STT + exchange + SEBI + stamp + IPFT + GST, per order, from config/charges.toml")},
        {QStringLiteral("Realised net"), true,
         QStringLiteral("Sell amt - buy amt - expense, only when bought and sold quantities match")},
    };
}

/// Fills grouped by order, for risk/option_book.hpp's book_expenses().
[[nodiscard]] inline std::vector<BookFill> gets_fills(const GetsAccount& a,
                                                      std::vector<GetsInstrument>& instruments,
                                                      const MasterIndex* master, int reference_year) {
    std::vector<BookFill> fills;
    QStringList order_keys;
    for (const auto& t : a.trades) {
        std::uint32_t id = 0;
        bool found = false;
        for (std::size_t i = 0; i < instruments.size(); ++i)
            if (instruments[i].symbol == t.symbol) { id = static_cast<std::uint32_t>(i); found = true; }
        if (!found) {
            instruments.push_back(resolve_gets_instrument(t.symbol, master, reference_year));
            id = static_cast<std::uint32_t>(instruments.size() - 1);
        }
        const std::int64_t value = t.value ? *t.value
            : (t.price ? *t.price * t.qty : 0);
        const QString key = t.order_number.isEmpty() ? t.trade_number
                                                     : t.order_number + QLatin1Char('/') + QString::number(t.side);
        const qsizetype at = key.isEmpty() ? -1 : order_keys.indexOf(key);
        if (at >= 0) {
            BookFill& f = fills[static_cast<std::size_t>(at)];
            f.qty = Qty{f.qty.raw() + t.qty};
            f.value = Notional{f.value.raw() + value};
            continue;
        }
        BookFill f{};
        f.instrument = id;
        f.side = t.side == 1 ? Side::Buy : Side::Sell;
        f.qty = Qty{t.qty};
        f.value = Notional{value};
        f.segment = instruments[id].segment;
        f.exchange = instruments[id].exchange;
        f.delivery = t.product == QStringLiteral("CNC");
        f.at = t.at.isValid() ? Timestamp{t.at.toMSecsSinceEpoch() * 1'000'000LL}
                              : Timestamp{a.fetched_at * 1'000'000'000LL};
        fills.push_back(f);
        order_keys << (key.isEmpty() ? QStringLiteral("#%1").arg(fills.size()) : key);
    }
    return fills;
}

[[nodiscard]] inline QVector<GetsRow> gets_expense_rows(const std::vector<BookExpenseRow>& rows_in,
                                                        const std::vector<GetsInstrument>& instruments) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (const auto& e : rows_in) {
        if (e.instrument >= instruments.size()) continue;
        const GetsInstrument& inst = instruments[e.instrument];
        const bool option = inst.parsed && inst.parsed->kind == instruments::FyersSymbolKind::Option;
        GetsRow r(14);
        r[0] = text(inst.symbol);
        r[1] = text(inst.segment == Segment::Opt ? QStringLiteral("OPT")
                    : inst.segment == Segment::Fut ? QStringLiteral("FUT") : QStringLiteral("CASH"));
        r[2] = inst.segment == Segment::Cash ? blank() : date(inst.expiry);
        r[3] = option ? money(inst.parsed->strike.raw()) : blank();
        r[4] = option ? text(inst.parsed->right == instruments::FyersRight::Call ? QStringLiteral("CE")
                                                                                : QStringLiteral("PE"))
                      : blank();
        r[5] = integer(e.buy_qty);
        r[6] = money(e.buy_value.raw());
        r[7] = e.buy_avg ? money(e.buy_avg->raw()) : blank();
        r[8] = integer(e.sell_qty);
        r[9] = money(e.sell_value.raw());
        r[10] = e.sell_avg ? money(e.sell_avg->raw()) : blank();
        r[11] = integer(static_cast<std::int64_t>(e.orders));
        if (e.expense) {
            r[12] = money(e.expense->raw());
        } else {
            QString why = QStringLiteral("charges unavailable");
            if (e.expense_error == CostError::NoSchedule)
                why = QStringLiteral("config/charges.toml not loaded in this build, or no schedule for the trade date");
            else if (e.expense_error == CostError::UnverifiedSchedule)
                why = QStringLiteral("config/charges.toml is not marked verified");
            else if (e.expense_error == CostError::UnknownSegment)
                why = QStringLiteral("no charge line for this segment");
            r[12] = blank(why);
        }
        if (e.buy_qty == e.sell_qty && e.buy_qty > 0 && e.expense && !e.overflow) {
            const std::int64_t net = e.sell_value.raw() - e.buy_value.raw() - e.expense->raw();
            r[13] = money(net, true);
        } else {
            r[13] = blank(QStringLiteral("open quantity remains"));
        }
        rows << r;
    }
    return rows;
}

// ---- trade history -----------------------------------------------------------

[[nodiscard]] inline QVector<GetsColumn> gets_trade_columns() {
    return {
        {QStringLiteral("Time")}, {QStringLiteral("Symbol")}, {QStringLiteral("Side")},
        {QStringLiteral("Qty")}, {QStringLiteral("Price")}, {QStringLiteral("Value")},
        {QStringLiteral("Product")}, {QStringLiteral("Order no")}, {QStringLiteral("Trade no")},
    };
}

[[nodiscard]] inline QVector<GetsRow> gets_trade_rows(const GetsAccount& a) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (const auto& t : a.trades) {
        GetsRow r(9);
        r[0] = time(t.at);
        r[1] = text(t.symbol);
        r[2] = text(t.side == 1 ? QStringLiteral("BUY") : QStringLiteral("SELL"));
        r[2].colour = t.side == 1 ? kUp : kDown;
        r[3] = integer(t.qty);
        r[4] = money(t.price);
        r[5] = money(t.value);
        r[6] = text(t.product);
        r[7] = text(t.order_number);
        r[8] = text(t.trade_number);
        rows << r;
    }
    return rows;
}

// ---- RMS -----------------------------------------------------------------------

struct GetsRmsLimits {
    double warn_utilisation_pct{80.0};
    double stop_utilisation_pct{95.0};
    int max_open_positions{50};              ///< config/altair.toml risk.max_open_positions
    std::int64_t max_loss_paise{0};          ///< 0: no MTM loss limit set
};

[[nodiscard]] inline QVector<GetsColumn> gets_rms_columns() {
    return {{QStringLiteral("Metric")}, {QStringLiteral("Value")}, {QStringLiteral("Limit")},
            {QStringLiteral("Status")}, {QStringLiteral("Source")}};
}

[[nodiscard]] inline QVector<GetsRow> gets_rms_rows(const GetsAccount& a, const GetsBook& b,
                                                    const GetsRmsLimits& lim) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    const auto status = [](const QString& s, const QColor& c) { GetsCell x = text(s); x.colour = c; return x; };
    const auto row = [&rows](GetsCell m, GetsCell v, GetsCell l, GetsCell s, const QString& src) {
        rows << GetsRow{std::move(m), std::move(v), std::move(l), std::move(s), gets_cell::text(src)};
    };
    const auto total = gets_fund(a, 1);
    const auto used = gets_fund(a, 2);
    const auto avail = gets_fund(a, 10);
    row(text(QStringLiteral("Total balance")), money(total), blank(), text(QString()), QStringLiteral("funds id 1"));
    row(text(QStringLiteral("Utilised margin")), money(used), blank(), text(QString()), QStringLiteral("funds id 2"));
    row(text(QStringLiteral("Available balance")), money(avail), blank(), text(QString()), QStringLiteral("funds id 10"));
    if (total && used && *total > 0) {
        const double pct = 100.0 * static_cast<double>(*used) / static_cast<double>(*total);
        const bool stop = pct >= lim.stop_utilisation_pct;
        const bool warn = pct >= lim.warn_utilisation_pct;
        row(text(QStringLiteral("Margin utilisation")), real(pct, 1, false, QStringLiteral("%")),
            text(QStringLiteral("warn %1% / stop %2%").arg(lim.warn_utilisation_pct).arg(lim.stop_utilisation_pct)),
            stop ? status(QStringLiteral("BREACH"), kDown) : warn ? status(QStringLiteral("WARN"), kWarn)
                 : status(QStringLiteral("OK"), kUp),
            QStringLiteral("utilised / total"));
    } else {
        row(text(QStringLiteral("Margin utilisation")), blank(QStringLiteral("funds not available")),
            blank(), status(QStringLiteral("UNKNOWN"), kWarn), QStringLiteral("utilised / total"));
    }
    std::int64_t open = 0;
    std::int64_t realized = 0;
    bool realized_known = a.positions_ok;
    for (const auto& p : a.positions) {
        if (p.net_qty != 0) ++open;
        if (p.realized) realized += *p.realized;
        else realized_known = false;
    }
    row(text(QStringLiteral("Open positions")), a.positions_ok ? integer(open) : blank(),
        integer(lim.max_open_positions),
        !a.positions_ok ? status(QStringLiteral("UNKNOWN"), kWarn)
            : open > lim.max_open_positions ? status(QStringLiteral("BREACH"), kDown)
                                            : status(QStringLiteral("OK"), kUp),
        QStringLiteral("positions; limit from config/altair.toml"));
    std::optional<std::int64_t> mtm = std::int64_t{0};
    for (std::size_t i = 0; i < b.views.size(); ++i) {
        if (b.watch_only[i]) continue;
        if (b.views[i].mtm && mtm) *mtm += b.views[i].mtm->raw();
        else mtm.reset();
    }
    row(text(QStringLiteral("Unrealised MtoM")), money(mtm, true),
        lim.max_loss_paise > 0 ? money(-lim.max_loss_paise) : blank(QStringLiteral("no loss limit set")),
        !mtm ? status(QStringLiteral("PARTIAL"), kWarn)
            : (lim.max_loss_paise > 0 && *mtm <= -lim.max_loss_paise) ? status(QStringLiteral("BREACH"), kDown)
                                                                      : status(QStringLiteral("OK"), kUp),
        QStringLiteral("LTP vs net average"));
    row(text(QStringLiteral("Realised P&L (today)")), realized_known ? money(realized, true) : blank(),
        blank(), text(QString()), QStringLiteral("broker value"));
    std::int64_t pending = 0;
    std::int64_t rejected = 0;
    for (const auto& o : a.orders) {
        if (o.status == static_cast<int>(GetsOrderStatus::Pending)
            || o.status == static_cast<int>(GetsOrderStatus::Transit)) ++pending;
        if (o.status == static_cast<int>(GetsOrderStatus::Rejected)) ++rejected;
    }
    row(text(QStringLiteral("Pending orders")), a.orders_ok ? integer(pending) : blank(), blank(),
        text(QString()), QStringLiteral("order book"));
    row(text(QStringLiteral("Rejected orders")), a.orders_ok ? integer(rejected) : blank(), blank(),
        rejected > 0 ? status(QStringLiteral("CHECK"), kWarn) : status(QStringLiteral("OK"), kUp),
        QStringLiteral("order book"));
    return rows;
}

[[nodiscard]] inline QVector<GetsColumn> gets_rejection_columns() {
    return {{QStringLiteral("Time")}, {QStringLiteral("Symbol")}, {QStringLiteral("Side")},
            {QStringLiteral("Qty")}, {QStringLiteral("Price")}, {QStringLiteral("Product")},
            {QStringLiteral("Order id")}, {QStringLiteral("Reason")}};
}

[[nodiscard]] inline QVector<GetsRow> gets_rejection_rows(const GetsAccount& a) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (const auto& o : a.orders) {
        if (o.status != static_cast<int>(GetsOrderStatus::Rejected)) continue;
        GetsRow r(8);
        r[0] = time(o.at);
        r[1] = text(o.symbol);
        r[2] = text(o.side == 1 ? QStringLiteral("BUY") : QStringLiteral("SELL"));
        r[3] = integer(o.qty);
        r[4] = o.type == 2 || o.type == 3 ? text(QStringLiteral("MKT")) : money(o.limit_price);
        r[5] = text(o.product);
        r[6] = text(o.id);
        r[7] = text(o.message.isEmpty() ? QStringLiteral("(no reason given)") : o.message, o.message);
        r[7].colour = kDown;
        rows << r;
    }
    return rows;
}

// ---- movers and index information ----------------------------------------------

[[nodiscard]] inline QVector<GetsColumn> gets_mover_columns() {
    return {{QStringLiteral("#")}, {QStringLiteral("Symbol")}, {QStringLiteral("LTP")},
            {QStringLiteral("Change")}, {QStringLiteral("% Change")}, {QStringLiteral("Open")},
            {QStringLiteral("High")}, {QStringLiteral("Low")}, {QStringLiteral("Prev close")},
            {QStringLiteral("Volume")}};
}

/// Top `n` gainers (or losers) among indices or stocks in `universe`.
/// Symbols without a quote or a % change are left out and counted.
[[nodiscard]] inline QVector<GetsRow> gets_mover_rows(const GetsQuotes& q, const QStringList& universe,
                                                      bool indices, bool gainers, int n,
                                                      int* unquoted = nullptr) {
    using namespace gets_cell;
    std::vector<const GetsQuote*> pool;
    int missing = 0;
    for (const QString& s : universe) {
        if (s.endsWith(QStringLiteral("-INDEX")) != indices) continue;
        const auto it = q.by_symbol.constFind(s);
        if (it == q.by_symbol.constEnd() || !it->ok || !it->change_pct) { ++missing; continue; }
        pool.push_back(&*it);
    }
    std::stable_sort(pool.begin(), pool.end(), [gainers](const GetsQuote* x, const GetsQuote* y) {
        return gainers ? *x->change_pct > *y->change_pct : *x->change_pct < *y->change_pct;
    });
    if (unquoted) *unquoted = missing;
    QVector<GetsRow> rows;
    int rank = 0;
    for (const GetsQuote* g : pool) {
        if (rank == n) break;   // top n, by request
        if (gainers ? *g->change_pct < 0.0 : *g->change_pct > 0.0) break;
        GetsRow r(10);
        r[0] = integer(++rank);
        r[1] = text(g->symbol, g->description);
        r[2] = money(g->ltp);
        r[3] = money(g->change, true);
        r[4] = real(g->change_pct, 2, true, QStringLiteral("%"));
        r[5] = money(g->open);
        r[6] = money(g->high);
        r[7] = money(g->low);
        r[8] = money(g->prev_close);
        r[9] = g->volume && !indices ? integer(*g->volume) : blank();
        rows << r;
    }
    return rows;
}

[[nodiscard]] inline QVector<GetsColumn> gets_index_columns() {
    return {{QStringLiteral("Index")}, {QStringLiteral("LTP")}, {QStringLiteral("Change")},
            {QStringLiteral("% Change")}, {QStringLiteral("Open")}, {QStringLiteral("High")},
            {QStringLiteral("Low")}, {QStringLiteral("Prev close")},
            {QStringLiteral("Day range %"), true, QStringLiteral("Where LTP sits between today's low (0%) and high (100%)")},
            {QStringLiteral("Last trade")}, {QStringLiteral("Status")}};
}

[[nodiscard]] inline QVector<GetsRow> gets_index_rows(const GetsQuotes& q, const QStringList& universe) {
    using namespace gets_cell;
    QVector<GetsRow> rows;
    for (const QString& s : universe) {
        if (!s.endsWith(QStringLiteral("-INDEX"))) continue;
        GetsRow r(11);
        r[0] = text(s);
        const auto it = q.by_symbol.constFind(s);
        if (it == q.by_symbol.constEnd() || !it->ok) {
            for (int c = 1; c < 10; ++c) r[c] = blank();
            r[10] = text(it == q.by_symbol.constEnd() ? QStringLiteral("not in quotes") : it->error);
            r[10].colour = kWarn;
            rows << r;
            continue;
        }
        const GetsQuote& g = *it;
        r[1] = money(g.ltp);
        r[2] = money(g.change, true);
        r[3] = real(g.change_pct, 2, true, QStringLiteral("%"));
        r[4] = money(g.open);
        r[5] = money(g.high);
        r[6] = money(g.low);
        r[7] = money(g.prev_close);
        if (g.ltp && g.high && g.low && *g.high > *g.low)
            r[8] = real(100.0 * static_cast<double>(*g.ltp - *g.low) / static_cast<double>(*g.high - *g.low), 1,
                        false, QStringLiteral("%"));
        else
            r[8] = blank();
        r[9] = g.time > 0 ? time(QDateTime::fromSecsSinceEpoch(g.time)) : blank();
        r[10] = text(QStringLiteral("OK"));
        rows << r;
    }
    return rows;
}

} // namespace altair::ui
