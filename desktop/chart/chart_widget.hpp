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
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWidget>

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
    void set_candles(std::vector<Candle> c, const Conservation& k) {
        candles_ = std::move(c);
        conservation_ = k;
        if (!candles_.empty()) {
            domain_.origin_ns = candles_.front().start_ns;
            const std::int64_t span =
                candles_.back().end_ns - candles_.front().start_ns;
            domain_.span_ns = span > 0 ? span : 1;
            axis_ = price_axis(candles_);
        }
        update();
    }

    [[nodiscard]] const Domain& domain() const noexcept { return domain_; }
    [[nodiscard]] const PriceAxis& axis() const noexcept { return axis_; }

protected:
    void mouseMoveEvent(QMouseEvent* e) override {
        hover_x_ = e->position().x();
        has_hover_ = true;
        update();
    }

    void leaveEvent(QEvent*) override {
        has_hover_ = false;
        update();
    }

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

        if (candles_.empty() || right <= left || bottom <= top) {
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
        const int n = static_cast<int>(candles_.size());
        const int span = right - left;
        // At least one pixel per candle, and a gap only when there is room.
        const double slot = static_cast<double>(span) / n;
        const int body = std::max(1, static_cast<int>(slot * 0.68));

        std::int64_t max_vol = 1;
        for (const Candle& c : candles_) {
            if (c.volume_known && c.volume > max_vol) max_vol = c.volume;
        }
        const int vol_top = bottom + 8;

        for (int i = 0; i < n; ++i) {
            const Candle& c = candles_[static_cast<std::size_t>(i)];
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
        if (!has_hover_ || candles_.empty()) {
            return;
        }
        const int cx = std::clamp(hover_x_, left, right);
        const float nx = static_cast<float>(cx - left)
                       / static_cast<float>(std::max(1, right - left));
        const auto at_ns = domain_.origin_ns
                         + static_cast<std::int64_t>(
                               static_cast<double>(domain_.span_ns) * nx);

        const Crosshair h = snap_crosshair(candles_, at_ns);
        if (!h.valid) {
            return;
        }
        const Candle& c = candles_[h.index];

        p.setPen(QPen(QColor(0x7F, 0x8C, 0x8D), 1, Qt::DotLine));
        const int snap_x =
            px_x(to_x(c.start_ns, domain_), left, right)
            + static_cast<int>((static_cast<double>(right - left)
                                / static_cast<double>(candles_.size())) / 2);
        p.drawLine(snap_x, top, snap_x, bottom);

        // THE CANDLE'S OWN VALUES, not an interpolation at the pixel.
        const QString text =
            QStringLiteral("O %1   H %2   L %3   C %4   V %5%6")
                .arg(format_paise(c.open), format_paise(c.high),
                     format_paise(c.low), format_paise(c.close))
                .arg(c.volume)
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
        p.drawText(10, height() - 8 - 16,
                   QStringLiteral("%1 candles · dashed = still forming · "
                                  "hover for that candle's own OHLC")
                       .arg(candles_.size()));
    }

    QString title_;
    QString note_;
    std::vector<Candle> candles_;
    Conservation conservation_;
    Domain domain_;
    PriceAxis axis_;
    int hover_x_ = 0;
    bool has_hover_ = false;
};

} // namespace altair::ui
