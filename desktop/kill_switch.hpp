// desktop/kill_switch.hpp -- the UI's only mutating channel.
//
// P11Q-05c.
//
// CLAUDE.md, "The in-process decision": "The UI holds no handle that can place
// an order. Its only mutating channel is a kill-switch request, which still
// goes through the same confirmation as P11-14 and is still executed by
// `oms/`, never by the UI."
//
// So this REQUESTS. It does not trip anything. `risk/limits.hpp` owns the real
// `KillSwitch` and `enforce_conservation` trips it when the paise do not
// balance; nothing in this file can reach either, and `desktop/` does not link
// `altair_risk` — the gate 3 allow-list would refuse it.
//
// A HALT REQUEST IS FAIL-SAFE AND CLEARING ONE IS NOT. THEY ARE NOT SYMMETRIC.
//
// This is the design decision worth writing down. Writing "stop trading" can
// only ever stop trading: if the file is spurious, or written twice, or
// written by a confused operator, the outcome is that nothing trades. Writing
// "resume" re-enables a system that something decided to halt, and if THAT is
// spurious the outcome is a live position nobody meant to hold.
//
// So the two directions are deliberately unequal:
//
//   REQUEST   any role with RequestKillSwitch. Typed confirmation. One click
//             and a phrase.
//   CLEAR     admin only, a DIFFERENT and longer phrase, and it refuses while
//             the reason field is empty, because "why is it safe to resume"
//             is the question a clear exists to answer.
//
// A confirmation dialog that is the same in both directions trains the
// operator to type through it, and the one that matters is the second.
//
// THE REQUEST IS A FILE, NOT A SIGNAL.
//
// `data/kill_request.json`, with the instant and the user. A file survives the
// UI crashing, which an in-process flag does not — and CLAUDE.md gave up blast
// radius when it put the window in the engine's process, so "the UI died" is a
// case this has to work across. `oms/` polls it when `oms/` exists; today
// nothing consumes it and the panel says so rather than implying the engine
// obeyed.
//
// CX02-B5. THE FAIL-SAFE DIRECTION WAS DISPLAYED AS SAFE (finding C17-014).
//
// A request file that was malformed, empty, or locked read as `present =
// false`, and the panel then printed, in green, "No halt requested. There is
// no request file." -- directly under a comment saying malformed is NOT "no
// request". The write that records a request truncated the file first and
// never checked write(), so a failed write left an EMPTY file where a valid
// request had been, which then read as no request. And the state was read
// once and never again, so another process's request or clear never showed.
//
// Now: the file has THREE states. Unreadable is ordinal zero -- a zeroed
// KillRequest is not "no halt" -- and it is treated as halted. Writes go
// through QSaveFile, so a failed write leaves the old file as it was. The
// panel re-reads the file every two seconds.
//
// CX02-B5b, FROM THE INDEPENDENT REVIEW. Three things B5 left or introduced:
//
//   R-AB-038  The CLEAR path passed `reason_out == nullptr`, so the dangerous
//             direction asked for NO reason at all -- the exact inverse of
//             what the header above and the panel's own on-screen text both
//             promise. And `QFile::remove` then deleted the only record of
//             who halted, when and why. A clear now demands a reason and is
//             RECORDED beside the request before the request is removed.
//   R-AB-039  B5's own two-second poll overwrote the "could not write the
//             request" notice within two seconds: the one message saying a
//             halt was NOT recorded was transient. A failed action latches,
//             and every refresh renders the latch above the file's state
//             until the operator acts again.
//   R-AB-045  A `data/` directory that cannot be read made `QFileInfo::exists`
//             return false, which read as Absent -- the fail-safe direction
//             showing green again. "Not there" is now distinguished from
//             "cannot tell": an unreadable parent directory is Unreadable.

#pragma once

#include "auth.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>

namespace altair::ui {

#ifndef ALTAIR_KILL_REQUEST_FILE
#define ALTAIR_KILL_REQUEST_FILE "data/kill_request.json"
#endif

/// What the request file says, as far as it can be read.
enum class HaltFileState : std::uint8_t {
    /// Ordinal zero, deliberately: a request file that exists but cannot be
    /// read, is not a request object, or whose directory cannot be examined.
    /// Treated as HALTED -- it may be one.
    Unreadable = 0,
    /// Positively not there, in a directory this process can read. The only
    /// state that means no halt was requested.
    Absent,
    /// A readable request.
    Present
};

/// What is on disk, if anything.
struct KillRequest {
    HaltFileState state = HaltFileState::Unreadable;
    QString requested_at;
    QString requested_by;
    QString reason;
    QString request_id;
    QByteArray raw_content;
    bool content_snapshot_complete = false;

    /// Fail-safe: anything but a positively absent file counts as a halt.
    [[nodiscard]] bool halted() const noexcept {
        return state != HaltFileState::Absent;
    }
};

[[nodiscard]] inline KillRequest
read_kill_request(const QString& path = QStringLiteral(ALTAIR_KILL_REQUEST_FILE)) {
    KillRequest k;
    const QFileInfo fi(path);
    if (!fi.exists()) {
        // R-AB-045. `exists()` is false both for "no such file" and for "I
        // could not find out" -- a permission change on data/, or an
        // unavailable share. If the directory it would live in cannot be
        // read, this process cannot tell those apart, and the fail-safe
        // answer is the unreadable one.
        const QFileInfo dir(fi.absolutePath());
        if (!dir.exists() || !dir.isReadable()) {
            return k;                          // Unreadable
        }
        k.state = HaltFileState::Absent;
        k.content_snapshot_complete = true;
        return k;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return k;                              // exists, cannot be read
    }
    const QByteArray bytes = f.readAll();
    if (f.error() != QFileDevice::NoError) {
        return k;
    }
    k.raw_content = bytes;
    k.content_snapshot_complete = true;
    QJsonParseError err{};
    const QJsonDocument d = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !d.isObject()) {
        return k;                              // malformed is NOT "no request"
    }
    const QJsonObject o = d.object();
    k.state = HaltFileState::Present;
    k.requested_at = o.value(QStringLiteral("requested_at")).toString();
    k.requested_by = o.value(QStringLiteral("requested_by")).toString();
    k.reason = o.value(QStringLiteral("reason")).toString();
    k.request_id = o.value(QStringLiteral("request_id")).toString();
    return k;
}

/// Stable per-request lock path shared by every writer and clearer.
[[nodiscard]] inline QString kill_request_lock_path(const QString& path) {
    return QFileInfo(path).absoluteFilePath() + QStringLiteral(".lock");
}

namespace kill_switch_detail {

[[nodiscard]] inline bool same_kill_request_snapshot(const KillRequest& a,
                                                      const KillRequest& b) {
    return a.content_snapshot_complete && b.content_snapshot_complete
        && a.state == b.state && a.raw_content == b.raw_content;
}

/// Write a request while the caller owns the interprocess lock.
[[nodiscard]] inline bool write_kill_request_unlocked(const QString& path,
                                                       QJsonObject o) {
    // Identity must change on every request, even if an operator repeats the
    // same reason in the same timestamp resolution.
    o[QStringLiteral("request_id")] =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

} // namespace kill_switch_detail

/// Write a request, or leave whatever was there untouched. Request and clear
/// share a stable QLockFile so a writer cannot be interleaved between clear's
/// audit commit and request removal.
///
/// QSaveFile writes a temp file and renames it into place on commit(), so a
/// failed write can never leave the empty file a truncate-then-write left.
[[nodiscard]] inline bool write_kill_request(const QString& path,
                                             const QJsonObject& o) {
    QLockFile lock(kill_request_lock_path(path));
    if (!lock.tryLock(5000)) {
        return false;
    }
    return kill_switch_detail::write_kill_request_unlocked(path, o);
}

/// Append a CLEAR beside the request, before the request is removed. The
/// history is an array so a later clear never erases an earlier audit record.
///
/// R-AB-038. Removing the file deletes the only account of who halted, when
/// and why; the clear is the direction that can hurt, so it is the one that
/// must leave a trail. Written to `<path>.cleared.json`, and a failure to
/// append it REFUSES the clear -- an unrecorded clear is what this prevents.
namespace kill_switch_detail {

[[nodiscard]] inline bool record_clear_unlocked(const QString& path,
                                                 const KillRequest& was,
                                                 const QString& by,
                                                 const QString& why) {
    const QString audit_path = path + QStringLiteral(".cleared.json");
    QJsonArray history;
    if (QFileInfo::exists(audit_path)) {
        QFile previous(audit_path);
        if (!previous.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QByteArray existing = previous.readAll();
        if (previous.error() != QFileDevice::NoError) {
            return false;
        }
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(existing, &error);
        if (error.error != QJsonParseError::NoError) {
            return false;                     // never replace unknown history
        }
        if (document.isArray()) {
            history = document.array();
        } else if (document.isObject()) {
            // Preserve the single-record format written by earlier builds.
            history.append(document.object());
        } else {
            return false;
        }
    }

    QJsonObject o;
    o[QStringLiteral("cleared_at")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    o[QStringLiteral("cleared_by")] = by;
    o[QStringLiteral("reason")] = why;
    o[QStringLiteral("was_requested_at")] = was.requested_at;
    o[QStringLiteral("was_requested_by")] = was.requested_by;
    o[QStringLiteral("was_reason")] = was.reason;
    o[QStringLiteral("was_request_id")] = was.request_id;
    o[QStringLiteral("was_state")] =
        was.state == HaltFileState::Present ? QStringLiteral("PRESENT")
      : was.state == HaltFileState::Absent  ? QStringLiteral("ABSENT")
                                            : QStringLiteral("UNREADABLE");
    history.append(o);

    QSaveFile output(audit_path);
    if (!output.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(history).toJson(QJsonDocument::Indented);
    if (output.write(bytes) != bytes.size()) {
        output.cancelWriting();
        return false;
    }
    return output.commit();
}

} // namespace kill_switch_detail

enum class ClearRequestResult : std::uint8_t {
    Cleared,
    RecordFailed,
    RemoveFailed,
    LockFailed,
    RequestChanged
};

namespace kill_switch_detail {
using AfterClearAuditHook = void (*)(void*);
}

/// Persist the audit record before removing the request, so the UI and the
/// test exercise the same ordering. A failed audit write must leave the halt
/// request in place. The optional hook is a deterministic test seam for
/// interleavings at the audit/removal boundary; production callers omit it.
[[nodiscard]] inline ClearRequestResult
clear_kill_request(const QString& path, const KillRequest& was,
                   const QString& by, const QString& why,
                   kill_switch_detail::AfterClearAuditHook after_audit = nullptr,
                   void* hook_context = nullptr) {
    QLockFile lock(kill_request_lock_path(path));
    if (!lock.tryLock(5000)) {
        return ClearRequestResult::LockFailed;
    }

    // The UI reads before entering this helper. Validate that exact snapshot
    // again after acquiring the process-shared lock, before auditing or clear.
    if (!kill_switch_detail::same_kill_request_snapshot(
            was, read_kill_request(path))) {
        return ClearRequestResult::RequestChanged;
    }

    if (!kill_switch_detail::record_clear_unlocked(path, was, by, why)) {
        return ClearRequestResult::RecordFailed;
    }

    if (after_audit != nullptr) {
        after_audit(hook_context);
    }

    // Also compare immediately after the audit commit. This is a defense
    // against legacy or external writers which do not honor the lock; never
    // remove a request whose identity/content differs from what was audited.
    if (!kill_switch_detail::same_kill_request_snapshot(
            was, read_kill_request(path))) {
        return ClearRequestResult::RequestChanged;
    }
    if (QFileInfo::exists(path) && !QFile::remove(path)) {
        return ClearRequestResult::RemoveFailed;
    }
    return ClearRequestResult::Cleared;
}

/// The sentence the panel shows for a state. Pure, so the one that matters --
/// Unreadable never reading as safe -- is testable without a window.
[[nodiscard]] inline QString halt_state_text(const KillRequest& k) {
    switch (k.state) {
    case HaltFileState::Absent:
        return QStringLiteral(
            "<b style='color:#3FB950'>No halt requested.</b> There is no "
            "request file. This says nothing about whether the engine's "
            "own <code>KillSwitch</code> has tripped — "
            "<code>enforce_conservation</code> trips that when the paise "
            "do not balance, and this window cannot see it.");
    case HaltFileState::Present:
        return QStringLiteral(
                   "<b style='color:#F85149'>HALT REQUESTED</b><br>"
                   "at %1 UTC by <b>%2</b><br>reason: %3<br><br>"
                   "<i>Requested, not executed.</i> Nothing consumes this "
                   "file yet.")
            .arg(k.requested_at.isEmpty() ? QStringLiteral("(no timestamp)")
                                          : k.requested_at.toHtmlEscaped(),
                 k.requested_by.isEmpty() ? QStringLiteral("(unattributed)")
                                          : k.requested_by.toHtmlEscaped(),
                 k.reason.isEmpty() ? QStringLiteral("(none given)")
                                    : k.reason.toHtmlEscaped());
    case HaltFileState::Unreadable:
        break;
    }
    return QStringLiteral(
        "<b style='color:#F85149'>HALT STATE UNKNOWN</b><br>"
        "The request file <code>" ALTAIR_KILL_REQUEST_FILE "</code> could not "
        "be read, is not a halt request, or sits in a directory this process "
        "cannot examine. Treat the system as HALTED until someone has looked "
        "at it. Clearing it is the resume direction, and takes the admin "
        "phrase like any other clear.");
}

/// What the panel shows: the file's state, with any LATCHED action failure
/// above it.
///
/// R-AB-039. The two-second poll used to overwrite "could not write the
/// request" with the file's own state, which is true about the file and
/// exactly the wrong thing to leave an operator looking at after they asked
/// for a halt. The latch survives every refresh until the operator acts again.
[[nodiscard]] inline QString halt_panel_text(const KillRequest& k,
                                             const QString& action_error) {
    if (action_error.isEmpty()) {
        return halt_state_text(k);
    }
    return QStringLiteral("<b style='color:#F85149'>%1</b><br><br>%2")
        .arg(action_error.toHtmlEscaped(), halt_state_text(k));
}

/// Is the confirmation's OK button live?
///
/// Pure, so the asymmetry can be tested: the CLEAR direction requires a
/// reason and the REQUEST direction does not (R-AB-038).
[[nodiscard]] inline bool confirm_enabled(bool phrase_matches,
                                          bool reason_required,
                                          const QString& reason) {
    return phrase_matches
        && (!reason_required || !reason.trimmed().isEmpty());
}

/// A confirmation that requires the exact phrase.
///
/// P11-14's discipline: a dialog with an OK button is a dialog people click.
/// The phrase differs between requesting and clearing on purpose -- muscle
/// memory built on one must not carry the other.
[[nodiscard]] inline bool confirm_phrase(QWidget* parent, const QString& title,
                                         const QString& body,
                                         const QString& phrase,
                                         QString* reason_out) {
    QDialog d(parent);
    d.setWindowTitle(title);
    d.setModal(true);
    d.setMinimumWidth(460);
    auto* v = new QVBoxLayout(&d);

    auto* b = new QLabel(body, &d);
    b->setWordWrap(true);
    v->addWidget(b);

    auto* form = new QFormLayout;
    auto* reason = new QLineEdit(&d);
    if (reason_out != nullptr) {
        form->addRow(QStringLiteral("Reason"), reason);
    }
    auto* typed = new QLineEdit(&d);
    typed->setPlaceholderText(phrase);
    form->addRow(QStringLiteral("Type <b>%1</b>").arg(phrase), typed);
    v->addLayout(form);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                        | QDialogButtonBox::Cancel, &d);
    auto* ok = bb->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    v->addWidget(bb);

    // The OK button stays dead until the phrase matches EXACTLY, and until a
    // reason is given where one is demanded. Enabling on a prefix, or
    // case-insensitively, is the same as not asking.
    const auto revalidate = [&] {
        ok->setEnabled(confirm_enabled(typed->text() == phrase,
                                       reason_out != nullptr, reason->text()));
    };
    QObject::connect(typed, &QLineEdit::textChanged, &d, revalidate);
    QObject::connect(reason, &QLineEdit::textChanged, &d, revalidate);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);

    if (d.exec() != QDialog::Accepted) {
        return false;
    }
    if (reason_out != nullptr) {
        *reason_out = reason->text().trimmed();
    }
    return true;
}

class KillSwitchPanel final : public QWidget {
    Q_OBJECT

public:
    explicit KillSwitchPanel(Role role, QString user, QWidget* parent = nullptr)
        : QWidget(parent), role_(role), user_(std::move(user)) {
        auto* v = new QVBoxLayout(this);

        v->addWidget(new QLabel(
            QStringLiteral("<h3>Trade handler — halt</h3>"), this));

        auto* note = new QLabel(
            QStringLiteral(
                "This <b>requests</b> a halt. It does not trip anything: "
                "<code>risk/limits.hpp</code> owns the real "
                "<code>KillSwitch</code>, <code>oms/</code> executes it, and "
                "<code>desktop/</code> links neither — the gate 3 allow-list "
                "refuses them. The request is a file, so it survives this "
                "window crashing, which an in-process flag would not."),
            this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note);

        state_ = new QLabel(this);
        state_->setWordWrap(true);
        v->addWidget(state_);

        request_ = new QPushButton(QStringLiteral("REQUEST HALT"), this);
        request_->setEnabled(may(role_, Capability::RequestKillSwitch));
        request_->setStyleSheet(QStringLiteral(
            "background:#DA3633;color:white;font-weight:bold;padding:8px;"));
        connect(request_, &QPushButton::clicked, this,
                &KillSwitchPanel::do_request);
        v->addWidget(request_);

        clear_ = new QPushButton(QStringLiteral("Clear the request"), this);
        // ADMIN ONLY, and not because clearing is administratively senior --
        // because it is the direction that can hurt. Requesting a halt fails
        // safe; clearing one re-enables a system something decided to stop.
        clear_->setEnabled(role_ == Role::Admin);
        connect(clear_, &QPushButton::clicked, this,
                &KillSwitchPanel::do_clear);
        v->addWidget(clear_);

        asym_ = new QLabel(
            QStringLiteral(
                "The two are deliberately unequal. Writing “stop” can only "
                "stop trading — a spurious one costs nothing but a restart. "
                "Writing “resume” re-enables a system something decided to "
                "halt, and a spurious one is a live position nobody meant to "
                "hold. So clearing is admin-only, takes a <i>different</i> "
                "phrase, refuses without a reason, and is recorded beside the "
                "request before the request is removed. A confirmation that "
                "reads the same in both directions trains you to type through "
                "the one that matters."),
            this);
        asym_->setWordWrap(true);
        asym_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(asym_);
        v->addStretch();

        // CX02-B5. Another process can request or clear; re-read the file
        // rather than showing what this panel last did.
        auto* poll = new QTimer(this);
        poll->setInterval(2000);
        connect(poll, &QTimer::timeout, this, [this] { refresh(); });
        poll->start();

        refresh();
    }

private Q_SLOTS:
    void do_request() {
        QString reason;
        if (!confirm_phrase(
                this, QStringLiteral("Request a halt"),
                QStringLiteral(
                    "This writes a halt request to "
                    "<code>" ALTAIR_KILL_REQUEST_FILE "</code>.<br><br>"
                    "It does not stop anything by itself — <code>oms/</code> "
                    "is what acts on it, and <code>oms/</code> cannot send an "
                    "order today, so nothing is currently listening. The file "
                    "is still written, timestamped and attributed."),
                QStringLiteral("HALT"), &reason)) {
            return;
        }
        QJsonObject o;
        o[QStringLiteral("requested_at")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        o[QStringLiteral("requested_by")] = user_;
        o[QStringLiteral("reason")] = reason;
        if (!write_kill_request(QStringLiteral(ALTAIR_KILL_REQUEST_FILE), o)) {
            // LATCHED, not painted over: the poll would erase it in two
            // seconds, and this is the message that says the halt was NOT
            // recorded (R-AB-039).
            action_error_ = QStringLiteral(
                "Could not write the halt request. NOTHING was recorded — the "
                "file on disk is unchanged, and it is shown below.");
            refresh();
            return;
        }
        action_error_.clear();
        refresh();
    }

    void do_clear() {
        QString reason;
        // A REASON IS REQUIRED HERE. The header and the panel both say so, and
        // until CX02-B5b this passed nullptr, so the dangerous direction asked
        // for nothing at all (R-AB-038).
        if (!confirm_phrase(
                this, QStringLiteral("Clear the halt request"),
                QStringLiteral(
                    "<b>This is the dangerous direction.</b><br><br>"
                    "Clearing re-enables a system that something decided to "
                    "stop. Before you do: is the reason it was halted "
                    "actually resolved, or has it only stopped being "
                    "visible?<br><br>"
                    "The reason you give is recorded beside the request."),
                QStringLiteral("RESUME TRADING"), &reason)) {
            return;
        }
        const QString path = QStringLiteral(ALTAIR_KILL_REQUEST_FILE);
        const KillRequest was = read_kill_request(path);
        const ClearRequestResult result =
            clear_kill_request(path, was, user_, reason);
        if (result == ClearRequestResult::RecordFailed) {
            action_error_ = QStringLiteral(
                "Could not record the clear, so nothing was cleared. The halt "
                "request is untouched.");
            refresh();
            return;
        }
        if (result == ClearRequestResult::LockFailed) {
            action_error_ = QStringLiteral(
                "Could not acquire the shared request lock. Nothing was "
                "cleared; the halt state remains in place.");
            refresh();
            return;
        }
        if (result == ClearRequestResult::RequestChanged) {
            action_error_ = QStringLiteral(
                "The halt request changed while clearing. Nothing was "
                "removed; review the current request before trying again.");
            refresh();
            return;
        }
        if (result == ClearRequestResult::RemoveFailed) {
            action_error_ = QStringLiteral(
                "The clear was recorded, but the request file could not be "
                "removed. The system is still halted.");
            refresh();
            return;
        }
        action_error_.clear();
        refresh();
    }

private:
    void refresh() {
        const KillRequest k = read_kill_request();
        state_->setText(halt_panel_text(k, action_error_));
        clear_->setEnabled(role_ == Role::Admin
                           && k.state != HaltFileState::Absent);
    }

    Role role_;
    QString user_;
    /// The last action failure, until the operator acts again. Rendered above
    /// the file's state on every refresh.
    QString action_error_;
    QLabel* state_ = nullptr;
    QLabel* asym_ = nullptr;
    QPushButton* request_ = nullptr;
    QPushButton* clear_ = nullptr;
};

} // namespace altair::ui
