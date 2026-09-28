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
//
// CX02-B3/B4. WHAT THE AUDIT FOUND AT THE ONE SAFETY STEP.
//
//   C17-010  The typed confirmation named side, lots and symbol -- and not the
//            limit price, the order type, the product or the validity. The
//            limit started at 2,400,000 paise and SURVIVED every instrument
//            change, so a NIFTY option picked from the chain was requested at
//            a Rs 24,000 limit, marketable at any price, behind a dialog that
//            never showed the number. Now the limit starts UNSET, is CLEARED
//            whenever the instrument changes, a LIMIT request without one is
//            refused, and the confirmation shows every field that decides the
//            order.
//   C17-011  append_intent checked only open(). A failed write or flush was
//            still logged as PENDING. Every step is checked now.
//   C17-013  set_price narrowed a qint64 to int and snapped every off-tick
//            price DOWN -- more aggressive for a SELL. Out-of-range prices are
//            refused, and a snap in the aggressive direction for the side
//            being requested is refused at submit.
//   C17-019  `at` was local time with no offset. It is UTC with `Z` now, which
//            oms/ requires.
//
// CX02-B3b/B4c, FROM THE INDEPENDENT REVIEW. Five more at the same boundary:
//
//   R-AB-046  B3 replaced an atomic append with a remembered size and a seek,
//             so a writer that appended in between was OVERWRITTEN, and
//             QLockFile's ten-second stale break could hand two writers the
//             same offset. The tail is read under the lock with a read-only
//             handle; the record then goes out through QIODevice::Append, so
//             the KERNEL guarantees it lands at the end even if the advisory
//             lock was broken. A spurious blank line is the worst case, and
//             the drainer skips those.
//   R-AB-042  `at` and `id` were stamped BEFORE the modal confirmation, so a
//             request the operator read for longer than the queue's TTL was
//             quarantined as Expired while this window logged PENDING. They
//             are stamped at the write.
//   R-AB-043  The writer emitted control characters the reader refuses: a
//             newline in `symbol` or `by` splits one record into two lines,
//             both quarantined, while the ticket says PENDING.
//   R-AB-006  ...and the same for any field outside the classes oms/ accepts.
//             The writer now refuses exactly what the reader refuses.
//   R-AB-049  An unnamed token was labelled "token 17512194" for the DISPLAY,
//             and submit() then copied that label into the request as the
//             tradingsymbol. A request may not name a contract that does not
//             exist.
//
//   R-AB-044  And all of it was untestable: every refusal lived inside
//             submit(), behind a modal dialog, so deleting any of them kept
//             the suite green. The refusals are now one pure function.

#pragma once

#include "auth.hpp"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLockFile>

#include <map>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <set>

namespace altair::ui {

#ifndef ALTAIR_INTENT_FILE
#  define ALTAIR_INTENT_FILE "data/order_intents.jsonl"
#endif

/// The largest limit the ticket's control holds, in paise (Rs 10,00,000). A
/// price above it is REFUSED by set_price, never narrowed into range.
inline constexpr int kTicketMaxLimitPaise = 100'000'000;

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

// ── the classes oms/ accepts, enforced HERE too (R-AB-006, R-AB-043) ──────
//
// `oms/order_intent.hpp` refuses anything outside these, and a refusal there
// is a request that silently never happens: this window has already said
// PENDING. The two sides share no header by design, so these mirror the
// reader's classes and the vectors pin the format between them.

[[nodiscard]] inline bool intent_text_ok(const QString& s, int max_len) {
    if (s.isEmpty() || s.size() > max_len) { return false; }
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        // Printable ASCII only. A control character -- a newline above all --
        // splits one record into two lines that are both refused.
        if (u < 0x20 || u > 0x7E) { return false; }
    }
    return true;
}

[[nodiscard]] inline bool intent_symbol_ok(const QString& s) {
    if (!intent_text_ok(s, 64)) { return false; }
    if (s.startsWith(QLatin1Char(' ')) || s.endsWith(QLatin1Char(' '))) {
        return false;
    }
    return !s.contains(QLatin1Char('"')) && !s.contains(QLatin1Char('\\'));
}

[[nodiscard]] inline bool intent_user_ok(const QString& s) {
    if (!intent_text_ok(s, 64)) { return false; }
    for (const QChar c : s) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('.')
            && c != QLatin1Char('_') && c != QLatin1Char('-')
            && c != QLatin1Char('@')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool intent_exchange_ok(const QString& s) {
    if (s.size() < 2 || s.size() > 8) { return false; }
    for (const QChar c : s) {
        if (c < QLatin1Char('A') || c > QLatin1Char('Z')) { return false; }
    }
    return true;
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

/// `at` for an intent: UTC, to the second, with `Z`. oms/ refuses a time with
/// no offset (C17-019), because a local time is an instant only on the
/// machine that wrote it.
[[nodiscard]] inline QString intent_timestamp(const QDateTime& when) {
    return when.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"))
         + QLatin1Char('Z');
}

/// Stamp a draft with the instant it is WRITTEN at.
///
/// R-AB-042: this used to happen before the modal confirmation, so an intent
/// carried the moment the dialog opened. The queue ages an intent by `at`, so
/// a request somebody read carefully was quarantined as Expired while this
/// window had already logged it PENDING.
[[nodiscard]] inline IntentDraft stamp_draft(IntentDraft d,
                                             const QDateTime& now) {
    d.at = intent_timestamp(now);
    d.id = QStringLiteral("%1-%2").arg(now.toMSecsSinceEpoch()).arg(d.token);
    return d;
}

/// Paise as rupees with exactly two decimals, from integers only.
[[nodiscard]] inline QString rupees_text(std::int64_t paise) {
    const bool neg = paise < 0;
    const std::int64_t a = neg ? -paise : paise;
    return QStringLiteral("%1%2.%3")
        .arg(neg ? QStringLiteral("-") : QString())
        .arg(a / 100)
        .arg(static_cast<int>(a % 100), 2, 10, QLatin1Char('0'));
}

/// The typed-confirmation body. Every field that decides what order this
/// becomes is on it: side, lots, symbol, exchange, type, LIMIT PRICE, product
/// and validity (C17-010). Pure, so the test can hold it to that.
[[nodiscard]] inline QString confirmation_text(const IntentDraft& d,
                                               const QString& phrase) {
    const QString price =
        d.market ? QStringLiteral("<b>MARKET</b> -- no limit price")
                 : QStringLiteral("<b>LIMIT Rs %1</b> (%2 paise)")
                       .arg(rupees_text(d.limit_paise))
                       .arg(d.limit_paise);
    return QStringLiteral(
               "This appends a request for<br><br>"
               "<b>%1 %2 lot(s) of %3</b> on %4<br>"
               "%5<br>"
               "product <b>%6</b> &middot; validity <b>%7</b><br><br>"
               "to <code>" ALTAIR_INTENT_FILE "</code>.<br><br>"
               "It does <b>not</b> place an order. <code>oms/</code> validates "
               "it and may refuse.<br><br>"
               "Type <b>%8</b> to confirm:")
        .arg(d.buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
        .arg(d.lots)
        .arg(d.symbol.toHtmlEscaped(), d.exchange.toHtmlEscaped(), price,
             d.product.toHtmlEscaped(), d.validity.toHtmlEscaped(), phrase);
}

/// What appending did. Ordinal 0 is Unset so a zeroed result does not read as
/// a request that was written.
enum class AppendResult : std::uint8_t {
    Unset = 0,
    Ok,
    /// Another writer held the queue's lock file for the whole wait.
    LockTimeout,
    OpenFailed,
    /// A read of the tail, the write, or the flush failed. The queue may hold
    /// a partial line; oms/ refuses that line, and this request was NOT made.
    WriteFailed
};

/// Append one record. APPEND ONLY: never rewrites, never truncates -- oms/
/// holds a byte offset into this file.
///
/// The tail is read under the lock with a READ-ONLY handle, then the record
/// goes out through `QIODevice::Append` in one write: the kernel puts it at
/// the end whatever else happened in between (R-AB-046). Binary mode, so the
/// record ends in LF on every platform; oms/ accepts the CRLF lines older
/// builds wrote in text mode.
[[nodiscard]] inline AppendResult append_intent(const QString& path,
                                                const IntentDraft& d) {
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(10'000);
    if (!lock.tryLock(2000)) {
        return AppendResult::LockTimeout;
    }

    // Does the file already end in a newline? A crashed writer leaves a torn
    // tail, and this record must not be glued onto it (C13-005).
    bool needs_newline = false;
    {
        QFile r(path);
        if (r.exists()) {
            if (!r.open(QIODevice::ReadOnly)) {
                return AppendResult::OpenFailed;
            }
            const qint64 size = r.size();
            if (size > 0) {
                char last = 0;
                if (!r.seek(size - 1) || !r.getChar(&last)) {
                    return AppendResult::OpenFailed;
                }
                needs_newline = (last != '\n');
            }
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return AppendResult::OpenFailed;
    }
    QByteArray record;
    if (needs_newline) {
        record.append('\n');
    }
    record.append(intent_line(d).toUtf8());
    record.append('\n');
    if (f.write(record) != record.size() || !f.flush()) {
        return AppendResult::WriteFailed;
    }
    f.close();
    if (f.error() != QFileDevice::NoError) {
        return AppendResult::WriteFailed;
    }
    return AppendResult::Ok;
}

/// Which way set_price moved a clicked price onto the tick.
enum class TickSnap : std::uint8_t { None = 0, Down, Up };

/// Is a snapped price too aggressive to submit for this side? A snap DOWN
/// makes a SELL cheaper than clicked; a snap UP makes a BUY dearer. Either is
/// refused, and the operator enters the limit instead (C17-013).
[[nodiscard]] constexpr bool snap_too_aggressive(bool buy, TickSnap s) noexcept {
    return (buy && s == TickSnap::Up) || (!buy && s == TickSnap::Down);
}

/// Why a request was refused. Ordinal 0 is None, so a zeroed result does not
/// read as a refusal -- and every other value stops the request.
enum class TicketRefusal : std::uint8_t {
    None = 0,
    /// No lot, tick or exchange for this contract. Rule 1, rule 9.
    NoSpec,
    /// An exchange the reader's class refuses.
    BadExchange,
    /// The name is one this window invented for an unknown token.
    InventedSymbol,
    /// A symbol or user name outside what oms/ accepts -- a control
    /// character, a non-ASCII byte, too long, or empty.
    BadSymbol,
    BadUser,
    /// A LIMIT order with no limit price.
    NoLimit,
    /// A limit that is not a multiple of the tick.
    OffTick,
    /// A clicked price that was snapped toward the aggressive side.
    AggressiveSnap
};

/// Every reason this window refuses to write a request, in one pure place.
///
/// R-AB-044: all of these lived inside `submit()`, behind a modal dialog, so
/// no test could reach them and deleting any one of them kept the suite green.
[[nodiscard]] inline TicketRefusal
refuse_request(qint64 lot, qint64 tick_paise, const QString& exchange,
               bool market, int limit_paise, bool buy, TickSnap snap,
               const QString& symbol, const QString& by, bool symbol_invented) {
    if (lot <= 0 || tick_paise <= 0 || exchange.isEmpty()) {
        return TicketRefusal::NoSpec;
    }
    if (!intent_exchange_ok(exchange)) { return TicketRefusal::BadExchange; }
    if (symbol_invented) { return TicketRefusal::InventedSymbol; }
    if (!intent_symbol_ok(symbol)) { return TicketRefusal::BadSymbol; }
    if (!intent_user_ok(by)) { return TicketRefusal::BadUser; }
    if (!market) {
        if (limit_paise <= 0) { return TicketRefusal::NoLimit; }
        if (limit_paise % tick_paise != 0) { return TicketRefusal::OffTick; }
        if (snap_too_aggressive(buy, snap)) {
            return TicketRefusal::AggressiveSnap;
        }
    }
    return TicketRefusal::None;
}

/// What the log says about a refusal. Separate from the decision so the
/// decision can be tested without parsing prose.
[[nodiscard]] inline QString refusal_message(TicketRefusal r,
                                             const QString& symbol,
                                             int limit_paise, qint64 tick,
                                             bool buy, TickSnap snap) {
    switch (r) {
    case TicketRefusal::NoSpec:
        return QStringLiteral(
                   "  REFUSED — %1 has no resolved spec (lot, tick, "
                   "exchange). Pick it from the option chain or a resolved "
                   "watchlist row.").arg(symbol);
    case TicketRefusal::BadExchange:
        return QStringLiteral(
            "  REFUSED — the exchange on this contract is not one oms/ "
            "accepts (two to eight capitals).");
    case TicketRefusal::InventedSymbol:
        return QStringLiteral(
            "  REFUSED — this window made up the name shown for that token. "
            "A request must not name a contract that does not exist; pick it "
            "from the chain or the watchlist so it carries the master's name.");
    case TicketRefusal::BadSymbol:
        return QStringLiteral(
            "  REFUSED — the tradingsymbol contains something oms/ refuses "
            "(a control character, a non-ASCII byte, or more than 64). The "
            "request would be quarantined, not executed.");
    case TicketRefusal::BadUser:
        return QStringLiteral(
            "  REFUSED — the signed-in user name is outside what oms/ "
            "accepts. Every request from it would be quarantined.");
    case TicketRefusal::NoLimit:
        return QStringLiteral(
            "  REFUSED — no limit price is set. The ticket does not carry a "
            "limit from one instrument to the next; enter one, or click a "
            "price.");
    case TicketRefusal::OffTick:
        return QStringLiteral(
                   "  REFUSED — limit %1 paise is not a multiple of the "
                   "%2-paise tick. The exchange rejects an off-tick price; "
                   "better to say so here than to find out from a rejection.")
            .arg(limit_paise).arg(tick);
    case TicketRefusal::AggressiveSnap:
        return QStringLiteral(
                   "  REFUSED — the clicked price was off the tick and was "
                   "moved %1 to fit it, which makes a %2 more aggressive than "
                   "what was clicked. Enter the limit you mean.")
            .arg(snap == TickSnap::Down ? QStringLiteral("DOWN")
                                        : QStringLiteral("UP"),
                 buy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
    case TicketRefusal::None:
        break;
    }
    return QString();
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

        // CX02-B4. The limit starts UNSET. It used to start at 2,400,000
        // paise -- Rs 24,000, the NIFTY level the first screen was built
        // against -- and that number survived every instrument change.
        price_ = new QSpinBox(box);
        price_->setRange(0, kTicketMaxLimitPaise);
        price_->setSpecialValueText(QStringLiteral("— set a limit —"));
        price_->setValue(0);
        price_->setSingleStep(1);
        price_->setSuffix(QStringLiteral("  paise"));
        form->addRow(QStringLiteral("Limit price"), price_);
        connect(market_, &QCheckBox::toggled, price_, &QSpinBox::setDisabled);
        // Any change to the value -- by hand or by set_price -- ends whatever
        // snap set_price recorded; set_price re-records it after setValue.
        connect(price_, &QSpinBox::valueChanged, this,
                [this](int) { snap_ = TickSnap::None; });

        // P39. Which side the hotkey asked for, and whether the selected
        // contract has a spec at all. Both shown, because an order ticket
        // that silently refuses at submit is worse than one that says so
        // while the operator is still typing.
        side_hint_ = new QLabel(QStringLiteral("F1 buy  ·  F2 sell"), box);
        side_hint_->setStyleSheet(QStringLiteral("color:#8A93A2;padding:4px;"));
        form->addRow(QStringLiteral("Side"), side_hint_);
        spec_line_ = new QLabel(box);
        form->addRow(QString(), spec_line_);
        last_token_ = current_token();
        connect(symbol_, &QComboBox::currentIndexChanged, this, [this](int) {
            on_instrument_changed();
            refresh_spec_line();
        });
        // And ONCE NOW. It was only refreshed on a change, so the ticket first
        // opened with a blank spec line beside an index -- NIFTY 50 or NIFTY
        // BANK -- which cannot be traded at all. Blank read as "fine".
        refresh_spec_line();

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
    /// instrument master, which is authoritative. A known token given a
    /// different name is RENAMED (C17-013): the request carries the name, and
    /// a stale one is an audit trail that names the wrong contract.
    ///
    /// An unnamed token shows as `token 12345`, which is unambiguous on
    /// screen -- and is REMEMBERED as invented, so submit() refuses to send it
    /// as a tradingsymbol (R-AB-049).
    ///
    /// It does NOT touch lots, product or validity. Those are the operator's.
    /// The LIMIT PRICE is different: a change of instrument CLEARS it
    /// (CX02-B4, C17-010), because a price typed for one contract is not a
    /// price for another, and the old rule of keeping it is how a Rs 24,000
    /// limit reached a Rs 100 option.
    void set_instrument(unsigned token, const QString& name) {
        if (token == 0) { return; }
        const QString trimmed = name.trimmed();
        const int at = symbol_->findData(QVariant::fromValue(token));
        if (at >= 0) {
            if (!trimmed.isEmpty() && symbol_->itemText(at) != trimmed) {
                symbol_->setItemText(at, trimmed);
                invented_.erase(token);
            }
            symbol_->setCurrentIndex(at);
            return;
        }
        if (trimmed.isEmpty()) {
            symbol_->addItem(QStringLiteral("token %1").arg(token),
                             QVariant::fromValue(token));
            invented_.insert(token);
        } else {
            symbol_->addItem(trimmed, QVariant::fromValue(token));
        }
        symbol_->setCurrentIndex(symbol_->count() - 1);
    }

    /// Load a contract together with its SPEC -- lot, tick and exchange.
    ///
    /// P39. set_instrument() above carries a token and a name and nothing
    /// else, so a request could be written for a contract whose lot size and
    /// tick size nobody had looked up. That is hard rule 1 and hard rule 9 in
    /// one place: a lot size must come from the spec store, and ambiguity must
    /// block. submit() now REFUSES any token that did not arrive through here.
    void set_contract(unsigned token, const QString& name, qint64 lot,
                      qint64 tick_paise, const QString& exchange) {
        if (token == 0) { return; }
        if (lot <= 0 || tick_paise <= 0 || exchange.isEmpty()) {
            // CX02-B4b. A pick with NO usable spec REVOKES whatever this
            // ticket held for the token -- the watchlist sends one for a row
            // the spec store blocked after it had been picked as resolvable.
            specs_.erase(token);
        } else {
            specs_[token] = Spec{lot, tick_paise, exchange};
        }
        set_instrument(token, name);
        refresh_spec_line();
    }

    /// Put a price in the ticket. EXPLICIT, and only ever called because the
    /// operator clicked a price.
    ///
    /// Snapped onto the tick DOWN, and the direction is remembered: submit()
    /// refuses a snap that makes the request more aggressive than what was
    /// clicked for the side being requested (C17-013). A price above the
    /// control's range is REFUSED and logged; it used to be narrowed to int
    /// and then clamped by the spin box, silently.
    void set_price(qint64 paise) {
        if (paise <= 0) { return; }
        if (paise > price_->maximum()) {
            log_->appendPlainText(
                QStringLiteral("  REFUSED — a price of %1 paise is above the "
                               "ticket's maximum of %2; it was not narrowed.")
                    .arg(paise).arg(price_->maximum()));
            return;
        }
        qint64 v = paise;
        TickSnap snap = TickSnap::None;
        const auto it = specs_.find(current_token());
        if (it != specs_.end() && it->second.tick > 0) {
            const qint64 rem = v % it->second.tick;
            if (rem != 0) {
                v -= rem;
                snap = TickSnap::Down;
                if (v <= 0) {
                    v = it->second.tick;
                    snap = TickSnap::Up;
                }
            }
        }
        market_->setChecked(false);
        // In range: 0 < v <= max(paise, tick) and paise <= maximum() is an
        // int. A tick above the maximum would be a spec error, and is refused.
        if (v > price_->maximum()) { return; }
        price_->setValue(static_cast<int>(v));
        snap_ = snap;
    }

    /// Hotkey entry: bring the ticket up on one side with the size selected.
    ///
    /// F1 / F2, as in ODIN. It FOCUSES; it does not submit. The typed
    /// confirmation in submit() is the safety property from P11-14 and a
    /// hotkey that routed around it would be a one-keystroke order.
    void focus_side(bool buy) {
        side_hint_->setText(buy ? QStringLiteral("BUY  —  F1")
                                : QStringLiteral("SELL  —  F2"));
        side_hint_->setStyleSheet(
            buy ? QStringLiteral("color:#FFFFFF;background:#1B6E3A;"
                                 "padding:4px;font-weight:bold;")
                : QStringLiteral("color:#FFFFFF;background:#8E2020;"
                                 "padding:4px;font-weight:bold;"));
        lots_->setFocus();
        lots_->selectAll();
    }

    /// For tests and for the terminal: which token the ticket would request.
    [[nodiscard]] unsigned current_token() const {
        return symbol_->currentData().toUInt();
    }

    /// For tests: does the ticket hold a spec for this token?
    [[nodiscard]] bool has_spec(unsigned token) const {
        return specs_.find(token) != specs_.end();
    }
    /// For tests: the exchange the spec will route to, empty if none.
    [[nodiscard]] QString spec_exchange(unsigned token) const {
        const auto it = specs_.find(token);
        return it == specs_.end() ? QString() : it->second.exchange;
    }
    /// For tests: the limit price in paise. Zero means UNSET.
    [[nodiscard]] int limit_paise() const { return price_->value(); }
    /// For tests: how the last set_price moved the price onto the tick.
    [[nodiscard]] TickSnap last_snap() const noexcept { return snap_; }
    /// For tests: the instrument name the request would carry.
    [[nodiscard]] QString current_symbol() const {
        return symbol_->currentText();
    }
    /// For tests: did this window invent the name shown for that token?
    [[nodiscard]] bool symbol_is_invented(unsigned token) const {
        return invented_.find(token) != invented_.end();
    }

private:
    /// A different instrument is selected: the limit belongs to the old one.
    void on_instrument_changed() {
        const unsigned now = current_token();
        if (now == last_token_) { return; }
        last_token_ = now;
        price_->setValue(0);
        snap_ = TickSnap::None;
    }

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
        d.by = user_;

        // ---- VALIDATED AT REQUEST TIME (P39, CX02-B4c) ------------------
        //
        // Rule 1: lot and tick come from the spec store. Rule 9: ambiguity
        // blocks rather than guessing. And the writer refuses exactly what the
        // reader refuses, so a request this window calls PENDING is one oms/
        // can actually act on.
        const auto it = specs_.find(d.token);
        const bool have = it != specs_.end();
        const qint64 lot = have ? it->second.lot : 0;
        const qint64 tick = have ? it->second.tick : 0;
        const QString exchange = have ? it->second.exchange : QString();
        const bool market = market_->isChecked();
        const TicketRefusal why =
            refuse_request(lot, tick, exchange, market, price_->value(), buy,
                           snap_, d.symbol, d.by,
                           invented_.find(d.token) != invented_.end());
        if (why != TicketRefusal::None) {
            log_->appendPlainText(refusal_message(why, d.symbol,
                                                  price_->value(), tick, buy,
                                                  snap_));
            return;
        }
        // The exchange comes from the SPEC. It used to be
        // `symbol.endsWith("FUT") ? "NFO" : "NSE"`, which sent every OPTION to
        // the cash segment -- NIFTY2690824000CE ends in CE, not FUT.
        d.exchange = exchange;
        d.lots = lots_->value();
        d.market = market;
        d.limit_paise = d.market ? 0 : price_->value();
        d.product = product_->currentText();
        d.validity = validity_->currentText();

        bool ok = false;
        const QString typed = QInputDialog::getText(
            this, QStringLiteral("Confirm request"),
            confirmation_text(d, phrase), QLineEdit::Normal, QString(), &ok);
        if (!ok || typed != phrase) {
            log_->appendPlainText(QStringLiteral("  cancelled — phrase not "
                                                 "typed exactly"));
            return;
        }

        // STAMPED HERE, not before the dialog: the queue ages an intent by
        // `at`, and a request read carefully for longer than the TTL would be
        // quarantined as Expired while this window said PENDING (R-AB-042).
        d = stamp_draft(d, QDateTime::currentDateTimeUtc());

        const AppendResult wrote =
            append_intent(QStringLiteral(ALTAIR_INTENT_FILE), d);
        if (wrote != AppendResult::Ok) {
            const QString reason =
                wrote == AppendResult::LockTimeout
                    ? QStringLiteral("Another writer held the queue's lock "
                                     "file for two seconds.")
                : wrote == AppendResult::OpenFailed
                    ? QStringLiteral("The queue file could not be opened.")
                    : QStringLiteral("The write did not complete. The file "
                                     "may now end in a partial line, which "
                                     "oms/ refuses.");
            QMessageBox::critical(
                this, QStringLiteral("Could not write the request"),
                reason + QStringLiteral(
                    " NOTHING was requested. This is reported rather than "
                    "retried: a request that may or may not have been written "
                    "is worse than one that certainly was not."));
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
    unsigned last_token_ = 0;
    TickSnap snap_ = TickSnap::None;
    /// Tokens whose displayed name this window made up. A request may not
    /// carry one as a tradingsymbol.
    std::set<unsigned> invented_;

    struct Spec {
        qint64 lot = 0;
        qint64 tick = 0;
        QString exchange;
    };
    std::map<unsigned, Spec> specs_;
    QLabel* side_hint_ = nullptr;
    QLabel* spec_line_ = nullptr;

    void refresh_spec_line() {
        if (spec_line_ == nullptr) { return; }
        const auto it = specs_.find(current_token());
        if (it == specs_.end()) {
            spec_line_->setText(QStringLiteral(
                "spec: UNRESOLVED — this contract cannot be requested"));
            spec_line_->setStyleSheet(QStringLiteral("color:#E06C5B;"));
            if (price_ != nullptr) { price_->setSingleStep(1); }
            return;
        }
        spec_line_->setText(
            QStringLiteral("spec: %1  ·  lot %2  ·  tick %3 paise")
                .arg(it->second.exchange)
                .arg(it->second.lot)
                .arg(it->second.tick));
        spec_line_->setStyleSheet(QStringLiteral("color:#7FB77E;"));
        // The spin box steps by the contract's own tick, from the spec -- not
        // a literal 5 (rule 1).
        if (price_ != nullptr && it->second.tick > 0
            && it->second.tick <= price_->maximum()) {
            price_->setSingleStep(static_cast<int>(it->second.tick));
        }
    }
    QComboBox* product_ = nullptr;
    QComboBox* validity_ = nullptr;
    QPushButton* buy_ = nullptr;
    QPushButton* sell_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
};

} // namespace altair::ui
