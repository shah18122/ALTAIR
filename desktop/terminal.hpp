// desktop/terminal.hpp -- account-first trading terminal.
//
// P32-01. Smit asked for the Watchlist, the Order Ticket and the Trade Handler
// to stop being three separate nav rows and become one terminal.
//
// THEY WERE THREE PAGES AND THE WORK IS ONE ACTION.
//
// Picking an instrument, sizing it, and being able to stop everything are not
// three tasks that happen to be adjacent -- they are one task, and a nav that
// separates them makes the operator hold state in their head across a page
// change. Worse, the state they were holding was a TOKEN: the watchlist knew
// the instrument and the ticket had its own hard-coded three-item combo, so
// the number had to be carried by eye between two screens that could not
// disagree loudly because they never saw each other.
//
// COMPOSED, NOT REWRITTEN.
//
// This file creates no new watchlist, no new ticket and no new halt control.
// It arranges the three widgets that already exist and wires the one link
// between them that was missing. That matters for a reason beyond effort:
// `test_order_ticket`, `test_pages` and the kill switch's own checks all
// cover those widgets' behaviour, and a rewrite would have moved that
// behaviour out from under its tests while every one of them stayed green.
//
// THE LINK IS SELECTION, AND IT IS ONE DIRECTION ONLY.
//
// Selecting a watchlist row loads that instrument into the ticket. The ticket
// never writes back into the watchlist. One direction because the reverse
// creates a loop between two widgets that each think they own the selection,
// and because there is no question the ticket can answer that the watchlist
// needs.
//
// WHAT THIS PAGE STILL CANNOT DO.
//
// Trade. Every constraint from P25-04 and P11-14 survives being put behind one
// tab: the ticket appends a REQUEST to a JSONL file that `oms/` drains, the
// halt control writes a REQUEST file that `risk/` and `oms/` act on, and
// `desktop/` links neither. Putting them side by side changes the layout and
// nothing else, and the gate-3 allow-list in desktop/CMakeLists.txt still
// refuses `altair_oms` and `altair_broker`.
//
// A terminal that looks like a broker's terminal and cannot place an order is
// exactly the thing to be careful about, so the ticket keeps the word REQUEST
// on both buttons and keeps its blue explanation banner. The layout got
// denser; the claims did not.

#pragma once

#include "auth.hpp"
#include <broker/account_snapshot.hpp>
#include "greek_watch.hpp"
#include "kill_switch.hpp"
#include "live_feed.hpp"
#include "option_chain.hpp"
#include "order_ticket.hpp"
#include "panels.hpp"
#include "funds_summary.hpp"
#include "gets_workspace.hpp"
#include "live_market.hpp"
#include "live_models.hpp"
#include "live_trading.hpp"
#include "paper_oms.hpp"
#include "paper_windows.hpp"
#include "position_table.hpp"
#include "price_client.hpp"
#include "data/series_io.hpp"

#include <QButtonGroup>
#include <QDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QLabel>
#include <QHash>
#include <QLockFile>
#include <QMessageBox>
#include <QProcess>
#include <QDir>
#include <QHeaderView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTextStream>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <expected>
#include <memory>

#if ALTAIR_HAVE_CHARGES_TOML
#include <app/demo_costs.hpp>
#include <risk/charges_toml.hpp>
#endif

namespace altair::ui {

/// One immutable, already-validated account input for the read-only Terminal.
/// The producer owns transport and credentials; this surface only consumes the
/// typed snapshot and keeps paper/broker provenance explicit.
struct TerminalAccountInput {
    broker_view::AccountSnapshot snapshot{};
    QString label;
    FundsSource funds_source{FundsSource::Broker};
    PositionSource positions_source{PositionSource::Broker};
};

enum class TerminalAccountError : std::uint8_t {
    SnapshotUnusable,
    PositionSessionMismatch,
    PositionApplyRejected,
    FundsApplyRejected
};


/// What is sitting in the intent queue, read back off disk.
///
/// P32-01. THE TICKET PROMISED THIS AND NOTHING DELIVERED IT.
///
/// order_ticket.hpp says, correctly and in bold, that a request survives this
/// window closing and that an undrained request is PENDING and is neither a
/// position nor an order. Its log pane then showed only what THIS SESSION had
/// requested -- so after a restart, the one state the design exists to make
/// survivable was the one state nobody could see.
///
/// That is the worst direction for this particular gap. The failure the whole
/// intent-queue design is built to avoid is a person believing they are flat
/// while a request sits in a file, and a restart was exactly when that belief
/// was unopposed.
///
/// IT PARSES NOTHING IT DOES NOT NEED.
///
/// This reads lines and shows them. It does not re-implement oms/'s parser --
/// there is no shared header with oms/ by design, and a second parser here
/// would be a second opinion about what a line means, which is precisely what
/// the conformance vectors exist to prevent. A line that oms/ would reject
/// still shows here, because "there is a line in the queue you should look at"
/// is true either way and is the thing an operator needs to know.
class PendingIntents final : public QWidget {
    Q_OBJECT

public:
    explicit PendingIntents(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 12, 12, 12);

        auto* head = new QLabel(
            QStringLiteral(
                "<b>Requests in the queue.</b> Read from <code>"
                ALTAIR_INTENT_FILE "</code> — the file <code>oms/</code> "
                "drains. A line here is a REQUEST that has not been drained: "
                "it is not a position and it is not an order.<br><br>"
                "This panel cannot tell you whether <code>oms/</code> has "
                "since acted on a line. It reports what is in the file."),
            this);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral(
            "background:#3A2E17;color:#F0E0C0;padding:8px;"
            "border:1px solid #7A5E20;"));
        v->addWidget(head);

        auto* row = new QHBoxLayout;
        reload_ = new QPushButton(QStringLiteral("Re-read the queue"), this);
        count_ = new QLabel(this);
        count_->setStyleSheet(QStringLiteral("color:#9FB3C8;"));
        row->addWidget(reload_);
        row->addWidget(count_, 1);
        v->addLayout(row);

        out_ = new QPlainTextEdit(this);
        out_->setReadOnly(true);
        out_->setStyleSheet(QStringLiteral(
            "background:#11171C;color:#D6DBDF;"
            "font-family:Consolas,monospace;font-size:12px;border:none;"));
        v->addWidget(out_, 1);

        connect(reload_, &QPushButton::clicked, this,
                &PendingIntents::reload);
        reload();
    }

public Q_SLOTS:
    void reload() {
        QFile f(QStringLiteral(ALTAIR_INTENT_FILE));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // NO FILE IS NOT ZERO REQUESTS WITH CERTAINTY -- it is no file.
            // The distinction matters here more than most places: "nothing is
            // pending" is a claim somebody may act on, and an unreadable file
            // does not support it.
            count_->setText(QStringLiteral("no queue file"));
            out_->setPlainText(QStringLiteral(
                "The queue file does not exist or cannot be read:\n  "
                ALTAIR_INTENT_FILE "\n\n"
                "That is NOT the same as \"nothing is pending\". It means "
                "this panel\ncannot say either way, which is the honest "
                "answer and not a\nreassuring one."));
            return;
        }
        QTextStream ts(&f);
        QString body;
        int lines = 0;
        while (!ts.atEnd()) {
            const QString ln = ts.readLine();
            if (ln.trimmed().isEmpty()) { continue; }
            ++lines;
            body += QStringLiteral("%1  %2\n")
                        .arg(lines, 4).arg(ln);
        }
        count_->setText(lines == 0
                            ? QStringLiteral("the queue file is empty")
                            : QStringLiteral("%1 request(s) in the queue")
                                  .arg(lines));
        out_->setPlainText(lines == 0
                               ? QStringLiteral(
                                     "The queue file exists and holds no "
                                     "requests.")
                               : body);
    }

private:
    QPushButton* reload_ = nullptr;
    QLabel* count_ = nullptr;
    QPlainTextEdit* out_ = nullptr;
};

/// The terminal's look. Dark, dense, monospaced numbers, one accent colour --
/// the ODIN / Bloomberg convention, because a screen read for hours in a
/// dealing room is designed for the eye that is tired, not the eye that is
/// being impressed. Scoped to this page by object name so it cannot leak into
/// the analysis pages, which are documents and read better light.
inline const char* kTerminalStyle = R"(
QWidget#AltairTerminal { background:#0E1116; color:#D0D6DE; }
QWidget#AltairTerminal QLabel { color:#D0D6DE; }
QWidget#AltairTerminal QTableWidget, QWidget#AltairTerminal QTableView {
    background:#15181D; alternate-background-color:#181C22; color:#D0D6DE;
    gridline-color:#232830; selection-background-color:#3A3212;
    selection-color:#FFFFFF; font-family:Consolas,'Cascadia Mono',monospace;
}
QWidget#AltairTerminal QHeaderView::section {
    background:#1C2129; color:#8A93A2; border:0; border-right:1px solid #232830;
    padding:3px 4px; font-weight:bold;
}
QWidget#AltairTerminal QTabWidget::pane { border:1px solid #232830; }
QWidget#AltairTerminal QTabBar::tab {
    background:#15181D; color:#8A93A2; padding:5px 12px; border:0;
}
QWidget#AltairTerminal QTabBar::tab:selected {
    background:#1C2129; color:#F4C95D; border-bottom:2px solid #F4C95D;
}
QWidget#AltairTerminal QPushButton {
    background:#1C2129; color:#D0D6DE; border:1px solid #2C333D;
    padding:4px 10px;
}
QWidget#AltairTerminal QPushButton:hover { border-color:#F4C95D; }
QWidget#AltairTerminal QComboBox, QWidget#AltairTerminal QSpinBox,
QWidget#AltairTerminal QLineEdit, QWidget#AltairTerminal QPlainTextEdit {
    background:#15181D; color:#D0D6DE; border:1px solid #2C333D; padding:2px 4px;
}
QWidget#AltairTerminal QSplitter::handle { background:#232830; }
QWidget#AltairTerminal QCheckBox, QWidget#AltairTerminal QRadioButton {
    color:#D0D6DE;
}
QWidget#AltairTerminal QGroupBox { color:#8A93A2; border:1px solid #232830;
    margin-top:14px; }
QWidget#AltairTerminal QGroupBox::title { subcontrol-origin:margin; left:8px;
    color:#F4C95D; }
QWidget#strip { background:#0A0C10; border-bottom:1px solid #F4C95D; }
QWidget#accountSurface { background:#0E1116; }
QLabel#sectionKicker { color:#F4C95D;font-weight:700;letter-spacing:1px; }
QLabel#accountCaveat { color:#8A93A2; }
)";

class TerminalPage final : public QWidget {
    Q_OBJECT

public:
    TerminalPage(Role role, const QString& user, QWidget* parent = nullptr)
        : QWidget(parent), user_(user) {
        setObjectName(QStringLiteral("AltairTerminal"));
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QString::fromLatin1(kTerminalStyle));

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        // ---- THE STRIP: indices, the stream, and the keys ---------------
        //
        // What a trader glances at between everything else: where the index
        // is, whether the prices on screen are live, and which key does what.
        auto* strip = new QWidget(this);
        strip->setObjectName(QStringLiteral("strip"));
        strip->setAttribute(Qt::WA_StyledBackground, true);
        auto* h = new QHBoxLayout(strip);
        h->setContentsMargins(10, 6, 10, 6);
        h->setSpacing(18);
        auto* title = new QLabel(QStringLiteral(
            "<span style='color:#F4C95D;font-weight:bold;letter-spacing:2px'>"
            "TERMINAL</span>"), strip);
        h->addWidget(title);
        nifty_ = make_tile(strip, h);
        bnf_ = make_tile(strip, h);
        vix_ = make_tile(strip, h);
        h->addStretch();
        stream_state_ = new QLabel(strip);
        stream_state_->setTextFormat(Qt::RichText);
        h->addWidget(stream_state_);
        // The stream connects by itself and keeps retrying; the button stays
        // for scripts and tests but the trader never needs it.
        stream_btn_ = new QPushButton(QStringLiteral("Connect stream"), strip);
        stream_btn_->hide();
        toast_ = new QLabel(strip);
        toast_->setObjectName(QStringLiteral("terminalToast"));
        toast_->setTextFormat(Qt::RichText);
        h->addWidget(toast_);
        // LIVE: off by default. On sends Buy/Sell to FYERS through the order
        // router after a typed confirmation (live_trading.hpp).
        live_btn_ = new QPushButton(QStringLiteral("PAPER"), strip);
        live_btn_->setObjectName(QStringLiteral("liveSwitch"));
        live_btn_->setToolTip(QStringLiteral(
            "Paper: orders fill against the live book and nothing reaches a broker. Click to switch LIVE on: "
            "Buy/Sell go to FYERS with real money, within limits you set, until 15:30 or until you switch it off."));
        h->addWidget(live_btn_);
        live_orders_btn_ = new QPushButton(QStringLiteral("Live orders"), strip);
        live_orders_btn_->setObjectName(QStringLiteral("liveOrdersButton"));
        live_orders_btn_->setToolTip(QStringLiteral("Every order sent to FYERS today and what became of it; cancel from here"));
        h->addWidget(live_orders_btn_);
        // Top gainers and losers of the streamed NSE stocks, beside LIVE; a
        // click opens the top ten of each.
        movers_ = new QLabel(strip);
        movers_->setObjectName(QStringLiteral("moversStrip"));
        movers_->setTextFormat(Qt::RichText);
        movers_->setToolTip(QStringLiteral("Top gainers and losers of the streamed stocks, every 2 s. Click for the top ten."));
        movers_->setText(QStringLiteral("<span style='color:#6B7380'>movers: waiting for prices</span>"));
        h->addWidget(movers_);
        // The market watch is home; Enter on a scrip opens its option chain.
        // Models toggles the live models over it (Ctrl+M; F4 back).
        models_btn_ = new QPushButton(QStringLiteral("Models"), strip);
        models_btn_->setCheckable(true);
        models_btn_->setToolTip(QStringLiteral(
            "Ctrl+M · live models: what each is doing and why, paper positions and trades with expenses. F4: the market watch."));
        h->addWidget(models_btn_);
        v->addWidget(strip);

        // ---- ACCOUNT-FIRST SURFACE -------------------------------------
        // P4-07: the Terminal opens on positions and funds. The older trade
        // controls remain reachable under Operations while their workflows
        // are migrated, but they no longer dominate the default view.
        surface_ = new QStackedWidget(this);
        // THE STREAM (P37) is created first: the market watch below reads it.
        client_ = new PriceClient(this);
        // Sized for the live universe: ~220 instruments ticking, plus the
        // board replay a new subscriber receives (~700 frames).
        client_->set_read_buffer(1024 * 1024);
        live_ = new LiveMarketWatch(client_, {}, surface_);
        surface_->addWidget(live_);
        models_ = new LiveModelsPanel(client_, {}, surface_);
        surface_->addWidget(models_);
        account_surface_ = new QWidget(surface_);
        account_surface_->setObjectName(QStringLiteral("accountSurface"));
        auto* account_layout = new QVBoxLayout(account_surface_);
        account_layout->setContentsMargins(10, 10, 10, 10);
        account_layout->setSpacing(8);

        account_state_ = new QLabel(QStringLiteral(
            "No FYERS snapshot yet. It is fetched by itself while this window is open (it needs a FYERS login today)."), account_surface_);
        account_state_->setObjectName(QStringLiteral("accountCaveat"));
        account_state_->setWordWrap(true);
        account_layout->addWidget(account_state_);

        auto* funds_title = new QLabel(QStringLiteral("AVAILABLE FUNDS BY BROKER"), account_surface_);
        funds_title->setObjectName(QStringLiteral("sectionKicker"));
        account_layout->addWidget(funds_title);
        auto* caveat = new QLabel(QStringLiteral(
            "Comparable INR arithmetic only — balances remain at their broker and are not fungible."),
            account_surface_);
        caveat->setObjectName(QStringLiteral("accountCaveat"));
        caveat->setWordWrap(true);
        account_layout->addWidget(caveat);
        funds_model_ = new FundsSummaryModel(this);
        funds_view_ = new QTableView(account_surface_);
        funds_view_->setObjectName(QStringLiteral("terminalFunds"));
        funds_view_->setModel(funds_model_);
        funds_view_->verticalHeader()->hide();
        funds_view_->horizontalHeader()->setStretchLastSection(true);
        funds_view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        funds_view_->setAlternatingRowColors(true);
        funds_view_->setSelectionMode(QAbstractItemView::NoSelection);
        funds_view_->setMaximumHeight(118);
        account_layout->addWidget(funds_view_);

        auto* position_head = new QHBoxLayout;
        auto* position_title = new QLabel(QStringLiteral("OPEN POSITIONS"), account_surface_);
        position_title->setObjectName(QStringLiteral("sectionKicker"));
        position_head->addWidget(position_title);
        position_head->addStretch();
        position_filter_ = new QLineEdit(account_surface_);
        position_filter_->setObjectName(QStringLiteral("positionFilter"));
        position_filter_->setAccessibleName(QStringLiteral("Filter open positions"));
        position_filter_->setPlaceholderText(QStringLiteral("Filter broker, account, exchange or instrument…"));
        position_filter_->setClearButtonEnabled(true);
        position_filter_->setMaximumWidth(360);
        position_head->addWidget(position_filter_);
        account_layout->addLayout(position_head);

        // Instruments resolve through the FYERS tickers the GETS workspace
        // read; before this the table showed "unresolved #N" for every row.
        positions_model_ = new PositionTableModel(
            [this](broker_view::InstrumentKey key) -> std::optional<InstrumentDisplay> {
                const auto it = instrument_names_.constFind(static_cast<std::uint32_t>(key));
                if (it == instrument_names_.constEnd()) return std::nullopt;
                return *it;
            }, this);
        positions_proxy_ = new QSortFilterProxyModel(this);
        positions_proxy_->setSourceModel(positions_model_);
        positions_proxy_->setSortRole(PositionTableModel::SortRole);
        positions_proxy_->setFilterKeyColumn(-1);
        positions_proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
        positions_view_ = new QTableView(account_surface_);
        positions_view_->setObjectName(QStringLiteral("terminalPositions"));
        positions_view_->setModel(positions_proxy_);
        positions_view_->setSortingEnabled(true);
        positions_view_->setAlternatingRowColors(true);
        positions_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        positions_view_->setSelectionMode(QAbstractItemView::SingleSelection);
        positions_view_->verticalHeader()->hide();
        positions_view_->horizontalHeader()->setSectionsMovable(true);
        positions_view_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        positions_view_->horizontalHeader()->setStretchLastSection(true);
        for (int column = 0; column < PositionTableModel::ColumnCount; ++column)
            positions_view_->setColumnHidden(column,
                PositionTableModel::hidden_by_default(column));
        account_layout->addWidget(positions_view_, 1);
        auto* empty_note = new QLabel(QStringLiteral(
            "Only open positions are shown. Unknown marks and stale evidence stay explicit; "
            "positions are never netted across broker accounts."), account_surface_);
        empty_note->setObjectName(QStringLiteral("accountCaveat"));
        empty_note->setWordWrap(true);
        account_layout->addWidget(empty_note);
        // P4-07 + GETS: the account surface is the first tab of the GETS
        // workspace, which adds the Greek, RMS and market tabs beside it and
        // feeds this Terminal the typed FYERS snapshot on every refresh.
        // Positions are shown only in the net position window (Alt+F6): the
        // paper book first, then the broker account and GETS tabs.
        net_window_ = new NetPositionWindow(this);
        gets_ = new GetsWorkspace(account_surface_, net_window_);
        gets_->on_account = [this](const GetsTypedAccount& typed) { apply_gets_account(typed); };
        // Live prices for the GETS tabs: every streamed FYERS ticker, from
        // its last tick (data/live/universe.csv maps ticker -> token).
        gets_->set_live_quotes([this](GetsQuotes& quotes) -> int {
            if (!client_->connected()) return 0;
            int priced = 0;
            for (const auto& r : live_->model()->rows()) {
                if (r.fyers.isEmpty()) continue;
                const LivePrice* p = client_->price(r.token);
                if (p == nullptr || p->last_paise <= 0) continue;
                GetsQuote g;
                g.symbol = r.fyers;
                g.ok = true;
                g.description = r.symbol;
                g.ltp = p->last_paise;
                const QuotePayload* q = p->has_quote ? &p->quote : nullptr;
                if (q != nullptr && q->has(kQuoteHasPrevClose) && q->prev_close > 0) {
                    g.prev_close = q->prev_close;
                    g.change = p->last_paise - q->prev_close;
                    g.change_pct = 100.0 * static_cast<double>(p->last_paise - q->prev_close) / static_cast<double>(q->prev_close);
                }
                if (q != nullptr && q->has(kQuoteHasOhlc)) { g.open = q->open; g.high = q->high; g.low = q->low; }
                if (q != nullptr && q->has(kQuoteHasAtp)) g.atp = q->avg_price;
                if (p->has_volume) g.volume = p->volume;
                g.time = p->exchange_ts_ns / 1'000'000'000LL;
                if (!quotes.by_symbol.contains(g.symbol)) quotes.order << g.symbol;
                quotes.by_symbol.insert(g.symbol, g);
                ++priced;
            }
            return priced;
        });
        net_window_->add_tab(gets_, QStringLiteral("Broker account · GETS"));
        gets_->set_demo_ltp([this](quint32 tok) {
            const LivePrice* p = client_->price(tok);
            return p != nullptr && p->last_paise > 0 ? static_cast<double>(p->last_paise) / 100.0 : 0.0;
        });

        connect(position_filter_, &QLineEdit::textChanged, this,
                [this](const QString& text) {
            positions_proxy_->setFilterFixedString(text);
        });

        // ---- LEGACY OPERATIONS WINDOW ----------------------------------
        // The gated order ticket, the intent queue, the halt control and the
        // feed panel: one window, opened by Halt controls. The market view no
        // longer carries them -- orders are + and −, the book is F3.
        ops_window_ = new QDialog(this);
        ops_window_->setObjectName(QStringLiteral("operationsWindow"));
        paper_ui::tool_window(ops_window_, QStringLiteral("Operations · halt, intent queue, gated ticket"), QSize(1180, 620));
        auto* ops_layout = new QVBoxLayout(ops_window_);
        ops_layout->setContentsMargins(4, 4, 4, 4);
        split_ = new QSplitter(Qt::Horizontal, ops_window_);
        watch_ = new WatchlistPanel(role, split_);
        split_->addWidget(watch_);
        chain_ = new OptionChainPanel(split_);
        split_->addWidget(chain_);

        // The right column is tabbed rather than stacked: an order ticket and
        // a halt control are both tall, and stacking them puts the halt below
        // the fold exactly when somebody is scrolling to find it.
        right_ = new QTabWidget(split_);
        ticket_ = new OrderTicket(role, user, right_);
        halt_ = new KillSwitchPanel(role, user, right_);
        right_->addTab(ticket_, QStringLiteral("Order"));
        pending_ = new PendingIntents(right_);
        right_->addTab(pending_, QStringLiteral("Book"));
        right_->addTab(halt_, QStringLiteral("Halt"));
        feed_ = new LiveFeedPanel(right_);
        right_->addTab(feed_, QStringLiteral("Feed"));
        connect(right_, &QTabWidget::currentChanged, this, [this](int i) {
            if (right_->widget(i) == pending_) { pending_->reload(); }
        });
        split_->addWidget(right_);

        // The chain is the widest column: thirteen numeric columns either side
        // of a strike is the whole point of the screen.
        split_->setStretchFactor(0, 24);
        split_->setStretchFactor(1, 50);
        split_->setStretchFactor(2, 26);
        ops_layout->addWidget(split_);
        // A TERMINAL OPENS ON THE MARKET.
        surface_->setCurrentWidget(live_);
        v->addWidget(surface_, 1);
        connect(models_btn_, &QPushButton::clicked, this, [this](bool on) {
            show_view(on ? QStringLiteral("models") : QStringLiteral("watch"));
        });
        live_->on_order_key = [this](bool buy) { open_order(buy); };
        // Greek Watch: Enter or double-click on a CE / PE in the chain.
        greek_ = new GreekWatchWindow(client_, this);
        paper_ui::tool_window(greek_, QStringLiteral("Greek Watch"), QSize(1320, 560));
        live_->on_greek = [this](quint32 tok) { open_greek(tok); };
#ifdef ALTAIR_SOURCE_DIR
        greek_->set_store(QStringLiteral(ALTAIR_SOURCE_DIR "/data/live/greek_watch.csv"));
#endif
        movers_->setCursor(Qt::PointingHandCursor);
        movers_->installEventFilter(this);
        movers_timer_.setInterval(2000);
        connect(&movers_timer_, &QTimer::timeout, this, [this] {
            restore_greeks();
            if (isVisible()) refresh_movers();
        });
        movers_timer_.start();
        build_paper_book();

        // ---- WIRES ------------------------------------------------------
        //
        // Every path into the ticket now carries a SPEC. The plain
        // instrumentPicked still loads the name, so an unresolved row is
        // visible in the ticket -- and refused at submit, which is P39-04.
        connect(watch_, &WatchlistPanel::instrumentPicked, ticket_,
                &OrderTicket::set_instrument);
        connect(watch_, &WatchlistPanel::contractPicked, ticket_,
                &OrderTicket::set_contract);
        connect(chain_, &OptionChainPanel::contractPicked, this,
                [this](unsigned tok, const QString& sym, qint64 lot,
                       qint64 tick, const QString& ex) {
                    ticket_->set_contract(tok, sym, lot, tick, ex);
                    right_->setCurrentWidget(ticket_);
                });

        // ---- THE STREAM (P37) -------------------------------------------
        connect(stream_btn_, &QPushButton::clicked, this, [this] {
            if (client_->connected()) {
                auto_connect_ = false;   // the operator said stop; stop retrying
                client_->stop();
            } else {
                auto_connect_ = true;
                client_->start(QStringLiteral("127.0.0.1"), 7421);
                stream_state_->setText(QStringLiteral(
                    "<span style='color:#F4C95D'>● CONNECTING</span>"));
            }
        });
        // CONNECT BY ITSELF. A terminal that waits for a button before it
        // shows a price is not live; this retries every 3 s until the price
        // service answers, and again whenever it goes away.
        reconnect_.setInterval(3000);
        connect(&reconnect_, &QTimer::timeout, this, [this] {
            if (auto_connect_ && !client_->connected()
                && client_->state() != QAbstractSocket::ConnectingState)
                client_->start(QStringLiteral("127.0.0.1"), 7421);
        });
        reconnect_.start();
        QTimer::singleShot(0, this, [this] {
            if (auto_connect_ && !client_->connected()) client_->start(QStringLiteral("127.0.0.1"), 7421);
        });
        connect(client_, &PriceClient::statusChanged, this,
                [this] { refresh_stream(); });
        connect(client_, &PriceClient::priceUpdated, this,
                [this](unsigned tok) { on_price(tok); });

        // ---- HOTKEYS ----------------------------------------------------
        //
        // Scoped to this page: WidgetWithChildrenShortcut fires only while the
        // Terminal (or something inside it) has focus, so F1 on the Forecast
        // page does not open an order ticket nobody can see.
        //
        // F1 and F2 FOCUS the ticket; they never submit. The typed
        // confirmation is the safety property and a hotkey that bypassed it
        // would be a one-keystroke order.
        const auto key = [this](QKeySequence k, auto fn) {
            auto* sc = new QShortcut(k, this);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            connect(sc, &QShortcut::activated, this, fn);
        };
        // GETS / ODIN keys. F1/F2 open the order window: paper, or -- only
        // while LIVE is on -- a FYERS order that asks for a confirmation
        // before it becomes a request for the order router.
        key(QKeySequence(Qt::Key_F1), [this] { open_order(true); });
        key(QKeySequence(Qt::Key_F2), [this] { open_order(false); });
        key(QKeySequence(Qt::Key_F3), [this] { show_book(order_book_); });
        key(QKeySequence(Qt::Key_F4), [this] { show_view(QStringLiteral("watch")); });
        key(QKeySequence(Qt::CTRL | Qt::Key_M), [this] { show_view(QStringLiteral("models")); });
        key(QKeySequence(Qt::Key_F5), [this] { show_market_picture(); });
        key(QKeySequence(Qt::Key_F6), [this] { show_market_picture(); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F9), [this] { show_market_picture(); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F7), [this] { show_security_info(); });
        key(QKeySequence(Qt::Key_F8), [this] { show_book(trade_book_); });
        key(QKeySequence(Qt::ALT | Qt::Key_F6), [this] { show_net_position(); });
        key(QKeySequence(Qt::Key_F10), [this] { show_book(message_log_); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F1), [this] { cancel_order(order_book_->selected()); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F2), [this] { modify_order(order_book_->selected()); });
        key(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { cancel_all_orders(); });
        key(QKeySequence(Qt::CTRL | Qt::Key_F), [this] { show_view(QStringLiteral("watch")); live_->focus_find(); });
        key(QKeySequence(Qt::Key_Insert), [this] { show_view(QStringLiteral("watch")); live_->focus_add(); });

        seed_strip_from_disk();
        refresh_stream();
        build_live_trading();
    }

    ~TerminalPage() override {
        // Let the router finish a pass rather than die mid-send; it resumes
        // from its files when it next starts.
        if (router_ != nullptr && router_->state() != QProcess::NotRunning) {
            router_->terminate();
            if (!router_->waitForFinished(1500)) router_->kill();
        }
    }

    /// Keep the account-first terminal within a laptop viewport. Wide chain
    /// content scrolls inside its table rather than enlarging the page minimum.
    [[nodiscard]] QSize minimumSizeHint() const override { return QSize(640, 480); }
    [[nodiscard]] QSize sizeHint() const override { return QSize(1280, 720); }

    /// For tests: the composed parts, so a test can drive selection without a
    /// window manager.
    [[nodiscard]] WatchlistPanel* watchlist() const noexcept { return watch_; }
    [[nodiscard]] OrderTicket* ticket() const noexcept { return ticket_; }
    [[nodiscard]] KillSwitchPanel* halt() const noexcept { return halt_; }
    [[nodiscard]] PendingIntents* queue() const noexcept { return pending_; }
    [[nodiscard]] LiveFeedPanel* feed() const noexcept { return feed_; }
    [[nodiscard]] OptionChainPanel* chain() const noexcept { return chain_; }
    [[nodiscard]] LiveMarketWatch* market_watch() const noexcept { return live_; }
    [[nodiscard]] LiveModelsPanel* live_models() const noexcept { return models_; }
    /// Show a view by name: watch, chain, models; positions opens the net
    /// position window (Alt+F6) and operations the operations window.
    bool show_view(const QString& v) {
        if (v == QLatin1String("watch")) {
            surface_->setCurrentWidget(live_);
            live_->set_view(LiveMarketWatch::View::Watch);
            models_btn_->setChecked(false);
            return true;
        }
        if (v == QLatin1String("chain")) {
            // The chain of the selected scrip (Enter does the same).
            surface_->setCurrentWidget(live_);
            if (!live_->open_chain()) live_->set_view(LiveMarketWatch::View::Chain);
            models_btn_->setChecked(false);
            return true;
        }
        if (v == QLatin1String("models")) { surface_->setCurrentWidget(models_); models_btn_->setChecked(true); return true; }
        if (v == QLatin1String("positions")) { show_net_position(); return true; }
        if (v == QLatin1String("operations")) { show_window(ops_window_); return true; }
        return false;
    }
    [[nodiscard]] PriceClient* stream() const noexcept { return client_; }
    [[nodiscard]] FundsSummaryModel* funds_model() const noexcept { return funds_model_; }
    [[nodiscard]] PositionTableModel* positions_model() const noexcept { return positions_model_; }
    [[nodiscard]] QTableView* positions_view() const noexcept { return positions_view_; }
    [[nodiscard]] GetsWorkspace* gets() const noexcept { return gets_; }
    [[nodiscard]] bool account_surface_visible() const noexcept {
        return net_window_ != nullptr && net_window_->isVisible() && net_window_->tabs()->currentWidget() == gets_;
    }
    void show_account_surface() {
        show_net_position();
        net_window_->tabs()->setCurrentWidget(gets_);
        gets_->tabs()->setCurrentWidget(account_surface_);
    }

    // ---- the paper book ------------------------------------------------------
    [[nodiscard]] PaperOms& paper() noexcept { return oms_; }
    [[nodiscard]] NetPositionWindow* net_position() const noexcept { return net_window_; }
    [[nodiscard]] PaperOrderBookWindow* order_book() const noexcept { return order_book_; }
    [[nodiscard]] PaperTradeBookWindow* trade_book() const noexcept { return trade_book_; }
    [[nodiscard]] QDialog* operations_window() const noexcept { return ops_window_; }
    /// Where manual_orders.csv and manual_trades.csv go (tests: a temp dir).
    void set_paper_dir(const QString& dir) { paper_dir_ = dir; }
    /// Start the live feed by itself when nothing streams (main.cpp; never in tests).
    /// And demo trading: the models paper-trade once the feed streams.
    void set_autostart_feed(bool on) {
        live_->set_autostart(on);
        models_->set_auto_demo(on);
    }

    /// Place a paper order as the order window would (tests, square-off).
    /// Refused while another Altair holds the paper book, or the book could
    /// not be written: an order the file never saw is an order a restart loses.
    std::expected<int, QString> place_paper(PaperOrder o) {
        if (const QString why = paper_writer(); !why.isEmpty()) {
            toast(why, false);
            return std::unexpected(why);
        }
        const PaperQuote q = paper_ui::quote_of(client_, o.inst.token);
        const auto r = oms_.place(std::move(o), q, now_ns());
        books_dirty();
        return r;
    }
    // ---- LIVE (live_trading.hpp; the order router decides and sends) ---------
    /// LIVE is on: the arm file says so and has not expired.
    [[nodiscard]] bool live_on() const noexcept { return live_on_; }
    [[nodiscard]] QPushButton* live_switch() const noexcept { return live_btn_; }
    [[nodiscard]] LiveTradingOrdersWindow* live_orders() const noexcept { return live_orders_; }
    /// Tests: where data/ is, whether the router is started, and the confirmation's answer.
    void set_live_root(const QString& root) { live_root_ = root; refresh_live(); }
    void set_router_autostart(bool on) { router_autostart_ = on; }
    void set_live_confirm(std::function<bool(const QString&)> f) { live_confirm_ = std::move(f); }

    /// Switch LIVE on with these limits (the dialog's OK; tests call it directly).
    bool arm_live(const LiveTradingLimits& l) {
        if (halted()) {
            toast(QStringLiteral("Trading is halted: resume first (F11+F12), then switch LIVE on"), false);
            return false;
        }
        const QString path = live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingArmFile);
        if (!write_live_arm(path, true, live_user(), l, QDateTime::currentDateTimeUtc())) {
            toast(QStringLiteral("LIVE not switched on: %1 could not be written").arg(path), false);
            return false;
        }
        log_text_->appendPlainText(QStringLiteral("%1  LIVE ON by %2: up to %3 lot(s) and Rs %4 per order, %5 orders, stop at a Rs %6 loss")
                                       .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), live_user())
                                       .arg(l.max_lots)
                                       .arg(paper_ui::money(l.max_order_value))
                                       .arg(l.max_orders_per_day)
                                       .arg(paper_ui::money(l.max_daily_loss)));
        log_text_->appendPlainText(QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                                                                live_strategies_text(l)));
        ensure_router();
        refresh_live();
        toast(QStringLiteral("LIVE: Buy/Sell now go to FYERS (until 15:30, or until you switch it off)"), true);
        return true;
    }
    bool disarm_live() {
        const QString path = live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingArmFile);
        const LiveTradingArm a = read_live_arm(path, QDateTime::currentSecsSinceEpoch());
        if (!write_live_arm(path, false, live_user(), a.limits, QDateTime::currentDateTimeUtc())) {
            toast(QStringLiteral("LIVE NOT switched off: %1 could not be written. Use the kill switch.").arg(path), false);
            return false;
        }
        log_text_->appendPlainText(QStringLiteral("%1  LIVE OFF").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
        refresh_live();
        toast(QStringLiteral("LIVE off: orders are paper again. Orders already at FYERS stay there; cancel them in Live orders."), true);
        return true;
    }

    // ---- HALT (F11 + F12, from the main window) --------------------------------
    [[nodiscard]] QString kill_path() const { return live_root_ + QStringLiteral("/data/kill_request.json"); }
    /// A halt is requested: the request file is there (or there and unreadable,
    /// which may be one). The router and the models engine read the same file.
    [[nodiscard]] bool halted() const {
        const KillRequest k = read_kill_request(kill_path());
        return k.state == HaltFileState::Present
            || (k.state == HaltFileState::Unreadable && QFileInfo::exists(kill_path()));
    }
    [[nodiscard]] QString halt_summary() const {
        const KillRequest k = read_kill_request(kill_path());
        return QStringLiteral("Halted by %1 at %2: %3")
            .arg(k.requested_by.isEmpty() ? QStringLiteral("?") : k.requested_by.toHtmlEscaped(),
                 k.requested_at.isEmpty() ? QStringLiteral("?")
                     : QDateTime::fromString(k.requested_at, Qt::ISODate).toLocalTime().toString(QStringLiteral("HH:mm:ss")),
                 k.reason.toHtmlEscaped());
    }
    /// Stop everything: write the kill request (the router cancels open orders
    /// and sends nothing; the engine opens nothing new) and switch LIVE off.
    bool halt_trading(const QString& by, const QString& reason) {
        (void)QDir().mkpath(live_root_ + QStringLiteral("/data"));
        QJsonObject o;
        o[QStringLiteral("requested_at")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        o[QStringLiteral("requested_by")] = by;
        o[QStringLiteral("reason")] = reason;
        const bool wrote = write_kill_request(kill_path(), o);
        const bool was_live = live_on_;
        if (was_live) (void)disarm_live();
        if (was_live || router_ != nullptr) ensure_router();   // the router is what cancels at FYERS
        log_text_->appendPlainText(QStringLiteral("%1  HALT by %2 (%3)%4")
                                       .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), by, reason,
                                            wrote ? QString() : QStringLiteral(" — REQUEST FILE NOT WRITTEN")));
        refresh_live();
        toast(wrote ? QStringLiteral("HALTED: open orders are being cancelled; no new orders or entries. F11+F12 again to resume.")
                    : QStringLiteral("HALT NOT WRITTEN: %1").arg(kill_path()), wrote);
        return wrote;
    }
    /// Clear the halt, recording who and why beside it. LIVE stays off.
    bool resume_trading(const QString& by, const QString& reason) {
        if (reason.trimmed().isEmpty()) return false;
        const KillRequest k = read_kill_request(kill_path());
        if (k.state == HaltFileState::Absent) return true;
        const ClearRequestResult r = clear_kill_request(kill_path(), k, by, reason.trimmed());
        log_text_->appendPlainText(QStringLiteral("%1  RESUME by %2 (%3): %4")
                                       .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), by, reason,
                                            r == ClearRequestResult::Cleared ? QStringLiteral("cleared") : QStringLiteral("NOT cleared")));
        refresh_live();
        if (r == ClearRequestResult::Cleared) toast(QStringLiteral("Resumed. LIVE is off: switch it on for real orders."), true);
        return r == ClearRequestResult::Cleared;
    }

    /// Send one order to FYERS: a confirmation, then a request for the router.
    /// Returns the request's id.
    std::expected<QString, QString> place_live(const PaperOrder& o, bool ask) {
        if (!live_on_) return std::unexpected(QStringLiteral("LIVE is off: switch it on first"));
        const qint64 lot = std::max<qint64>(1, o.inst.lot);
        if (o.qty <= 0 || o.qty % lot != 0) return std::unexpected(QStringLiteral("The quantity is not a whole number of lots"));
        IntentDraft d;
        d.by = live_user();
        d.token = o.inst.token;
        d.symbol = o.inst.symbol;
        d.exchange = o.inst.exchange;
        d.buy = o.side == PaperSide::Buy;
        d.lots = static_cast<int>(o.qty / lot);
        d.market = o.type == PaperType::Market;
        d.limit_paise = d.market ? 0 : o.limit_paise;
        d.product = o.product;
        d.validity = QStringLiteral("DAY");
        if (!intent_symbol_ok(d.symbol) || !intent_exchange_ok(d.exchange) || d.token == 0)
            return std::unexpected(QStringLiteral("%1 cannot be sent as a live order").arg(d.symbol));
        if (!d.market && d.limit_paise <= 0) return std::unexpected(QStringLiteral("A limit order needs a price"));
        const PaperQuote q = paper_ui::quote_of(client_, o.inst.token);
        const qint64 ref = d.buy ? (q.ask > 0 ? q.ask : q.ltp) : (q.bid > 0 ? q.bid : q.ltp);
        if (ask) {
            const QString text = live_order_confirmation(d, lot, ref);
            bool yes = false;
            if (live_confirm_) {
                yes = live_confirm_(text);
            } else {
                QMessageBox box(QMessageBox::Warning, QStringLiteral("Send a real order to FYERS?"), text, QMessageBox::NoButton, this);
                box.setTextFormat(Qt::RichText);
                auto* send = box.addButton(QStringLiteral("Send to FYERS"), QMessageBox::AcceptRole);
                auto* no = box.addButton(QStringLiteral("Do not send"), QMessageBox::RejectRole);
                box.setDefaultButton(no);
                box.setEscapeButton(no);
                box.exec();
                yes = box.clickedButton() == send;
            }
            if (!yes) return std::unexpected(QStringLiteral("Not sent"));
        }
        const IntentDraft stamped = stamp_draft(d, QDateTime::currentDateTimeUtc());
        ensure_router();
        const QString path = live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingIntentFile);
        if (append_intent(path, stamped) != AppendResult::Ok)
            return std::unexpected(QStringLiteral("The order was NOT sent: %1 could not be written").arg(path));
        log_text_->appendPlainText(QStringLiteral("%1  LIVE REQUEST %2 %3 lot(s) %4 %5 %6 (%7)")
                                       .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                                            d.buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                                       .arg(d.lots)
                                       .arg(d.symbol, d.market ? QStringLiteral("MARKET") : QStringLiteral("LIMIT ") + rupees_text(d.limit_paise),
                                            d.product, stamped.id));
        toast(QStringLiteral("Sent to the order router: %1 %2 — see Live orders").arg(d.buy ? QStringLiteral("BUY") : QStringLiteral("SELL"), d.symbol));
        show_live_orders();
        return stamped.id;
    }

    void show_live_orders() {
        live_orders_->refresh(read_live_router(live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingOrdersFile), now_ns()));
        live_orders_->show();
        live_orders_->raise();
    }

    /// Empty when this window may write the paper book; else why not.
    [[nodiscard]] QString paper_writer() {
        if (!paper_write_error_.isEmpty()) return paper_write_error_;
        const QString lock_path = paper_dir_ + QStringLiteral("/manual.lock");
        if (!paper_lock_ || paper_lock_path_ != lock_path) {
            QDir().mkpath(paper_dir_);
            paper_lock_ = std::make_unique<QLockFile>(lock_path);
            paper_lock_->setStaleLockTime(0);   // stale only when its owner is gone, never by age
            paper_lock_path_ = lock_path;
            paper_locked_ = paper_lock_->tryLock(0);
        }
        return paper_locked_ ? QString()
                             : QStringLiteral("Another Altair window holds the paper book (%1); orders are placed in that one.")
                                   .arg(lock_path);
    }
    /// The paper instrument for a watch row.
    [[nodiscard]] static PaperInstrument paper_instrument(const LiveRow& r) { return paper_instrument_of(r); }

    /// Apply the FYERS snapshot the GETS workspace built. A stale or refused
    /// snapshot leaves the tables as they were and says so above them.
    void apply_gets_account(const GetsTypedAccount& typed) {
        if (!typed.snapshot) {
            account_state_->setText(QStringLiteral("FYERS snapshot not applied: %1").arg(typed.refusal));
            return;
        }
        for (auto it = typed.instruments.begin(); it != typed.instruments.end(); ++it)
            instrument_names_.insert(it.key(), it.value());
        const QString id = QString::fromLatin1(typed.snapshot->account_id.data());
        const Timestamp now{QDateTime::currentMSecsSinceEpoch() * 1'000'000LL};
        const auto applied = apply_account({*typed.snapshot, QStringLiteral("FYERS · %1").arg(id),
                                            FundsSource::Broker, PositionSource::Broker}, now);
        const QString at = QDateTime::fromMSecsSinceEpoch(
            typed.snapshot->observed.observed_at.ns_since_epoch() / 1'000'000, QTimeZone(19800))
            .toString(QStringLiteral("HH:mm:ss"));
        if (applied) {
            account_state_->setText(QStringLiteral("FYERS %1 snapshot of %2 IST applied.%3")
                .arg(id, at, typed.skipped.isEmpty() ? QString()
                    : QStringLiteral(" Skipped: %1.").arg(typed.skipped.join(QStringLiteral("; ")))));
        } else {
            account_state_->setText(QStringLiteral(
                "FYERS snapshot of %1 IST not applied (%2). The tables below keep their previous rows; "
                "the next fetch is within a minute.")
                .arg(at, applied.error() == TerminalAccountError::SnapshotUnusable
                             ? QStringLiteral("older than its 30 s validity")
                             : QStringLiteral("rows refused by the table")));
        }
    }

    /// Apply one typed, credential-free account snapshot to the read-only
    /// Terminal. `now` is UTC nanoseconds supplied by the owner; this method
    /// never reads a clock or contacts a broker. Preconditions: `input.label`
    /// identifies the account for display, and all quantities use the units in
    /// `broker_view::AccountSnapshot`. Stale, mismatched or duplicate data is
    /// refused without partially mutating either table.
    [[nodiscard]] std::expected<void, TerminalAccountError>
    apply_account(const TerminalAccountInput& input, Timestamp now) {
        const auto& snapshot = input.snapshot;
        if (!broker_view::usable(snapshot, now)) {
            return std::unexpected(TerminalAccountError::SnapshotUnusable);
        }
        if (snapshot.positions.status == broker_view::SnapshotSectionStatus::Present
            && snapshot.typed_positions.account_session != snapshot.account_session) {
            return std::unexpected(TerminalAccountError::PositionSessionMismatch);
        }

        // PositionTableModel validates all rows and performs its own diff before
        // publishing model changes. Apply it first so a duplicate/mismatch can
        // never leave newly published funds beside old positions.
        if (snapshot.positions.status == broker_view::SnapshotSectionStatus::Present) {
            const AccountView account{snapshot.account_session,
                                      input.positions_source, input.label};
            const auto applied = positions_model_->apply(
                account, snapshot.typed_positions, now);
            if (!applied) {
                return std::unexpected(TerminalAccountError::PositionApplyRejected);
            }
        } else {
            positions_model_->drop(snapshot.account_session);
        }

        FundsAccountView funds;
        funds.session = snapshot.account_session;
        funds.source = input.funds_source;
        funds.label = input.label;
        funds.connected = snapshot.funds.status
            == broker_view::SnapshotSectionStatus::Present;
        if (funds.connected) funds.funds = snapshot.typed_funds;
        if (!funds_model_->apply(funds)) {
            return std::unexpected(TerminalAccountError::FundsApplyRejected);
        }
        return {};
    }

    void show_halt_controls() {
        right_->setCurrentWidget(halt_);
        show_window(ops_window_);
    }

    // ---- Greek Watch (from the option chain) -----------------------------------
    [[nodiscard]] GreekWatchWindow* greek_watch() const noexcept { return greek_; }
    /// An option's contract and what it is valued on: from the open chain, else
    /// from the instrument master.
    bool greek_leg(quint32 tok, GreekLeg& g) const {
        const LiveChainView* chain = live_->chain();
        if (const ChainContract* c = chain->contract(tok)) {
            g.c = *c;
            g.under = chain->underlying();
            g.fut = chain->future_token();
            g.fut_expiry = chain->future_expiry();
            g.spot = chain->spot_token();
            return true;
        }
        const auto& master = live_->master();
        const auto it = std::find_if(master.begin(), master.end(), [tok](const MasterScrip& m) {
            return m.token == tok && (m.type == QLatin1String("CE") || m.type == QLatin1String("PE"));
        });
        if (it == master.end()) return false;
        g.c.token = it->token; g.c.symbol = it->symbol; g.c.exchange = it->exchange; g.c.strike = it->strike;
        g.c.expiry_day = live_detail::parse_day(it->expiry); g.c.lot = it->lot; g.c.tick = it->tick;
        g.c.call = it->type == QLatin1String("CE");
        g.under = it->name;
        const QString today = QDate::currentDate().toString(Qt::ISODate);
        for (const auto& m : master)
            if (m.name == it->name && m.type == QLatin1String("FUT") && m.expiry >= today) {
                const qint64 e = live_detail::parse_day(m.expiry);
                if (g.fut == 0 || e < g.fut_expiry) { g.fut = m.token; g.fut_expiry = e; }
            }
        g.spot = index_spot_token(it->name);
        if (g.spot == 0)
            for (const auto& m : master)
                if (m.exchange == QLatin1String("NSE") && m.type == QLatin1String("EQ") && m.symbol == it->name) { g.spot = m.token; break; }
        return true;
    }
    /// Add an option to Greek Watch and show it; its prices are asked of the feed.
    bool open_greek(quint32 tok) {
        GreekLeg g;
        if (!greek_leg(tok, g)) { toast(QStringLiteral("Greek Watch takes options: pick a CE or PE"), false); return false; }
        greek_->add(g);
        stream_greeks();
        show_window(greek_);
        return true;
    }
    /// Ask the feed for every Greek Watch option and what it is valued on.
    void stream_greeks() {
        std::vector<std::pair<quint32, QString>> want;
        for (const auto& g : greek_->legs()) {
            want.emplace_back(g.c.token, g.c.symbol);
            if (g.fut != 0) want.emplace_back(g.fut, QStringLiteral("future"));
            if (g.spot != 0) want.emplace_back(g.spot, QStringLiteral("spot"));
        }
        live_->set_extra_requests(std::move(want));
    }
    /// The Greek Watch kept from the last session, once the master is read.
    void restore_greeks() {
        if (greek_restored_ || live_->master().empty()) return;
        greek_restored_ = true;
        for (const auto& [tok, units] : greek_->stored()) {
            GreekLeg g;
            if (!greek_leg(tok, g)) continue;
            g.units = units;
            greek_->add(g);
        }
        stream_greeks();
    }
    /// The movers strip's text now (tests).
    [[nodiscard]] QString movers_text() const { return movers_->text(); }

    /// Top gainers and losers of the streamed NSE stocks by % change.
    struct Mover { QString symbol; double pct = 0.0; double ltp = 0.0; };
    [[nodiscard]] std::vector<Mover> movers() const {
        std::vector<Mover> out;
        for (const auto& r : live_->model()->rows()) {
            if (r.kind != QLatin1String("equity") || !r.fyers.startsWith(QLatin1String("NSE:"))) continue;
            const LivePrice* p = client_->price(r.token);
            if (p == nullptr || p->last_paise <= 0 || !p->has_quote || !p->quote.has(kQuoteHasPrevClose) || p->quote.prev_close <= 0) continue;
            out.push_back({r.symbol, 100.0 * static_cast<double>(p->last_paise - p->quote.prev_close) / static_cast<double>(p->quote.prev_close),
                           static_cast<double>(p->last_paise) / 100.0});
        }
        std::sort(out.begin(), out.end(), [](const Mover& a, const Mover& b) { return a.pct > b.pct; });
        return out;
    }
    void refresh_movers() {
        const auto m = movers();
        if (m.empty()) {
            movers_->setText(QStringLiteral("<span style='color:#6B7380'>movers: waiting for prices</span>"));
            return;
        }
        QStringList up, down;
        for (std::size_t i = 0; i < m.size() && i < 3; ++i)
            if (m[i].pct > 0) up << QStringLiteral("%1 <span style='color:#7FD17F'>+%2%</span>").arg(m[i].symbol).arg(m[i].pct, 0, 'f', 2);
        for (std::size_t i = 0; i < m.size() && i < 3; ++i) {
            const Mover& x = m[m.size() - 1 - i];
            if (x.pct < 0) down << QStringLiteral("%1 <span style='color:#F07A6A'>%2%</span>").arg(x.symbol).arg(x.pct, 0, 'f', 2);
        }
        movers_->setText(QStringLiteral("<span style='color:#7FD17F'>▲</span> %1 &nbsp; <span style='color:#F07A6A'>▼</span> %2")
                             .arg(up.isEmpty() ? QStringLiteral("—") : up.join(QStringLiteral(" · ")),
                                  down.isEmpty() ? QStringLiteral("—") : down.join(QStringLiteral(" · "))));
    }
    void show_movers() {
        const auto m = movers();
        QDialog d(this);
        d.setWindowTitle(QStringLiteral("Top movers · streamed NSE stocks"));
        auto* l = new QHBoxLayout(&d);
        const auto side = [&d, l](const QString& title, const std::vector<Mover>& rows) {
            auto* box = new QVBoxLayout;
            box->addWidget(new QLabel(title, &d));
            auto* t = new QTableWidget(static_cast<int>(rows.size()), 3, &d);
            t->setHorizontalHeaderLabels({QStringLiteral("Symbol"), QStringLiteral("LTP"), QStringLiteral("% change")});
            t->verticalHeader()->hide();
            t->setEditTriggers(QAbstractItemView::NoEditTriggers);
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                t->setItem(i, 0, new QTableWidgetItem(rows[static_cast<std::size_t>(i)].symbol));
                t->setItem(i, 1, new QTableWidgetItem(QString::number(rows[static_cast<std::size_t>(i)].ltp, 'f', 2)));
                t->setItem(i, 2, new QTableWidgetItem(QString::number(rows[static_cast<std::size_t>(i)].pct, 'f', 2)));
            }
            box->addWidget(t);
            l->addLayout(box);
        };
        std::vector<Mover> gain, lose;
        for (std::size_t i = 0; i < m.size() && gain.size() < 10; ++i) if (m[i].pct > 0) gain.push_back(m[i]);
        for (std::size_t i = 0; i < m.size() && lose.size() < 10; ++i) if (m[m.size() - 1 - i].pct < 0) lose.push_back(m[m.size() - 1 - i]);
        side(QStringLiteral("<b style='color:#7FD17F'>Top gainers</b>"), gain);
        side(QStringLiteral("<b style='color:#F07A6A'>Top losers</b>"), lose);
        d.resize(640, 380);
        d.exec();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (obj == movers_ && e->type() == QEvent::MouseButtonRelease) { show_movers(); return true; }
        return QWidget::eventFilter(obj, e);
    }

public:
    /// For tests: what a strip tile currently SAYS.
    [[nodiscard]] QString tile_text(unsigned tok) const {
        return tok == 256265u ? nifty_.label->text()
             : tok == 260105u ? bnf_.label->text()
             : tok == 264969u ? vix_.label->text() : QString();
    }

    /// One price, from wherever it came. on_price feeds the stream through
    /// here and a test feeds it directly, so the WIRING -- which flag goes to
    /// which widget -- is under test, not just each end of it.
    void apply_price(unsigned tok, qint64 paise, bool replay,
                     std::int64_t tick_ns, bool simulated = false) {
        if (paise <= 0) { return; }
        const double px = static_cast<double>(paise) / 100.0;
        // A frame with no exchange stamp is placed on today; only a LIVE one
        // can be missing it, and live is today.
        const std::int64_t ts = tick_ns > 0
            ? tick_ns : QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const TileTag tag = simulated ? TileTag::Sim : replay ? TileTag::Replay : TileTag::Live;
        if (tok == 256265u) { move_tile(nifty_, px, ts, tag); }
        else if (tok == 260105u) { move_tile(bnf_, px, ts, tag); }
        else if (tok == 264969u) { move_tile(vix_, px, ts, tag); }
        chain_->set_spot(tok, paise, replay || simulated, tick_ns);
        refresh_stream();
    }

private:
    [[nodiscard]] static std::int64_t now_ns() { return QDateTime::currentMSecsSinceEpoch() * 1'000'000LL; }

    static void show_window(QWidget* w) {
        w->show();
        w->raise();
        w->activateWindow();
    }

    void toast(const QString& text, bool good = true) {
        toast_->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                            .arg(good ? QStringLiteral("#7EE787") : QStringLiteral("#FF7B72"), text.toHtmlEscaped()));
        toast_timer_.start(8000);
    }

    void build_paper_book() {
        order_book_ = new PaperOrderBookWindow(this);
        trade_book_ = new PaperTradeBookWindow(this);
        message_log_ = new QDialog(this);
        message_log_->setObjectName(QStringLiteral("messageLog"));
        paper_ui::tool_window(message_log_, QStringLiteral("Message log (F10)"), QSize(760, 320));
        auto* ml = new QVBoxLayout(message_log_);
        log_text_ = new QPlainTextEdit(message_log_);
        log_text_->setReadOnly(true);
        log_text_->setMaximumBlockCount(5000);
        ml->addWidget(log_text_);
        picture_ = new MarketPictureWindow(client_, this);
        order_book_->on_cancel = [this](int id) { cancel_order(id); };
        order_book_->on_modify = [this](int id) { modify_order(id); };
        order_book_->on_cancel_all = [this] { cancel_all_orders(); };
        net_window_->on_square_off = [this](const PaperPosition& p) {
            if (p.net == 0) return;
            PaperOrder o;
            o.inst = p.inst;
            o.product = p.product;
            o.side = p.net > 0 ? PaperSide::Sell : PaperSide::Buy;
            o.type = PaperType::Market;
            o.qty = std::llabs(p.net);
            o.note = QStringLiteral("square off");
            const auto r = place_paper(o);
            toast(r ? QStringLiteral("Square off %1 sent").arg(p.inst.symbol) : r.error(), r.has_value());
        };
#ifdef ALTAIR_SOURCE_DIR
        paper_dir_ = QStringLiteral(ALTAIR_SOURCE_DIR "/data/live/paper");
#else
        paper_dir_ = QStringLiteral("data/live/paper");
#endif
        {
            std::vector<PaperOrder> orders;
            std::vector<PaperTrade> trades;
            paper_store::load(paper_dir_ + QStringLiteral("/manual_orders.csv"),
                              paper_dir_ + QStringLiteral("/manual_trades.csv"), now_ns(), orders, trades);
            oms_.restore(std::move(orders), std::move(trades));
        }
        oms_.on_trade = [this](const PaperTrade& t) {
            const QString path = paper_dir_ + QStringLiteral("/manual_trades.csv");
            if (!paper_store::append_trade(path, t)) paper_write_failed(path);
        };
        oms_.on_order = [this](const PaperOrder& o) {
            const QString path = paper_dir_ + QStringLiteral("/manual_orders.csv");
            if (!paper_store::append_order(path, o)) paper_write_failed(path);
        };
#if ALTAIR_HAVE_CHARGES_TOML
        if (const auto rep = load_charges_file(ALTAIR_CHARGES_TOML, schedules_); rep) charges_verified_ = rep->verified;
        else schedules_.clear();
#endif
        oms_.set_expenses([this](const PaperTrade& t) -> std::optional<double> {
#if ALTAIR_HAVE_CHARGES_TOML
            // Refused while config/charges.toml is unverified, as everywhere,
            // unless "Price UNVERIFIED expenses" (Models) is ticked.
            if (schedules_.empty() || (!charges_verified_ && !models_->price_unverified())) return std::nullopt;
            std::vector<ChargeSchedule> s = schedules_;
            for (auto& x : s) x.verified = true;
            const Segment seg = (t.inst.exchange == QLatin1String("NSE") || t.inst.exchange == QLatin1String("BSE")) ? Segment::Cash
                              : t.inst.symbol.endsWith(QLatin1String("FUT")) ? Segment::Fut : Segment::Opt;
            const auto c = demo_costs::fill(seg, t.side == PaperSide::Buy ? Side::Buy : Side::Sell,
                                            static_cast<double>(t.qty), static_cast<double>(t.price_paise) / 100.0,
                                            (t.ns > 0 ? t.ns : now_ns()) / 1'000'000'000LL + 19800, s,
                                            t.inst.exchange == QLatin1String("BSE") || t.inst.exchange == QLatin1String("BFO")
                                                ? Exchange::BSE : Exchange::NSE);
            if (!c.priced) return std::nullopt;
            return c.total;
#else
            (void)t;
            return std::nullopt;
#endif
        });
        connect(client_, &PriceClient::priceUpdated, this, [this](unsigned tok) {
            oms_.on_quote(tok, paper_ui::quote_of(client_, tok), now_ns());
            if (oms_.open_orders() > 0 || net_window_->isVisible()) books_dirty();
        });
        books_timer_.setInterval(300);
        books_timer_.setSingleShot(true);
        connect(&books_timer_, &QTimer::timeout, this, [this] { refresh_books(); });
        toast_timer_.setSingleShot(true);
        connect(&toast_timer_, &QTimer::timeout, this, [this] { toast_->clear(); });
        refresh_books();
    }

    void books_dirty() { if (!books_timer_.isActive()) books_timer_.start(); }

    // ---- LIVE plumbing ---------------------------------------------------------
    [[nodiscard]] QString live_user() const { return intent_user_ok(user_) ? user_ : QStringLiteral("terminal"); }

    void build_live_trading() {
#ifdef ALTAIR_SOURCE_DIR
        live_root_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#else
        live_root_ = QDir::currentPath();
#endif
        live_orders_ = new LiveTradingOrdersWindow(this);
        live_orders_->on_cancel = [this](const QString& id, bool all) { cancel_live(id, all); };
        connect(live_btn_, &QPushButton::clicked, this, [this] { toggle_live(); });
        connect(live_orders_btn_, &QPushButton::clicked, this, [this] { show_live_orders(); });
        live_timer_.setInterval(1000);
        connect(&live_timer_, &QTimer::timeout, this, [this] { refresh_live(); });
        live_timer_.start();
        refresh_live();
    }

    void toggle_live() {
        const LiveTradingArm a = read_live_arm(live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingArmFile),
                                               QDateTime::currentSecsSinceEpoch());
        if (a.armed) { disarm_live(); return; }
        LiveTradingArmDialog d(a.limits, this);
        if (d.exec() == QDialog::Accepted) arm_live(d.limits());
    }

    void cancel_live(const QString& id, bool all) {
        if (!all && id.isEmpty()) { toast(QStringLiteral("Select an order with a FYERS id"), false); return; }
        ensure_router();
        const QString path = live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingCancelsFile);
        if (!append_live_cancel(path, id, all)) { toast(QStringLiteral("Cancel NOT sent: %1 could not be written").arg(path), false); return; }
        toast(all ? QStringLiteral("Cancel all sent to the order router") : QStringLiteral("Cancel %1 sent to the order router").arg(id), true);
    }

    /// Start altair_order_router unless one is running here. Another one
    /// already holding its lock exits by itself (code 3), which is fine.
    void ensure_router() {
        if (!router_autostart_ || router_ != nullptr) return;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (now_ms - router_started_ms_ < 15'000) return;   // a router that keeps dying is not restarted every second
        const QString exe = live_detail::find_helper(QStringLiteral("altair_order_router"));
        if (exe.isEmpty()) {
            router_note_ = QStringLiteral("altair_order_router is not built (build the net preset: .\\build.bat net)");
            return;
        }
        router_started_ms_ = now_ms;
        router_ = new QProcess(this);
        router_->setWorkingDirectory(live_root_);
        router_->setProcessChannelMode(QProcess::MergedChannels);
        connect(router_, &QProcess::readyRead, this, [this] {
            const QStringList lines = QString::fromLocal8Bit(router_->readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString& l : lines)
                log_text_->appendPlainText(QStringLiteral("%1  ROUTER %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), l.trimmed()));
        });
        connect(router_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            router_note_ = code == 3 ? QString() : QStringLiteral("the order router exited (%1); see the message log (F10)").arg(code);
            router_->deleteLater();
            router_ = nullptr;
            refresh_live();
        });
        router_->start(exe, {QStringLiteral("--root"), live_root_});
    }

    void refresh_live() {
        if (live_btn_ == nullptr || live_orders_ == nullptr) return;
        const LiveTradingArm a = read_live_arm(live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingArmFile),
                                               QDateTime::currentSecsSinceEpoch());
        const LiveTradingView v = read_live_router(live_root_ + QLatin1Char('/') + QLatin1String(kLiveTradingOrdersFile), now_ns());
        live_on_ = a.armed;
        if (halted()) {
            live_btn_->setText(QStringLiteral("\u25A0 HALTED"));
            live_btn_->setStyleSheet(QStringLiteral("background:#6E1B1B;color:#FFFFFF;font-weight:700;border-radius:4px;padding:3px 10px;"));
            live_btn_->setToolTip(QStringLiteral("Trading is halted (F11+F12). %1. F11+F12 again to resume.").arg(halt_summary()));
        } else if (live_on_) {
            const QString until = QDateTime::fromSecsSinceEpoch(a.expires_unix).toString(QStringLiteral("HH:mm"));
            live_btn_->setText(!v.running ? QStringLiteral("● LIVE · router down")
                               : v.killed ? QStringLiteral("● LIVE · KILL ON")
                               : v.why.isEmpty() ? QStringLiteral("● LIVE") : QStringLiteral("● LIVE · refusing"));
            live_btn_->setStyleSheet(QStringLiteral("background:#DA3633;color:#FFFFFF;font-weight:700;border-radius:4px;padding:3px 10px;"));
            live_btn_->setToolTip(QStringLiteral("Orders go to FYERS until %1. %3. %2 Click to switch LIVE off.")
                                      .arg(until, live_strategies_text(a.limits), !v.running ? (router_note_.isEmpty() ? QStringLiteral("The order router is not running.")
                                                                                      : router_note_)
                                                  : v.why.isEmpty() ? QString() : QStringLiteral("Refusing: ") + v.why));
            if (!v.running) ensure_router();
        } else {
            live_btn_->setText(QStringLiteral("PAPER"));
            live_btn_->setStyleSheet(QString());
        }
        // Say what became of each live order, once.
        for (const LiveTradingOrder& o : v.orders) {
            const auto it = live_seen_.constFind(o.intent);
            if (it != live_seen_.constEnd() && *it == o.status) continue;
            const bool first = it == live_seen_.constEnd() && !live_seen_loaded_;
            live_seen_.insert(o.intent, o.status);
            if (first) continue;
            log_text_->appendPlainText(QStringLiteral("%1  LIVE %2 %3 %4 %5: %6%7")
                                           .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), o.side)
                                           .arg(o.qty)
                                           .arg(o.symbol, o.status, o.message,
                                                o.id.isEmpty() ? QString() : QStringLiteral(" (FYERS %1)").arg(o.id)));
            if (!o.open())
                toast(QStringLiteral("LIVE %1 %2 %3: %4").arg(o.side, o.symbol, o.status, o.message),
                      o.status == QLatin1String("FILLED") || o.status == QLatin1String("CANCELLED"));
        }
        live_seen_loaded_ = true;
        if (live_orders_->isVisible()) live_orders_->refresh(v);
    }

    void refresh_books() {
        for (const QString& m : oms_.take_messages()) {
            log_text_->appendPlainText(QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), m));
            const bool bad = m.startsWith(QLatin1String("REJECTED"));
            if (m.startsWith(QLatin1String("EXECUTED")) || bad || m.startsWith(QLatin1String("placed"))
                || m.startsWith(QLatin1String("cancelled")))
                toast(m, !bad);
        }
        if (order_book_->isVisible()) order_book_->refresh(oms_);
        if (trade_book_->isVisible()) trade_book_->refresh(oms_);
        if (net_window_->isVisible()) net_window_->refresh(oms_);
    }

    void show_book(QDialog* w) {
        if (w == order_book_) order_book_->refresh(oms_);
        if (w == trade_book_) trade_book_->refresh(oms_);
        show_window(w);
    }
    void show_net_position() {
        net_window_->refresh(oms_);
        show_window(net_window_);
    }
    void show_market_picture() {
        const LiveRow* r = live_->row_of_token(live_->selected_token());
        if (r == nullptr) { toast(QStringLiteral("Select a scrip first"), false); return; }
        picture_->show_for(*r);
        show_window(picture_);
    }
    void show_security_info() {
        const LiveRow* r = live_->row_of_token(live_->selected_token());
        if (r == nullptr) { toast(QStringLiteral("Select a scrip first"), false); return; }
        QDialog d(this);
        d.setWindowTitle(QStringLiteral("Security information · %1").arg(r->symbol));
        auto* l = new QVBoxLayout(&d);
        auto* text = new QLabel(security_info_html(*r, client_->price(r->token)), &d);
        text->setTextFormat(Qt::RichText);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(text);
        d.exec();
    }

    /// + / F1 and − / F2: the order window for the selected scrip.
    void open_order(bool buy) {
        const LiveRow* r = live_->row_of_token(live_->selected_token());
        if (r == nullptr) { toast(QStringLiteral("Select a scrip in the watch or the chain first"), false); return; }
        const PaperInstrument inst = paper_instrument(*r);
        if (!inst.tradable) {
            toast(QStringLiteral("%1 is an index: pick its future or an option").arg(r->symbol), false);
            return;
        }
        PaperOrderWindow w(buy ? PaperSide::Buy : PaperSide::Sell, inst, client_, this);
        const bool live = live_on();
        w.set_live(live);
        if (w.exec() != QDialog::Accepted) return;
        if (live) {
            const auto sent = place_live(w.order(), true);
            if (!sent) toast(sent.error(), false);
            return;
        }
        const auto placed = place_paper(w.order());
        if (!placed) toast(placed.error(), false);
    }
    void cancel_order(int id) {
        if (id <= 0) { toast(QStringLiteral("Select a pending order in the order book (F3)"), false); return; }
        if (!oms_.cancel(id, now_ns())) toast(QStringLiteral("Order #%1 is not pending").arg(id), false);
        books_dirty();
    }
    void cancel_all_orders() {
        const int n = oms_.cancel_all(now_ns());
        if (n == 0) toast(QStringLiteral("No pending orders"), false);
        books_dirty();
    }
    void modify_order(int id) {
        const PaperOrder* o = oms_.order(id);
        if (o == nullptr || o->status != PaperStatus::Open) {
            toast(QStringLiteral("Select a pending order in the order book (F3) to modify"), false);
            return;
        }
        PaperOrderWindow w(o->side, o->inst, client_, this, o);
        if (w.exec() != QDialog::Accepted) return;
        if (!oms_.modify(id, w.qty(), w.limit_paise(), paper_ui::quote_of(client_, o->inst.token), now_ns()))
            toast(QStringLiteral("Modify refused: quantity must be whole lots and a limit needs a price"), false);
        books_dirty();
    }

    enum class TileTag : std::uint8_t { Close, Live, Replay, Sim };

    struct Tile {
        QLabel* label = nullptr;
        double close = 0.0;       ///< the change basis; 0 = none, shown as —
        QString name;
        UiStamped daily;          ///< the settled closes the basis comes from
        std::int64_t basis_day = -1;   ///< ist_day the basis was found for
    };

    static Tile make_tile(QWidget* parent, QHBoxLayout* h) {
        Tile t;
        t.label = new QLabel(parent);
        t.label->setTextFormat(Qt::RichText);
        h->addWidget(t.label);
        return t;
    }

    /// The change is against the last close BEFORE the tick's IST day, found
    /// once per day rather than once per tick.
    static void move_tile(Tile& t, double price, std::int64_t ts_ns, TileTag tag) {
        const std::int64_t day = ist_day(ts_ns);
        if (day != t.basis_day) {
            t.close = prev_close_before(t.daily, ts_ns);
            t.basis_day = day;
        }
        paint_tile(t, price, tag);
    }

    static void paint_tile(Tile& t, double price, TileTag tag) {
        // ABSENCE IS NOT ZERO. With no prior close the change is unknown, and
        // "+0.00 (0.00%)" -- what the first version printed -- is a claim
        // that the market did not move.
        QString move = QStringLiteral(
            "<span style='color:#6B7380'>— no prior close</span>");
        if (t.close > 0.0) {
            const double chg = price - t.close;
            const double pct = 100.0 * chg / t.close;
            const QString col = chg > 0 ? QStringLiteral("#7FD17F")
                              : chg < 0 ? QStringLiteral("#F07A6A")
                                        : QStringLiteral("#D0D6DE");
            move = QStringLiteral("<span style='color:%1'>%2%3 (%4%)</span>")
                       .arg(col)
                       .arg(chg >= 0 ? QStringLiteral("+") : QString())
                       .arg(chg, 0, 'f', 2)
                       .arg(pct, 0, 'f', 2);
        }
        const QString suffix =
            tag == TileTag::Close
                ? QStringLiteral(" <span style='color:#6B7380'>close</span>")
            : tag == TileTag::Replay
                ? QStringLiteral(" <span style='color:#F4C95D'>replay</span>")
            : tag == TileTag::Sim
                ? QStringLiteral(" <span style='color:#F4C95D'>SIM</span>")
                : QString();
        t.label->setText(QStringLiteral(
            "<span style='color:#8A93A2'>%1</span> "
            "<b style='font-family:Consolas'>%2</b> %3%4")
            .arg(t.name)
            .arg(price, 0, 'f', 2)
            .arg(move, suffix));
    }

    /// Never blank. The last settled close from dataset/ until the stream
    /// delivers, and SAID to be a close -- a grey tile that looks live is
    /// worse than one that admits it is yesterday's.
    void seed_strip_from_disk() {
        const auto seed = [](Tile& t, const char* name, const char* dir) {
            t.name = QString::fromLatin1(name);
            t.daily = ui_load_stamped(spot_path(
                QStringLiteral(ALTAIR_DATASET_DIR), QString::fromLatin1(dir),
                "1d"));
            // prev_close_before scans from the end and needs ascending
            // stamps. A file out of order gets NO basis -- the tile says
            // "no prior close" -- rather than a change against the wrong day.
            if (!stamps_ascending(t.daily)) {
                t.daily = UiStamped{};
                t.label->setToolTip(QStringLiteral(
                    "The daily file's dates are out of order, so no change is "
                    "shown."));
            }
            if (t.daily.closes.empty()) { return; }
            const std::int64_t last = t.daily.stamps_ns.back();
            move_tile(t, t.daily.closes.back(), last, TileTag::Close);
        };
        seed(nifty_, "NIFTY 50", "nifty");
        seed(bnf_, "BANKNIFTY", "banknifty");
        seed(vix_, "INDIA VIX", "indiavix");
    }

    void on_price(unsigned tok) {
        const LivePrice* p = client_->price(tok);
        if (p == nullptr) { return; }
        apply_price(tok, p->last_paise, p->replay, p->exchange_ts_ns, p->simulated);
    }

    void refresh_stream() {
        if (!client_->connected()) {
            stream_state_->setText(QStringLiteral(
                "<span style='color:#6B7380'>● STREAM OFF</span>"));
            stream_btn_->setText(QStringLiteral("Connect stream"));
            return;
        }
        // REPLAY IS NEVER SHOWN AS LIVE. The service stamps every replayed
        // frame, and a strip that said LIVE over a 2015 replay is the one
        // mistake this screen must not make.
        const LivePrice* n = client_->price(256265u);
        const bool replay = n != nullptr && n->replay;
        const bool sim = n != nullptr && n->simulated;
        // Short on the strip; the counts are in the status bar and the tooltip.
        stream_state_->setText(QStringLiteral("<span style='color:%1'>● %2</span>")
            .arg(replay || sim ? QStringLiteral("#F4C95D") : QStringLiteral("#7FD17F"))
            .arg(sim ? QStringLiteral("SIM") : replay ? QStringLiteral("REPLAY") : QStringLiteral("LIVE")));
        stream_state_->setToolTip(QStringLiteral("%1 frames · %2 gaps").arg(client_->frames()).arg(client_->gaps()));
        stream_btn_->setText(QStringLiteral("Disconnect"));
    }

    QSplitter* split_ = nullptr;
    QStackedWidget* surface_ = nullptr;
    QWidget* account_surface_ = nullptr;
    QLabel* account_state_ = nullptr;
    GetsWorkspace* gets_ = nullptr;
    QHash<std::uint32_t, InstrumentDisplay> instrument_names_;
    QLabel* movers_ = nullptr;
    QTimer movers_timer_;
    GreekWatchWindow* greek_ = nullptr;
    bool greek_restored_ = false;
    QLabel* toast_ = nullptr;
    QDialog* ops_window_ = nullptr;
    NetPositionWindow* net_window_ = nullptr;
    PaperOrderBookWindow* order_book_ = nullptr;
    PaperTradeBookWindow* trade_book_ = nullptr;
    QDialog* message_log_ = nullptr;
    QPlainTextEdit* log_text_ = nullptr;
    MarketPictureWindow* picture_ = nullptr;
    void paper_write_failed(const QString& path) {
        if (paper_write_error_.isEmpty())
            log_text_->appendPlainText(QStringLiteral("PAPER BOOK NOT WRITTEN: %1. New orders are refused until Altair restarts.").arg(path));
        paper_write_error_ = QStringLiteral("The paper book could not be written (%1); new orders are refused.").arg(path);
        toast(paper_write_error_, false);
    }

    PaperOms oms_;
    QString paper_dir_;
    std::unique_ptr<QLockFile> paper_lock_;
    QString paper_lock_path_, paper_write_error_;
    bool paper_locked_ = false;
    QTimer books_timer_, toast_timer_;
#if ALTAIR_HAVE_CHARGES_TOML
    std::vector<ChargeSchedule> schedules_;
    bool charges_verified_ = false;
#endif
    FundsSummaryModel* funds_model_ = nullptr;
    QTableView* funds_view_ = nullptr;
    PositionTableModel* positions_model_ = nullptr;
    QSortFilterProxyModel* positions_proxy_ = nullptr;
    QTableView* positions_view_ = nullptr;
    QLineEdit* position_filter_ = nullptr;
    WatchlistPanel* watch_ = nullptr;
    OptionChainPanel* chain_ = nullptr;
    QTabWidget* right_ = nullptr;
    OrderTicket* ticket_ = nullptr;
    KillSwitchPanel* halt_ = nullptr;
    PendingIntents* pending_ = nullptr;
    LiveFeedPanel* feed_ = nullptr;
    PriceClient* client_ = nullptr;
    LiveMarketWatch* live_ = nullptr;
    LiveModelsPanel* models_ = nullptr;
    QPushButton* models_btn_ = nullptr;
    QTimer reconnect_;
    bool auto_connect_ = true;
    QLabel* stream_state_ = nullptr;
    QPushButton* stream_btn_ = nullptr;
    Tile nifty_, bnf_, vix_;
    // ---- LIVE ----
    QString user_;
    QString live_root_;
    QPushButton* live_btn_ = nullptr;
    QPushButton* live_orders_btn_ = nullptr;
    LiveTradingOrdersWindow* live_orders_ = nullptr;
    QProcess* router_ = nullptr;
    QString router_note_;
    QTimer live_timer_;
    bool live_on_ = false;
    bool router_autostart_ = true;
    qint64 router_started_ms_ = 0;
    QHash<QString, QString> live_seen_;
    bool live_seen_loaded_ = false;
    std::function<bool(const QString&)> live_confirm_;
};

} // namespace altair::ui
