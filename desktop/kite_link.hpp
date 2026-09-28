// desktop/kite_link.hpp -- Link Kite, without the UI ever holding a credential.
//
// P26-01.
//
// HOW THIS KEEPS GATE 3.
//
// `desktop/` must not link `broker/`, and `broker/` is the only directory that
// touches a credential. So this panel does not do the handshake -- it LAUNCHES
// `altair_kite_login` as a separate process and reports what came back. The
// api key and secret are read from the environment by that process, in its own
// address space, and never enter this one. The UI links no new library and the
// allow-list in desktop/CMakeLists.txt is untouched.
//
// WHAT THE TWO STEPS ARE, AND WHY THE FIRST ONE CANNOT BE AUTOMATED.
//
// Step 1 is a browser login with a Zerodha user id, password and TOTP. Nothing
// in Altair asks for those and nothing in Altair can supply them. This panel
// opens the login page in YOUR browser and stops there.
//
// Step 2 is the checksum and the POST, and that is what the subprocess does,
// given whatever the redirect carried back.
//
// A REQUEST TOKEN IS SINGLE USE AND EXPIRES IN MINUTES.
//
// So the panel says so, next to the paste box, rather than leaving somebody to
// discover it from a 403 twenty minutes later. It has already happened once in
// this project's history.
//
// AND THE TOKEN IS REDACTED IN EVERYTHING THIS PANEL SHOWS.
//
// The redirect URL you paste carries a live credential. It is redacted before
// it reaches the log pane, because that pane is the thing most likely to end up
// in a screenshot. The subprocess never prints a token either -- its own header
// commits to that -- so displaying its output verbatim is safe.

#pragma once

#include "auth.hpp"
#include "broker_status.hpp"
#include "credential_setup.hpp"
#include "helper_process.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
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

/// Redact a request_token anywhere it appears.
///
/// Length is kept because it is diagnostic -- Kite's tokens are 32 characters
/// and a 31-character one is a copy that clipped -- and the value is not.
[[nodiscard]] inline QString redact_request_token(const QString& s) {
    QString out = s;
    static const QRegularExpression re(
        QStringLiteral("request_token=([A-Za-z0-9]+)"));
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out.replace(m.captured(0),
                    QStringLiteral("request_token=<%1 chars, redacted>")
                        .arg(m.captured(1).size()));
    }
    return out;
}

/// Pull the Kite login URL out of the subprocess's usage text.
///
/// The binary prints it when run with no arguments. Parsing its stdout rather
/// than building the URL here is what keeps the api key out of this process:
/// the key is in a URL the browser is about to be handed anyway, and the UI
/// never reads the environment variable that holds it.
[[nodiscard]] inline QString login_url_from(const QString& stdout_text) {
    static const QRegularExpression re(
        QStringLiteral(R"(https://kite\.zerodha\.com/connect/login\S*)"));
    const auto m = re.match(stdout_text);
    return m.hasMatch() ? m.captured(0) : QString();
}

/// Where `altair_kite_login` is, or empty.
///
/// It is built by the `net` preset only -- it needs an HTTPS client, and the
/// default preset has none. A build without it is not a broken install and the
/// panel says which preset supplies it rather than reporting a failure.
[[nodiscard]] inline QString find_kite_login() {
    const QString exe =
#if defined(_WIN32)
        QStringLiteral("altair_kite_login.exe");
#else
        QStringLiteral("altair_kite_login");
#endif
    QStringList tried;
    tried << QCoreApplication::applicationDirPath() + QStringLiteral("/") + exe;
    // build/<preset>/desktop -> build/net/app
    tried << QCoreApplication::applicationDirPath()
                 + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
    tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
    for (const QString& p : tried) {
        const QFileInfo fi(p);
        if (fi.exists() && fi.isFile()) { return fi.canonicalFilePath(); }
    }
    return QString();
}

class KiteLinkPanel final : public QWidget {
public:
    /// `on_linked` is called after a SUCCESSFUL exchange, so the pill repaints
    /// immediately instead of waiting for the next five-second poll. A person
    /// who just linked an account and sees a stale pill concludes it did not
    /// work and does it again with a spent token.
    KiteLinkPanel(Role role, std::function<QString()> on_linked,
                  QWidget* parent = nullptr)
        : QWidget(parent), role_(role), on_linked_(std::move(on_linked)) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(14, 14, 14, 14);

        auto* head = new QLabel(
            QStringLiteral(
                "<b>Two steps, and Altair cannot do the first one.</b><br>"
                "Step 1 is a browser login with your Zerodha id, password and "
                "TOTP. Nothing here asks for those and nothing here can supply "
                "them — the button below opens the page in your browser and "
                "stops.<br><br>"
                "Step 2 is the checksum and the token exchange, and that runs "
                "in <code>altair_kite_login</code>, a separate process. The api "
                "key and secret are read from the environment by that process. "
                "This window never holds them and links no broker code."),
            this);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral(
            "background:#1B3A4B;color:#CFE8F3;padding:8px;"
            "border:1px solid #2E6E8E;"));
        v->addWidget(head);

        exe_ = find_kite_login();
        auto* where = new QLabel(this);
        where->setWordWrap(true);
        if (exe_.isEmpty()) {
            where->setText(QStringLiteral(
                "<b>altair_kite_login was not found.</b> It needs an HTTPS "
                "client and is built by the <code>net</code> preset only. "
                "Run <code>build.bat net</code>. This is not a broken install; "
                "a default build simply has no transport to Kite."));
            where->setStyleSheet(QStringLiteral("color:#C0392B;"));
        } else {
            where->setText(QStringLiteral("Using <code>%1</code>").arg(exe_));
            where->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        }
        v->addWidget(where);

        v->addWidget(new BrokerCredentialForm(
            CredentialBroker::Kite, role_, [this] { step_open(); },
            [this] {
                if (on_linked_) (void)on_linked_();
            }, this));

        auto* box = new QGroupBox(QStringLiteral("Link"), this);
        auto* bv = new QVBoxLayout(box);

        open_ = new QPushButton(
            QStringLiteral("1 · Open the Kite login in your browser"), box);
        bv->addWidget(open_);

        auto* hint = new QLabel(
            QStringLiteral(
                "After you log in, Zerodha redirects to your registered "
                "callback with <code>?request_token=…</code> in the address "
                "bar. Copy the <b>whole URL</b> and paste it below.<br>"
                "<b>A request_token is single use and expires in minutes.</b>"),
            box);
        hint->setWordWrap(true);
        bv->addWidget(hint);

        paste_ = new QLineEdit(box);
        paste_->setPlaceholderText(
            QStringLiteral("paste the whole redirect URL here"));
        // Not a password field: you are pasting it, you can see it in the
        // address bar you copied it from, and hiding it would only stop you
        // noticing that the paste clipped.
        bv->addWidget(paste_);

        exchange_ = new QPushButton(
            QStringLiteral("2 · Exchange it for a session"), box);
        bv->addWidget(exchange_);
        v->addWidget(box);

        const bool allowed = may(role_, Capability::ChangeFeedSource);
        const bool have = !exe_.isEmpty();
        open_->setEnabled(allowed && have);
        exchange_->setEnabled(allowed && have);
        if (!allowed) {
            auto* no = new QLabel(
                QStringLiteral("Linking a broker account requires the admin "
                               "role."), this);
            no->setStyleSheet(QStringLiteral("color:#C0392B;"));
            v->addWidget(no);
        }

        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setStyleSheet(QStringLiteral(
            "background:#11171C;color:#D6DBDF;"
            "font-family:Consolas,monospace;font-size:12px;border:none;"));
        log_->setPlainText(QStringLiteral(
            "Nothing run yet.\n\n"
            "Anything shown here has its request_token redacted. The exchange\n"
            "process never prints an access token at all."));
        v->addWidget(log_, 1);

        connect(open_, &QPushButton::clicked, this, [this] { step_open(); });
        connect(exchange_, &QPushButton::clicked, this,
                [this] { step_exchange(); });
    }

private:
    void say(const QString& s) { log_->appendPlainText(redact_request_token(s)); }

    void set_link_buttons_enabled(bool enabled) {
        const bool permitted = may(role_, Capability::ChangeFeedSource);
        const bool available = !exe_.isEmpty();
        open_->setEnabled(enabled && permitted && available);
        exchange_->setEnabled(enabled && permitted && available);
    }

    void start_helper(const QString& program, const QStringList& arguments,
                      int timeout_ms, QByteArray standard_input,
                      std::function<void(HelperProcessResult)> completed) {
        set_link_buttons_enabled(false);
        QString working_directory;
        // THE WORKING DIRECTORY IS LOAD-BEARING. P26-02.
        //
        // altair_kite_login writes "data/kite_session.json" -- a RELATIVE path
        // -- and QProcess inherits this window's working directory, which for
        // a desktop shortcut is the build output folder. The first successful
        // link through this panel therefore reported SESSION ESTABLISHED and
        // wrote build/default/desktop/data/kite_session.json, while every
        // reader in the tree looks at ALTAIR_SOURCE_DIR/data. The exchange had
        // worked; the file was simply somewhere nobody reads, and the pill and
        // every fetch went on saying TokenException against a four-day-old
        // session.
        //
        // Pinning it here rather than teaching the binary an absolute path:
        // the binary is also run by hand from a shell, where a relative path
        // is the right behaviour, and the caller is the one that knows where
        // the tree is.
#ifdef ALTAIR_SOURCE_DIR
        working_directory = QStringLiteral(ALTAIR_SOURCE_DIR);
#endif
        const auto generation = helper_.start(
            program, arguments, working_directory, timeout_ms,
            std::move(standard_input),
            [this, completed = std::move(completed)](
                HelperProcessResult result) mutable {
                set_link_buttons_enabled(true);
                completed(std::move(result));
            });
        if (!generation) {
            set_link_buttons_enabled(true);
            say(QStringLiteral("· another Kite helper command is still running"));
        }
    }

    void step_open() {
        say(QStringLiteral("\n· asking %1 for the login URL...").arg(exe_));
        start_helper(exe_, {}, 30000, {}, [this](HelperProcessResult result) {
            if (!result.ran_to_completion()) {
                say(QStringLiteral("· Kite helper failed: %1").arg(result.detail));
                return;
            }
            // Exit 2 is "credentials are not configured", and its message is
            // the useful one. A no-argument run returns the browser URL with
            // success; a helper launched by hand no longer looks like a crash.
            if (result.exit_code == 2) {
                say(result.output.trimmed());
                say(QStringLiteral(
                    "· save API key, API secret and the exact registered redirect "
                    "URL in the App credentials form above, then try again."));
                return;
            }
            const QString url = login_url_from(result.output);
            if (url.isEmpty()) {
                say(QStringLiteral("· no login URL in the output:"));
                say(result.output.trimmed());
                return;
            }
            say(QStringLiteral("· opening the Kite login in your browser"));
            if (!QDesktopServices::openUrl(QUrl(url))) {
                say(QStringLiteral(
                    "· could not open a browser. The URL is in the output of "
                    "`altair_kite_login` with no arguments."));
            }
        });
    }

    void step_exchange() {
        const QString arg = paste_->text().trimmed();
        if (arg.isEmpty()) {
            say(QStringLiteral("\n· nothing pasted"));
            return;
        }
        say(QStringLiteral("\n· exchanging %1").arg(redact_request_token(arg)));
        start_helper(exe_, {QStringLiteral("--stdin")}, 30000,
                     arg.toUtf8() + '\n',
                     [this](HelperProcessResult result) {
            if (!result.ran_to_completion()) {
                say(QStringLiteral("· Kite helper failed: %1").arg(result.detail));
                return;
            }
            say(result.output.trimmed());
            if (result.exit_code == 0) {
                // Clear the box: the token is now spent, and a second press
                // with the same value gets a misleading 403.
                paste_->clear();
                say(QStringLiteral("· session written. That token is now SPENT."));
                if (on_linked_) {
                    const QString note = on_linked_();
                    if (!note.isEmpty()) say(note);
                }
                update_dataset();
#if !ALTAIR_HAVE_NET
                say(QStringLiteral(
                    "· note: this build has no HTTP client, so the pill will "
                    "stay NO TRANSPORT. The session file is valid and the "
                    "network helpers can use it. Run `build.bat net` for a "
                    "window that can."));
#endif
            } else {
                say(QStringLiteral(
                    "· exchange did not succeed (exit %1). The token was not "
                    "spent if Kite refused the checksum; it was if Kite "
                    "accepted it and something later failed.")
                        .arg(result.exit_code));
            }
                     });
    }

    /// Bring dataset/ up to date, immediately after linking.
    ///
    /// P34-01. Smit asked that connecting a token update the whole tree
    /// rather than leaving fifteen series at whatever date the last manual
    /// fetch reached. A daily token is exactly the window in which this is
    /// possible, so it happens at the moment the token exists rather than
    /// waiting to be remembered.
    ///
    /// SUBPROCESS, same boundary as everything else here: desktop/ links no
    /// broker/ and holds no credential. altair_kite_update reads the session
    /// file; this window does not and cannot.
    ///
    /// IT MERGES AND IT IS IDEMPOTENT -- running it twice adds nothing -- so
    /// doing it on every link is safe rather than merely convenient.
    void update_dataset() {
        QString exe =
#if defined(_WIN32)
            QStringLiteral("altair_kite_update.exe");
#else
            QStringLiteral("altair_kite_update");
#endif
        QStringList tried;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../app/") + exe;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
        tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
        QString found;
        for (const QString& c : tried) {
            if (QFileInfo(c).isFile()) {
                found = QFileInfo(c).canonicalFilePath();
                break;
            }
        }
        if (found.isEmpty()) {
            say(QStringLiteral(
                "· altair_kite_update is not in this build, so dataset/ was "
                "NOT refreshed. It needs the `net` preset."));
            return;
        }

        say(QStringLiteral(
            "· updating dataset/ -- NIFTY, BANKNIFTY and INDIA VIX at 1m, "
            "5m, 15m, 60m and daily. This takes a minute."));
        start_helper(
            found, {QStringLiteral("--go")}, 600000, {},
            [this](HelperProcessResult result) {
                if (!result.ran_to_completion()) {
                    say(QStringLiteral(
                        "· the updater did not finish (%1). dataset/ may be "
                        "partly updated; it merges, so nothing was lost.")
                            .arg(result.detail));
                    return;
                }
                const QString out = result.output.trimmed();
                for (const QString& line : out.split(QChar('\n'))) {
                    if (!line.trimmed().isEmpty()) say(line);
                }
                if (result.exit_code != 0) {
                    say(QStringLiteral("· dataset updater exited with code %1")
                            .arg(result.exit_code));
                }
            });
    }

    Role role_;
    /// Returns ONE LINE saying what the window did with the new data --
    /// logged here, beside the updater's own output, because that is where
    /// the user is reading. A `void` callback could not say "the grid was not
    /// reloaded, and here is why".
    std::function<QString()> on_linked_;
    QString exe_;
    QPushButton* open_ = nullptr;
    QPushButton* exchange_ = nullptr;
    QLineEdit* paste_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    // Destroyed first, suppressing any in-flight callback before the widget
    // fields above are torn down.
    HelperProcess helper_;
};

} // namespace altair::ui
