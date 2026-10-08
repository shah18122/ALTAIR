// desktop/shell_pages.hpp -- the full-screen shell's own pieces.
//
//   AboutPage            ALTAIR, Gautam Global LLP, the licence and the
//                        third-party notices.
//   LiveDataflowWidget   the path a tick takes TODAY: FYERS sockets -> price
//                        service -> bus -> Terminal / models engine -> order
//                        router -> FYERS orders, each stage lit by whether its
//                        file or process is fresh.
//   HaltChord            F11 and F12 held together, anywhere in the app.
//   HaltResumeDialog     the second chord while halted: resume, with a reason.
//
// None of this places an order. The halt is a request file
// (data/kill_request.json) that the order router and the models engine obey.

#pragma once

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace altair::ui {

inline constexpr const char* kShellTitle = "ALTAIR : GAUTAM GLOBAL LLP : SMIT SHAH";

// ---- About -------------------------------------------------------------------

#ifndef ALTAIR_VERSION_TEXT
#define ALTAIR_VERSION_TEXT "1.0"
#endif

[[nodiscard]] inline QString about_html() {
    return QStringLiteral(
        "<div style='color:#E6EDF3'>"
        "<div style='font-size:30px;font-weight:800;letter-spacing:6px;color:#F0B765'>ALTAIR</div>"
        "<div style='font-size:13px;color:#8FA3AE;letter-spacing:2px'>TRADING WORKSTATION</div><br>"
        "<table cellspacing='0' cellpadding='5'>"
        "<tr><td style='color:#8FA3AE'>Licensed to</td><td><b>GAUTAM GLOBAL LLP</b></td></tr>"
        "<tr><td style='color:#8FA3AE'>Licensee</td><td><b>SMIT SHAH</b></td></tr>"
        "<tr><td style='color:#8FA3AE'>Licence</td><td><b style='color:#7EE787'>LIFETIME</b> &nbsp;·&nbsp; perpetual, "
        "no renewal, no expiry date</td></tr>"
        "<tr><td style='color:#8FA3AE'>Seats</td><td>Unlimited installations for the licensee's own trading</td></tr>"
        "<tr><td style='color:#8FA3AE'>Version</td><td>%1 &nbsp;·&nbsp; built %2</td></tr>"
        "<tr><td style='color:#8FA3AE'>Broker</td><td>FYERS (market data, 50-level depth and orders)</td></tr>"
        "</table><br>"
        "<b>What the licence covers.</b> Use of every part of ALTAIR — the Terminal, the models, the strategies, "
        "the order router and their updates — for as long as the licensee trades, with no subscription and no "
        "time limit. Market data and orders are FYERS's service under the licensee's own FYERS account and are not "
        "part of this licence.<br><br>"
        "<b>No warranty on outcomes.</b> ALTAIR places orders only as configured (LIVE switch, per-strategy switches "
        "and limits). Trading carries risk of loss; past or simulated results do not promise future ones.<br><br>"
        "<b>Third-party software</b> (each under its own licence):<br>"
        "&nbsp;&nbsp;Qt 6 — LGPL v3 &nbsp;·&nbsp; Boost (Asio, Beast) — Boost Software License 1.0 &nbsp;·&nbsp; "
        "OpenSSL — Apache 2.0 &nbsp;·&nbsp; toml++ — MIT<br><br>"
        "<span style='color:#8FA3AE'>© GAUTAM GLOBAL LLP. All rights reserved.</span>"
        "</div>")
        .arg(QStringLiteral(ALTAIR_VERSION_TEXT), QStringLiteral(__DATE__));
}

class AboutPage final : public QWidget {
public:
    explicit AboutPage(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("aboutPage"));
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral("#aboutPage{background:#0D1117;} #aboutPage QLabel{color:#E6EDF3;}"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(28, 24, 28, 24);
        auto* text = new QLabel(about_html(), this);
        text->setObjectName(QStringLiteral("aboutText"));
        text->setTextFormat(Qt::RichText);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setMaximumWidth(860);
        v->addWidget(text);
        v->addStretch();
    }
};

// ---- Data flow (today's path) ---------------------------------------------------

/// One stage of the live path and how to tell it is working.
struct LiveFlowStage {
    QString name;      ///< what it is
    QString detail;    ///< process, port or file
    QString evidence;  ///< file whose freshness says it is running ("" = none)
    int fresh_s = 0;   ///< how recent the file must be
};

[[nodiscard]] inline std::vector<LiveFlowStage> live_flow_stages() {
    return {
        {QStringLiteral("FYERS sockets"), QStringLiteral("HSM ticks + quotes · TBT 50-level depth"), {}, 0},
        {QStringLiteral("Price service"), QStringLiteral("altair_price_service · feed_status.json"),
         QStringLiteral("data/live/feed_status.json"), 30},
        {QStringLiteral("Loopback bus"), QStringLiteral("127.0.0.1:7421 · two producer rings"),
         QStringLiteral("data/live/universe.csv"), 0},
        {QStringLiteral("Terminal"), QStringLiteral("market watch · chain · depth · Greek Watch"), {}, 0},
        {QStringLiteral("Models engine"), QStringLiteral("altair_live_engine · engine_state.json"),
         QStringLiteral("data/live/engine_state.json"), 30},
        {QStringLiteral("Order intents"), QStringLiteral("data/order_intents.jsonl"),
         QStringLiteral("data/order_intents.jsonl"), 0},
        {QStringLiteral("Order router"), QStringLiteral("altair_order_router · live_orders/orders.json"),
         QStringLiteral("data/live_orders/orders.json"), 30},
        {QStringLiteral("FYERS orders"), QStringLiteral("api.fyers.in /api/v3/orders"), {}, 0},
    };
}

/// Whether a stage's evidence file says it is up: present, and (when a
/// freshness is set) written within that many seconds.
[[nodiscard]] inline int live_flow_state(const QString& root, const LiveFlowStage& s, qint64 now_s) {
    if (s.evidence.isEmpty()) return 1;                       // nothing to check: drawn neutral
    const QFileInfo fi(root + QLatin1Char('/') + s.evidence);
    if (!fi.exists()) return 0;
    if (s.fresh_s <= 0) return 2;
    return now_s - fi.lastModified().toSecsSinceEpoch() <= s.fresh_s ? 2 : 0;
}

class LiveDataflowWidget final : public QWidget {
public:
    explicit LiveDataflowWidget(QString root, QWidget* parent = nullptr)
        : QWidget(parent), root_(std::move(root)), stages_(live_flow_stages()) {
        setObjectName(QStringLiteral("liveDataflow"));
        setMinimumHeight(250);
        setMaximumHeight(270);
        poll_.setInterval(2000);
        QObject::connect(&poll_, &QTimer::timeout, this, [this] { update(); });
        poll_.start();
    }
    void set_root(const QString& root) { root_ = root; update(); }
    [[nodiscard]] int state_of(int i) const {
        return i >= 0 && i < static_cast<int>(stages_.size())
            ? live_flow_state(root_, stages_[static_cast<std::size_t>(i)], QDateTime::currentSecsSinceEpoch()) : -1;
    }
    [[nodiscard]] int stage_count() const noexcept { return static_cast<int>(stages_.size()); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(0x12, 0x18, 0x1E));
        const int cols = 4, margin = 16, gap = 26, box_h = 70;
        const int box_w = (width() - 2 * margin - (cols - 1) * gap) / cols;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        p.setPen(QColor(0xD6, 0xDB, 0xDF));
        p.drawText(margin, margin + 10, QStringLiteral("Live path — green: running now · amber: no file to check · red: not running"));
        for (int i = 0; i < static_cast<int>(stages_.size()); ++i) {
            const int row = i / cols;
            const int col = row % 2 == 0 ? i % cols : cols - 1 - i % cols;   // snake: the arrow never jumps back
            const int x = margin + col * (box_w + gap);
            const int y = margin + 28 + row * (box_h + 40);
            const int st = live_flow_state(root_, stages_[static_cast<std::size_t>(i)], now);
            const QColor c = st == 2 ? QColor(0x3F, 0xB9, 0x50) : st == 1 ? QColor(0xE3, 0xA3, 0x4A) : QColor(0xF8, 0x51, 0x49);
            p.setPen(QPen(c, 2));
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 36));
            p.drawRoundedRect(QRect(x, y, box_w, box_h), 8, 8);
            p.setPen(QColor(0xE6, 0xED, 0xF3));
            QFont f = font();
            f.setBold(true);
            p.setFont(f);
            p.drawText(QRect(x + 8, y + 6, box_w - 16, 22), Qt::AlignLeft | Qt::AlignVCenter, stages_[static_cast<std::size_t>(i)].name);
            f.setBold(false);
            p.setFont(f);
            p.setPen(QColor(0x9F, 0xB0, 0xB9));
            p.drawText(QRect(x + 8, y + 28, box_w - 16, box_h - 32), Qt::AlignLeft | Qt::TextWordWrap,
                       stages_[static_cast<std::size_t>(i)].detail);
            if (i + 1 < static_cast<int>(stages_.size())) {
                p.setPen(QPen(QColor(0x8F, 0xA3, 0xAE), 2));
                const int next_row = (i + 1) / cols;
                if (next_row != row) {
                    const int cx = x + box_w / 2;
                    p.drawLine(cx, y + box_h, cx, y + box_h + 40);
                } else if (row % 2 == 0) {
                    p.drawLine(x + box_w, y + box_h / 2, x + box_w + gap, y + box_h / 2);
                } else {
                    p.drawLine(x, y + box_h / 2, x - gap, y + box_h / 2);
                }
            }
        }
    }

private:
    QString root_;
    std::vector<LiveFlowStage> stages_;
    QTimer poll_;
};

/// The page: the diagram, what each file is, and the kill/arm switches.
[[nodiscard]] inline QString live_flow_notes_html() {
    return QStringLiteral(
        "<b>Market data.</b> <code>altair_price_service</code> logs in with today's FYERS session, opens the HSM "
        "socket (ticks, quotes, OHLC) and the TBT socket (50-level depth) and publishes every update on the loopback "
        "bus. The Terminal and the models engine are subscribers; neither talks to FYERS for prices.<br>"
        "<b>Models.</b> <code>altair_live_engine</code> runs every model on the bus during market hours (09:15–15:30), "
        "paper-trades them (demo) and writes <code>data/live/engine_state.json</code> every second and "
        "<code>data/live/paper/*.csv</code> (fills, round trips with gross, expenses and net).<br>"
        "<b>Orders.</b> A real order is a line in <code>data/order_intents.jsonl</code> — from the Terminal while LIVE "
        "is on, or from a strategy whose own switch is on. <code>altair_order_router</code> checks it against "
        "<code>data/live_trading.json</code> (armed, limits, expiry) and sends it to FYERS; what became of it is in "
        "<code>data/live_orders/</code>.<br>"
        "<b>Halt.</b> F11 + F12 together writes <code>data/kill_request.json</code>: the router cancels open orders and "
        "sends nothing more, the engine stops new entries, LIVE switches off. F11 + F12 again offers Resume.");
}

// ---- F11 + F12 ------------------------------------------------------------------

/// Watches the whole application for F11 and F12 held together. Fires once
/// per chord: both keys must be released before it can fire again. Losing
/// focus forgets what was held, so a key released in another window does not
/// leave a half-chord armed.
class HaltChord final : public QObject {
public:
    std::function<void()> fired;

    explicit HaltChord(QObject* parent = nullptr) : QObject(parent) {}

    /// Feed one key event (the filter does; tests call it directly).
    /// True when the event completed the chord.
    bool key(int code, bool press, bool autorepeat) {
        if (code != Qt::Key_F11 && code != Qt::Key_F12) return false;
        bool& held = code == Qt::Key_F11 ? f11_ : f12_;
        if (!press) {
            held = false;
            if (!f11_ && !f12_) latched_ = false;
            return false;
        }
        if (autorepeat && held) return false;
        held = true;
        if (f11_ && f12_ && !latched_) {
            latched_ = true;
            if (fired) fired();
            return true;
        }
        return false;
    }
    void reset() { f11_ = f12_ = latched_ = false; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            // Each key event reaches the filter once per receiver up the
            // parent chain; only the first delivery (to the focus widget or
            // the window) counts.
            auto* k = static_cast<QKeyEvent*>(event);
            if (k->key() != Qt::Key_F11 && k->key() != Qt::Key_F12) break;
            if (k->timestamp() == last_ts_ && k->type() == last_type_ && k->key() == last_key_) return false;
            last_ts_ = k->timestamp();
            last_type_ = k->type();
            last_key_ = k->key();
            key(k->key(), event->type() == QEvent::KeyPress, k->isAutoRepeat());
            return false;
        }
        case QEvent::ApplicationDeactivate:
        case QEvent::WindowDeactivate:
            reset();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    bool f11_ = false, f12_ = false, latched_ = false;
    ulong last_ts_ = 0;
    QEvent::Type last_type_ = QEvent::None;
    int last_key_ = 0;
};

/// Resume after a halt: says who halted, when and why, and needs a reason.
class HaltResumeDialog final : public QDialog {
public:
    HaltResumeDialog(const QString& halted_text, QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Trading is halted"));
        setObjectName(QStringLiteral("haltResumeDialog"));
        auto* v = new QVBoxLayout(this);
        auto* head = new QLabel(QStringLiteral(
            "<b style='color:#F85149;font-size:15px'>■ HALTED</b><br>%1<br><br>"
            "Resume lets the models open new demo positions again and lets the order router send orders again. "
            "LIVE stays <b>off</b>: switch it on yourself if you want real orders.").arg(halted_text), this);
        head->setWordWrap(true);
        head->setTextFormat(Qt::RichText);
        v->addWidget(head);
        reason_ = new QLineEdit(this);
        reason_->setObjectName(QStringLiteral("haltResumeReason"));
        reason_->setPlaceholderText(QStringLiteral("Why resume? (required, recorded beside the halt)"));
        v->addWidget(reason_);
        auto* buttons = new QDialogButtonBox(this);
        resume_ = buttons->addButton(QStringLiteral("Resume trading"), QDialogButtonBox::AcceptRole);
        resume_->setObjectName(QStringLiteral("haltResumeButton"));
        buttons->addButton(QStringLiteral("Stay halted"), QDialogButtonBox::RejectRole);
        resume_->setEnabled(false);
        QObject::connect(reason_, &QLineEdit::textChanged, this,
                         [this](const QString& t) { resume_->setEnabled(!t.trimmed().isEmpty()); });
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        v->addWidget(buttons);
        resize(520, 220);
    }
    [[nodiscard]] QString reason() const { return reason_->text().trimmed(); }
    [[nodiscard]] QLineEdit* reason_edit() const noexcept { return reason_; }

private:
    QLineEdit* reason_ = nullptr;
    QPushButton* resume_ = nullptr;
};

} // namespace altair::ui
