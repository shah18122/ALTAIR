// desktop/start_page.hpp -- Start here: ALTAIR in plain words.
//
// Three cards, no jargon:
//   * Right now: is the feed running, is anything real switched on, what the
//     models are doing -- read every few seconds from the files the feed,
//     the engine and the LIVE switch write (data/live/feed_status.json,
//     data/live/engine_state.json, data/live_trading.json, data/kill_request.json).
//   * The Terminal: the five things on the screen and the keys that matter.
//   * What the tests found: every strategy and model family in one line --
//     what it does, what it made after costs, and whether to trust it. Each
//     verdict names its source document and the date it was measured; the
//     numbers are the documents', not recomputed here.
//
// Reads files; places nothing.

#pragma once

#include "kill_switch.hpp"
#include "live_trading.hpp"
#include "simple_mode.hpp"
#include "theme.hpp"
#include "verdicts.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <vector>

namespace altair::ui {

namespace start_detail {

[[nodiscard]] inline QJsonObject read_json(const QString& path, qint64* age_s = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (age_s != nullptr) *age_s = QDateTime::currentSecsSinceEpoch() - QFileInfo(f).lastModified().toSecsSinceEpoch();
    return QJsonDocument::fromJson(f.readAll()).object();
}

[[nodiscard]] inline QString dot(const char* colour, const QString& text) {
    return QStringLiteral("<span style='color:%1'>●</span> %2").arg(QString::fromLatin1(colour), text);
}

} // namespace start_detail

/// "Right now", from the files (see the header). `now_unix`: the wall clock.
[[nodiscard]] inline QString start_now_html(const QString& root, qint64 now_unix) {
    using namespace start_detail;
    QStringList lines;
    qint64 feed_age = 0;
    const QJsonObject feed = read_json(root + QStringLiteral("/data/live/feed_status.json"), &feed_age);
    if (feed.isEmpty() || feed_age > 120) {
        lines << dot("#8A93A2", QStringLiteral("<b>Prices: not running.</b> The live feed starts by itself on weekdays from "
                                               "09:00 IST once FYERS is logged in (Brokers page)."));
    } else {
        const QString src = feed.value(QStringLiteral("source")).toString().toUpper();
        lines << dot(src == QLatin1String("SIM") ? "#F4C95D" : "#7FD17F",
                     src == QLatin1String("SIM") ? QStringLiteral("<b>Prices: SIMULATION</b> -- a replayed or made-up market, not today's")
                                                 : QStringLiteral("<b>Prices: live from %1</b> (%2)").arg(src.toHtmlEscaped(),
                                                       feed.value(QStringLiteral("state")).toString().toHtmlEscaped()));
    }
    const LiveTradingArm arm = read_live_arm(root + QStringLiteral("/data/live_trading.json"), now_unix);
    // Fail-safe, as everywhere: a halt file that cannot be read counts as a halt.
    const bool halted = read_kill_request(root + QStringLiteral("/data/kill_request.json")).halted();
    if (halted)
        lines << dot("#F07A6A", QStringLiteral("<b>HALTED</b> (F11+F12). Nothing is sent; press F11+F12 again to resume, with a reason."));
    else if (arm.armed)
        lines << dot("#F07A6A", QStringLiteral("<b>REAL ORDERS ARE ON</b> (LIVE armed by %1). Orders you place go to FYERS, within "
                                               "the limits you set. F11+F12 stops everything.").arg(arm.by.toHtmlEscaped()));
    else
        lines << dot("#7FD17F", QStringLiteral("<b>Paper only.</b> Every order is a demo order; nothing reaches FYERS until you "
                                               "switch LIVE on."));
    qint64 eng_age = 0;
    const QJsonObject eng = read_json(root + QStringLiteral("/data/live/engine_state.json"), &eng_age);
    if (eng.isEmpty() || eng_age > 30) {
        lines << dot("#8A93A2", QStringLiteral("<b>Models: not running.</b> They start by themselves once prices stream, and "
                                               "trade on paper only."));
    } else {
        int in_pos = 0, abstain = 0;
        const QJsonArray ms = eng.value(QStringLiteral("models")).toArray();
        for (const auto& v : ms) {
            const QString s = v.toObject().value(QStringLiteral("state")).toString();
            if (s == QLatin1String("in position")) ++in_pos;
            if (s == QLatin1String("abstaining")) ++abstain;
        }
        const QString halt = eng.value(QStringLiteral("halt")).toString();
        lines << dot("#7FD17F", QStringLiteral("<b>Models: %1 running on paper</b> -- %2 holding a position, %3 sitting out "
                                               "(they sit out when they are not sure)%4.")
                                    .arg(ms.size()).arg(in_pos).arg(abstain)
                                    .arg(halt.isEmpty() ? QString() : QStringLiteral("; entries stopped: ") + halt.toHtmlEscaped()));
    }
    return lines.join(QStringLiteral("<br>"));
}

[[nodiscard]] inline QString start_terminal_html() {
    return QStringLiteral(
        "<b>1. The watch</b> (left): your scrips, prices moving live. Add one with <b>Insert</b> "
        "(Exchange → Segment → Symbol → Expiry → Type → Strike).<br>"
        "<b>2. The depth</b> (right): the queue of buyers and sellers -- 50 levels for the NSE scrip you select, "
        "5 for the rest. The title says which, and why.<br>"
        "<b>3. Time &amp; sales</b> (under the depth): every trade, newest first.<br>"
        "<b>4. The option chain</b>: <b>Enter</b> on any scrip. <b>VV ±</b> says which strikes are dear (+) or cheap (−) "
        "against the smile; <b>Enter</b> on a strike opens it in Greek Watch.<br>"
        "<b>5. Orders</b>: <b>F1</b> buy, <b>F2</b> sell -- paper unless LIVE is on. <b>F11+F12</b> stops everything.<br>"
        "<span style='color:#8FA3AE'>MCX and currency show prices and take paper orders only.</span>");
}

class StartPage final : public QWidget {
public:
    explicit StartPage(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("startPage"));
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral("#startPage{background:#0D1117;} #startPage QLabel{color:#E6EDF3;} "
                                     "QLabel#startCard{background:#131A22;border:1px solid #222A33;border-radius:6px;padding:14px;}"));
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget(scroll);
        body->setObjectName(QStringLiteral("startBody"));
        body->setStyleSheet(QStringLiteral("#startBody{background:#0D1117;}"));
        auto* v = new QVBoxLayout(body);
        v->setContentsMargins(28, 22, 28, 22);
        v->setSpacing(14);
        const auto card = [&](const QString& title, QLabel*& label, const QString& html) {
            auto* t = new QLabel(title, body);
            t->setObjectName(QStringLiteral("sectionKicker"));
            v->addWidget(t);
            label = new QLabel(html, body);
            label->setObjectName(QStringLiteral("startCard"));
            label->setTextFormat(Qt::RichText);
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setMaximumWidth(1100);
            v->addWidget(label);
        };
        // The one switch for the whole app (desktop/simple_mode.hpp).
        auto* sw = new QHBoxLayout;
        auto* sw_text = new QLabel(QStringLiteral(
            "<span style='color:#8FA3AE'><b>Simple view</b> shows the columns and tabs you read every minute, in plain "
            "words; <b>Advanced view</b> shows everything. The same switch sits on the Terminal's top bar and on Demo "
            "Trading.</span>"), body);
        sw_text->setTextFormat(Qt::RichText);
        sw_text->setWordWrap(true);
        sw->addWidget(simple_mode::button(body));
        sw->addWidget(sw_text, 1);
        v->addLayout(sw);
        QLabel* term = nullptr;
        QLabel* tests = nullptr;
        card(QStringLiteral("RIGHT NOW"), now_, start_now_html(root_, QDateTime::currentSecsSinceEpoch()));
        card(QStringLiteral("THE TERMINAL, IN FIVE THINGS"), term, start_terminal_html());
        card(QStringLiteral("WHAT THE TESTS FOUND"), tests,
             start_verdicts_html(start_verdicts())
                 + QStringLiteral("<br><span style='color:#8FA3AE'>Green: works. Amber: real but not proven. Red: does not "
                                  "work, or not worth it. Every number is after expenses, from the document named under "
                                  "it. Ideas for what to test next: docs/research/research-ideas.md.</span>"));
        v->addStretch();
        scroll->setWidget(body);
        outer->addWidget(scroll);
        timer_.setInterval(3000);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] {
            if (isVisible()) now_->setText(start_now_html(root_, QDateTime::currentSecsSinceEpoch()));
        });
        timer_.start();
    }
    [[nodiscard]] QLabel* now_label() const noexcept { return now_; }

private:
    QString root_;
    QLabel* now_ = nullptr;
    QTimer timer_;
};

} // namespace altair::ui
