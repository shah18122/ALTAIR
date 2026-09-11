// desktop/terminal.hpp -- the trading terminal: watchlist, ticket, and halt,
// on one screen.
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
#include "kill_switch.hpp"
#include "live_feed.hpp"
#include "option_chain.hpp"
#include "order_ticket.hpp"
#include "panels.hpp"
#include "price_client.hpp"
#include "data/series_io.hpp"

#include <QFile>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QShortcut>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextStream>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace altair::ui {


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
        auto* keys = new QLabel(QStringLiteral(
            "<span style='color:#8A93A2'>"
            "<b style='color:#7FD17F'>F1</b> buy &nbsp;"
            "<b style='color:#F07A6A'>F2</b> sell &nbsp;"
            "<b>F3</b> book &nbsp;<b>F4</b> chain &nbsp;<b>F5</b> watch "
            "&nbsp;<b>Esc</b> back</span>"), strip);
        keys->setTextFormat(Qt::RichText);
        h->addWidget(keys);
        v->addWidget(strip);

        // ---- THREE COLUMNS: watch, chain, order ------------------------
        split_ = new QSplitter(Qt::Horizontal, this);
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
        v->addWidget(split_, 1);

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

    /// For tests: the composed parts, so a test can drive selection without a
    /// window manager.
    [[nodiscard]] WatchlistPanel* watchlist() const noexcept { return watch_; }
    [[nodiscard]] OrderTicket* ticket() const noexcept { return ticket_; }
    [[nodiscard]] KillSwitchPanel* halt() const noexcept { return halt_; }
    [[nodiscard]] PendingIntents* queue() const noexcept { return pending_; }
    [[nodiscard]] LiveFeedPanel* feed() const noexcept { return feed_; }
    [[nodiscard]] OptionChainPanel* chain() const noexcept { return chain_; }
    [[nodiscard]] PriceClient* stream() const noexcept { return client_; }

private:
    struct Tile {
        QLabel* label = nullptr;
        double close = 0.0;       ///< the last settled close, for the change
        QString name;
    };

    static Tile make_tile(QWidget* parent, QHBoxLayout* h) {
        Tile t;
        t.label = new QLabel(parent);
        t.label->setTextFormat(Qt::RichText);
        h->addWidget(t.label);
        return t;
    }

    void paint_tile(Tile& t, double price, bool live) {
        const double chg = t.close > 0.0 ? price - t.close : 0.0;
        const double pct = t.close > 0.0 ? 100.0 * chg / t.close : 0.0;
        const QString col = chg > 0 ? QStringLiteral("#7FD17F")
                          : chg < 0 ? QStringLiteral("#F07A6A")
                                    : QStringLiteral("#D0D6DE");
        t.label->setText(QStringLiteral(
            "<span style='color:#8A93A2'>%1</span> "
            "<b style='font-family:Consolas'>%2</b> "
            "<span style='color:%3'>%4%5 (%6%)</span>%7")
            .arg(t.name)
            .arg(price, 0, 'f', 2)
            .arg(col)
            .arg(chg >= 0 ? QStringLiteral("+") : QString())
            .arg(chg, 0, 'f', 2)
            .arg(pct, 0, 'f', 2)
            .arg(live ? QString()
                      : QStringLiteral(" <span style='color:#6B7380'>close</span>")));
    }

    /// Never blank. The last settled close from dataset/ until the stream
    /// delivers, and SAID to be a close -- a grey tile that looks live is
    /// worse than one that admits it is yesterday's.
    void seed_strip_from_disk() {
        const auto seed = [](Tile& t, const char* name, const char* dir) {
            t.name = QString::fromLatin1(name);
            const UiStamped s = ui_load_stamped(spot_path(
                QStringLiteral(ALTAIR_DATASET_DIR), QString::fromLatin1(dir),
                "1d"));
            if (s.closes.size() >= 2) {
                t.close = s.closes[s.closes.size() - 2];
                return s.closes.back();
            }
            return s.closes.empty() ? 0.0 : s.closes.back();
        };
        paint_tile(nifty_, seed(nifty_, "NIFTY 50", "nifty"), false);
        paint_tile(bnf_, seed(bnf_, "BANKNIFTY", "banknifty"), false);
        paint_tile(vix_, seed(vix_, "INDIA VIX", "indiavix"), false);
    }

    void on_price(unsigned tok) {
        const LivePrice* p = client_->price(tok);
        if (p == nullptr || p->last_paise <= 0) { return; }
        const double px = static_cast<double>(p->last_paise) / 100.0;
        if (tok == 256265u) { paint_tile(nifty_, px, true); }
        else if (tok == 260105u) { paint_tile(bnf_, px, true); }
        else if (tok == 264969u) { paint_tile(vix_, px, true); }
        chain_->set_spot(tok, p->last_paise);
        refresh_stream();
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
