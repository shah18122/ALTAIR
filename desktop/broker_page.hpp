// desktop/broker_page.hpp -- one broker control center for the workspace.
//
// The single, read-only broker view: is FYERS linked, and where does account
// data come from?  It never loads a token into the desktop process.
//
// FYERS is the only broker: its card, login and account, and the live feed
// (altair_price_service --live). The daily login runs on its own before the
// open (broker/tools/fyers_auto_login.py, ops/fyers-auto-login.md).
//
// The layout takes the useful shape from the Greeksoft reference terminal:
// a broker/account strip first, then positions/holdings/orders and linking
// actions behind tabs.  The reference binaries and databases remain external
// reference material; no Greeksoft code or credential is copied here.

#pragma once

#include "account_widgets.hpp"
#include "broker_activity.hpp"
#include "broker_status.hpp"
#include "combined_account.hpp"
#include "fyers_link.hpp"
#include "fyers_panel.hpp"

#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace altair::ui {

/// The account route is deliberately metadata-only.  A linked session is not
/// the same thing as a successful API call, so the page says "session saved"
/// rather than claiming that a broker is connected.
class BrokerPage final : public QWidget {
public:
    BrokerPage(Role role, std::function<void()> on_changed,
               QWidget* parent = nullptr)
        : QWidget(parent), role_(role), on_changed_(std::move(on_changed)) {
        setObjectName(QStringLiteral("brokerControlCenter"));
        setStyleSheet(QStringLiteral(R"QSS(
            QWidget#brokerControlCenter { background:#0D1117; color:#E6EDF3; }
            QLabel#brokerEyebrow { color:#E3A34A; font-size:10px; font-weight:700; letter-spacing:2px; }
            QLabel#brokerTitle { color:#E6EDF3; font-size:25px; font-weight:700; }
            QLabel#brokerSubtitle { color:#8B949E; font-size:12px; }
            QLabel#brokerGuard { color:#8B949E; background:#161B22; border:1px solid #30363D;
                border-radius:4px; padding:6px 9px; font-size:10px; font-weight:700; }
            QFrame#routeRail { background:#161B22; border:1px solid #30363D; border-radius:8px; }
            QFrame#brokerPrimaryCard { background:#161B22; border:1px solid #B88845; border-radius:10px; }
            QFrame#brokerSecondaryCard { background:#161B22; border:1px solid #30363D; border-radius:10px; }
            QLabel#brokerMark { color:#0D1117; background:#E3A34A; border-radius:5px;
                font-size:16px; font-weight:800; padding:7px; }
            QLabel#brokerCardTitle { color:#E6EDF3; font-size:16px; font-weight:700; }
            QLabel#brokerCardRole { color:#E3A34A; font-size:9px; font-weight:700; letter-spacing:1px; }
            QLabel#brokerCardDetail { color:#8B949E; font-size:11px; }
            QLabel#brokerCardScope { color:#6E7681; font-size:10px; }
            QPushButton#brokerPrimaryAction { background:#E3A34A; color:#0D1117; border:0;
                border-radius:5px; padding:7px 12px; font-weight:700; }
            QPushButton#brokerPrimaryAction:hover { background:#F0B765; }
            QPushButton#brokerSecondaryAction { background:#21262D; color:#E6EDF3;
                border:1px solid #30363D; border-radius:5px; padding:7px 12px; font-weight:600; }
            QPushButton#brokerSecondaryAction:hover { background:#30363D; border-color:#8B949E; }
            QLabel#brokerMetricLabel { color:#6E7681; font-size:9px; font-weight:700; letter-spacing:1px; }
            QLabel#brokerMetricValue { color:#E6EDF3; font-size:12px; font-weight:600; }
            QLabel#brokerNotice { background:#11161D; color:#8B949E; padding:10px;
                border-left:3px solid #E3A34A; border-radius:3px; }
            QTabWidget::pane { border:1px solid #30363D; background:#0D1117; top:-1px; }
            QTabBar::tab { background:#0D1117; color:#8B949E; border:0; border-bottom:2px solid transparent;
                padding:9px 14px; margin-right:2px; font-weight:600; }
            QTabBar::tab:hover { color:#E6EDF3; background:#161B22; }
            QTabBar::tab:selected { color:#F0B765; border-bottom:2px solid #E3A34A; background:#161B22; }
        )QSS"));

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 18, 22, 20);
        root->setSpacing(14);

        auto* header = new QHBoxLayout;
        auto* title_stack = new QVBoxLayout;
        title_stack->setSpacing(2);
        auto* eyebrow = new QLabel(QStringLiteral("ACCOUNTS  ·  LOGIN  ·  LIVE FEED"), this);
        eyebrow->setObjectName(QStringLiteral("brokerEyebrow"));
        auto* title = new QLabel(QStringLiteral("Brokers"), this);
        title->setObjectName(QStringLiteral("brokerTitle"));
        auto* subtitle = new QLabel(QStringLiteral(
            "FYERS: log in from its card (or let the 08:30 auto-login do it), read the account, "
            "and the live feed runs on the session."), this);
        subtitle->setObjectName(QStringLiteral("brokerSubtitle"));
        subtitle->setWordWrap(true);
        title_stack->addWidget(eyebrow);
        title_stack->addWidget(title);
        title_stack->addWidget(subtitle);
        header->addLayout(title_stack, 1);
        auto* guard = new QLabel(QStringLiteral("READ ONLY  ·  LIVE ORDERS OFF"), this);
        guard->setObjectName(QStringLiteral("brokerGuard"));
        guard->setAlignment(Qt::AlignCenter);
        header->addWidget(guard, 0, Qt::AlignTop);
        root->addLayout(header);

        auto* route = new QFrame(this);
        route->setObjectName(QStringLiteral("routeRail"));
        auto* route_layout = new QHBoxLayout(route);
        route_layout->setContentsMargins(14, 10, 14, 10);
        auto* route_caption = new QLabel(QStringLiteral("LIVE FEED"), route);
        route_caption->setObjectName(QStringLiteral("brokerMetricLabel"));
        route_layout->addWidget(route_caption);
        route_label_ = new QLabel(route);
        route_label_->setStyleSheet(QStringLiteral(
            "color:#F0B765;font-size:12px;font-weight:700;padding-left:8px;"));
        route_layout->addWidget(route_label_);
        route_hint_ = new QLabel(route);
        route_hint_->setWordWrap(true);
        route_hint_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        route_hint_->setStyleSheet(QStringLiteral("color:#8B949E;font-size:10px;"));
        route_layout->addWidget(route_hint_, 1);
        root->addWidget(route);

        auto* overview = new QWidget(this);
        auto* ov = new QVBoxLayout(overview);
        ov->setContentsMargins(0, 0, 0, 0);
        ov->setSpacing(8);

        auto* cards = new QGridLayout;
        cards->setContentsMargins(0, 0, 0, 0);
        cards->setHorizontalSpacing(12);
        cards->setVerticalSpacing(12);
        cards->addWidget(make_fyers_card(), 0, 0);
        ov->addLayout(cards);
        auto* metrics = new QFrame(overview);
        metrics->setObjectName(QStringLiteral("routeRail"));
        auto* metric_row = new QHBoxLayout(metrics);
        metric_row->setContentsMargins(14, 9, 14, 9);
        metric_row->setSpacing(26);
        metric_row->addLayout(metric(QStringLiteral("LIVE FEED"), QStringLiteral("FYERS ONLY")));
        metric_row->addLayout(metric(QStringLiteral("ACCOUNT"), QStringLiteral("FYERS · READ ONLY")));
        metric_row->addLayout(metric(QStringLiteral("ORDERS"), QStringLiteral("PAPER · FYERS WHEN LIVE")));
        metric_row->addStretch(1);
        ov->addWidget(metrics);

        combined_ = new CombinedAccountView(fyers_account_path(), overview);
        ov->addWidget(combined_, 1);

        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("brokerWorkspaceTabs"));
        tabs_->setDocumentMode(true);
        tabs_->addTab(overview, QStringLiteral("Overview"));
        fyers_link_ = new FyersLinkPanel(
            role_,
            [this]() {
                refresh();
                if (on_changed_) on_changed_();
                if (fyers_account_ != nullptr) fyers_account_->refresh_from_broker();
            },
            tabs_);
        tabs_->addTab(fyers_link_, QStringLiteral("Log in · FYERS"));
        fyers_account_ = new FyersAccountPanel(
            fyers_account_path(), [this] {
                refresh();
                if (on_changed_) on_changed_();
            }, tabs_);
        tabs_->addTab(fyers_account_, QStringLiteral("FYERS account"));
        tabs_->addTab(new BrokerActivityPanel(activity_projection_path(), tabs_),
                      QStringLiteral("Activity && routes"));
        // A login outcome is repeated on its card, so the Overview says what
        // happened even before the tab is looked at.
        fyers_link_->outcome()->on_change = [this](LoginOutcomeKind k, const QString& html) {
            card_note(fyers_note_, k, html);
        };
        root->addWidget(tabs_, 1);

        refresh();
    }

    /// Re-read only the broker/session metadata and repaint the overview.
    void refresh() {
        const QDateTime service_now = QDateTime::currentDateTimeUtc();
        const auto fyers_service = probe_service_snapshot(
            fyers_account_path(), broker_view::BrokerId::Fyers, service_now);
        fyers_state_->setText(fyers_service ? service_broker_label(*fyers_service)
                                            : fyers_label(fyers.link));
        fyers_state_->setStyleSheet(status_css(fyers_service
            ? broker_colour(fyers_service->authentication) : fyers_colour(fyers.link)));
        fyers_detail_->setText(fyers_service
            ? QStringLiteral("Fresh verified helper evidence; it expires automatically.")
            : fyers.detail);
        fyers_detail_->setToolTip(fyers_detail_->text());

        if (combined_ != nullptr) combined_->refresh();
        const bool fyers_snapshot = QFileInfo::exists(fyers_account_path());
        const bool fyers_ready = fyers_service.has_value() || fyers.link == FyersLink::SessionSaved;
        route_label_->setText(fyers_ready ? QStringLiteral("FYERS")
                                          : QStringLiteral("NONE  ·  log in to FYERS"));
        if (fyers_snapshot) {
            route_hint_->setText(QStringLiteral(
                "A read-only FYERS snapshot is available; its age is shown in the FYERS account tab."));
        } else {
            route_hint_->setText(QStringLiteral(
                "Log in to FYERS and fetch its read-only snapshot; live routing still requires service evidence."));
        }
    }

private:
    static void card_note(QLabel* note, LoginOutcomeKind kind, const QString& html) {
        if (note == nullptr) return;
        note->setStyleSheet(kind == LoginOutcomeKind::Done ? QStringLiteral("color:#7EE787;font-size:11px;")
                            : kind == LoginOutcomeKind::Problem ? QStringLiteral("color:#FF7B72;font-size:11px;")
                                                                : QStringLiteral("color:#E3B341;font-size:11px;"));
        note->setText(html);
        note->show();
    }

    static QString status_css(const QColor& colour) {
        return QStringLiteral("color:#FFFFFF;background:%1;font-weight:700;"
                              "padding:3px 6px;border-radius:3px;")
            .arg(colour.name());
    }

    static QString fyers_account_path() {
#ifdef ALTAIR_FYERS_ACCOUNT_FILE
        return QStringLiteral(ALTAIR_FYERS_ACCOUNT_FILE);
#else
        return QStringLiteral("data/fyers_account.json");
#endif
    }

    static QString activity_projection_path() {
#ifdef ALTAIR_BROKER_ACTIVITY_FILE
        return QStringLiteral(ALTAIR_BROKER_ACTIVITY_FILE);
#else
        return QStringLiteral("data/broker_activity.json");
#endif
    }

    QVBoxLayout* metric(const QString& label, const QString& value) {
        auto* layout = new QVBoxLayout;
        layout->setSpacing(1);
        auto* top = new QLabel(label, this);
        top->setObjectName(QStringLiteral("brokerMetricLabel"));
        auto* bottom = new QLabel(value, this);
        bottom->setObjectName(QStringLiteral("brokerMetricValue"));
        layout->addWidget(top);
        layout->addWidget(bottom);
        return layout;
    }

    QFrame* make_fyers_card() {
        auto* box = new QFrame(this);
        box->setObjectName(QStringLiteral("brokerPrimaryCard"));
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(16, 15, 16, 15);
        v->setSpacing(7);
        auto* header = new QHBoxLayout;
        auto* mark = new QLabel(QStringLiteral("FY"), box);
        mark->setObjectName(QStringLiteral("brokerMark"));
        mark->setFixedSize(42, 42);
        mark->setAlignment(Qt::AlignCenter);
        header->addWidget(mark);
        auto* names = new QVBoxLayout;
        names->setSpacing(0);
        auto* name = new QLabel(QStringLiteral("FYERS"), box);
        name->setObjectName(QStringLiteral("brokerCardTitle"));
        auto* role = new QLabel(QStringLiteral("MARKET DATA (FIRST CHOICE) · ACCOUNT"), box);
        role->setObjectName(QStringLiteral("brokerCardRole"));
        names->addWidget(name); names->addWidget(role);
        header->addLayout(names, 1);
        v->addLayout(header);
        fyers_state_ = new QLabel(box);
        fyers_state_->setObjectName(QStringLiteral("fyersConnectionState"));
        fyers_state_->setWordWrap(true);
        v->addWidget(fyers_state_);
        fyers_detail_ = new QLabel(box);
        fyers_detail_->setWordWrap(true);
        fyers_detail_->setObjectName(QStringLiteral("brokerCardDetail"));
        v->addWidget(fyers_detail_);
        auto* scope = new QLabel(
            QStringLiteral("OAuth link · funds · positions · holdings · orders"), box);
        scope->setObjectName(QStringLiteral("brokerCardScope"));
        v->addWidget(scope);
        auto* actions = new QHBoxLayout;
        auto* login = new QPushButton(QStringLiteral("Log in to FYERS"), box);
        login->setObjectName(QStringLiteral("brokerPrimaryAction"));
        auto* account = new QPushButton(QStringLiteral("Account"), box);
        account->setObjectName(QStringLiteral("brokerSecondaryAction"));
        connect(login, &QPushButton::clicked, this, [this] {
            tabs_->setCurrentWidget(fyers_link_);
            fyers_link_->open_login();
        });
        connect(account, &QPushButton::clicked, this, [this] { tabs_->setCurrentWidget(fyers_account_); });
        actions->addWidget(login); actions->addWidget(account); actions->addStretch(1);
        v->addLayout(actions);
        fyers_note_ = new QLabel(box);
        fyers_note_->setObjectName(QStringLiteral("brokerLoginNote"));
        fyers_note_->setWordWrap(true);
        fyers_note_->hide();
        v->addWidget(fyers_note_);
        return box;
    }

    Role role_;
    std::function<void()> on_changed_;
    QTabWidget* tabs_ = nullptr;
    QLabel* route_label_ = nullptr;
    QLabel* route_hint_ = nullptr;
    QLabel* fyers_state_ = nullptr;
    QLabel* fyers_detail_ = nullptr;
    FyersAccountPanel* fyers_account_ = nullptr;
    FyersLinkPanel* fyers_link_ = nullptr;
    CombinedAccountView* combined_ = nullptr;
    QLabel* fyers_note_ = nullptr;
};

} // namespace altair::ui
