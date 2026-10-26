// desktop/news_ticker.hpp -- market headlines scrolling right to left.
//
// One thin bar across the top of every page. The headlines come from
// altair_news (desktop/news/news_main.cpp), run every few minutes as a child
// process; this widget opens no connection of its own. The last good set is
// cached in data/live/news_cache.json so the bar is full the moment the
// window opens.
//
// Smooth by construction: the strip of text is painted once into a pixmap
// whenever the headlines change, and each frame only blits it at an offset
// taken from a monotonic clock, so a slow frame never makes the text jump
// or crawl. The animation stops while the bar is hidden or the window is
// minimised. Hover pauses it; a click opens the headline in the browser.
#pragma once

#include "news/rss.hpp"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace altair::ui {

class NewsTicker final : public QWidget {
public:
    /// `root`: the ALTAIR folder (config/news_feeds.txt, data/live/).
    /// `helper`: path to altair_news; empty means "not built", said on the bar.
    NewsTicker(QString root, QString helper, QWidget* parent = nullptr)
        : QWidget(parent), root_(std::move(root)), helper_(std::move(helper)) {
        setObjectName(QStringLiteral("newsTicker"));
        setAccessibleName(QStringLiteral("Market news ticker"));
        setFixedHeight(28);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Market headlines from free RSS feeds (config/news_feeds.txt). "
                                  "Hover to pause, click to open, right-click for options. Display only: nothing trades on them."));
        frame_.setTimerType(Qt::PreciseTimer);
        frame_.setInterval(16);
        connect(&frame_, &QTimer::timeout, this, [this] { advance(); });
        refresh_.setInterval(5 * 60 * 1000);
        connect(&refresh_, &QTimer::timeout, this, [this] { fetch(); });
        load_cache();
        if (items_.empty())
            set_message(helper_.isEmpty() ? QStringLiteral("News: altair_news is not built (build.bat net)")
                                          : QStringLiteral("News: fetching headlines…"));
    }

    /// Start fetching (now, then every five minutes). Separate from the
    /// constructor so tests and scripted runs stay offline.
    void start() {
        refresh_.start();
        fetch();
    }

    void set_items(std::vector<news::NewsItem> items) {
        if (items.empty()) return;
        items_ = std::move(items);
        message_.clear();
        rebuild();
    }
    [[nodiscard]] const std::vector<news::NewsItem>& items() const noexcept { return items_; }
    [[nodiscard]] bool paused() const noexcept { return paused_ || hover_; }
    void set_paused(bool p) { paused_ = p; }
    /// Pixels per second. Default 60: readable without chasing.
    void set_speed(double px_per_s) { speed_ = std::clamp(px_per_s, 10.0, 400.0); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x0B, 0x0F, 0x14));
        p.setPen(QColor(0x21, 0x26, 0x2D));
        p.drawLine(0, height() - 1, width(), height() - 1);
        // The label is fixed; the strip slides under it.
        const int label_w = paint_label(p);
        p.setClipRect(label_w, 0, width() - label_w, height());
        if (strip_.isNull()) return;
        const qreal dpr = strip_.devicePixelRatio();
        const int sw = static_cast<int>(strip_.width() / dpr);
        if (sw <= 0) return;
        int x = label_w + static_cast<int>(-offset_);
        while (x < width()) {
            p.drawPixmap(x, 0, strip_);
            x += sw;
        }
    }
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        if (!qFuzzyCompare(strip_.devicePixelRatio(), devicePixelRatioF())) rebuild();
        if (!watching_window_ && window() != this) { window()->installEventFilter(this); watching_window_ = true; }
        resume_animation();
    }
    // Minimised: the bar is still "visible" to Qt, so stop the frames here.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == window() && e->type() == QEvent::WindowStateChange) {
            if (window()->isMinimized()) frame_.stop(); else resume_animation();
        }
        return QWidget::eventFilter(o, e);
    }
    void hideEvent(QHideEvent* e) override { QWidget::hideEvent(e); frame_.stop(); }
    void changeEvent(QEvent* e) override {
        QWidget::changeEvent(e);
        if (e->type() == QEvent::FontChange) rebuild();
    }
    void enterEvent(QEnterEvent* e) override { QWidget::enterEvent(e); hover_ = true; }
    void leaveEvent(QEvent* e) override { QWidget::leaveEvent(e); hover_ = false; clock_.restart(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        const auto* item = item_at(e->position().toPoint().x());
        setToolTip(item ? QStringLiteral("%1 · %2%3").arg(item->source, item->title,
                                                         item->link.isEmpty() ? QString{} : QStringLiteral("\nClick to open"))
                        : QString{});
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (const auto* item = item_at(e->position().toPoint().x()); item && !item->link.isEmpty())
            QDesktopServices::openUrl(QUrl(item->link));
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu menu(this);
        auto* pause = menu.addAction(paused_ ? QStringLiteral("Resume") : QStringLiteral("Pause"));
        auto* now = menu.addAction(QStringLiteral("Refresh headlines now"));
        now->setEnabled(!helper_.isEmpty() && proc_.isNull());
        auto* speed = menu.addMenu(QStringLiteral("Speed"));
        for (const auto& [label, v] : {std::pair{QStringLiteral("Slow"), 35.0}, std::pair{QStringLiteral("Normal"), 60.0},
                                       std::pair{QStringLiteral("Fast"), 110.0}}) {
            auto* a = speed->addAction(label);
            a->setCheckable(true);
            a->setChecked(qFuzzyCompare(speed_, v));
            connect(a, &QAction::triggered, this, [this, v] { set_speed(v); });
        }
        menu.addSeparator();
        auto* hide_bar = menu.addAction(QStringLiteral("Hide the news bar (Ctrl+Shift+N shows it)"));
        const QAction* chosen = menu.exec(e->globalPos());
        if (chosen == pause) paused_ = !paused_;
        else if (chosen == now) fetch();
        else if (chosen == hide_bar) { hide(); if (on_hidden) on_hidden(); }
    }

public:
    /// Told when the person hides the bar, so the shell can remember it.
    std::function<void()> on_hidden;

private:
    struct Span { int x0, x1; std::size_t item; };

    QString root_, helper_;
    std::vector<news::NewsItem> items_;
    QString message_;
    QPixmap strip_;
    std::vector<Span> spans_;   ///< logical x ranges of each headline within one copy of the strip
    double offset_ = 0.0;
    int label_w_ = 64;
    double speed_ = 60.0;
    bool paused_ = false;
    bool hover_ = false;
    bool watching_window_ = false;
    QTimer frame_, refresh_;
    QElapsedTimer clock_;
    QPointer<QProcess> proc_;

    [[nodiscard]] QString cache_path() const { return root_ + QStringLiteral("/data/live/news_cache.json"); }

    int paint_label(QPainter& p) {
        QFont f = font();
        f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        f.setPointSizeF(std::max(7.0, f.pointSizeF() * 0.85));
        p.setFont(f);
        const QString text = QStringLiteral("NEWS");
        const int w = QFontMetrics(f).horizontalAdvance(text) + 24;
        p.fillRect(0, 0, w, height() - 1, QColor(0x16, 0x1B, 0x22));
        p.setPen(QColor(0xE3, 0xA3, 0x4A));
        p.drawText(QRect(0, 0, w, height() - 1), Qt::AlignCenter, text);
        p.setPen(QColor(0x30, 0x36, 0x3D));
        p.drawLine(w, 4, w, height() - 5);
        label_w_ = w + 1;
        return label_w_;
    }

    void set_message(const QString& m) { message_ = m; rebuild(); }

    void rebuild() {
        spans_.clear();
        const QFont f = font();
        const QFontMetrics fm(f);
        const QString sep = QStringLiteral("     ◆     ");
        // Lay out one copy of the strip: source in amber, headline in text.
        struct Piece { QString text; QColor colour; int item; };
        std::vector<Piece> pieces;
        if (items_.empty()) {
            pieces.push_back({message_, QColor(0x8B, 0x94, 0x9E), -1});
        } else {
            for (std::size_t i = 0; i < items_.size(); ++i) {
                const auto& it = items_[i];
                QString when;
                if (it.ts > 0) when = QDateTime::fromSecsSinceEpoch(it.ts).toString(QStringLiteral("HH:mm")) + QLatin1Char(' ');
                pieces.push_back({when + it.source.toUpper() + QStringLiteral("  "), QColor(0xE3, 0xA3, 0x4A), static_cast<int>(i)});
                pieces.push_back({it.title, QColor(0xE6, 0xED, 0xF3), static_cast<int>(i)});
                pieces.push_back({sep, QColor(0x48, 0x4F, 0x58), -1});
            }
        }
        int w = 24;
        for (const auto& pc : pieces) w += fm.horizontalAdvance(pc.text);
        w = std::max(w, 64);
        const qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(static_cast<int>(w * dpr), static_cast<int>(height() * dpr)));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(f);
        int x = 24;
        const int baseline = (height() - fm.height()) / 2 + fm.ascent();
        for (const auto& pc : pieces) {
            const int adv = fm.horizontalAdvance(pc.text);
            p.setPen(pc.colour);
            p.drawText(x, baseline, pc.text);
            if (pc.item >= 0) {
                if (!spans_.empty() && spans_.back().item == static_cast<std::size_t>(pc.item)) spans_.back().x1 = x + adv;
                else spans_.push_back({x, x + adv, static_cast<std::size_t>(pc.item)});
            }
            x += adv;
        }
        p.end();
        strip_ = pm;
        offset_ = std::fmod(offset_, static_cast<double>(w));
        update();
    }

    void resume_animation() {
        if (!isVisible() || window()->isMinimized()) return;
        clock_.restart();
        frame_.start();
    }

    void advance() {
        const qint64 ms = clock_.restart();
        if (paused() || strip_.isNull()) return;
        const double w = strip_.width() / strip_.devicePixelRatio();
        if (w <= 0.0) return;
        // Clamp a long stall (a modal dialog, a breakpoint) to one frame.
        offset_ = std::fmod(offset_ + speed_ * std::min<qint64>(ms, 50) / 1000.0, w);
        update();
    }

    [[nodiscard]] const news::NewsItem* item_at(int x) const {
        if (strip_.isNull() || items_.empty()) return nullptr;
        const int label_w = label_w_;
        const double w = strip_.width() / strip_.devicePixelRatio();
        if (x < label_w || w <= 0.0) return nullptr;
        const double local = std::fmod(x - label_w + offset_, w);
        for (const auto& s : spans_)
            if (local >= s.x0 && local < s.x1) return &items_[s.item];
        return nullptr;
    }

    void load_cache() {
        QFile f(cache_path());
        if (f.open(QIODevice::ReadOnly)) set_items(news::from_json(f.readAll()));
    }

    void fetch() {
        if (helper_.isEmpty() || !proc_.isNull()) return;
        auto* p = new QProcess(this);
        proc_ = p;
        QStringList args;
        const QString feeds = root_ + QStringLiteral("/config/news_feeds.txt");
        if (QFileInfo::exists(feeds)) args << QStringLiteral("--feeds") << feeds;
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus status) {
            const QByteArray out = p->readAllStandardOutput();
            p->deleteLater();
            if (status == QProcess::NormalExit && code == 0) {
                auto items = news::from_json(out);
                if (!items.empty()) {
                    set_items(std::move(items));
                    QDir().mkpath(QFileInfo(cache_path()).absolutePath());
                    QSaveFile cache(cache_path());
                    if (cache.open(QIODevice::WriteOnly)) { cache.write(out); (void)cache.commit(); }
                    return;
                }
            }
            if (items_.empty()) set_message(QStringLiteral("News: no feed answered (offline, or the feeds in config/news_feeds.txt moved)"));
        });
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            p->deleteLater();
            if (items_.empty()) set_message(QStringLiteral("News: altair_news could not start"));
        });
        p->start(helper_, args);
    }
};

} // namespace altair::ui
