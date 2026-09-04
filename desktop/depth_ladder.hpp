// desktop/depth_ladder.hpp -- the L2 ladder.
//
// P11Q-03.
//
// FIVE SLOTS ARE NOT FIVE LEVELS, AND THE ZEROED ONES ARE THE HAZARD.
//
// `feed/tick.hpp` D6 says it outright: "an unpopulated level is zeroed -- a
// strategy reading a zeroed bid as real would see a price of 0 and infinite
// edge." A ladder is the most literal possible renderer of that array, so the
// obvious loop `for k in 0..kDepthLevels` draws three rows of Rs 0.00 under a
// two-level book and they look exactly like liquidity. `bid_levels` and
// `ask_levels` are the bound, and they are the ARGUMENT to the loop rather
// than a filter applied afterwards.
//
// A CROSSED BOOK IS NOT AN ERROR AND MUST STILL DRAW.
//
// The P2-08 carried debt: "a crossed book is NOT an error, and rejecting one
// breaks every pre-open." Bid above ask is legitimate during the auction and a
// fault at any other time, and the ladder cannot tell which -- so it RENDERS
// the book and marks it, rather than refusing to draw. A ladder that goes
// blank during pre-open is a ladder nobody can use when it matters.
//
// NEVER UPDATED IS NOT EMPTY.
//
// `BookError::NotFound` carries the comment "never updated -- NOT the same as
// empty (D1)". An instrument whose feed has not arrived and one whose book has
// no resting orders render identically as a blank ladder, and they are
// completely different problems: one is our fault. So the view carries
// `never_updated` and the widget says which.
//
// AND IMBALANCE WITH ONE SIDE EMPTY IS NOT +/-1.
//
// (bid - ask) / (bid + ask) is +1 when the ask side is empty, which reads as
// maximum buying pressure. It is not a measurement of pressure at all -- it is
// the absence of one side. The field is optional and unset in that case, for
// the same reason `data()` returns an invalid QVariant for a tick that has not
// arrived rather than 0.00.

#pragma once

#include <book/l2_book.hpp>
#include <feed/tick.hpp>

#include "format.hpp"

#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <vector>

namespace altair::ui {

struct LadderRow {
    bool is_ask = false;
    Price px{};
    Qty qty{};
    std::uint32_t orders = 0;
    /// Running total from the touch outwards, per side. What a market order of
    /// this size would have to sweep.
    std::int64_t cumulative = 0;
};

struct LadderView {
    /// The feed has never delivered this instrument. Distinct from a book with
    /// no resting orders -- see the header.
    bool never_updated = true;
    bool crossed = false;
    bool tradable = false;
    /// Asks worst-to-best, then bids best-to-worst: the conventional ladder,
    /// read top to bottom, with the spread in the middle.
    std::vector<LadderRow> rows;
    std::int64_t bid_total = 0;
    std::int64_t ask_total = 0;
    /// (bid - ask) / (bid + ask). UNSET when either side is empty, because
    /// that is an absence rather than an extreme.
    std::optional<double> imbalance;
    /// Rejections since the last ACCEPTED update. Non-zero means "right now",
    /// not "ever" -- BookState D4.
    std::uint16_t rejects = 0;
};

/// Turn a book into rows. `b == nullptr` means the feed never delivered it.
///
/// Pure, so gate 4 can cover the part that matters without a window. The
/// widget below only paints what this returns.
[[nodiscard]] inline LadderView ladder_view(const BookState* b) {
    LadderView v;
    if (b == nullptr) {
        return v;                       // never_updated stays true
    }
    v.never_updated = false;
    v.crossed = b->crossed;
    v.tradable = is_tradable(*b);
    v.rejects = b->consecutive_rejects;

    // THE BOUND IS bid_levels / ask_levels, NOT kDepthLevels. Clamped as well,
    // because a decoder that wrote 7 into a uint8 field would otherwise walk
    // off a five-element array -- and the clamp is silent here on purpose:
    // this is a renderer, and the place to raise about a bad level count is
    // the decoder that produced it.
    const std::size_t na =
        b->ask_levels < kDepthLevels ? b->ask_levels : kDepthLevels;
    const std::size_t nb =
        b->bid_levels < kDepthLevels ? b->bid_levels : kDepthLevels;

    for (std::size_t i = 0; i < na; ++i) {
        v.ask_total += b->ask[i].qty.raw();
    }
    for (std::size_t i = 0; i < nb; ++i) {
        v.bid_total += b->bid[i].qty.raw();
    }

    // Asks printed worst first so the touch sits next to the bid touch.
    // Cumulative still counts FROM the touch, so row order and sweep order
    // disagree deliberately -- the number answers "what would I pay through",
    // which is not a function of which way the screen reads.
    for (std::size_t i = na; i-- > 0;) {
        LadderRow r;
        r.is_ask = true;
        r.px = b->ask[i].px;
        r.qty = b->ask[i].qty;
        r.orders = b->ask[i].orders;
        for (std::size_t k = 0; k <= i; ++k) {
            r.cumulative += b->ask[k].qty.raw();
        }
        v.rows.push_back(r);
    }
    for (std::size_t i = 0; i < nb; ++i) {
        LadderRow r;
        r.is_ask = false;
        r.px = b->bid[i].px;
        r.qty = b->bid[i].qty;
        r.orders = b->bid[i].orders;
        for (std::size_t k = 0; k <= i; ++k) {
            r.cumulative += b->bid[k].qty.raw();
        }
        v.rows.push_back(r);
    }

    if (v.bid_total > 0 && v.ask_total > 0) {
        const double bid = static_cast<double>(v.bid_total);
        const double ask = static_cast<double>(v.ask_total);
        v.imbalance = (bid - ask) / (bid + ask);
    }
    return v;
}

class DepthLadder final : public QWidget {
    Q_OBJECT

public:
    explicit DepthLadder(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);

        head_ = new QLabel(this);
        head_->setWordWrap(true);
        v->addWidget(head_);

        t_ = new QTableWidget(0, 4, this);
        t_->setHorizontalHeaderLabels({QStringLiteral("Orders"),
                                       QStringLiteral("Qty"),
                                       QStringLiteral("Price"),
                                       QStringLiteral("Cum")});
        t_->verticalHeader()->setVisible(false);
        t_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t_->setSelectionMode(QAbstractItemView::NoSelection);
        t_->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(t_);

        foot_ = new QLabel(this);
        foot_->setWordWrap(true);
        foot_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(foot_);

        show_book(nullptr);
    }

    void show_book(const BookState* b) {
        const LadderView v = ladder_view(b);

        if (v.never_updated) {
            head_->setText(QStringLiteral(
                "<b style='color:#B9770B'>No book.</b> The feed has never "
                "delivered depth for this instrument — which is not the same "
                "as a book with nothing in it."));
            t_->setRowCount(0);
            foot_->setText(QStringLiteral(
                "The replay tape carries trades only: <code>ReplayTick</code> "
                "has a last price and a quantity and no depth at all. A live "
                "Kite full-mode subscription (P2-02) is what fills this."));
            return;
        }

        head_->setText(
            v.crossed
                ? QStringLiteral(
                      "<b style='color:#C0392B'>CROSSED</b> — bid is at or "
                      "above ask. Legitimate during the pre-open auction, a "
                      "fault at any other time. Drawn rather than refused: a "
                      "ladder that blanks during pre-open is one nobody can "
                      "use when it matters.")
                : (v.tradable
                       ? QStringLiteral("<b style='color:#1B8A4B'>Tradable</b>"
                                        " — both sides have liquidity.")
                       : QStringLiteral(
                             "<b style='color:#B9770B'>Not tradable</b> — one "
                             "side is empty. Not crossed; there is simply "
                             "nothing there to trade against.")));

        t_->setRowCount(static_cast<int>(v.rows.size()));
        int row = 0;
        for (const LadderRow& r : v.rows) {
            const QColor fg = r.is_ask ? QColor(0xC0, 0x39, 0x2B)
                                       : QColor(0x1B, 0x8A, 0x4B);
            auto put = [&](int col, const QString& s) {
                auto* it = new QTableWidgetItem(s);
                it->setForeground(fg);
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                t_->setItem(row, col, it);
            };
            put(0, QString::number(r.orders));
            put(1, QString::number(r.qty.raw()));
            put(2, format_paise(r.px.raw()));
            put(3, QString::number(r.cumulative));
            ++row;
        }
        t_->resizeColumnsToContents();

        QString f = QStringLiteral("bid %1 · ask %2 · ")
                        .arg(v.bid_total).arg(v.ask_total);
        // UNSET, not zero and not ±1. One empty side is an absence, and
        // (bid-ask)/(bid+ask) would report it as maximum pressure.
        f += v.imbalance.has_value()
                 ? QStringLiteral("imbalance %1")
                       .arg(*v.imbalance, 0, 'f', 3)
                 : QStringLiteral("imbalance — (one side empty, so there is "
                                  "no ratio to take)");
        if (v.rejects > 0) {
            f += QStringLiteral(
                     "  ·  <b style='color:#C0392B'>%1 rejected updates since "
                     "the last accepted one</b> — that is NOW, not a running "
                     "total").arg(v.rejects);
        }
        foot_->setText(f);
    }

private:
    QLabel* head_ = nullptr;
    QTableWidget* t_ = nullptr;
    QLabel* foot_ = nullptr;
};

} // namespace altair::ui
