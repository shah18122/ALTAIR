// desktop/paper_windows.hpp -- the GETS-style windows around the paper book.
//
//   + / F1   buy order entry        - / F2   sell order entry
//   F3       order book             F8       trade book
//   Alt+F6   net position (and the broker account and GETS tabs)
//   F5/F6    market picture (five best bids and offers), Shift+F9 snap quote
//   Shift+F7 security information   F10      message log
//   Shift+F1 cancel the selected pending order, Shift+F2 modify it,
//   Shift+F3 cancel every pending order
//
// Every window says PAPER. Nothing here reaches a broker.

#pragma once

#include "live_market.hpp"
#include "paper_oms.hpp"
#include "price_client.hpp"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <functional>

namespace altair::ui {

namespace paper_ui {

inline constexpr const char* kBuyColour = "#1F6FEB";
inline constexpr const char* kSellColour = "#DA3633";

/// The market for `token` as the stream last showed it, with the feed times
/// the paper book judges freshness by. The touch comes from the quote topic;
/// the five levels from the book topic, each with its own stamp.
[[nodiscard]] inline PaperQuote quote_of(const PriceClient* client, quint32 token) {
    PaperQuote q;
    const LivePrice* p = client != nullptr ? client->price(token) : nullptr;
    if (p == nullptr) return q;
    q.ltp = p->last_paise;
    q.ns = p->last_ns;
    q.sim = p->simulated;
    if (p->has_quote && p->quote.has(kQuoteHasTop)) {
        q.bid = p->quote.bid; q.ask = p->quote.ask;
        q.bid_qty = p->quote.bid_qty; q.ask_qty = p->quote.ask_qty;
        q.quote_ns = p->quote_ns;
    }
    if (p->levels > 0) {
        q.levels = std::min<int>(p->levels, 5);
        q.book_ns = p->book_ns;
        for (int k = 0; k < q.levels; ++k) {
            q.bids[k] = PaperLevel{p->bids[k].price_paise, p->bids[k].qty};
            q.asks[k] = PaperLevel{p->asks[k].price_paise, p->asks[k].qty};
        }
    }
    return q;
}

[[nodiscard]] inline QString hhmmss(std::int64_t ns) { return live_detail::ist(ns, false); }

} // namespace paper_ui

/// The paper instrument for a market-watch row: NSE for an equity, NFO for
/// F&O; an index is shown but cannot be traded.
[[nodiscard]] inline PaperInstrument paper_instrument_of(const LiveRow& r) {
    PaperInstrument i;
    i.token = r.token;
    i.symbol = r.symbol;
    i.exchange = r.kind == QLatin1String("equity") ? QStringLiteral("NSE") : QStringLiteral("NFO");
    i.lot = std::max<qint64>(1, r.lot);
    i.tick_paise = std::max<qint64>(1, std::llround(r.tick * 100.0));
    i.tradable = r.kind != QLatin1String("index");
    return i;
}

namespace paper_ui {

[[nodiscard]] inline QTableWidget* table(const QStringList& heads, QWidget* parent) {
    auto* t = new QTableWidget(0, static_cast<int>(heads.size()), parent);
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->hide();
    t->verticalHeader()->setDefaultSectionSize(22);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setDefaultSectionSize(92);
    return t;
}

inline void cell(QTableWidget* t, int r, int c, const QString& text, bool number = false, const QColor& fg = {}) {
    auto* it = new QTableWidgetItem(text);
    if (number) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (fg.isValid()) it->setForeground(fg);
    t->setItem(r, c, it);
}

[[nodiscard]] inline QString money(double v) {
    return QLocale(QLocale::English, QLocale::India).toString(v, 'f', 2);
}

inline void tool_window(QDialog* d, const QString& title, QSize size) {
    d->setWindowTitle(title);
    d->setWindowFlag(Qt::Tool, true);
    d->setModal(false);
    d->resize(size);
    d->setAttribute(Qt::WA_DeleteOnClose, false);
}

} // namespace paper_ui

// ---------------------------------------------------------------------------
// Order entry: the blue buy and red sell windows
// ---------------------------------------------------------------------------

class PaperOrderWindow final : public QDialog {
public:
    PaperOrderWindow(PaperSide side, const PaperInstrument& inst, const PriceClient* client,
                     QWidget* parent = nullptr, const PaperOrder* modifying = nullptr)
        : QDialog(parent), inst_(inst), client_(client), side_(side) {
        setObjectName(QStringLiteral("paperOrderWindow"));
        setModal(true);
        setMinimumWidth(620);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 10);
        v->setSpacing(8);
        header_ = new QLabel(this);
        header_->setObjectName(QStringLiteral("orderHeader"));
        header_->setTextFormat(Qt::RichText);
        v->addWidget(header_);

        auto* g = new QGridLayout;
        g->setContentsMargins(12, 0, 12, 0);
        g->setHorizontalSpacing(10);
        const auto head = [this, g](int col, const char* text) {
            auto* l = new QLabel(QString::fromLatin1(text), this);
            l->setStyleSheet(QStringLiteral("color:#8B949E;font-size:10px;font-weight:700;"));
            g->addWidget(l, 0, col);
        };
        head(0, "EXCH"); head(1, "SYMBOL"); head(2, "PRODUCT"); head(3, "TYPE"); head(4, "LOTS");
        head(5, "QTY"); head(6, "PRICE"); head(7, "VALIDITY");
        auto* exch = new QLabel(inst_.exchange, this);
        auto* sym = new QLabel(QStringLiteral("<b>%1</b>").arg(inst_.symbol.toHtmlEscaped()), this);
        product_ = new QComboBox(this);
        product_->addItems(inst_.exchange == QLatin1String("NSE")
                               ? QStringList{QStringLiteral("CNC"), QStringLiteral("MIS")}
                               : QStringList{QStringLiteral("NRML"), QStringLiteral("MIS")});
        type_ = new QComboBox(this);
        type_->addItems({QStringLiteral("MKT"), QStringLiteral("LMT")});
        lots_ = new QSpinBox(this);
        lots_->setRange(1, 100000);
        lots_->setObjectName(QStringLiteral("orderLots"));
        qty_ = new QLabel(this);
        price_ = new QDoubleSpinBox(this);
        price_->setObjectName(QStringLiteral("orderPrice"));
        price_->setDecimals(2);
        price_->setRange(0.0, 10'000'000.0);
        price_->setSingleStep(static_cast<double>(std::max<qint64>(1, inst_.tick_paise)) / 100.0);
        auto* validity = new QLabel(QStringLiteral("DAY"), this);
        g->addWidget(exch, 1, 0);
        g->addWidget(sym, 1, 1);
        g->addWidget(product_, 1, 2);
        g->addWidget(type_, 1, 3);
        g->addWidget(lots_, 1, 4);
        g->addWidget(qty_, 1, 5);
        g->addWidget(price_, 1, 6);
        g->addWidget(validity, 1, 7);
        v->addLayout(g);

        market_ = new QLabel(this);
        market_->setTextFormat(Qt::RichText);
        market_->setContentsMargins(12, 0, 12, 0);
        v->addWidget(market_);
        auto* note = new QLabel(QStringLiteral(
            "PAPER ORDER · market fills at the live ask (buy) or bid (sell); a limit rests until the book "
            "crosses it · nothing reaches a broker · F1/F2 switch side · Enter submits · Esc closes"), this);
        note->setWordWrap(true);
        note->setContentsMargins(12, 0, 12, 0);
        note->setStyleSheet(QStringLiteral("color:#8B949E;font-size:11px;"));
        v->addWidget(note);

        auto* buttons = new QHBoxLayout;
        buttons->setContentsMargins(12, 0, 12, 0);
        buttons->addStretch();
        submit_ = new QPushButton(this);
        submit_->setObjectName(QStringLiteral("orderSubmit"));
        submit_->setDefault(true);
        auto* cancel = new QPushButton(QStringLiteral("Close (Esc)"), this);
        buttons->addWidget(submit_);
        buttons->addWidget(cancel);
        v->addLayout(buttons);
        connect(submit_, &QPushButton::clicked, this, &QDialog::accept);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(type_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_fields(); });
        connect(lots_, &QSpinBox::valueChanged, this, [this](int) { refresh_fields(); });
        auto* buy_key = new QShortcut(QKeySequence(Qt::Key_F1), this);
        connect(buy_key, &QShortcut::activated, this, [this] { set_side(PaperSide::Buy); });
        auto* sell_key = new QShortcut(QKeySequence(Qt::Key_F2), this);
        connect(sell_key, &QShortcut::activated, this, [this] { set_side(PaperSide::Sell); });

        if (modifying != nullptr) {
            modify_id_ = modifying->id;
            product_->setCurrentText(modifying->product);
            type_->setCurrentIndex(modifying->type == PaperType::Limit ? 1 : 0);
            type_->setEnabled(false);
            product_->setEnabled(false);
            lots_->setValue(static_cast<int>(modifying->qty / std::max<qint64>(1, inst_.lot)));
            if (modifying->limit_paise > 0) price_->setValue(static_cast<double>(modifying->limit_paise) / 100.0);
        } else {
            const PaperQuote q = paper_ui::quote_of(client_, inst_.token);
            const qint64 touch = side == PaperSide::Buy ? q.ask : q.bid;
            const qint64 seed = touch > 0 ? touch : q.ltp;
            if (seed > 0) price_->setValue(static_cast<double>(seed) / 100.0);
        }
        if (client_ != nullptr)
            connect(client_, &PriceClient::priceUpdated, this, [this](unsigned tok) {
                if (tok == inst_.token) refresh_market();
            });
        set_side(side);
        refresh_market();
        lots_->setFocus();
        lots_->selectAll();
    }

    [[nodiscard]] PaperSide side() const noexcept { return side_; }
    [[nodiscard]] int modifying() const noexcept { return modify_id_; }
    [[nodiscard]] qint64 qty() const { return static_cast<qint64>(lots_->value()) * std::max<qint64>(1, inst_.lot); }
    [[nodiscard]] qint64 limit_paise() const { return std::llround(price_->value() * 100.0); }

    /// The order as entered.
    [[nodiscard]] PaperOrder order() const {
        PaperOrder o;
        o.inst = inst_;
        o.product = product_->currentText();
        o.side = side_;
        o.type = type_->currentIndex() == 1 ? PaperType::Limit : PaperType::Market;
        o.qty = qty();
        o.limit_paise = o.type == PaperType::Limit ? limit_paise() : 0;
        return o;
    }

    /// For tests: drive the fields without a keyboard.
    void set_for_test(int lots, bool limit, double price) {
        lots_->setValue(lots);
        type_->setCurrentIndex(limit ? 1 : 0);
        price_->setValue(price);
    }

private:
    void set_side(PaperSide s) {
        side_ = s;
        const bool buy = s == PaperSide::Buy;
        const char* colour = buy ? paper_ui::kBuyColour : paper_ui::kSellColour;
        header_->setStyleSheet(QStringLiteral("background:%1;color:#FFFFFF;padding:9px 12px;font-weight:700;font-size:14px;")
                                   .arg(QString::fromLatin1(colour)));
        header_->setText(QStringLiteral("%1 ORDER ENTRY &nbsp;·&nbsp; PAPER%2")
                             .arg(buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                             .arg(modify_id_ > 0 ? QStringLiteral(" &nbsp;·&nbsp; MODIFY #%1").arg(modify_id_) : QString()));
        submit_->setText(modify_id_ > 0 ? QStringLiteral("Modify (Enter)")
                                        : buy ? QStringLiteral("Buy (Enter)") : QStringLiteral("Sell (Enter)"));
        submit_->setStyleSheet(QStringLiteral("background:%1;color:#FFFFFF;border:0;border-radius:5px;padding:7px 18px;font-weight:700;")
                                   .arg(QString::fromLatin1(colour)));
        setWindowTitle(buy ? QStringLiteral("Buy order entry (paper)") : QStringLiteral("Sell order entry (paper)"));
        refresh_fields();
    }

    void refresh_fields() {
        price_->setEnabled(type_->currentIndex() == 1);
        qty_->setText(QStringLiteral("%1").arg(qty()));
    }

    void refresh_market() {
        const PaperQuote q = paper_ui::quote_of(client_, inst_.token);
        const auto px = [](qint64 p) { return p > 0 ? live_detail::px(p) : QStringLiteral("—"); };
        market_->setText(QStringLiteral(
            "<span style='color:#7FB8F0'>Bid <b>%1</b></span> &nbsp; <span style='color:#F0A07F'>Ask <b>%2</b></span>"
            " &nbsp; LTP <b>%3</b> &nbsp; <span style='color:#8B949E'>lot %4 · tick %5%6</span>")
            .arg(px(q.bid), px(q.ask), px(q.ltp)).arg(inst_.lot)
            .arg(static_cast<double>(inst_.tick_paise) / 100.0, 0, 'f', 2)
            .arg(q.sim ? QStringLiteral(" · <span style='color:#F4C95D'>SIM</span>") : QString()));
    }

    PaperInstrument inst_;
    const PriceClient* client_;
    PaperSide side_;
    int modify_id_ = 0;
    QLabel* header_ = nullptr;
    QComboBox* product_ = nullptr;
    QComboBox* type_ = nullptr;
    QSpinBox* lots_ = nullptr;
    QLabel* qty_ = nullptr;
    QDoubleSpinBox* price_ = nullptr;
    QLabel* market_ = nullptr;
    QPushButton* submit_ = nullptr;
};

// ---------------------------------------------------------------------------
// Order book (F3) and trade book (F8)
// ---------------------------------------------------------------------------

class PaperOrderBookWindow final : public QDialog {
public:
    explicit PaperOrderBookWindow(QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("paperOrderBook"));
        paper_ui::tool_window(this, QStringLiteral("Order book (F3) · paper"), QSize(980, 360));
        auto* v = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        filter_ = new QComboBox(this);
        filter_->addItems({QStringLiteral("All"), QStringLiteral("Pending"), QStringLiteral("Executed"),
                           QStringLiteral("Cancelled / rejected")});
        bar->addWidget(new QLabel(QStringLiteral("Show"), this));
        bar->addWidget(filter_);
        bar->addStretch();
        auto* cancel = new QPushButton(QStringLiteral("Cancel (Shift+F1)"), this);
        auto* modify = new QPushButton(QStringLiteral("Modify (Shift+F2)"), this);
        auto* all = new QPushButton(QStringLiteral("Cancel all (Shift+F3)"), this);
        bar->addWidget(cancel);
        bar->addWidget(modify);
        bar->addWidget(all);
        v->addLayout(bar);
        table_ = paper_ui::table({QStringLiteral("Time"), QStringLiteral("Order #"), QStringLiteral("Symbol"),
                                  QStringLiteral("B/S"), QStringLiteral("Product"), QStringLiteral("Type"),
                                  QStringLiteral("Qty"), QStringLiteral("Price"), QStringLiteral("Status"),
                                  QStringLiteral("Filled @"), QStringLiteral("Note")}, this);
        table_->setObjectName(QStringLiteral("paperOrderTable"));
        table_->setColumnWidth(2, 210);
        v->addWidget(table_, 1);
        connect(filter_, &QComboBox::currentIndexChanged, this, [this](int) { if (oms_) refresh(*oms_); });
        connect(cancel, &QPushButton::clicked, this, [this] { if (on_cancel && selected() > 0) on_cancel(selected()); });
        connect(modify, &QPushButton::clicked, this, [this] { if (on_modify && selected() > 0) on_modify(selected()); });
        connect(all, &QPushButton::clicked, this, [this] { if (on_cancel_all) on_cancel_all(); });
        const auto key = [this](QKeySequence k, std::function<void()> fn) {
            auto* sc = new QShortcut(k, this);
            connect(sc, &QShortcut::activated, this, std::move(fn));
        };
        key(QKeySequence(Qt::SHIFT | Qt::Key_F1), [this] { if (on_cancel && selected() > 0) on_cancel(selected()); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F2), [this] { if (on_modify && selected() > 0) on_modify(selected()); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { if (on_cancel_all) on_cancel_all(); });
    }

    void refresh(const PaperOms& oms) {
        oms_ = &oms;
        const int keep = selected();
        const int f = filter_->currentIndex();
        std::vector<const PaperOrder*> rows;
        for (const auto& o : oms.orders()) {
            const bool show = f == 0 || (f == 1 && o.status == PaperStatus::Open)
                           || (f == 2 && o.status == PaperStatus::Filled)
                           || (f == 3 && (o.status == PaperStatus::Cancelled || o.status == PaperStatus::Rejected));
            if (show) rows.push_back(&o);
        }
        // Pending first, then newest first: what can still be acted on is on top.
        std::stable_sort(rows.begin(), rows.end(), [](const PaperOrder* a, const PaperOrder* b) {
            const bool pa = a->status == PaperStatus::Open, pb = b->status == PaperStatus::Open;
            if (pa != pb) return pa;
            return a->id > b->id;
        });
        table_->setRowCount(static_cast<int>(rows.size()));
        for (int r = 0; r < table_->rowCount(); ++r) {
            const PaperOrder& o = *rows[static_cast<std::size_t>(r)];
            const bool buy = o.side == PaperSide::Buy;
            const QColor side_fg = buy ? QColor(0x58, 0xA6, 0xFF) : QColor(0xFF, 0x7B, 0x72);
            paper_ui::cell(table_, r, 0, paper_ui::hhmmss(o.placed_ns));
            paper_ui::cell(table_, r, 1, QString::number(o.id), true);
            table_->item(r, 1)->setData(Qt::UserRole, o.id);
            paper_ui::cell(table_, r, 2, o.inst.symbol);
            paper_ui::cell(table_, r, 3, buy ? QStringLiteral("BUY") : QStringLiteral("SELL"), false, side_fg);
            paper_ui::cell(table_, r, 4, o.product);
            paper_ui::cell(table_, r, 5, o.type == PaperType::Market ? QStringLiteral("MKT") : QStringLiteral("LMT"));
            paper_ui::cell(table_, r, 6, o.filled > 0 && o.filled < o.qty ? QStringLiteral("%1/%2").arg(o.filled).arg(o.qty)
                                                                          : QString::number(o.qty), true);
            paper_ui::cell(table_, r, 7, o.type == PaperType::Limit ? PaperOms::rupees(o.limit_paise) : QStringLiteral("MKT"), true);
            paper_ui::cell(table_, r, 8, paper_status_text(o.status), false,
                           o.status == PaperStatus::Open ? QColor(0xE3, 0xB3, 0x41)
                           : o.status == PaperStatus::Filled ? QColor(0x7E, 0xE7, 0x87) : QColor(0x8B, 0x94, 0x9E));
            paper_ui::cell(table_, r, 9, o.fill_paise > 0 ? PaperOms::rupees(o.fill_paise) : QStringLiteral("—"), true);
            paper_ui::cell(table_, r, 10, o.note + (o.sim ? QStringLiteral(" SIM") : QString()));
            if (o.id == keep) table_->selectRow(r);
        }
        if (table_->currentRow() < 0 && table_->rowCount() > 0) table_->selectRow(0);
    }

    [[nodiscard]] int selected() const {
        const int r = table_->currentRow();
        const auto* it = r >= 0 ? table_->item(r, 1) : nullptr;
        return it != nullptr ? it->data(Qt::UserRole).toInt() : 0;
    }
    [[nodiscard]] QTableWidget* table() const noexcept { return table_; }

    std::function<void(int)> on_cancel, on_modify;
    std::function<void()> on_cancel_all;

private:
    const PaperOms* oms_ = nullptr;
    QComboBox* filter_ = nullptr;
    QTableWidget* table_ = nullptr;
};

class PaperTradeBookWindow final : public QDialog {
public:
    explicit PaperTradeBookWindow(QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("paperTradeBook"));
        paper_ui::tool_window(this, QStringLiteral("Trade book (F8) · paper"), QSize(940, 340));
        auto* v = new QVBoxLayout(this);
        table_ = paper_ui::table({QStringLiteral("Time"), QStringLiteral("Trade #"), QStringLiteral("Order #"),
                                  QStringLiteral("Symbol"), QStringLiteral("B/S"), QStringLiteral("Product"),
                                  QStringLiteral("Qty"), QStringLiteral("Price"), QStringLiteral("Basis"),
                                  QStringLiteral("Expenses")}, this);
        table_->setObjectName(QStringLiteral("paperTradeTable"));
        table_->setColumnWidth(3, 210);
        table_->setColumnWidth(8, 170);
        v->addWidget(table_, 1);
    }
    void refresh(const PaperOms& oms) {
        const auto& t = oms.trades();
        table_->setRowCount(static_cast<int>(t.size()));
        for (int r = 0; r < table_->rowCount(); ++r) {
            const PaperTrade& x = t[t.size() - 1 - static_cast<std::size_t>(r)];   // newest first
            const bool buy = x.side == PaperSide::Buy;
            paper_ui::cell(table_, r, 0, paper_ui::hhmmss(x.ns));
            paper_ui::cell(table_, r, 1, QString::number(x.id), true);
            paper_ui::cell(table_, r, 2, QString::number(x.order_id), true);
            paper_ui::cell(table_, r, 3, x.inst.symbol);
            paper_ui::cell(table_, r, 4, buy ? QStringLiteral("BUY") : QStringLiteral("SELL"), false,
                           buy ? QColor(0x58, 0xA6, 0xFF) : QColor(0xFF, 0x7B, 0x72));
            paper_ui::cell(table_, r, 5, x.product);
            paper_ui::cell(table_, r, 6, QString::number(x.qty), true);
            paper_ui::cell(table_, r, 7, PaperOms::rupees(x.price_paise), true);
            paper_ui::cell(table_, r, 8, x.basis + (x.sim ? QStringLiteral(" · SIM") : QString()));
            paper_ui::cell(table_, r, 9, std::isnan(x.expenses) ? QStringLiteral("unpriced") : paper_ui::money(x.expenses), true);
        }
    }
    [[nodiscard]] QTableWidget* table() const noexcept { return table_; }

private:
    QTableWidget* table_ = nullptr;
};

// ---------------------------------------------------------------------------
// Net position (Alt+F6): the paper book, and the broker account tabs beside it
// ---------------------------------------------------------------------------

class NetPositionWindow final : public QDialog {
public:
    explicit NetPositionWindow(QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("netPositionWindow"));
        paper_ui::tool_window(this, QStringLiteral("Net position (Alt+F6)"), QSize(1180, 560));
        auto* v = new QVBoxLayout(this);
        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("netPositionTabs"));
        tabs_->setDocumentMode(true);
        auto* paper = new QWidget(tabs_);
        auto* pv = new QVBoxLayout(paper);
        pv->setContentsMargins(6, 6, 6, 6);
        totals_ = new QLabel(paper);
        totals_->setTextFormat(Qt::RichText);
        pv->addWidget(totals_);
        table_ = paper_ui::table({QStringLiteral("Symbol"), QStringLiteral("Product"), QStringLiteral("Net Qty"),
                                  QStringLiteral("Buy Qty"), QStringLiteral("Avg Buy"), QStringLiteral("Sell Qty"),
                                  QStringLiteral("Avg Sell"), QStringLiteral("Mark"), QStringLiteral("Realised"),
                                  QStringLiteral("Unrealised"), QStringLiteral("Expenses"), QStringLiteral("Net P&L")}, paper);
        table_->setObjectName(QStringLiteral("paperPositionTable"));
        table_->setColumnWidth(0, 210);
        pv->addWidget(table_, 1);
        auto* bar = new QHBoxLayout;
        auto* square = new QPushButton(QStringLiteral("Square off selected"), paper);
        auto* square_all = new QPushButton(QStringLiteral("Square off all"), paper);
        bar->addWidget(new QLabel(QStringLiteral(
            "<span style='color:#8B949E'>Paper positions from the + / − orders. A long is marked at the bid, a short at "
            "the ask. Square off sends a paper market order for the net quantity.</span>"), paper), 1);
        bar->addWidget(square);
        bar->addWidget(square_all);
        pv->addLayout(bar);
        tabs_->addTab(paper, QStringLiteral("Net position · paper"));
        v->addWidget(tabs_, 1);
        connect(square, &QPushButton::clicked, this, [this] {
            const int r = table_->currentRow();
            if (r >= 0 && r < static_cast<int>(rows_.size()) && on_square_off) on_square_off(rows_[static_cast<std::size_t>(r)]);
        });
        connect(square_all, &QPushButton::clicked, this, [this] {
            if (!on_square_off) return;
            for (const auto& p : rows_) if (p.net != 0) on_square_off(p);
        });
    }

    /// The broker account and GETS tabs live here too: positions are shown
    /// only in this window.
    void add_tab(QWidget* page, const QString& title) { tabs_->addTab(page, title); }
    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    [[nodiscard]] QTableWidget* table() const noexcept { return table_; }

    void refresh(const PaperOms& oms) {
        rows_ = oms.positions();
        table_->setRowCount(static_cast<int>(rows_.size()));
        double realised = 0, unrealised = 0, expenses = 0;
        bool unpriced = false, unmarked = false;
        const QColor up(0x7E, 0xE7, 0x87), down(0xFF, 0x7B, 0x72), flat(0xC9, 0xD1, 0xD9);
        const auto tint = [&](double x) { return x > 0 ? up : x < 0 ? down : flat; };
        for (int r = 0; r < table_->rowCount(); ++r) {
            const PaperPosition& p = rows_[static_cast<std::size_t>(r)];
            paper_ui::cell(table_, r, 0, p.inst.symbol);
            paper_ui::cell(table_, r, 1, p.product);
            paper_ui::cell(table_, r, 2, QString::number(p.net), true, p.net > 0 ? QColor(0x58, 0xA6, 0xFF) : p.net < 0 ? down : flat);
            paper_ui::cell(table_, r, 3, QString::number(p.buy_qty), true);
            paper_ui::cell(table_, r, 4, p.buy_qty > 0 ? paper_ui::money(p.buy_value / static_cast<double>(p.buy_qty)) : QStringLiteral("—"), true);
            paper_ui::cell(table_, r, 5, QString::number(p.sell_qty), true);
            paper_ui::cell(table_, r, 6, p.sell_qty > 0 ? paper_ui::money(p.sell_value / static_cast<double>(p.sell_qty)) : QStringLiteral("—"), true);
            paper_ui::cell(table_, r, 7, p.mark_paise > 0 ? PaperOms::rupees(p.mark_paise) : QStringLiteral("—"), true);
            paper_ui::cell(table_, r, 8, paper_ui::money(p.realised), true, tint(p.realised));
            paper_ui::cell(table_, r, 9, p.net != 0 && p.mark_paise <= 0 ? QStringLiteral("no quote") : paper_ui::money(p.unrealised), true, tint(p.unrealised));
            paper_ui::cell(table_, r, 10, p.expenses_unpriced && p.expenses == 0.0 ? QStringLiteral("unpriced")
                                          : paper_ui::money(p.expenses) + (p.expenses_unpriced ? QStringLiteral(" + unpriced") : QString()), true);
            const double np = p.net_pnl();
            paper_ui::cell(table_, r, 11, std::isfinite(np) ? paper_ui::money(np)
                                          : p.expenses_unpriced ? QStringLiteral("unavailable: unpriced") : QStringLiteral("unavailable: no quote"),
                           true, tint(std::isfinite(np) ? np : 0.0));
            realised += p.realised;
            unrealised += p.unrealised;
            expenses += p.expenses;
            unpriced = unpriced || p.expenses_unpriced;
            unmarked = unmarked || (p.net != 0 && p.mark_paise <= 0);
        }
        // An unpriced expense is not zero, and an unmarked position is not
        // flat: with either, there is no net figure to show.
        const double net = realised + unrealised - expenses;
        const bool net_known = !unpriced && !unmarked;
        totals_->setText(QStringLiteral(
            "<b>PAPER</b> &nbsp; realised <b>%1</b> &nbsp; unrealised <b>%2</b> &nbsp; expenses <b>%3</b>%4 &nbsp; "
            "<span style='color:%5'>net <b>%6</b></span> &nbsp; <span style='color:#8B949E'>%7 open order(s)</span>")
            .arg(paper_ui::money(realised), paper_ui::money(unrealised), paper_ui::money(expenses))
            .arg(unpriced ? QStringLiteral(" (some fills unpriced)") : QString())
            .arg(!net_known ? QStringLiteral("#8B949E") : net >= 0 ? QStringLiteral("#7EE787") : QStringLiteral("#FF7B72"))
            .arg(net_known ? paper_ui::money(net)
                           : unpriced ? QStringLiteral("unavailable (unpriced expenses)") : QStringLiteral("unavailable (a position has no fresh quote)"))
            .arg(oms.open_orders()));
    }

    std::function<void(const PaperPosition&)> on_square_off;

private:
    QTabWidget* tabs_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* totals_ = nullptr;
    std::vector<PaperPosition> rows_;
};

// ---------------------------------------------------------------------------
// Market picture (F5 / F6 / Shift+F9), security information (Shift+F7),
// message log (F10) and the shortcut list
// ---------------------------------------------------------------------------

class MarketPictureWindow final : public QDialog {
public:
    MarketPictureWindow(const PriceClient* client, QWidget* parent = nullptr) : QDialog(parent), client_(client) {
        setObjectName(QStringLiteral("marketPicture"));
        paper_ui::tool_window(this, QStringLiteral("Market picture (F5)"), QSize(560, 330));
        auto* v = new QVBoxLayout(this);
        head_ = new QLabel(this);
        head_->setTextFormat(Qt::RichText);
        head_->setWordWrap(true);
        v->addWidget(head_);
        depth_ = make_live_table(6, {QStringLiteral("Orders"), QStringLiteral("Bid Qty"), QStringLiteral("Bid"),
                                     QStringLiteral("Ask"), QStringLiteral("Ask Qty"), QStringLiteral("Orders")}, this);
        v->addWidget(depth_, 1);
        if (client_ != nullptr)
            connect(client_, &PriceClient::priceUpdated, this, [this](unsigned tok) {
                if (tok == row_.token && isVisible()) dirty_ = true;
            });
        timer_.setInterval(200);
        connect(&timer_, &QTimer::timeout, this, [this] { if (dirty_) { dirty_ = false; refresh(); } });
        timer_.start();
    }
    void show_for(const LiveRow& row) {
        row_ = row;
        setWindowTitle(QStringLiteral("Market picture · %1").arg(row.symbol));
        refresh();
    }
    void refresh() {
        const LivePrice* p = client_ != nullptr ? client_->price(row_.token) : nullptr;
        show_live_depth(depth_, p);
        const QuotePayload* q = p != nullptr && p->has_quote ? &p->quote : nullptr;
        const auto px = [](qint64 v) { return v > 0 ? live_detail::px(v) : QStringLiteral("—"); };
        head_->setText(QStringLiteral(
            "<b>%1</b> &nbsp; LTP <b>%2</b> &nbsp; LTQ %3 &nbsp; ATP %4 &nbsp; O %5 H %6 L %7 &nbsp; prev %8<br>"
            "<span style='color:#8B949E'>volume %9 · OI %10 · total buy %11 · total sell %12 · LTT %13</span>")
            .arg(row_.symbol.toHtmlEscaped(), px(p != nullptr ? p->last_paise : 0),
                 p != nullptr && p->last_qty > 0 ? live_detail::qty(p->last_qty) : QStringLiteral("—"),
                 px(q != nullptr && q->has(kQuoteHasAtp) ? q->avg_price : 0),
                 px(q != nullptr && q->has(kQuoteHasOhlc) ? q->open : 0),
                 px(q != nullptr && q->has(kQuoteHasOhlc) ? q->high : 0),
                 px(q != nullptr && q->has(kQuoteHasOhlc) ? q->low : 0),
                 px(q != nullptr && q->has(kQuoteHasPrevClose) ? q->prev_close : 0))
            .arg(p != nullptr && p->has_volume ? live_detail::qty(p->volume) : QStringLiteral("—"),
                 p != nullptr && p->has_oi ? live_detail::qty(p->oi) : QStringLiteral("—"),
                 q != nullptr && q->has(kQuoteHasTotals) ? live_detail::qty(q->total_buy) : QStringLiteral("—"),
                 q != nullptr && q->has(kQuoteHasTotals) ? live_detail::qty(q->total_sell) : QStringLiteral("—"),
                 live_detail::ist(q != nullptr && q->has(kQuoteHasLtt) ? q->last_trade_ns : (p != nullptr ? p->exchange_ts_ns : 0), false)));
    }
    [[nodiscard]] quint32 token() const noexcept { return row_.token; }

private:
    const PriceClient* client_;
    LiveRow row_;
    QLabel* head_ = nullptr;
    QTableWidget* depth_ = nullptr;
    QTimer timer_;
    bool dirty_ = false;
};

[[nodiscard]] inline QString security_info_html(const LiveRow& r, const LivePrice* p) {
    const QuotePayload* q = p != nullptr && p->has_quote ? &p->quote : nullptr;
    const QString expiry = r.expiry_day > 0 ? QDate(1970, 1, 1).addDays(r.expiry_day).toString(QStringLiteral("dd-MMM-yyyy"))
                                            : QStringLiteral("—");
    return QStringLiteral(
        "<table cellspacing=6>"
        "<tr><td style='color:#8B949E'>Symbol</td><td><b>%1</b></td></tr>"
        "<tr><td style='color:#8B949E'>FYERS ticker</td><td>%2</td></tr>"
        "<tr><td style='color:#8B949E'>Token (Kite)</td><td>%3</td></tr>"
        "<tr><td style='color:#8B949E'>Instrument</td><td>%4 · underlying %5</td></tr>"
        "<tr><td style='color:#8B949E'>Expiry · strike</td><td>%6 · %7</td></tr>"
        "<tr><td style='color:#8B949E'>Lot · tick</td><td>%8 · %9</td></tr>"
        "<tr><td style='color:#8B949E'>Circuit</td><td>%10</td></tr>"
        "<tr><td style='color:#8B949E'>Group</td><td>%11</td></tr></table>")
        .arg(r.symbol.toHtmlEscaped(), r.fyers.toHtmlEscaped(), QString::number(r.token), r.kind, r.underlying, expiry,
             r.strike > 0 ? QString::number(r.strike, 'f', 2) : QStringLiteral("—"))
        .arg(r.lot)
        .arg(r.tick, 0, 'f', 2)
        .arg(q != nullptr && q->has(kQuoteHasCircuit)
                 ? QStringLiteral("%1 – %2").arg(live_detail::px(q->lower_circuit), live_detail::px(q->upper_circuit))
                 : QStringLiteral("not sent by the feed"))
        .arg(r.group);
}

} // namespace altair::ui
