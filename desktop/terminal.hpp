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
#include "kill_switch.hpp"
#include "live_feed.hpp"
#include "option_chain.hpp"
#include "order_ticket.hpp"
#include "panels.hpp"
#include "funds_summary.hpp"
#include "gets_workspace.hpp"
#include "position_table.hpp"
#include "price_client.hpp"
#include "data/series_io.hpp"

#include <QFile>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QLabel>
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
        : QWidget(parent) {
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
            "ALTAIR TERMINAL</span>"), strip);
        h->addWidget(title);
        nifty_ = make_tile(strip, h);
        bnf_ = make_tile(strip, h);
        vix_ = make_tile(strip, h);
        h->addStretch();
        stream_state_ = new QLabel(strip);
        stream_state_->setTextFormat(Qt::RichText);
        h->addWidget(stream_state_);
        stream_btn_ = new QPushButton(QStringLiteral("Connect stream"), strip);
        stream_btn_->setToolTip(QStringLiteral(
            "Subscribe to altair_price_service on 127.0.0.1:7421. Start it "
            "with --replay nifty 1m to test without a Kite token, or --go for "
            "live."));
        h->addWidget(stream_btn_);
        positions_btn_ = new QPushButton(QStringLiteral("Positions & Greeks"), strip);
        positions_btn_->setToolTip(QStringLiteral(
            "GETS-style workspace: positions and funds, Greek watch, portfolio Greeks, simulation, "
            "expenses, trade history, RMS, top movers and indices"));
        h->addWidget(positions_btn_);
        operations_btn_ = new QPushButton(QStringLiteral("Operations"), strip);
        operations_btn_->setToolTip(QStringLiteral("Legacy order, queue and emergency controls"));
        h->addWidget(operations_btn_);
        auto* keys = new QLabel(QStringLiteral(
            "<span style='color:#8A93A2'>"
            "<b style='color:#7FD17F'>F1</b> buy &nbsp;"
            "<b style='color:#F07A6A'>F2</b> sell &nbsp;"
            "<b>F3</b> book &nbsp;<b>F4</b> chain &nbsp;<b>F5</b> watch "
            "&nbsp;<b>Esc</b> back</span>"), strip);
        keys->setTextFormat(Qt::RichText);
        h->addWidget(keys);
        v->addWidget(strip);

        // ---- ACCOUNT-FIRST SURFACE -------------------------------------
        // P4-07: the Terminal opens on positions and funds. The older trade
        // controls remain reachable under Operations while their workflows
        // are migrated, but they no longer dominate the default view.
        surface_ = new QStackedWidget(this);
        account_surface_ = new QWidget(surface_);
        account_surface_->setObjectName(QStringLiteral("accountSurface"));
        auto* account_layout = new QVBoxLayout(account_surface_);
        account_layout->setContentsMargins(10, 10, 10, 10);
        account_layout->setSpacing(8);

        account_state_ = new QLabel(QStringLiteral(
            "No FYERS snapshot applied yet. Press Refresh FYERS above."), account_surface_);
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
        gets_ = new GetsWorkspace(account_surface_, surface_);
        gets_->on_account = [this](const GetsTypedAccount& typed) { apply_gets_account(typed); };
        surface_->addWidget(gets_);

        connect(position_filter_, &QLineEdit::textChanged, this,
                [this](const QString& text) {
            positions_proxy_->setFilterFixedString(text);
        });

        // ---- LEGACY OPERATIONS SURFACE ---------------------------------
        split_ = new QSplitter(Qt::Horizontal, surface_);
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
        surface_->addWidget(split_);
        surface_->setCurrentWidget(gets_);
        v->addWidget(surface_, 1);
        connect(positions_btn_, &QPushButton::clicked, this,
                [this] { surface_->setCurrentWidget(gets_); });
        connect(operations_btn_, &QPushButton::clicked, this,
                [this] { surface_->setCurrentWidget(split_); });

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
        client_ = new PriceClient(this);
        client_->set_read_buffer(256 * 1024);
        connect(stream_btn_, &QPushButton::clicked, this, [this] {
            if (client_->connected()) {
                client_->stop();
            } else {
                client_->start(QStringLiteral("127.0.0.1"), 7421);
                stream_state_->setText(QStringLiteral(
                    "<span style='color:#F4C95D'>● CONNECTING</span>"));
            }
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
        const auto key = [this](int k, auto fn) {
            auto* sc = new QShortcut(QKeySequence(k), this);
            sc->setContext(Qt::WidgetWithChildrenShortcut);
            connect(sc, &QShortcut::activated, this, fn);
        };
        key(Qt::Key_F1, [this] {
            right_->setCurrentWidget(ticket_);
            ticket_->focus_side(true);
        });
        key(Qt::Key_F2, [this] {
            right_->setCurrentWidget(ticket_);
            ticket_->focus_side(false);
        });
        key(Qt::Key_F3, [this] { right_->setCurrentWidget(pending_); });
        key(Qt::Key_F4, [this] { chain_->setFocus(); });
        key(Qt::Key_F5, [this] { watch_->setFocus(); });
        key(Qt::Key_Escape, [this] { chain_->setFocus(); });

        seed_strip_from_disk();
        refresh_stream();
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
    [[nodiscard]] PriceClient* stream() const noexcept { return client_; }
    [[nodiscard]] FundsSummaryModel* funds_model() const noexcept { return funds_model_; }
    [[nodiscard]] PositionTableModel* positions_model() const noexcept { return positions_model_; }
    [[nodiscard]] QTableView* positions_view() const noexcept { return positions_view_; }
    [[nodiscard]] GetsWorkspace* gets() const noexcept { return gets_; }
    [[nodiscard]] bool account_surface_visible() const noexcept {
        return surface_ != nullptr && surface_->currentWidget() == gets_;
    }
    void show_account_surface() {
        surface_->setCurrentWidget(gets_);
        gets_->tabs()->setCurrentWidget(account_surface_);
    }

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
                "press Refresh FYERS.")
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
        surface_->setCurrentWidget(split_);
        right_->setCurrentWidget(halt_);
    }
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
                     std::int64_t tick_ns) {
        if (paise <= 0) { return; }
        const double px = static_cast<double>(paise) / 100.0;
        // A frame with no exchange stamp is placed on today; only a LIVE one
        // can be missing it, and live is today.
        const std::int64_t ts = tick_ns > 0
            ? tick_ns : QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const TileTag tag = replay ? TileTag::Replay : TileTag::Live;
        if (tok == 256265u) { move_tile(nifty_, px, ts, tag); }
        else if (tok == 260105u) { move_tile(bnf_, px, ts, tag); }
        else if (tok == 264969u) { move_tile(vix_, px, ts, tag); }
        chain_->set_spot(tok, paise, replay, tick_ns);
        refresh_stream();
    }

private:
    enum class TileTag : std::uint8_t { Close, Live, Replay };

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
        apply_price(tok, p->last_paise, p->replay, p->exchange_ts_ns);
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
        stream_state_->setText(QStringLiteral(
            "<span style='color:%1'>● %2</span> "
            "<span style='color:#8A93A2'>%3 frames · %4 gaps</span>")
            .arg(replay ? QStringLiteral("#F4C95D") : QStringLiteral("#7FD17F"))
            .arg(replay ? QStringLiteral("REPLAY") : QStringLiteral("LIVE"))
            .arg(client_->frames())
            .arg(client_->gaps()));
        stream_btn_->setText(QStringLiteral("Disconnect"));
    }

    QSplitter* split_ = nullptr;
    QStackedWidget* surface_ = nullptr;
    QWidget* account_surface_ = nullptr;
    QLabel* account_state_ = nullptr;
    GetsWorkspace* gets_ = nullptr;
    QHash<std::uint32_t, InstrumentDisplay> instrument_names_;
    QPushButton* positions_btn_ = nullptr;
    QPushButton* operations_btn_ = nullptr;
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
    QLabel* stream_state_ = nullptr;
    QPushButton* stream_btn_ = nullptr;
    Tile nifty_, bnf_, vix_;
};

} // namespace altair::ui
