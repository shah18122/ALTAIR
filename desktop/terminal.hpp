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
#include "order_ticket.hpp"
#include "panels.hpp"

#include <QFile>
#include <QHBoxLayout>
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

/// Watchlist on the left, ticket and halt on the right.
class TerminalPage final : public QWidget {
    Q_OBJECT

public:
    TerminalPage(Role role, const QString& user, QWidget* parent = nullptr)
        : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        auto* bar = new QLabel(
            QStringLiteral(
                "<b>TERMINAL</b> &nbsp; watchlist, order ticket and halt, on "
                "one screen. Selecting a row on the left loads that instrument "
                "into the ticket. &nbsp; <b>The buttons request; they do not "
                "trade.</b>"),
            this);
        bar->setWordWrap(true);
        bar->setContentsMargins(12, 8, 12, 8);
        bar->setStyleSheet(QStringLiteral(
            "background:#161C22;color:#9FB3C8;border-bottom:1px solid #2C3E50;"));
        v->addWidget(bar);

        split_ = new QSplitter(Qt::Horizontal, this);

        watch_ = new WatchlistPanel(role, split_);
        split_->addWidget(watch_);

        // The right column is tabbed rather than stacked: an order ticket and
        // a halt control are both tall, and stacking them puts the halt below
        // the fold exactly when somebody is scrolling to find it.
        right_ = new QTabWidget(split_);
        ticket_ = new OrderTicket(role, user, right_);
        halt_ = new KillSwitchPanel(role, user, right_);
        right_->addTab(ticket_, QStringLiteral("Order"));
        right_->addTab(halt_, QStringLiteral("Halt"));
        pending_ = new PendingIntents(right_);
        right_->addTab(pending_, QStringLiteral("Queue"));

        // Re-read the queue whenever that tab is opened, so it cannot show a
        // count from before a request this session made.
        connect(right_, &QTabWidget::currentChanged, this, [this](int i) {
            if (right_->widget(i) == pending_) { pending_->reload(); }
        });
        split_->addWidget(right_);

        // The watchlist is the wider half: it carries twelve columns and the
        // ticket is a form. 62/38 rather than a stretch factor, because a
        // splitter with equal factors gives the form room it cannot use.
        split_->setStretchFactor(0, 62);
        split_->setStretchFactor(1, 38);
        v->addWidget(split_, 1);

        // THE ONE WIRE. A watchlist row carries the token and the master's own
        // symbol; the ticket needs exactly those two things and had neither.
        connect(watch_, &WatchlistPanel::instrumentPicked, ticket_,
                &OrderTicket::set_instrument);
    }

    /// For tests: the composed parts, so a test can drive selection without a
    /// window manager.
    [[nodiscard]] WatchlistPanel* watchlist() const noexcept { return watch_; }
    [[nodiscard]] OrderTicket* ticket() const noexcept { return ticket_; }
    [[nodiscard]] KillSwitchPanel* halt() const noexcept { return halt_; }
    [[nodiscard]] PendingIntents* queue() const noexcept { return pending_; }

private:
    QSplitter* split_ = nullptr;
    WatchlistPanel* watch_ = nullptr;
    QTabWidget* right_ = nullptr;
    OrderTicket* ticket_ = nullptr;
    KillSwitchPanel* halt_ = nullptr;
    PendingIntents* pending_ = nullptr;
};

} // namespace altair::ui
