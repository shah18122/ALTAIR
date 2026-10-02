// desktop/fyers_link.hpp -- browser OAuth handoff for the FYERS primary.
//
// The desktop is intentionally read-side only. This panel launches the
// credential-boundary helper, opens the official login URL, and accepts the
// redirect URL for exchange. The app secret and access token remain in the
// helper process and the gitignored session file, never in Qt widgets.

#pragma once

#include "auth.hpp"
#include "broker_status.hpp"
#include "credential_setup.hpp"
#include "helper_process.hpp"
#include "login_outcome.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace altair::ui {

[[nodiscard]] inline QString redact_fyers_redirect(const QString& input) {
    QString out = input;
    static const QRegularExpression re(
        QStringLiteral(R"((auth_code|code)=(?!<\d+ chars, redacted>)([^&#\s]+))"));
    auto it = re.globalMatch(input);
    while (it.hasNext()) {
        const auto match = it.next();
        out.replace(match.captured(0),
                    QStringLiteral("%1=<%2 chars, redacted>")
                        .arg(match.captured(1))
                        .arg(match.captured(2).size()));
    }
    return out;
}

[[nodiscard]] inline QString fyers_login_url_from(const QString& output) {
    static const QRegularExpression re(
        QStringLiteral(R"(https://api-t1\.fyers\.in/api/v3/generate-authcode\S*)"));
    const auto match = re.match(output);
    return match.hasMatch() ? match.captured(0) : QString{};
}

[[nodiscard]] inline QString find_fyers_login() {
#if defined(_WIN32)
    const QString exe = QStringLiteral("altair_fyers_login.exe");
#else
    const QString exe = QStringLiteral("altair_fyers_login");
#endif
    QStringList tried;
    const QString app = QCoreApplication::applicationDirPath();
    tried << app + QStringLiteral("/") + exe;
    tried << app + QStringLiteral("/../app/") + exe;
    tried << app + QStringLiteral("/../Helpers/") + exe;
    tried << app + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
    tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
    for (const QString& path : tried) {
        const QFileInfo file(path);
        if (file.exists() && file.isFile()) return file.canonicalFilePath();
    }
    return {};
}

class FyersLinkPanel final : public QWidget {
public:
    FyersLinkPanel(Role role, std::function<void()> on_linked,
                   QWidget* parent = nullptr)
        : QWidget(parent), role_(role), on_linked_(std::move(on_linked)) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(14, 14, 14, 14);

        auto* head = new QLabel(
            QStringLiteral(
                "<b>Connect FYERS as Altair's primary broker.</b><br>"
                "The browser handles FYERS login and approval. Altair never "
                "asks for your FYERS password, PIN, TOTP, app secret, or access "
                "token. The exchange runs in a separate helper process."), this);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral(
            "background:#1B3A4B;color:#CFE8F3;padding:8px;"
            "border:1px solid #2E6E8E;"));
        v->addWidget(head);

        exe_ = find_fyers_login();
        auto* where = new QLabel(this);
        where->setWordWrap(true);
        if (exe_.isEmpty()) {
            where->setText(QStringLiteral(
                "<b>altair_fyers_login was not found.</b> Build the <code>net</code> "
                "preset first; the default desktop preset intentionally has no "
                "HTTPS credential transport."));
            where->setStyleSheet(QStringLiteral("color:#F85149;"));
        } else {
            where->setText(QStringLiteral("Using <code>%1</code>").arg(exe_));
            where->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        }
        v->addWidget(where);

        creds_ = new BrokerCredentialForm(
            CredentialBroker::Fyers, role_, [this] { open_login(); },
            [this] {
                if (on_linked_) on_linked_();
            }, this);
        v->addWidget(creds_);

        auto* box = new QGroupBox(QStringLiteral("FYERS OAuth"), this);
        auto* bv = new QVBoxLayout(box);
        open_ = new QPushButton(QStringLiteral("1 · Open FYERS login in browser"), box);
        open_->setObjectName(QStringLiteral("fyersOpenLogin"));
        bv->addWidget(open_);
        outcome_ = new LoginOutcome(box);
        bv->addWidget(outcome_);
        auto* hint = new QLabel(
            QStringLiteral(
                "After approval, FYERS redirects to the registered callback. "
                "Paste the <b>whole redirect URL</b> below. The URL's "
                "auth_code is one-time and short-lived."), box);
        hint->setWordWrap(true);
        bv->addWidget(hint);
        paste_ = new QLineEdit(box);
        paste_->setPlaceholderText(QStringLiteral("paste the FYERS redirect URL here"));
        bv->addWidget(paste_);
        exchange_ = new QPushButton(QStringLiteral("2 · Exchange for FYERS session"), box);
        bv->addWidget(exchange_);
        v->addWidget(box);

        const bool allowed = may(role_, Capability::ChangeFeedSource);
        const bool have = !exe_.isEmpty();
        // Open stays clickable without the helper: the click says why.
        open_->setEnabled(allowed);
        exchange_->setEnabled(allowed && have);
        if (!allowed) {
            auto* no = new QLabel(QStringLiteral(
                "Linking a broker account requires the admin role."), this);
            no->setStyleSheet(QStringLiteral("color:#F85149;"));
            v->addWidget(no);
        }

        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setStyleSheet(QStringLiteral(
            "background:#11171C;color:#D6DBDF;font-family:Consolas,monospace;"
            "font-size:12px;border:none;"));
        log_->setPlainText(QStringLiteral(
            "Nothing run yet.\n\n"
            "Redirect auth_code values are redacted in this pane. Access and "
            "refresh tokens are never printed."));
        v->addWidget(log_, 1);

        connect(open_, &QPushButton::clicked, this, [this] { open_login(); });
        connect(exchange_, &QPushButton::clicked, this,
                [this] { exchange_redirect(); });
    }

    /// Step 1, as the button runs it. Public for the Brokers card's "Log in".
    void open_login() {
        outcome_->hide_url();
        if (!may(role_, Capability::ChangeFeedSource)) {
            outcome_->show_state(LoginOutcomeKind::Problem,
                QStringLiteral("Linking a broker account requires the admin role."));
            return;
        }
        if (exe_.isEmpty()) {
            outcome_->show_state(LoginOutcomeKind::Problem, QStringLiteral(
                "<b>altair_fyers_login is not in this build.</b> It needs the "
                "<code>net</code> preset: run <code>.\\build.bat net</code> in "
                "PowerShell, then start the desktop from <code>build\\net\\desktop</code>."));
            return;
        }
        outcome_->show_state(LoginOutcomeKind::Working,
                             QStringLiteral("Asking altair_fyers_login for the login URL…"));
        say(QStringLiteral("\n· asking the FYERS helper for a login URL..."));
        run_async({}, {}, [this](int code, QString output) {
            if (code == 2) {
                say(output.trimmed());
                outcome_->show_state(LoginOutcomeKind::Problem, QStringLiteral(
                    "<b>FYERS app credentials are not saved yet.</b> Fill <b>FYERS app ID</b>, "
                    "<b>FYERS app secret</b> and the <b>registered redirect URL</b> in "
                    "<i>App credentials</i> above, then press <b>Save &amp; connect</b>."));
                if (creds_ != nullptr) creds_->focus_first();
                return;
            }
            const QString url = fyers_login_url_from(output);
            if (url.isEmpty()) {
                const QString first = output.trimmed().section(QLatin1Char('\n'), 0, 0);
                outcome_->show_state(LoginOutcomeKind::Problem,
                    QStringLiteral("<b>No login URL came back</b> (exit %1): %2")
                        .arg(code).arg(first.toHtmlEscaped()));
                say(QStringLiteral("· no FYERS login URL was returned:"));
                say(output.trimmed());
                return;
            }
            outcome_->show_url(url);
            say(QStringLiteral("· opening the FYERS login in your browser"));
            if (QDesktopServices::openUrl(QUrl(url))) {
                outcome_->show_state(LoginOutcomeKind::Done, QStringLiteral(
                    "<b>FYERS login opened in your browser.</b> Approve it, then copy the whole "
                    "address you land on into the box below and press <b>2</b>. If no browser "
                    "appeared, use the link below."));
            } else {
                outcome_->show_state(LoginOutcomeKind::Problem, QStringLiteral(
                    "<b>No browser opened.</b> Click or copy the login link below."));
            }
        });
    }

    [[nodiscard]] LoginOutcome* outcome() const noexcept { return outcome_; }

    /// Tests point the panel at a stand-in helper.
    void set_helper_for_test(const QString& path) {
        exe_ = path;
        set_link_buttons_enabled(true);
    }

private:
    void say(const QString& text) { log_->appendPlainText(redact_fyers_redirect(text)); }

    void set_link_buttons_enabled(bool enabled) {
        const bool permitted = may(role_, Capability::ChangeFeedSource);
        const bool available = !exe_.isEmpty();
        open_->setEnabled(enabled && permitted);
        exchange_->setEnabled(enabled && permitted && available);
    }

    void run_async(const QStringList& args, QByteArray standard_input,
                   std::function<void(int, QString)> completed) {
        set_link_buttons_enabled(false);
        QString working_directory;
#ifdef ALTAIR_SOURCE_DIR
        working_directory = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        const auto generation = helper_.start(
            exe_, args, working_directory, 30000, std::move(standard_input),
            [this, completed = std::move(completed)](
                HelperProcessResult result) mutable {
                set_link_buttons_enabled(true);
                if (!result.ran_to_completion()) {
                    outcome_->show_state(LoginOutcomeKind::Problem,
                        QStringLiteral("<b>The FYERS helper did not run:</b> %1")
                            .arg(result.detail.toHtmlEscaped()));
                    say(QStringLiteral("· FYERS helper failed: %1")
                            .arg(result.detail));
                    return;
                }
                completed(result.exit_code, std::move(result.output));
            });
        if (!generation) {
            set_link_buttons_enabled(true);
            say(QStringLiteral("· another FYERS helper command is still running"));
        }
    }

    void exchange_redirect() {
        const QString redirect = paste_->text().trimmed();
        if (redirect.isEmpty()) {
            outcome_->show_state(LoginOutcomeKind::Problem,
                QStringLiteral("Paste the whole address the FYERS login sent you to, then press 2."));
            say(QStringLiteral("\n· nothing pasted"));
            return;
        }
        if (exe_.isEmpty()) return;
        outcome_->show_state(LoginOutcomeKind::Working, QStringLiteral("Exchanging the auth code…"));
        say(QStringLiteral("\n· exchanging %1").arg(redact_fyers_redirect(redirect)));
        run_async({QStringLiteral("--stdin")}, redirect.toUtf8() + '\n',
                  [this](int code, QString output) {
            say(output.trimmed());
            if (code == 0) {
                outcome_->hide_url();
                outcome_->show_state(LoginOutcomeKind::Done,
                    QStringLiteral("<b>FYERS session saved for today.</b>"));
                paste_->clear();
                say(QStringLiteral(
                    "· FYERS session metadata written; auth_code is now spent."));
                if (on_linked_) on_linked_();
            } else {
                outcome_->show_state(LoginOutcomeKind::Problem,
                    QStringLiteral("<b>FYERS refused the exchange</b> (exit %1). The auth code is "
                                   "single use: log in again and paste the new address.").arg(code));
                say(QStringLiteral("· FYERS exchange did not succeed (exit %1).")
                        .arg(code));
            }
                  });
    }

    Role role_;
    std::function<void()> on_linked_;
    QString exe_;
    QPushButton* open_ = nullptr;
    QPushButton* exchange_ = nullptr;
    QLineEdit* paste_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    BrokerCredentialForm* creds_ = nullptr;
    LoginOutcome* outcome_ = nullptr;
    // Declared last so it is destroyed first and suppresses callbacks before
    // the widget pointers above are torn down.
    HelperProcess helper_;
};

} // namespace altair::ui
