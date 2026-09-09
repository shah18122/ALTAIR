// desktop/order_ticket.hpp -- Buy and Sell, without the UI being able to trade.
//
// P25-04.
//
// WHAT THESE BUTTONS DO, IN ONE SENTENCE.
//
// They append a line to `data/order_intents.jsonl`. That is all. `oms/` drains
// that file, re-derives every field from the point-in-time spec store,
// validates against risk limits, and decides. Nothing in this file can reach a
// broker, and `desktop/` links neither `altair_oms` nor `altair_broker` -- the
// gate-3 allow-list in desktop/CMakeLists.txt still refuses both.
//
// WHY IT IS A FILE AND NOT A FUNCTION CALL.
//
// `kill_switch.hpp` already answers this: "A file survives the process, which
// is the exact case this has to work across." The in-process decision means a
// paint bug takes down the process holding live positions. A request written
// just before that must still exist afterwards -- to be drained, or to be
// found by somebody who needs to know it was made.
//
// THE WORD ON THE BUTTON IS "REQUEST", AND THE STATUS IS "PENDING".
//
// The failure this design must not have is a person believing they are flat
// while a request sits in a file. So the button does not say BUY, it says
// REQUEST BUY; the confirmation names the file; and the panel shows the
// request as PENDING with the plain sentence that it is not an order until
// oms/ says so.
//
// NO SHARED HEADER WITH oms/.
//
// This file has its own emitter and does not include `oms/order_intent.hpp`.
// Both sides test against `oms/tests/vectors/intents.jsonl` -- the pattern
// CLAUDE.md sets for server/ and a remote client: shared test vectors, not
// shared code, so a drift fails a test instead of meaning two different orders
// at the two ends of one file.
//
// LOTS, NOT QUANTITY.
//
// The ticket asks for LOTS. It never multiplies by a lot size, because a lot
// size this window read at startup is a lot size that may have changed, and
// rule 1 puts that number in the spec store. oms/ does the multiplication at
// drain time.

#pragma once

#include "auth.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>

namespace altair::ui {

#ifndef ALTAIR_INTENT_FILE
#  define ALTAIR_INTENT_FILE "data/order_intents.jsonl"
#endif

/// One request, as this side builds it.
///
/// Deliberately NOT named OrderIntent and deliberately not the same type oms/
/// parses: they agree on a WIRE FORMAT, pinned by vectors, and nothing else.
struct IntentDraft {
    QString id;
    QString at;
    QString by;
    std::uint32_t token = 0;
    QString symbol;
    QString exchange;
    bool buy = true;
    int lots = 1;
    bool market = false;
    std::int64_t limit_paise = 0;
    QString product = QStringLiteral("NRML");
    QString validity = QStringLiteral("DAY");
};

/// JSON-escape the two characters the schema admits.
///
/// The oms-side reader unescapes exactly these. Anything else is refused there
/// rather than half-decoded, so anything else must not be written here -- a
/// tradingsymbol with a quote in it is a symbol this build does not trade.
[[nodiscard]] inline QString intent_escape(const QString& s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')) {
            out.append(QLatin1Char('\\'));
        }
        out.append(c);
    }
    return out;
}

/// Serialise one draft to a single JSONL line, WITHOUT the trailing newline.
///
/// Field order is part of the contract: the conformance vectors are compared
/// byte for byte, so a reordering here fails the test rather than producing a
/// line that happens to parse.
[[nodiscard]] inline QString intent_line(const IntentDraft& d) {
    return QStringLiteral(
        "{\"v\":1,\"id\":\"%1\",\"at\":\"%2\",\"by\":\"%3\",\"token\":%4,"
        "\"symbol\":\"%5\",\"exchange\":\"%6\",\"side\":\"%7\",\"lots\":%8,"
        "\"order_type\":\"%9\",\"limit_paise\":%10,\"product\":\"%11\","
        "\"validity\":\"%12\"}")
        .arg(intent_escape(d.id), intent_escape(d.at), intent_escape(d.by))
        .arg(d.token)
        .arg(intent_escape(d.symbol), intent_escape(d.exchange),
             d.buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
        .arg(d.lots)
        .arg(d.market ? QStringLiteral("MARKET") : QStringLiteral("LIMIT"))
        .arg(d.market ? 0 : d.limit_paise)
        .arg(intent_escape(d.product), intent_escape(d.validity));
}

/// Append one line. Returns false if the file could not be opened.
///
/// APPEND ONLY. Never rewrites, never truncates: oms/ holds a byte offset into
/// this file, and a truncating writer against a reader holding an offset is a
/// silent-corruption machine.
[[nodiscard]] inline bool append_intent(const QString& path,
                                        const IntentDraft& d) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    QTextStream ts(&f);
    ts << intent_line(d) << "\n";
    return true;
}

/// The ticket.
class OrderTicket final : public QWidget {
    Q_OBJECT

public:
    OrderTicket(Role role, QString user, QWidget* parent = nullptr)
        : QWidget(parent), role_(role), user_(std::move(user)) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(14, 14, 14, 14);

        auto* head = new QLabel(
            QStringLiteral(
                "<b>These buttons do not place an order.</b><br>"
                "They append a REQUEST to <code>" ALTAIR_INTENT_FILE
                "</code>. <code>oms/</code> drains that file, re-derives every "
                "field from the point-in-time spec store, checks it against "
                "risk limits, and decides. This window links neither "
                "<code>oms/</code> nor <code>broker/</code> and cannot reach "
                "an exchange.<br><br>"
                "A request that has not been drained is <b>PENDING</b>. It is "
                "not a position and it is not an order."),
            this);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral(
            "background:#1B3A4B;color:#CFE8F3;padding:8px;"
            "border:1px solid #2E6E8E;"));
        v->addWidget(head);

        auto* box = new QGroupBox(QStringLiteral("Request"), this);
        auto* form = new QFormLayout(box);
        symbol_ = new QComboBox(box);
        // Tokens come from the instrument master, same as the tape. A symbol
        // typed by hand is a symbol that can be wrong; a list cannot be.
        symbol_->addItem(QStringLiteral("NIFTY BANK"), 260105u);
        symbol_->addItem(QStringLiteral("NIFTY 50"), 256265u);
        symbol_->addItem(QStringLiteral("NIFTY26SEPFUT"), 17512194u);
        form->addRow(QStringLiteral("Instrument"), symbol_);

        lots_ = new QSpinBox(box);
        lots_->setRange(1, 100);
        lots_->setValue(1);
        form->addRow(QStringLiteral("Lots (not units)"), lots_);

        market_ = new QCheckBox(QStringLiteral("Market order"), box);
        form->addRow(QString(), market_);

        price_ = new QSpinBox(box);
        price_->setRange(1, 100'000'000);
        price_->setValue(2'400'000);
        price_->setSingleStep(5);
        price_->setSuffix(QStringLiteral("  paise"));
        form->addRow(QStringLiteral("Limit price"), price_);
        connect(market_, &QCheckBox::toggled, price_, &QSpinBox::setDisabled);

        product_ = new QComboBox(box);
        product_->addItems({QStringLiteral("NRML"), QStringLiteral("MIS"),
                            QStringLiteral("CNC")});
        form->addRow(QStringLiteral("Product"), product_);

        validity_ = new QComboBox(box);
        validity_->addItems({QStringLiteral("DAY"), QStringLiteral("IOC")});
        form->addRow(QStringLiteral("Validity"), validity_);
        v->addWidget(box);

        auto* row = new QHBoxLayout;
        buy_ = new QPushButton(QStringLiteral("REQUEST BUY"), this);
        sell_ = new QPushButton(QStringLiteral("REQUEST SELL"), this);
        buy_->setStyleSheet(QStringLiteral(
            "background:#1E6B3A;color:#EAF7EE;padding:8px;font-weight:bold;"));
        sell_->setStyleSheet(QStringLiteral(
            "background:#8C2F2F;color:#FBEAEA;padding:8px;font-weight:bold;"));
        const bool allowed = may(role_, Capability::RequestOrder);
        buy_->setEnabled(allowed);
        sell_->setEnabled(allowed);
        row->addWidget(buy_);
        row->addWidget(sell_);
        v->addLayout(row);

        if (!allowed) {
            auto* no = new QLabel(
                QStringLiteral("Requires the admin role. Staff sees every "
                               "position and every number here and still "
                               "cannot ask for a trade."),
                this);
            no->setStyleSheet(QStringLiteral("color:#C0392B;"));
            no->setWordWrap(true);
            v->addWidget(no);
        }

        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setStyleSheet(QStringLiteral(
            "background:#11171C;color:#D6DBDF;"
            "font-family:Consolas,monospace;font-size:12px;border:none;"));
        log_->setPlainText(QStringLiteral(
            "No request made in this session.\n\n"
            "Requests are appended to " ALTAIR_INTENT_FILE " and survive this\n"
            "window closing. That is deliberate: the engine and the UI share a\n"
            "process, so a crash here must not lose a request somebody made."));
        v->addWidget(log_, 1);

        connect(buy_, &QPushButton::clicked, this, [this] { submit(true); });
        connect(sell_, &QPushButton::clicked, this, [this] { submit(false); });
    }

public Q_SLOTS:
    /// Load an instrument picked somewhere else -- the watchlist, today.
    ///
    /// P32-01. THE COMBO WAS THREE HARD-CODED TOKENS AND THE WATCHLIST HAD
    /// EVERY INSTRUMENT. Before the terminal merged them, an operator watching
    /// a strike had to read its token off one page and find it on another,
    /// and the ticket could only offer the three somebody typed into this
    /// file. Anything else was unreachable from the ticket at all.
    ///
    /// A token arriving here is ADDED if the combo does not have it, and the
    /// name comes from the caller because the caller read it from the
    /// instrument master, which is authoritative. This never invents a name
    /// for a token it does not recognise: an unnamed instrument shows its
    /// token, which is unambiguous, rather than a guess that reads like a
    /// contract.
    ///
    /// It does NOT touch lots, product, validity or the limit price. Those
    /// are the operator's, and silently resetting them on a selection change
    /// is how somebody sends the size they typed for a different instrument.
    void set_instrument(unsigned token, const QString& name) {
        if (token == 0) { return; }
        const int at = symbol_->findData(QVariant::fromValue(token));
        if (at >= 0) {
            symbol_->setCurrentIndex(at);
            return;
        }
        const QString label =
            name.trimmed().isEmpty()
                ? QStringLiteral("token %1").arg(token)
                : name.trimmed();
        symbol_->addItem(label, QVariant::fromValue(token));
        symbol_->setCurrentIndex(symbol_->count() - 1);
    }

    /// For tests and for the terminal: which token the ticket would request.
    [[nodiscard]] unsigned current_token() const {
        return symbol_->currentData().toUInt();
    }

private:
    /// The typed confirmation, deliberately different per side.
    ///
    /// Same reasoning as P11-14's kill switch: "A confirmation dialog that is
    /// the same in both directions trains the muscle memory that defeats it."
    void submit(bool buy) {
        if (!may(role_, Capability::RequestOrder)) { return; }
        const QString phrase = buy ? QStringLiteral("REQUEST BUY")
                                   : QStringLiteral("REQUEST SELL");
        IntentDraft d;
        d.buy = buy;
        d.token = symbol_->currentData().toUInt();
        d.symbol = symbol_->currentText();
        // The exchange follows the instrument, not a control: a future on NSE
        // is a rejected order, and a person picking it from a dropdown is a
        // person who can pick it wrongly.
        d.exchange = d.symbol.endsWith(QStringLiteral("FUT"))
                         ? QStringLiteral("NFO") : QStringLiteral("NSE");
        d.lots = lots_->value();
        d.market = market_->isChecked();
        d.limit_paise = d.market ? 0 : price_->value();
        d.product = product_->currentText();
        d.validity = validity_->currentText();
        d.by = user_;
        d.at = QDateTime::currentDateTime().toString(Qt::ISODate);
        d.id = QStringLiteral("%1-%2")
                   .arg(QDateTime::currentMSecsSinceEpoch())
                   .arg(d.token);

        bool ok = false;
        const QString typed = QInputDialog::getText(
            this, QStringLiteral("Confirm request"),
            QStringLiteral(
                "This appends a request for <b>%1 %2 lot(s) of %3</b> to<br>"
                "<code>" ALTAIR_INTENT_FILE "</code>.<br><br>"
                "It does <b>not</b> place an order. <code>oms/</code> validates "
                "it and may refuse.<br><br>"
                "Type <b>%4</b> to confirm:")
                .arg(buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                .arg(d.lots).arg(d.symbol, phrase),
            QLineEdit::Normal, QString(), &ok);
        if (!ok || typed != phrase) {
            log_->appendPlainText(QStringLiteral("  cancelled — phrase not "
                                                 "typed exactly"));
            return;
        }

        if (!append_intent(QStringLiteral(ALTAIR_INTENT_FILE), d)) {
            QMessageBox::critical(
                this, QStringLiteral("Could not write the request"),
                QStringLiteral(
                    "The queue file could not be opened for append. NOTHING "
                    "was requested. This is reported rather than retried: a "
                    "request that may or may not have been written is worse "
                    "than one that certainly was not."));
            log_->appendPlainText(QStringLiteral("  WRITE FAILED — nothing "
                                                 "was requested"));
            return;
        }
        log_->appendPlainText(
            QStringLiteral("PENDING  %1\n         %2\n"
                           "         not an order until oms/ drains it")
                .arg(d.at, intent_line(d)));
    }

    Role role_;
    QString user_;
    QComboBox* symbol_ = nullptr;
    QSpinBox* lots_ = nullptr;
    QCheckBox* market_ = nullptr;
    QSpinBox* price_ = nullptr;
    QComboBox* product_ = nullptr;
    QComboBox* validity_ = nullptr;
    QPushButton* buy_ = nullptr;
    QPushButton* sell_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
};

} // namespace altair::ui
