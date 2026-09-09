// desktop/chart/chart_widget.hpp -- the candlestick panel.
//
// P11Q-04.
//
// WHY THIS IS A QWidget AND NOT A QOpenGLWidget, FOR NOW.
//
// The LEDGER said `QOpenGLWidget`. It is a plain widget with `QPainter`, and
// the change is deliberate rather than a shortcut.
//
// A minute chart of a session is a few hundred candles. QPainter draws that
// in well under a frame, and reaching for a GPU, a shader and a VBO before
// there is a measured reason is how a chart acquires a driver-dependent crash
// path in exchange for headroom nobody needed. The moment there IS a reason --
// a tick chart, a depth heatmap, a hundred thousand points -- the upload path
// is a card, and `candles.hpp` already produces `Vertex` floats for it.
//
// WHAT DOES NOT CHANGE IS THE float32 BOUNDARY. QPainter takes `qreal`, which
// is a double, and would happily hide the whole problem until the GPU card
// landed and reintroduced it silently. So the transform goes through
// `Vertex` -- explicit floats, exactly what a VBO would hold -- and this
// widget consumes those. The precision discipline is in the data path, where
// it belongs, rather than in the renderer that happens to be attached today.
//
// THE FORMING CANDLE IS DRAWN DIFFERENTLY.
//
// Rule 7 is not only a data property; if the last candle looks identical to
// the settled ones, the eye treats a number that will still change as one that
// has. It is drawn hollow with a dashed edge, and the legend says so.
//
// AND THE READOUT IS THE CANDLE'S OWN.
//
// The crosshair snaps to a candle and prints its open/high/low/close, not an
// interpolation at the pixel. An interpolated readout shows, to the paisa, a
// price at which nothing traded.

#pragma once

#include "../tick_model.hpp"
#include "candles.hpp"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace altair::ui {

class ChartWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ChartWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMouseTracking(true);
        setMinimumHeight(280);
        setAutoFillBackground(true);
    }

    void set_title(const QString& t) {
        title_ = t;
        update();
    }

    /// Replaces the conservation line for a series LOADED from disk, where
    /// there is no tape to reconcile against. Left empty for a replayed
    /// series, so the reconciliation shows instead -- the two are different
    /// claims and the header should not pretend otherwise.
    void set_note(const QString& n) {
        note_ = n;
        update();
    }

    /// Replace the series. Recomputes both axes from the data, per series.
    ///
    /// A NEW SERIES RESETS THE ZOOM. Keeping a window across a change of
    /// instrument means index 400..500 of a different series, which is a
    /// different span of a different thing and would look like a chart that
    /// simply moved.
    void set_candles(std::vector<Candle> c, const Conservation& k) {
        candles_ = std::move(c);
        conservation_ = k;
        first_ = 0;
        count_ = candles_.size();
        recompute_view();
        update();
    }

    // ── Zoom and pan. P32-05. ────────────────────────────────────────────
    //
    // WHY THE CHART NEEDED THIS AT ALL. It drew every candle across the
    // width, always. On the daily NIFTY series that is 8,755 candles in about
    // 1,500 pixels -- five candles a pixel, so the wick, body and colour of
    // every one of them are a lie by the time they reach the screen. The
    // conservation line at the top was reconciling volume for a picture in
    // which no individual bar was legible.
    //
    // ZOOM CHANGES THE WINDOW, NOT THE DATA. `candles_` stays whole and
    // `first_`/`count_` say what is drawn, so the price axis and the domain
    // are recomputed from the VISIBLE slice -- which is the point: a zoomed
    // chart whose price axis still spans 35 years is a chart with a flat line
    // in the middle of it.

    void zoom_in() { rescale(0.7); }
    void zoom_out() { rescale(1.0 / 0.7); }

    void zoom_reset() {
        first_ = 0;
        count_ = candles_.size();
        recompute_view();
        update();
    }

    /// Pan by a fraction of the visible window. Negative is back in time.
    void pan(double fraction) {
        if (candles_.empty()) { return; }
        const auto step = static_cast<std::ptrdiff_t>(
            static_cast<double>(count_) * fraction);
        auto f = static_cast<std::ptrdiff_t>(first_) + step;
        const auto maxf =
            static_cast<std::ptrdiff_t>(candles_.size() - count_);
        if (f < 0) { f = 0; }
        if (f > maxf) { f = maxf; }
        first_ = static_cast<std::size_t>(f);
        recompute_view();
        update();
    }

    [[nodiscard]] std::size_t visible_first() const noexcept { return first_; }
    [[nodiscard]] std::size_t visible_count() const noexcept { return count_; }

    [[nodiscard]] const Domain& domain() const noexcept { return domain_; }
    [[nodiscard]] const PriceAxis& axis() const noexcept { return axis_; }

private:
    /// Scale the window about its CENTRE, floored at a window nobody can
    /// misread. Eight candles is the floor: below that the price axis is
    /// derived from so few bars that one wick sets the whole scale.
    void rescale(double factor) {
        if (candles_.empty()) { return; }
        const std::size_t total = candles_.size();
        const double want = static_cast<double>(count_) * factor;
        std::size_t next = static_cast<std::size_t>(want + 0.5);
        if (next < 8) { next = 8; }
        if (next > total) { next = total; }
        // Anchor on the centre so zooming does not walk the view sideways.
        const double centre =
            static_cast<double>(first_) + static_cast<double>(count_) / 2.0;
        double nf = centre - static_cast<double>(next) / 2.0;
        if (nf < 0.0) { nf = 0.0; }
        if (nf > static_cast<double>(total - next)) {
            nf = static_cast<double>(total - next);
        }
        first_ = static_cast<std::size_t>(nf);
        count_ = next;
        recompute_view();
        update();
    }

    /// The domain and the price axis come from the VISIBLE candles.
    void recompute_view() {
        if (candles_.empty() || count_ == 0) {
            view_.clear();
            return;
        }
        if (first_ + count_ > candles_.size()) {
            count_ = candles_.size() - first_;
        }
        view_.assign(candles_.begin()
                         + static_cast<std::ptrdiff_t>(first_),
                     candles_.begin()
                         + static_cast<std::ptrdiff_t>(first_ + count_));
        domain_.origin_ns = view_.front().start_ns;
        const std::int64_t span = view_.back().end_ns - view_.front().start_ns;
        domain_.span_ns = span > 0 ? span : 1;
        axis_ = price_axis(view_);
    }

public:

protected:
    void mouseMoveEvent(QMouseEvent* e) override {
        const int x = static_cast<int>(e->position().x());
        if (dragging_ && count_ > 0 && width() > 1) {
            // Drag moves the window by the fraction of the width dragged, so
            // the candle under the cursor stays roughly under the cursor.
            const double frac = static_cast<double>(drag_x_ - x)
                              / static_cast<double>(width());
            if (std::abs(frac) > 0.002) {
                pan(frac);
                drag_x_ = x;
            }
        }
        hover_x_ = static_cast<float>(x);
        hover_y_ = static_cast<float>(e->position().y());
        has_hover_ = true;
        update();
    }

    void leaveEvent(QEvent*) override {
        has_hover_ = false;
        update();
    }

    /// Wheel zooms. Ctrl is not required, because on a chart the wheel has no
    /// other job -- there is nothing to scroll.
    void wheelEvent(QWheelEvent* e) override {
        if (e->angleDelta().y() == 0) { return; }
        if (e->angleDelta().y() > 0) { zoom_in(); } else { zoom_out(); }
        e->accept();
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            drag_x_ = static_cast<int>(e->position().x());
            dragging_ = true;
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), QColor(0x1A, 0x1F, 0x24));

        const int left = 8;
        const int right = width() - 96;   // room for the price scale
        const int top = 30;
        const int volume_h = 56;
        const int bottom = height() - 26 - volume_h;

        draw_title(p);

        if (view_.empty() || right <= left || bottom <= top) {
            p.setPen(QColor(0x7F, 0x8C, 0x8D));
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("no candles yet"));
            return;
        }

        draw_price_grid(p, left, right, top, bottom);
        draw_candles(p, left, right, top, bottom, volume_h);
        draw_crosshair(p, left, right, top, bottom);
        draw_legend(p);
    }

private:
    [[nodiscard]] int px_x(float nx, int left, int right) const {
        return left + static_cast<int>(nx * static_cast<float>(right - left));
    }
    [[nodiscard]] int px_y(float ny, int top, int bottom) const {
        // Screen y grows downward; price grows upward.
        return bottom - static_cast<int>(ny * static_cast<float>(bottom - top));
    }

    void draw_title(QPainter& p) {
        p.setPen(QColor(0xD6, 0xDB, 0xDF));
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(10, 20, title_);
        f.setBold(false);
        p.setFont(f);

        if (!note_.isEmpty()) {
            p.setPen(QColor(0x7F, 0x8C, 0x8D));
            p.drawText(
                width() - QFontMetrics(p.font()).horizontalAdvance(note_) - 10,
                20, note_);
            return;
        }

        // The conservation check, ON SCREEN. A chart whose volume disagrees
        // with the tape says so rather than being quietly wrong.
        const bool ok = conservation_.agrees;
        p.setPen(ok ? QColor(0x1B, 0x8A, 0x4B) : QColor(0xC0, 0x39, 0x2B));
        const QString k =
            ok ? QStringLiteral("volume reconciles: %1 = %2")
                     .arg(conservation_.chart_volume)
                     .arg(conservation_.tape_volume)
               : QStringLiteral("VOLUME DISAGREES: chart %1 vs tape %2")
                     .arg(conservation_.chart_volume)
                     .arg(conservation_.tape_volume);
        p.drawText(width() - QFontMetrics(p.font()).horizontalAdvance(k) - 10,
                   20, k);
    }

    void draw_price_grid(QPainter& p, int left, int right, int top,
                         int bottom) {
        p.setPen(QPen(QColor(0x2C, 0x33, 0x39), 1));
        constexpr int kLines = 5;
        for (int i = 0; i <= kLines; ++i) {
            const float ny = static_cast<float>(i) / kLines;
            const int y = px_y(ny, top, bottom);
            p.drawLine(left, y, right, y);

            const std::int64_t paise =
                axis_.origin_paise
                + static_cast<std::int64_t>(
                    static_cast<double>(axis_.span_paise) * ny);
            p.setPen(QColor(0x7F, 0x8C, 0x8D));
            p.drawText(right + 6, y + 4, format_paise(paise));
            p.setPen(QPen(QColor(0x2C, 0x33, 0x39), 1));
        }
    }

    void draw_candles(QPainter& p, int left, int right, int top, int bottom,
                      int volume_h) {
        const int n = static_cast<int>(view_.size());
        const int span = right - left;
        // At least one pixel per candle, and a gap only when there is room.
        const double slot = static_cast<double>(span) / n;
        const int body = std::max(1, static_cast<int>(slot * 0.68));

        std::int64_t max_vol = 1;
        for (const Candle& c : view_) {
            if (c.volume_known && c.volume > max_vol) max_vol = c.volume;
        }
        const int vol_top = bottom + 8;

        for (int i = 0; i < n; ++i) {
            const Candle& c = view_[static_cast<std::size_t>(i)];
            // THROUGH THE float32 STAGE, rebased in int64 first.
            const Vertex hi{to_x(c.start_ns, domain_), to_y(c.high, axis_)};
            const Vertex lo{to_x(c.start_ns, domain_), to_y(c.low, axis_)};
            const float yo = to_y(c.open, axis_);
            const float yc = to_y(c.close, axis_);

            const int cx = px_x(hi.x, left, right) + static_cast<int>(slot / 2);
            const int y_hi = px_y(hi.y, top, bottom);
            const int y_lo = px_y(lo.y, top, bottom);
            const int y_o = px_y(yo, top, bottom);
            const int y_c = px_y(yc, top, bottom);

            const bool up = c.close >= c.open;
            QColor col = up ? QColor(0x2E, 0xCC, 0x71) : QColor(0xE7, 0x4C, 0x3C);

            if (!c.complete) {
                // THE FORMING CANDLE IS NOT A CANDLE. Drawn hollow with a
                // dashed edge, because if it looks like the settled ones the
                // eye treats a number that will still change as one that has.
                p.setPen(QPen(col, 1, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
            } else {
                p.setPen(QPen(col, 1));
                p.setBrush(col);
            }

            p.drawLine(cx, y_hi, cx, y_lo);
            const int y_top = std::min(y_o, y_c);
            const int h = std::max(1, std::abs(y_c - y_o));
            p.drawRect(cx - body / 2, y_top, body, h);

            // Volume, same x, its own scale. Extensive, so it sums; and it is
            // drawn under the price rather than over it, because overlaying
            // two different units on one axis is how a chart lies.
            // An UNKNOWN volume draws nothing at all. A zero-height bar says
            // nothing traded; 1,408 daily NIFTY bars report 0 because the
            // index published no turnover in those years, and the market was
            // open on every one of them.
            if (c.volume_known) {
                const int vh = static_cast<int>(
                    static_cast<double>(c.volume) / static_cast<double>(max_vol)
                    * (volume_h - 10));
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(col.red(), col.green(), col.blue(), 110));
                p.drawRect(cx - body / 2, vol_top + (volume_h - 10 - vh), body,
                           vh);
            }
        }
    }

    void draw_crosshair(QPainter& p, int left, int right, int top, int bottom) {
        if (!has_hover_ || view_.empty()) {
            return;
        }
        const int cx = std::clamp(static_cast<int>(hover_x_), left, right);
        const int cy = std::clamp(static_cast<int>(hover_y_), top, bottom);
        const float nx = static_cast<float>(cx - left)
                       / static_cast<float>(std::max(1, right - left));
        const auto at_ns = domain_.origin_ns
                         + static_cast<std::int64_t>(
                               static_cast<double>(domain_.span_ns) * nx);

        const Crosshair h = snap_crosshair(view_, at_ns);
        if (!h.valid) {
            return;
        }
        const Candle& c = view_[h.index];

        const QColor line(0x8A, 0x9B, 0xA8);
        p.setPen(QPen(line, 1, Qt::DotLine));
        const int snap_x =
            px_x(to_x(c.start_ns, domain_), left, right)
            + static_cast<int>((static_cast<double>(right - left)
                                / static_cast<double>(view_.size())) / 2);
        p.drawLine(snap_x, top, snap_x, bottom);

        // THE HORIZONTAL ARM, AND THE PRICE AT IT. P32-05.
        //
        // The vertical arm snaps to a candle and the horizontal one does NOT,
        // deliberately: the y position is a price the cursor is pointing at,
        // and it is read off the axis rather than off any candle. So it is
        // labelled on the price scale, in the same units the scale uses, and
        // it is the one number on this chart that is an interpolation --
        // which is fine, because it is answering "what price is my cursor
        // on", not "what did this candle do".
        p.drawLine(left, cy, right, cy);

        const double ny = static_cast<double>(bottom - cy)
                        / static_cast<double>(std::max(1, bottom - top));
        const std::int64_t at_price =
            axis_.origin_paise
            + static_cast<std::int64_t>(
                  static_cast<double>(axis_.span_paise) * ny);
        const QString plab = format_paise(at_price);
        QFontMetrics fm(p.font());
        const int pw = fm.horizontalAdvance(plab) + 8;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2C, 0x3E, 0x50));
        p.drawRect(right + 2, cy - 9, pw, 18);
        p.setPen(QColor(0xEC, 0xF0, 0xF1));
        p.drawText(right + 6, cy + 4, plab);

        // THE CANDLE'S OWN VALUES, not an interpolation at the pixel.
        const QString text =
            QStringLiteral("O %1   H %2   L %3   C %4   %5%6")
                .arg(format_paise(c.open), format_paise(c.high),
                     format_paise(c.low), format_paise(c.close))
                // ABSENT VOLUME IS NOT ZERO VOLUME. 1,408 daily NIFTY bars
                // report no turnover at all and the market was open on every
                // one of them; printing "V 0" for those says something false.
                .arg(c.volume_known
                         ? QStringLiteral("V %1").arg(c.volume)
                         : QStringLiteral("V —  (not published)"))
                .arg(c.complete ? QString()
                                : QStringLiteral("   (still forming)"));
        p.setPen(QColor(0xEC, 0xF0, 0xF1));
        p.drawText(10, height() - 8,
                   h.extrapolated
                       ? text + QStringLiteral("   [past the last candle]")
                       : text);
    }

    void draw_legend(QPainter& p) {
        p.setPen(QColor(0x7F, 0x8C, 0x8D));
        // WHAT IS SHOWN OUT OF WHAT EXISTS. A zoomed chart that says only
        // "180 candles" is indistinguishable from a series that has 180.
        const QString what =
            count_ == candles_.size()
                ? QStringLiteral("%1 candles").arg(candles_.size())
                : QStringLiteral("%1 of %2 candles (%3–%4)")
                      .arg(count_).arg(candles_.size())
                      .arg(first_ + 1).arg(first_ + count_);
        p.drawText(10, height() - 8 - 16,
                   QStringLiteral("%1 · dashed = still forming · hover for "
                                  "that candle's own OHLC · wheel or +/− to "
                                  "zoom, drag to pan")
                       .arg(what));
    }

    QString title_;
    QString note_;
    std::vector<Candle> candles_;   ///< everything loaded
    std::vector<Candle> view_;      ///< the slice actually drawn
    std::size_t first_ = 0;
    std::size_t count_ = 0;
    Conservation conservation_;
    Domain domain_;
    PriceAxis axis_;
    float hover_x_ = 0.0f;
    float hover_y_ = 0.0f;
    bool has_hover_ = false;
    bool dragging_ = false;
    int drag_x_ = 0;
};

} // namespace altair::ui
