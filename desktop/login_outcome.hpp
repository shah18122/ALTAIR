// desktop/login_outcome.hpp -- what a broker login button just did, said
// right under the button.
//
// The Kite login button reported everything into a log box at the bottom of
// the tab. When the helper refused ("app credentials are incomplete") the
// only visible effect of a click was a line nobody was looking at, so the
// button looked dead: "nothing opens". This widget puts the outcome where the
// eye already is, and when a login URL exists it shows the link and a copy
// button, so a browser that did not open is never a dead end.

#pragma once

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <utility>

namespace altair::ui {

enum class LoginOutcomeKind { Working, Done, Problem };

class LoginOutcome final : public QWidget {
public:
    explicit LoginOutcome(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("brokerLoginOutcome"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 4, 0, 4);
        v->setSpacing(4);
        text_ = new QLabel(this);
        text_->setObjectName(QStringLiteral("brokerLoginOutcomeText"));
        text_->setWordWrap(true);
        text_->setTextFormat(Qt::RichText);
        v->addWidget(text_);
        auto* row = new QHBoxLayout;
        url_ = new QLabel(this);
        url_->setObjectName(QStringLiteral("brokerLoginUrl"));
        url_->setTextFormat(Qt::RichText);
        url_->setOpenExternalLinks(true);
        url_->setTextInteractionFlags(Qt::TextBrowserInteraction);
        url_->setWordWrap(true);
        copy_ = new QPushButton(QStringLiteral("Copy login URL"), this);
        copy_->setObjectName(QStringLiteral("brokerLoginCopy"));
        row->addWidget(url_, 1);
        row->addWidget(copy_);
        v->addLayout(row);
        connect(copy_, &QPushButton::clicked, this, [this] {
            if (auto* cb = QGuiApplication::clipboard()) cb->setText(link_);
            copy_->setText(QStringLiteral("Copied"));
        });
        url_->hide();
        copy_->hide();
        hide();
    }

    void show_state(LoginOutcomeKind kind, const QString& html) {
        kind_ = kind;
        const char* css =
            kind == LoginOutcomeKind::Done
                ? "color:#D2F4DA;background:#12301C;border:1px solid #2EA043;"
            : kind == LoginOutcomeKind::Problem
                ? "color:#FFD8D3;background:#3A1714;border:1px solid #DA3633;"
                : "color:#F3E3C0;background:#2E2410;border:1px solid #B9770B;";
        text_->setStyleSheet(QString::fromLatin1(css)
                             + QStringLiteral("border-radius:5px;padding:7px 9px;"));
        text_->setText(html);
        show();
        if (on_change) on_change(kind, html);
    }

    /// The link the browser should have opened, clickable and copyable.
    void show_url(const QString& url) {
        link_ = url;
        const QString shown = url.size() > 90 ? url.left(87) + QStringLiteral("…") : url;
        url_->setText(QStringLiteral("<a href=\"%1\" style=\"color:#79C0FF\">%2</a>")
                          .arg(url.toHtmlEscaped(), shown.toHtmlEscaped()));
        url_->setToolTip(QStringLiteral("Click to open, or copy it into any browser."));
        copy_->setText(QStringLiteral("Copy login URL"));
        url_->show();
        copy_->show();
    }

    void hide_url() {
        link_.clear();
        url_->hide();
        copy_->hide();
    }

    [[nodiscard]] QString text() const { return text_->text(); }
    [[nodiscard]] QString url() const { return link_; }
    [[nodiscard]] LoginOutcomeKind kind() const noexcept { return kind_; }

    /// Mirrors every state change, so the broker card can repeat it.
    std::function<void(LoginOutcomeKind, const QString&)> on_change;

private:
    QLabel* text_ = nullptr;
    QLabel* url_ = nullptr;
    QPushButton* copy_ = nullptr;
    QString link_;
    LoginOutcomeKind kind_ = LoginOutcomeKind::Working;
};

} // namespace altair::ui
