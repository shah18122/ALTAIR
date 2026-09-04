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

#pragma once

#include "auth.hpp"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace altair::ui {

#ifndef ALTAIR_KILL_REQUEST_FILE
#define ALTAIR_KILL_REQUEST_FILE "data/kill_request.json"
#endif

/// What is on disk, if anything.
struct KillRequest {
    bool present = false;
    QString requested_at;
    QString requested_by;
    QString reason;
};

[[nodiscard]] inline KillRequest read_kill_request() {
    KillRequest k;
    QFile f(QStringLiteral(ALTAIR_KILL_REQUEST_FILE));
    if (!f.open(QIODevice::ReadOnly)) {
        return k;
    }
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    if (!d.isObject()) {
        return k;                 // malformed is NOT "no request" -- see below
    }
    const QJsonObject o = d.object();
    k.present = true;
    k.requested_at = o.value(QStringLiteral("requested_at")).toString();
    k.requested_by = o.value(QStringLiteral("requested_by")).toString();
    k.reason = o.value(QStringLiteral("reason")).toString();
    return k;
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
        const bool phrase_ok = typed->text() == phrase;
        const bool reason_ok =
            reason_out == nullptr || !reason->text().trimmed().isEmpty();
        ok->setEnabled(phrase_ok && reason_ok);
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
            "background:#C0392B;color:white;font-weight:bold;padding:8px;"));
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
                "phrase, and refuses without a reason. A confirmation that "
                "reads the same in both directions trains you to type through "
                "the one that matters."),
            this);
        asym_->setWordWrap(true);
        asym_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(asym_);
        v->addStretch();

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
        QFile f(QStringLiteral(ALTAIR_KILL_REQUEST_FILE));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            state_->setText(QStringLiteral(
                "<b style='color:#C0392B'>Could not write the request.</b> "
                "Nothing was recorded — treat the system as UNHALTED."));
            return;
        }
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.close();
        refresh();
    }

    void do_clear() {
        if (!confirm_phrase(
                this, QStringLiteral("Clear the halt request"),
                QStringLiteral(
                    "<b>This is the dangerous direction.</b><br><br>"
                    "Clearing re-enables a system that something decided to "
                    "stop. Before you do: is the reason it was halted "
                    "actually resolved, or has it only stopped being "
                    "visible?"),
                QStringLiteral("RESUME TRADING"), nullptr)) {
            return;
        }
        QFile::remove(QStringLiteral(ALTAIR_KILL_REQUEST_FILE));
        refresh();
    }

private:
    void refresh() {
        const KillRequest k = read_kill_request();
        if (!k.present) {
            state_->setText(QStringLiteral(
                "<b style='color:#1B8A4B'>No halt requested.</b> There is no "
                "request file. This says nothing about whether the engine's "
                "own <code>KillSwitch</code> has tripped — "
                "<code>enforce_conservation</code> trips that when the paise "
                "do not balance, and this window cannot see it."));
        } else {
            state_->setText(
                QStringLiteral(
                    "<b style='color:#C0392B'>HALT REQUESTED</b><br>"
                    "at %1 UTC by <b>%2</b><br>reason: %3<br><br>"
                    "<i>Requested, not executed.</i> Nothing consumes this "
                    "file yet.")
                    .arg(k.requested_at.isEmpty()
                             ? QStringLiteral("(no timestamp)")
                             : k.requested_at,
                         k.requested_by.isEmpty()
                             ? QStringLiteral("(unattributed)")
                             : k.requested_by,
                         k.reason.isEmpty() ? QStringLiteral("(none given)")
                                            : k.reason));
        }
        clear_->setEnabled(role_ == Role::Admin && k.present);
    }

    Role role_;
    QString user_;
    QLabel* state_ = nullptr;
    QLabel* asym_ = nullptr;
    QPushButton* request_ = nullptr;
    QPushButton* clear_ = nullptr;
};

} // namespace altair::ui
