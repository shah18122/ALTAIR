// desktop/broker_page.hpp -- one broker control center for the workspace.
//
// The old shell exposed Kite account/link and FYERS link as separate routes.
// That made the operator hunt through the tree to answer the first question
// at the terminal: which account is linked, and where will account data come
// from?  This page is the single, read-only broker view.  It keeps the two
// providers separate, gives FYERS primary priority, and never loads a token
// into the desktop process.
//
// FYERS is the broker: its card, login and account come first, and the live
// feed is FYERS only (altair_price_service --live). Kite is LEGACY: its card
// and tabs sit in a collapsed "Legacy: Kite" corner, kept so it can be revived,
// and it is never used as a fall-back.
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
#include "kite_link.hpp"
#include "kite_panel.hpp"

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
#include <QToolButton>
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
            QWidget#brokerControlCenter { background:#0D1418; color:#E7EDF0; }
            QLabel#brokerEyebrow { color:#D8A657; font-size:10px; font-weight:700; letter-spacing:2px; }
            QLabel#brokerTitle { color:#F4F7F8; font-size:25px; font-weight:700; }
            QLabel#brokerSubtitle { color:#91A2AC; font-size:12px; }
            QLabel#brokerGuard { color:#9FB0B9; background:#141E23; border:1px solid #283940;
                border-radius:4px; padding:6px 9px; font-size:10px; font-weight:700; }
            QFrame#routeRail { background:#121C21; border:1px solid #293A42; border-radius:8px; }
            QFrame#brokerPrimaryCard { background:#162128; border:1px solid #B88845; border-radius:10px; }
            QFrame#brokerSecondaryCard { background:#131D22; border:1px solid #2B3C44; border-radius:10px; }
            QLabel#brokerMark { color:#0D1418; background:#D8A657; border-radius:5px;
                font-size:16px; font-weight:800; padding:7px; }
            QLabel#brokerCardTitle { color:#F3F6F7; font-size:16px; font-weight:700; }
            QLabel#brokerCardRole { color:#D8A657; font-size:9px; font-weight:700; letter-spacing:1px; }
            QLabel#brokerCardDetail { color:#9AABB4; font-size:11px; }
            QLabel#brokerCardScope { color:#71838D; font-size:10px; }
            QPushButton#brokerPrimaryAction { background:#D8A657; color:#10171B; border:0;
                border-radius:5px; padding:7px 12px; font-weight:700; }
            QPushButton#brokerPrimaryAction:hover { background:#E4B66D; }
            QPushButton#brokerSecondaryAction { background:#1C2930; color:#DCE5E9;
                border:1px solid #344951; border-radius:5px; padding:7px 12px; font-weight:600; }
            QPushButton#brokerSecondaryAction:hover { background:#26373F; border-color:#55717C; }
            QLabel#brokerMetricLabel { color:#71838D; font-size:9px; font-weight:700; letter-spacing:1px; }
            QLabel#brokerMetricValue { color:#DCE5E9; font-size:12px; font-weight:600; }
            QLabel#brokerNotice { background:#10191E; color:#A8B7BE; padding:10px;
                border-left:3px solid #D8A657; border-radius:3px; }
            QTabWidget::pane { border:1px solid #26373F; background:#10181D; top:-1px; }
            QTabBar::tab { background:#111A1F; color:#84969F; border:0; border-bottom:2px solid transparent;
                padding:9px 14px; margin-right:2px; font-weight:600; }
            QTabBar::tab:hover { color:#D7E0E4; background:#172228; }
            QTabBar::tab:selected { color:#F0C078; border-bottom:2px solid #D8A657; background:#172228; }
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
            "FYERS and Zerodha together: log in to either from its card, see both accounts as one book, "
            "and the live feed uses whichever is logged in."), this);
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
            "color:#F0C078;font-size:12px;font-weight:700;padding-left:8px;"));
        route_layout->addWidget(route_label_);
        route_hint_ = new QLabel(route);
        route_hint_->setWordWrap(true);
        route_hint_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        route_hint_->setStyleSheet(QStringLiteral("color:#83959E;font-size:10px;"));
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
        // Kite, stashed in a corner: collapsed until asked for.
        auto* legacy_row = new QHBoxLayout;
        legacy_row->addStretch(1);
        legacy_ = new QToolButton(overview);
        legacy_->setObjectName(QStringLiteral("legacyKiteToggle"));
        legacy_->setCheckable(true);
        legacy_->setText(QStringLiteral("\u25B8 Legacy: Kite"));
        legacy_->setToolTip(QStringLiteral("Zerodha Kite is legacy: not used for the feed or orders. Open to log in or "
                                           "read its account."));
        legacy_row->addWidget(legacy_);
        ov->addLayout(legacy_row);
        kite_card_ = make_kite_card();
        kite_card_->setVisible(false);
        ov->addWidget(kite_card_);

        auto* metrics = new QFrame(overview);
        metrics->setObjectName(QStringLiteral("routeRail"));
        auto* metric_row = new QHBoxLayout(metrics);
        metric_row->setContentsMargins(14, 9, 14, 9);
        metric_row->setSpacing(26);
        metric_row->addLayout(metric(QStringLiteral("LIVE FEED"), QStringLiteral("FYERS ONLY")));
        metric_row->addLayout(metric(QStringLiteral("ACCOUNT"), QStringLiteral("FYERS · READ ONLY")));
        metric_row->addLayout(metric(QStringLiteral("ORDERS"), QStringLiteral("PAPER · FYERS WHEN LIVE")));
        metric_row->addLayout(metric(QStringLiteral("KITE"), QStringLiteral("LEGACY · NO FAILOVER")));
        metric_row->addStretch(1);
        ov->addWidget(metrics);

        combined_ = new CombinedAccountView(fyers_account_path(), kite_account_path(), overview);
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
        kite_link_ = new KiteLinkPanel(
            role_,
            [this]() {
                refresh();
                if (on_changed_) on_changed_();
                return QString{};
            },
            tabs_);
        tabs_->addTab(kite_link_, QStringLiteral("Legacy · Kite login"));
        fyers_account_ = new FyersAccountPanel(
            fyers_account_path(), [this] {
                refresh();
                if (on_changed_) on_changed_();
            }, tabs_);
        tabs_->addTab(fyers_account_, QStringLiteral("FYERS account"));
        kite_account_ = new KitePanel(kite_account_path(), kite_session_path(), tabs_);
        tabs_->addTab(kite_account_, QStringLiteral("Legacy · Zerodha account"));
        // The legacy tabs show only while the corner is open.
        const auto show_legacy = [this](bool on) {
            legacy_->setText(on ? QStringLiteral("\u25BE Legacy: Kite") : QStringLiteral("\u25B8 Legacy: Kite"));
            kite_card_->setVisible(on);
            tabs_->setTabVisible(tabs_->indexOf(kite_link_), on);
            tabs_->setTabVisible(tabs_->indexOf(kite_account_), on);
        };
        connect(legacy_, &QToolButton::toggled, this, show_legacy);
        show_legacy(false);
        tabs_->addTab(new BrokerActivityPanel(activity_projection_path(), tabs_),
                      QStringLiteral("Activity && routes"));
        // A login outcome is repeated on its card, so the Overview says what
        // happened even before the tab is looked at.
        fyers_link_->outcome()->on_change = [this](LoginOutcomeKind k, const QString& html) {
            card_note(fyers_note_, k, html);
        };
        kite_link_->outcome()->on_change = [this](LoginOutcomeKind k, const QString& html) {
            card_note(kite_note_, k, html);
        };
        root->addWidget(tabs_, 1);

        refresh();
    }

    /// Re-read only the broker/session metadata and repaint the overview.
    void refresh() {
        const QDateTime service_now = QDateTime::currentDateTimeUtc();
        const auto fyers_service = probe_service_snapshot(
            fyers_account_path(), broker_view::BrokerId::Fyers, service_now);
        const auto kite_service = probe_service_snapshot(
            kite_account_path(), broker_view::BrokerId::ZerodhaKite, service_now);
        const FyersState fyers = probe_fyers();
        // The API key lives in the OS vault, which this process never reads;
        // the session file is what says whether Kite is logged in.
        const BrokerState kite = probe_broker(kite_session_path(), true);

        fyers_state_->setText(fyers_service ? service_broker_label(*fyers_service)
                                            : fyers_label(fyers.link));
        fyers_state_->setStyleSheet(status_css(fyers_service
            ? broker_colour(fyers_service->authentication) : fyers_colour(fyers.link)));
        fyers_detail_->setText(fyers_service
            ? QStringLiteral("Fresh verified helper evidence; it expires automatically.")
            : fyers.detail);
        fyers_detail_->setToolTip(fyers_detail_->text());

        kite_state_->setText(kite_service ? service_broker_label(*kite_service)
                                          : broker_label(kite.link));
        kite_state_->setStyleSheet(status_css(broker_colour(
            kite_service ? kite_service->authentication : kite.link)));
        kite_detail_->setText(kite_service
            ? QStringLiteral("Fresh verified helper evidence; it expires automatically.")
            : kite_detail(kite));
        kite_detail_->setToolTip(kite_detail_->text());

        if (combined_ != nullptr) combined_->refresh();
        const bool fyers_snapshot = QFileInfo::exists(fyers_account_path());
        const bool kite_snapshot = QFileInfo::exists(kite_account_path());
        const bool fyers_ready = fyers_service.has_value() || fyers.link == FyersLink::SessionSaved;
        const bool kite_ready = kite_service.has_value()
            || kite.link == BrokerLink::Unverified || kite.link == BrokerLink::Authenticated;
        (void)kite_ready;   // legacy: shown on its own card, never a route
        route_label_->setText(fyers_ready ? QStringLiteral("FYERS")
                                          : QStringLiteral("NONE  ·  log in to FYERS (Kite is legacy, not a fall-back)"));
        if (fyers_snapshot && kite_snapshot) {
            route_hint_->setText(QStringLiteral(
                "Read-only FYERS and Zerodha snapshots are available; each tab shows its own age."));
        } else if (fyers_snapshot) {
            route_hint_->setText(QStringLiteral(
                "A read-only FYERS snapshot is available; its age is shown in the FYERS account tab."));
        } else if (kite_snapshot) {
            route_hint_->setText(QStringLiteral(
                "A read-only Zerodha snapshot is available; its age is shown in the Zerodha account tab."));
        } else {
            route_hint_->setText(QStringLiteral(
                "Link a broker and fetch its read-only snapshot; live routing still requires service evidence."));
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

    static QString kite_detail(const BrokerState& state) {
        QString detail = state.detail;
        if (!state.user_id.isEmpty()) {
            detail += QStringLiteral("  User: %1").arg(state.user_id);
        }
        if (!state.broker.isEmpty()) {
            detail += QStringLiteral("  Broker: %1").arg(state.broker);
        }
        return detail;
    }

    static QString kite_session_path() {
#ifdef ALTAIR_SESSION_FILE
        return QStringLiteral(ALTAIR_SESSION_FILE);
#else
        return QStringLiteral("data/kite_session.json");
#endif
    }

    static QString kite_account_path() {
#ifdef ALTAIR_ACCOUNT_FILE
        return QStringLiteral(ALTAIR_ACCOUNT_FILE);
#else
        return QStringLiteral("data/kite_account.json");
#endif
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

    QFrame* make_kite_card() {
        auto* box = new QFrame(this);
        box->setObjectName(QStringLiteral("brokerSecondaryCard"));
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(16, 15, 16, 15);
        v->setSpacing(7);
        auto* header = new QHBoxLayout;
        auto* mark = new QLabel(QStringLiteral("ZK"), box);
        mark->setObjectName(QStringLiteral("brokerMark"));
        mark->setFixedSize(42, 42);
        mark->setAlignment(Qt::AlignCenter);
        header->addWidget(mark);
        auto* names = new QVBoxLayout;
        names->setSpacing(0);
        auto* name = new QLabel(QStringLiteral("Zerodha Kite"), box);
        name->setObjectName(QStringLiteral("brokerCardTitle"));
        auto* role = new QLabel(QStringLiteral("LEGACY · ACCOUNT ONLY"), box);
        role->setObjectName(QStringLiteral("brokerCardRole"));
        names->addWidget(name); names->addWidget(role);
        header->addLayout(names, 1);
        v->addLayout(header);
        kite_state_ = new QLabel(box);
        kite_state_->setObjectName(QStringLiteral("kiteConnectionState"));
        kite_state_->setWordWrap(true);
        v->addWidget(kite_state_);
        kite_detail_ = new QLabel(box);
        kite_detail_->setWordWrap(true);
        kite_detail_->setObjectName(QStringLiteral("brokerCardDetail"));
        v->addWidget(kite_detail_);
        auto* scope = new QLabel(
            QStringLiteral("Funds · positions · holdings · orders · account snapshot"), box);
        scope->setObjectName(QStringLiteral("brokerCardScope"));
        v->addWidget(scope);
        auto* actions = new QHBoxLayout;
        auto* login = new QPushButton(QStringLiteral("Log in to Kite"), box);
        login->setObjectName(QStringLiteral("brokerPrimaryAction"));
        auto* account = new QPushButton(QStringLiteral("Account"), box);
        account->setObjectName(QStringLiteral("brokerSecondaryAction"));
        connect(login, &QPushButton::clicked, this, [this] {
            tabs_->setCurrentWidget(kite_link_);
            kite_link_->open_login();
        });
        connect(account, &QPushButton::clicked, this, [this] { tabs_->setCurrentWidget(kite_account_); });
        actions->addWidget(login); actions->addWidget(account); actions->addStretch(1);
        v->addLayout(actions);
        kite_note_ = new QLabel(box);
        kite_note_->setObjectName(QStringLiteral("brokerLoginNote"));
        kite_note_->setWordWrap(true);
        kite_note_->hide();
        v->addWidget(kite_note_);
        return box;
    }

    Role role_;
    std::function<void()> on_changed_;
    QTabWidget* tabs_ = nullptr;
    QLabel* route_label_ = nullptr;
    QLabel* route_hint_ = nullptr;
    QLabel* fyers_state_ = nullptr;
    QLabel* fyers_detail_ = nullptr;
    QLabel* kite_state_ = nullptr;
    QLabel* kite_detail_ = nullptr;
    FyersAccountPanel* fyers_account_ = nullptr;
    KitePanel* kite_account_ = nullptr;
    FyersLinkPanel* fyers_link_ = nullptr;
    KiteLinkPanel* kite_link_ = nullptr;
    CombinedAccountView* combined_ = nullptr;
    QToolButton* legacy_ = nullptr;
    QFrame* kite_card_ = nullptr;
    QLabel* fyers_note_ = nullptr;
    QLabel* kite_note_ = nullptr;
};

} // namespace altair::ui
