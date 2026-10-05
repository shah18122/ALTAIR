// desktop/live_trading.hpp -- the Terminal's LIVE switch and its order book.
//
// The desktop still holds no handle that can place an order. LIVE writes
// data/live_trading.json (armed, until when, the limits) after a typed
// confirmation, and starts altair_order_router; an order is a request appended
// to data/order_intents.jsonl (order_ticket.hpp) after a confirmation of its
// own; the router -- oms/live_router.hpp -- decides, sends to FYERS and writes
// data/live_orders/orders.json, which the window below shows. A cancel is a
// line in data/live_orders/cancels.jsonl. Nothing here talks to a broker.
//
// OFF BY DEFAULT, AND OFF AGAIN BY ITSELF: an arm lasts until 15:30 IST the
// day it was given (never overnight); switching LIVE off rewrites the file
// with "armed": false. The kill switch (kill_switch.hpp) stops the router
// sending and cancels what is open.
#pragma once

#include "order_ticket.hpp"
#include "paper_windows.hpp"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>

#include <cstdint>
#include <functional>
#include <optional>

namespace altair::ui {

inline constexpr const char* kLiveTradingArmFile = "data/live_trading.json";
inline constexpr const char* kLiveTradingOrdersFile = "data/live_orders/orders.json";
inline constexpr const char* kLiveTradingCancelsFile = "data/live_orders/cancels.jsonl";
inline constexpr const char* kLiveTradingIntentFile = "data/order_intents.jsonl";
/// The router writes orders.json at least every two seconds; older is "not running".
inline constexpr std::int64_t kLiveTradingBeatNs = 6'000'000'000LL;

/// The limits the router enforces, as the arm dialog edits them. Rupees.
struct LiveTradingLimits {
    int max_lots = 1;
    double max_order_value = 25'00'000.0;
    int max_orders_per_day = 20;
    int max_open_orders = 5;
    double max_daily_loss = 5'000.0;
    double price_band_pct = 3.0;
};

struct LiveTradingArm {
    bool present = false;
    bool armed = false;          ///< armed and not expired
    qint64 expires_unix = 0;
    QString by;
    LiveTradingLimits limits;
};

[[nodiscard]] inline LiveTradingArm read_live_arm(const QString& path, qint64 now_unix) {
    LiveTradingArm a;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return a;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    a.present = !o.isEmpty();
    a.expires_unix = static_cast<qint64>(o.value(QStringLiteral("expires_unix")).toDouble());
    a.armed = o.value(QStringLiteral("armed")).toBool() && a.expires_unix > now_unix;
    a.by = o.value(QStringLiteral("by")).toString();
    LiveTradingLimits& l = a.limits;
    l.max_lots = o.value(QStringLiteral("max_lots")).toInt(l.max_lots);
    l.max_order_value = o.value(QStringLiteral("max_order_value")).toDouble(l.max_order_value);
    l.max_orders_per_day = o.value(QStringLiteral("max_orders_per_day")).toInt(l.max_orders_per_day);
    l.max_open_orders = o.value(QStringLiteral("max_open_orders")).toInt(l.max_open_orders);
    l.max_daily_loss = o.value(QStringLiteral("max_daily_loss")).toDouble(l.max_daily_loss);
    l.price_band_pct = o.value(QStringLiteral("price_band_pct")).toDouble(l.price_band_pct);
    return a;
}

/// When an arm given now ends: 15:30 IST today, or half an hour from now when
/// that has passed (the router refuses orders after 15:30 anyway).
[[nodiscard]] inline qint64 live_arm_expiry(const QDateTime& now) {
    const QDateTime ist = now.toTimeZone(QTimeZone(19'800));
    const QDateTime close(ist.date(), QTime(15, 30), QTimeZone(19'800));
    const qint64 n = now.toSecsSinceEpoch(), c = close.toSecsSinceEpoch();
    return c > n + 60 ? c : n + 1800;
}

/// Write the arm file in one step (QSaveFile: a reader sees the old file or the new).
[[nodiscard]] inline bool write_live_arm(const QString& path, bool armed, const QString& by, const LiveTradingLimits& l,
                                         const QDateTime& now) {
    QJsonObject o;
    o[QStringLiteral("armed")] = armed;
    o[QStringLiteral("by")] = by;
    o[QStringLiteral("armed_unix")] = static_cast<double>(now.toSecsSinceEpoch());
    o[QStringLiteral("expires_unix")] = static_cast<double>(armed ? live_arm_expiry(now) : now.toSecsSinceEpoch());
    o[QStringLiteral("max_lots")] = l.max_lots;
    o[QStringLiteral("max_order_value")] = l.max_order_value;
    o[QStringLiteral("max_orders_per_day")] = l.max_orders_per_day;
    o[QStringLiteral("max_open_orders")] = l.max_open_orders;
    o[QStringLiteral("max_daily_loss")] = l.max_daily_loss;
    o[QStringLiteral("price_band_pct")] = l.price_band_pct;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

/// One row of the router's book.
struct LiveTradingOrder {
    qint64 at_ns = 0;
    QString intent, id, symbol, side, type, product, status, message;
    qint64 lots = 0, qty = 0, limit_paise = 0, filled = 0, avg_paise = 0;
    [[nodiscard]] bool open() const {
        return status == QLatin1String("PENDING") || status == QLatin1String("OPEN") || status == QLatin1String("UNCERTAIN");
    }
};

/// What the router last wrote.
struct LiveTradingView {
    bool present = false;
    bool running = false;          ///< heartbeat fresh
    bool armed = false, killed = false, dry_run = false, session = false;
    QString why;
    std::optional<qint64> day_pnl_paise;
    qint64 orders_today = 0, open = 0, max_orders_per_day = 0, max_daily_loss_paise = 0;
    QVector<LiveTradingOrder> orders;
};

[[nodiscard]] inline LiveTradingView read_live_router(const QString& path, qint64 now_ns) {
    LiveTradingView v;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return v;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject r = root.value(QStringLiteral("router")).toObject();
    if (r.isEmpty()) return v;
    v.present = true;
    const qint64 beat = static_cast<qint64>(r.value(QStringLiteral("beat_ns")).toDouble());
    v.running = beat > 0 && now_ns - beat < kLiveTradingBeatNs && now_ns - beat > -kLiveTradingBeatNs;
    v.armed = r.value(QStringLiteral("armed")).toBool();
    v.killed = r.value(QStringLiteral("killed")).toBool();
    v.dry_run = r.value(QStringLiteral("dry_run")).toBool();
    v.session = r.value(QStringLiteral("session")).toBool();
    v.why = r.value(QStringLiteral("why")).toString();
    if (r.value(QStringLiteral("day_pnl_paise")).isDouble())
        v.day_pnl_paise = static_cast<qint64>(r.value(QStringLiteral("day_pnl_paise")).toDouble());
    v.orders_today = static_cast<qint64>(r.value(QStringLiteral("orders_today")).toDouble());
    v.open = static_cast<qint64>(r.value(QStringLiteral("open")).toDouble());
    v.max_orders_per_day = static_cast<qint64>(r.value(QStringLiteral("max_orders_per_day")).toDouble());
    v.max_daily_loss_paise = static_cast<qint64>(r.value(QStringLiteral("max_daily_loss_paise")).toDouble());
    for (const QJsonValue x : root.value(QStringLiteral("orders")).toArray()) {
        const QJsonObject o = x.toObject();
        LiveTradingOrder w;
        w.at_ns = static_cast<qint64>(o.value(QStringLiteral("at_ns")).toDouble());
        w.intent = o.value(QStringLiteral("intent")).toString();
        w.id = o.value(QStringLiteral("id")).toString();
        w.symbol = o.value(QStringLiteral("symbol")).toString();
        w.side = o.value(QStringLiteral("side")).toString();
        w.type = o.value(QStringLiteral("type")).toString();
        w.product = o.value(QStringLiteral("product")).toString();
        w.status = o.value(QStringLiteral("status")).toString();
        w.message = o.value(QStringLiteral("message")).toString();
        w.lots = static_cast<qint64>(o.value(QStringLiteral("lots")).toDouble());
        w.qty = static_cast<qint64>(o.value(QStringLiteral("qty")).toDouble());
        w.limit_paise = static_cast<qint64>(o.value(QStringLiteral("limit_paise")).toDouble());
        w.filled = static_cast<qint64>(o.value(QStringLiteral("filled")).toDouble());
        w.avg_paise = static_cast<qint64>(o.value(QStringLiteral("avg_paise")).toDouble());
        v.orders.push_back(w);
    }
    return v;
}

/// Ask the router to cancel one order (by FYERS id) or every open one.
[[nodiscard]] inline bool append_live_cancel(const QString& path, const QString& fyers_id, bool all) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(10'000);
    if (!lock.tryLock(2000)) return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    QJsonObject o;
    if (all) o[QStringLiteral("all")] = true;
    else o[QStringLiteral("id")] = fyers_id;
    o[QStringLiteral("at_unix")] = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    return f.write(line) == line.size() && f.flush();
}

/// The confirmation every live order asks for: what will be sent, in full.
[[nodiscard]] inline QString live_order_confirmation(const IntentDraft& d, qint64 lot, qint64 ref_paise) {
    const qint64 qty = static_cast<qint64>(d.lots) * lot;
    const qint64 px = d.market ? ref_paise : d.limit_paise;
    const QString price = d.market ? QStringLiteral("<b>MARKET</b>") : QStringLiteral("<b>LIMIT Rs %1</b>").arg(rupees_text(d.limit_paise));
    const QString value = px > 0 ? QStringLiteral("about <b>Rs %1</b>%2").arg(rupees_text(px * qty),
                                                                              d.market ? QStringLiteral(" at the last price") : QString())
                                 : QStringLiteral("value unknown (no price yet)");
    return QStringLiteral(
               "<p style='color:#FF7B72;font-weight:700'>REAL ORDER &middot; FYERS &middot; REAL MONEY</p>"
               "<p><b>%1 %2 lot(s) = %3</b> of <b>%4</b> on %5<br>%6 &middot; %7 &middot; %8<br>%9</p>"
               "<p>The order router checks it against your limits and FYERS's last price, then sends it. "
               "Follow it in Live orders.</p>")
        .arg(d.buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
        .arg(d.lots)
        .arg(qty)
        .arg(d.symbol.toHtmlEscaped(), d.exchange.toHtmlEscaped(), price, d.product.toHtmlEscaped(), d.validity.toHtmlEscaped(),
             value);
}

/// Switch LIVE on: the limits, and the word LIVE typed out.
class LiveTradingArmDialog final : public QDialog {
public:
    LiveTradingArmDialog(const LiveTradingLimits& start, QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("liveArmDialog"));
        setWindowTitle(QStringLiteral("Switch LIVE trading on"));
        setModal(true);
        setMinimumWidth(560);
        auto* v = new QVBoxLayout(this);
        auto* warn = new QLabel(QStringLiteral(
            "<p style='color:#FF7B72;font-weight:700;font-size:14px'>LIVE: orders go to FYERS with real money.</p>"
            "<p>Until you switch LIVE off, or 15:30 today, Buy (F1) and Sell (F2) in this Terminal send real orders. "
            "Each one asks you to confirm first. The order router refuses anything outside these limits, and the "
            "kill switch (Operations) stops it and cancels what is open.</p>"
            "<p><b>Test with 1 lot first.</b></p>"), this);
        warn->setWordWrap(true);
        warn->setTextFormat(Qt::RichText);
        v->addWidget(warn);
        auto* form = new QFormLayout;
        lots_ = new QSpinBox(this);
        lots_->setRange(1, 50);
        lots_->setValue(start.max_lots);
        value_ = new QDoubleSpinBox(this);
        value_->setRange(1'000.0, 1'00'00'000.0);
        value_->setDecimals(0);
        value_->setSingleStep(50'000.0);
        value_->setValue(start.max_order_value);
        orders_ = new QSpinBox(this);
        orders_->setRange(1, 200);
        orders_->setValue(start.max_orders_per_day);
        open_ = new QSpinBox(this);
        open_->setRange(1, 50);
        open_->setValue(start.max_open_orders);
        loss_ = new QDoubleSpinBox(this);
        loss_->setRange(100.0, 10'00'000.0);
        loss_->setDecimals(0);
        loss_->setSingleStep(1'000.0);
        loss_->setValue(start.max_daily_loss);
        band_ = new QDoubleSpinBox(this);
        band_->setRange(0.5, 20.0);
        band_->setDecimals(1);
        band_->setSuffix(QStringLiteral(" %"));
        band_->setValue(start.price_band_pct);
        form->addRow(QStringLiteral("Most lots per order"), lots_);
        form->addRow(QStringLiteral("Largest order value (Rs, price × quantity)"), value_);
        form->addRow(QStringLiteral("Most orders today"), orders_);
        form->addRow(QStringLiteral("Most open orders at once"), open_);
        form->addRow(QStringLiteral("Stop new orders at a loss today of (Rs)"), loss_);
        form->addRow(QStringLiteral("Refuse a limit this far from the last price"), band_);
        v->addLayout(form);
        auto* type_row = new QHBoxLayout;
        type_row->addWidget(new QLabel(QStringLiteral("Type <b>LIVE</b> to switch on:"), this));
        phrase_ = new QLineEdit(this);
        phrase_->setObjectName(QStringLiteral("liveArmPhrase"));
        type_row->addWidget(phrase_);
        v->addLayout(type_row);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        ok_ = buttons->button(QDialogButtonBox::Ok);
        ok_->setText(QStringLiteral("Switch LIVE on"));
        ok_->setEnabled(false);
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
        v->addWidget(buttons);
        connect(phrase_, &QLineEdit::textChanged, this, [this](const QString& t) { ok_->setEnabled(t == QLatin1String("LIVE")); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (phrase_->text() == QLatin1String("LIVE")) accept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    [[nodiscard]] LiveTradingLimits limits() const {
        LiveTradingLimits l;
        l.max_lots = lots_->value();
        l.max_order_value = value_->value();
        l.max_orders_per_day = orders_->value();
        l.max_open_orders = open_->value();
        l.max_daily_loss = loss_->value();
        l.price_band_pct = band_->value();
        return l;
    }
    /// For tests: type the phrase.
    void type_phrase(const QString& t) { phrase_->setText(t); }
    [[nodiscard]] bool can_accept() const { return ok_->isEnabled(); }

private:
    QSpinBox* lots_ = nullptr;
    QDoubleSpinBox* value_ = nullptr;
    QSpinBox* orders_ = nullptr;
    QSpinBox* open_ = nullptr;
    QDoubleSpinBox* loss_ = nullptr;
    QDoubleSpinBox* band_ = nullptr;
    QLineEdit* phrase_ = nullptr;
    QPushButton* ok_ = nullptr;
};

/// The router's book: every live request and what became of it.
class LiveTradingOrdersWindow final : public QDialog {
public:
    explicit LiveTradingOrdersWindow(QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("liveOrdersWindow"));
        paper_ui::tool_window(this, QStringLiteral("Live orders · FYERS"), QSize(1080, 360));
        auto* v = new QVBoxLayout(this);
        state_ = new QLabel(this);
        state_->setTextFormat(Qt::RichText);
        state_->setWordWrap(true);
        v->addWidget(state_);
        auto* bar = new QHBoxLayout;
        bar->addStretch();
        auto* cancel = new QPushButton(QStringLiteral("Cancel selected"), this);
        auto* all = new QPushButton(QStringLiteral("Cancel all open"), this);
        bar->addWidget(cancel);
        bar->addWidget(all);
        v->addLayout(bar);
        table_ = paper_ui::table({QStringLiteral("Time"), QStringLiteral("Symbol"), QStringLiteral("B/S"), QStringLiteral("Qty"),
                                  QStringLiteral("Type"), QStringLiteral("Limit"), QStringLiteral("Status"), QStringLiteral("Filled"),
                                  QStringLiteral("Avg"), QStringLiteral("FYERS id"), QStringLiteral("Message")},
                                 this);
        table_->setObjectName(QStringLiteral("liveOrderTable"));
        table_->setColumnWidth(1, 190);
        table_->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(table_, 1);
        connect(cancel, &QPushButton::clicked, this, [this] {
            const int r = table_->currentRow();
            if (r < 0 || r >= table_->rowCount() || !on_cancel) return;
            const QTableWidgetItem* id = table_->item(r, 9);
            if (id != nullptr && !id->text().isEmpty()) on_cancel(id->text(), false);
        });
        connect(all, &QPushButton::clicked, this, [this] { if (on_cancel) on_cancel(QString(), true); });
    }

    std::function<void(const QString& id, bool all)> on_cancel;

    void refresh(const LiveTradingView& v) {
        QString s;
        if (!v.present) {
            s = QStringLiteral("<span style='color:#8B949E'>The order router has not run yet. Switch LIVE on in the Terminal to start it.</span>");
        } else {
            const QString run = v.running ? QStringLiteral("<span style='color:#7EE787'>router running</span>")
                                          : QStringLiteral("<span style='color:#FF7B72'>router NOT running</span>");
            const QString mode = v.dry_run ? QStringLiteral(" · <b>DRY RUN</b>")
                               : v.armed && v.why.isEmpty() ? QStringLiteral(" · <span style='color:#FF7B72;font-weight:700'>LIVE</span>")
                                                            : QString();
            const QString pnl = v.day_pnl_paise ? QStringLiteral(" · P&amp;L today Rs %1").arg(rupees_text(*v.day_pnl_paise)) : QString();
            const QString why = v.why.isEmpty() ? QString() : QStringLiteral("<br><span style='color:#E3B341'>Refusing new orders: %1</span>")
                                                                  .arg(v.why.toHtmlEscaped());
            s = QStringLiteral("%1%2%3 · orders today %4/%5 · open %6%7")
                    .arg(run, mode, pnl)
                    .arg(v.orders_today)
                    .arg(v.max_orders_per_day)
                    .arg(v.open)
                    .arg(why);
        }
        state_->setText(s);
        table_->setRowCount(static_cast<int>(v.orders.size()));
        for (int i = 0; i < v.orders.size(); ++i) {
            const LiveTradingOrder& o = v.orders[v.orders.size() - 1 - i];   // newest first
            const QColor fg = o.status == QLatin1String("FILLED") ? QColor(QStringLiteral("#7EE787"))
                            : o.open() ? QColor(QStringLiteral("#E3B341"))
                            : QColor(QStringLiteral("#FF7B72"));
            paper_ui::cell(table_, i, 0, paper_ui::hhmmss(o.at_ns));
            paper_ui::cell(table_, i, 1, o.symbol);
            paper_ui::cell(table_, i, 2, o.side == QLatin1String("BUY") ? QStringLiteral("B") : QStringLiteral("S"), false,
                           QColor(QString::fromLatin1(o.side == QLatin1String("BUY") ? paper_ui::kBuyColour : paper_ui::kSellColour)));
            paper_ui::cell(table_, i, 3, QString::number(o.qty), true);
            paper_ui::cell(table_, i, 4, o.type);
            paper_ui::cell(table_, i, 5, o.limit_paise > 0 ? rupees_text(o.limit_paise) : QStringLiteral("—"), true);
            paper_ui::cell(table_, i, 6, o.status, false, o.status == QLatin1String("REFUSED") || o.status == QLatin1String("DRY RUN")
                                                             ? QColor(QStringLiteral("#8B949E")) : fg);
            paper_ui::cell(table_, i, 7, QString::number(o.filled), true);
            paper_ui::cell(table_, i, 8, o.avg_paise > 0 ? rupees_text(o.avg_paise) : QStringLiteral("—"), true);
            paper_ui::cell(table_, i, 9, o.id);
            paper_ui::cell(table_, i, 10, o.message);
        }
    }

private:
    QLabel* state_ = nullptr;
    QTableWidget* table_ = nullptr;
};

} // namespace altair::ui
