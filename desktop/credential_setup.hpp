// desktop/credential_setup.hpp -- broker app setup through the OS-vault helper.

#pragma once

#include "auth.hpp"
#include "helper_process.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <cstdint>
#include <utility>

namespace altair::ui {

enum class CredentialBroker : std::uint8_t { Fyers, Kite };

[[nodiscard]] inline QString find_credential_helper() {
#if defined(_WIN32)
    const QString name = QStringLiteral("altair_credential_helper.exe");
#else
    const QString name = QStringLiteral("altair_credential_helper");
#endif
    const QString app = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        app + QStringLiteral("/") + name,
        app + QStringLiteral("/../app/") + name,
        app + QStringLiteral("/../Helpers/") + name,
    };
    for (const auto& candidate : candidates) {
        QFileInfo file(candidate);
        if (file.isFile()) return file.canonicalFilePath();
    }
    return {};
}

[[nodiscard]] inline QByteArray credential_frame(
    CredentialBroker broker, QByteArray first, QByteArray secret,
    QByteArray redirect) {
    QByteArray frame = QByteArrayLiteral("ALTAIR-CREDENTIAL/1\nSAVE\n");
    frame += broker == CredentialBroker::Fyers ? "FYERS\n" : "KITE\n";
    frame += first.toBase64() + '\n';
    frame += secret.toBase64() + '\n';
    frame += redirect.toBase64() + '\n';
    first.fill('\0');
    secret.fill('\0');
    redirect.fill('\0');
    return frame;
}

[[nodiscard]] inline QByteArray credential_control_frame(
    CredentialBroker broker, const char* operation) {
    QByteArray frame = QByteArrayLiteral("ALTAIR-CREDENTIAL/1\n");
    frame += operation;
    frame += '\n';
    frame += broker == CredentialBroker::Fyers ? "FYERS\n" : "KITE\n";
    return frame;
}

class BrokerCredentialForm final : public QGroupBox {
public:
    BrokerCredentialForm(CredentialBroker broker, Role role,
                         std::function<void()> on_connect,
                         std::function<void()> on_changed,
                         QWidget* parent = nullptr)
        : QGroupBox(QStringLiteral("App credentials · protected OS vault"), parent),
          broker_(broker), role_(role), on_connect_(std::move(on_connect)),
          on_changed_(std::move(on_changed)), helper_path_(find_credential_helper()) {
        auto* root = new QVBoxLayout(this);
        auto* note = new QLabel(
            QStringLiteral(
                "Values briefly pass through this setup form and a private pipe, "
                "then are stored by Windows Credential Manager or macOS Keychain. "
                "They are never written to project files or process arguments."),
            this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#9FB3C8;font-size:11px;"));
        root->addWidget(note);

        auto* form = new QFormLayout;
        first_ = new QLineEdit(this);
        secret_ = new QLineEdit(this);
        redirect_ = new QLineEdit(this);
        secret_->setEchoMode(QLineEdit::Password);
        secret_->setClearButtonEnabled(true);
        redirect_->setPlaceholderText(broker_ == CredentialBroker::Fyers
            ? QStringLiteral("https://…/fyersredirect")
            : QStringLiteral("https://…/kite/callback"));
        form->addRow(broker_ == CredentialBroker::Fyers
                         ? QStringLiteral("FYERS app ID")
                         : QStringLiteral("Kite API key"),
                     first_);
        form->addRow(broker_ == CredentialBroker::Fyers
                         ? QStringLiteral("FYERS app secret")
                         : QStringLiteral("Kite API secret"),
                     secret_);
        form->addRow(QStringLiteral("Registered redirect URL"), redirect_);
        root->addLayout(form);

        auto* actions = new QHBoxLayout;
        save_ = new QPushButton(QStringLiteral("Save / replace"), this);
        connect_ = new QPushButton(QStringLiteral("Save & connect"), this);
        remove_ = new QPushButton(QStringLiteral("Delete saved credentials"), this);
        actions->addWidget(save_);
        actions->addWidget(connect_);
        actions->addWidget(remove_);
        root->addLayout(actions);
        status_ = new QLabel(this);
        status_->setWordWrap(true);
        root->addWidget(status_);

        const bool allowed = may(role_, Capability::ChangeFeedSource);
        const bool available = !helper_path_.isEmpty();
        set_actions_enabled(allowed && available);
        if (!allowed) {
            status_->setText(QStringLiteral("Admin role is required to change credentials."));
        } else if (!available) {
            status_->setText(QStringLiteral(
                "altair_credential_helper was not found in this build/install."));
        } else {
            query_status();
        }

        QObject::connect(save_, &QPushButton::clicked, this,
                         [this] { save(false); });
        QObject::connect(connect_, &QPushButton::clicked, this,
                         [this] { save(true); });
        QObject::connect(remove_, &QPushButton::clicked, this,
                         [this] { erase(); });
    }

private:
    void set_actions_enabled(bool enabled) {
        save_->setEnabled(enabled);
        connect_->setEnabled(enabled);
        remove_->setEnabled(enabled);
    }

    [[nodiscard]] bool validate() {
        const QString first = first_->text().trimmed();
        const QString secret = secret_->text();
        const QUrl redirect{redirect_->text().trimmed()};
        if (first.isEmpty() || first.toUtf8().size() > 4096) {
            status_->setText(QStringLiteral("Enter a valid app/API identifier."));
            return false;
        }
        if (secret.isEmpty() || secret.toUtf8().size() > 4096) {
            status_->setText(QStringLiteral("Enter a secret of at most 4096 bytes."));
            return false;
        }
        if (!redirect.isValid() || redirect.scheme() != QStringLiteral("https")
            || redirect.host().isEmpty()
            || redirect.toString().toUtf8().size() > 4096) {
            status_->setText(QStringLiteral(
                "Enter the exact registered HTTPS redirect URL."));
            return false;
        }
        return true;
    }

    void invoke(QByteArray frame,
                std::function<void(const HelperProcessResult&)> completed) {
        set_actions_enabled(false);
        const auto started = helper_.start(
            helper_path_, {}, QCoreApplication::applicationDirPath(), 10000,
            std::move(frame),
            [this, completed = std::move(completed)](
                HelperProcessResult result) mutable {
                const bool allowed = may(role_, Capability::ChangeFeedSource);
                set_actions_enabled(allowed && !helper_path_.isEmpty());
                if (!result.ran_to_completion()) {
                    status_->setText(QStringLiteral("Credential helper failed: %1")
                                         .arg(result.detail));
                    return;
                }
                completed(result);
            });
        if (!started) {
            set_actions_enabled(true);
            status_->setText(QStringLiteral("A credential operation is already running."));
        }
    }

    void query_status() {
        invoke(credential_control_frame(broker_, "STATUS"),
               [this](const HelperProcessResult& result) {
                   status_->setText(result.output.contains(QStringLiteral("OK complete"))
                       ? QStringLiteral("All required values are saved in the OS vault.")
                       : QStringLiteral("Credential setup is incomplete."));
               });
    }

    void save(bool connect_after) {
        if (!validate()) return;
        QByteArray frame = credential_frame(
            broker_, first_->text().trimmed().toUtf8(), secret_->text().toUtf8(),
            redirect_->text().trimmed().toUtf8());
        // Do not retain a secret in a visible widget after dispatch.
        secret_->clear();
        invoke(std::move(frame), [this, connect_after](const HelperProcessResult& result) {
            if (result.exit_code != 0
                || !result.output.contains(QStringLiteral("OK saved"))) {
                status_->setText(QStringLiteral("The OS vault refused the save (%1).")
                                     .arg(result.output.trimmed()));
                return;
            }
            status_->setText(QStringLiteral("Credentials saved in the OS vault."));
            if (on_changed_) on_changed_();
            if (connect_after && on_connect_) on_connect_();
        });
    }

    void erase() {
        invoke(credential_control_frame(broker_, "DELETE"),
               [this](const HelperProcessResult& result) {
                   if (result.exit_code == 0
                       && result.output.contains(QStringLiteral("OK deleted"))) {
                       first_->clear();
                       secret_->clear();
                       redirect_->clear();
                       status_->setText(QStringLiteral("Saved credentials deleted."));
                       if (on_changed_) on_changed_();
                   } else {
                       status_->setText(QStringLiteral("Credential deletion failed (%1).")
                                            .arg(result.output.trimmed()));
                   }
               });
    }

    CredentialBroker broker_;
    Role role_;
    std::function<void()> on_connect_;
    std::function<void()> on_changed_;
    QString helper_path_;
    QLineEdit* first_ = nullptr;
    QLineEdit* secret_ = nullptr;
    QLineEdit* redirect_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* connect_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLabel* status_ = nullptr;
    HelperProcess helper_;
};

} // namespace altair::ui
