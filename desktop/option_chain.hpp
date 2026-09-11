// desktop/option_chain.hpp -- the option chain, as an execution screen.
//
// P39-01. CALLS | STRIKE | PUTS, the way ODIN and every Indian terminal lays
// it out: Greeks fanning outward from the strike, in-the-money halves shaded,
// the at-the-money row marked, and a click on either side loading that exact
// contract into the order ticket.
//
// THE NUMBERS ARE A MODEL UNTIL OPTION QUOTES ARRIVE, AND THE SCREEN SAYS SO.
//
// No option has ever been ticked in this tree -- dataset/opt/ does not exist
// and the Kite token expired -- so there is no market price to invert for
// implied volatility. The chain is therefore Black-76 at ONE volatility:
//
//   NIFTY       India VIX, which IS the market's 30-day implied vol for NIFTY.
//               The best single number available, and still flat: real NIFTY
//               options carry a smile, so the wings are mispriced.
//   BANKNIFTY   its own 20-day REALISED volatility. There is no BANKNIFTY VIX
//               in the dataset, and borrowing NIFTY's would understate it.
//
// The strikes, lots, ticks, expiries and tokens are all REAL -- read from the
// Kite instrument master by the engine's own parser. Only the prices and
// Greeks are modelled, and the header says which vol was used and where it
// came from.
//
// A MODEL PRICE NEVER SEEDS AN ORDER. Clicking a row loads the contract and
// its spec into the ticket; it does not touch the limit price. Putting a
// flat-vol wing price into an order ticket is precisely the plausible, wrong
// number rule 9 exists to stop. When live quotes exist, the click will carry
// the quoted price instead.
//
// PRICED IN STRUCTURE-OF-ARRAYS, PAINTED ROW BY ROW. analytics/chain_soa.hpp
// sweeps the whole expiry in one cache-resident pass; the grid then reads one
// strike at a time. bench/bench_chain.cpp measured that at this size -- 21
// strikes, far inside L1 -- the two layouts are within 1-2% of each other for
// both jobs, so there is no second copy in AoS to keep in step.

#pragma once

#include "chain_panel.hpp"
#include "data/series_io.hpp"

#include <analytics/chain_soa.hpp>

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <vector>

namespace altair::ui {

/// Column layout. CALLS read right-to-left toward the strike, PUTS
/// left-to-right away from it, so the most-watched numbers sit next to the
/// strike on both sides.
enum ChainCol : int {
    kC_Theta = 0, kC_Vega, kC_Gamma, kC_Delta, kC_Iv, kC_Px,
    kStrike,
    kP_Px, kP_Iv, kP_Delta, kP_Gamma, kP_Vega, kP_Theta,
    kChainCols
};

class OptionChainPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OptionChainPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(4);

        auto* bar = new QHBoxLayout;
        bar->addWidget(new QLabel(QStringLiteral("OPTION CHAIN"), this));
        under_ = new QComboBox(this);
        under_->addItem(QStringLiteral("NIFTY"), QStringLiteral("nifty"));
        under_->addItem(QStringLiteral("BANKNIFTY"), QStringLiteral("banknifty"));
        bar->addWidget(under_);
        bar->addWidget(new QLabel(QStringLiteral("expiry"), this));
        expiry_ = new QComboBox(this);
        expiry_->setMinimumWidth(130);
        bar->addWidget(expiry_);
        bar->addWidget(new QLabel(QStringLiteral("strikes ±"), this));
        width_ = new QSpinBox(this);
        width_->setRange(3, 60);
        width_->setValue(10);
        bar->addWidget(width_);
        bar->addStretch();
        badge_ = new QLabel(this);
        badge_->setTextFormat(Qt::RichText);
        bar->addWidget(badge_);
        v->addLayout(bar);

        head_ = new QLabel(this);
        head_->setTextFormat(Qt::RichText);
        // WRAP, or the header's one long line becomes the page's MINIMUM
        // width: adding "(valued at tick 13:04 04-Sep)" pushed the window to
        // 1964 px on a 1920 screen and clipped the order ticket off the edge.
        head_->setWordWrap(true);
        v->addWidget(head_);

        grid_ = new QTableWidget(0, kChainCols, this);
        grid_->setHorizontalHeaderLabels({
            QStringLiteral("Θ ₹/day"), QStringLiteral("ν ₹/1%"),
            QStringLiteral("Γ /₹"), QStringLiteral("Δ"), QStringLiteral("IV"),
            QStringLiteral("CALL ₹"),
            QStringLiteral("STRIKE"),
            QStringLiteral("PUT ₹"), QStringLiteral("IV"), QStringLiteral("Δ"),
            QStringLiteral("Γ /₹"), QStringLiteral("ν ₹/1%"),
            QStringLiteral("Θ ₹/day")});
        grid_->verticalHeader()->setVisible(false);
        grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        grid_->setSelectionBehavior(QAbstractItemView::SelectItems);
        grid_->setSelectionMode(QAbstractItemView::SingleSelection);
        grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        grid_->verticalHeader()->setDefaultSectionSize(22);
        v->addWidget(grid_, 1);

        foot_ = new QLabel(this);
        foot_->setWordWrap(true);
        foot_->setStyleSheet(QStringLiteral("color:#8A93A2;"));
        v->addWidget(foot_);

        connect(under_, &QComboBox::currentIndexChanged, this,
                [this](int) { load_underlying(); });
        connect(expiry_, &QComboBox::currentIndexChanged, this,
                [this](int) { reprice(); });
        connect(width_, &QSpinBox::valueChanged, this,
                [this](int) { reprice(); });
        connect(grid_, &QTableWidget::cellClicked, this,
                [this](int r, int c) { on_click(r, c); });

        load_underlying();
    }

    /// A live price for the underlying, from the price service. The chain
    /// re-prices around it; everything else stays as it was.
    ///
    /// `replay` is the frame's own flag. It is carried rather than inferred
    /// because the first version knew only "this came from the stream" and
    /// labelled a REPLAYED price LIVE -- on a screen whose strip, two inches
    /// above, correctly said REPLAY. A replay shown as live is the one mistake
    /// this screen must not make.
    ///
    /// `tick_ns` is the frame's exchange timestamp, and the chain is VALUED
    /// AT IT (see reprice). 0 means the frame carried none.
    void set_spot(unsigned token, qint64 paise, bool replay,
                  std::int64_t tick_ns) {
        if (paise <= 0 || token != spot_token_) { return; }
        spot_paise_ = static_cast<double>(paise);
        spot_ts_ns_ = tick_ns;
        spot_live_ = true;
        spot_replay_ = replay;
        reprice();
    }

    [[nodiscard]] int strikes_shown() const { return grid_->rowCount(); }
    /// For tests: drive a click without a window manager, the same way the
    /// terminal test drives watchlist selection.
    void click_cell(int row, int col) { on_click(row, col); }
    /// For tests: the header line, so a test can check what it CLAIMS.
    [[nodiscard]] QString header_text() const { return head_->text(); }
    /// For tests: the days to expiry the last reprice USED; 0 if it refused.
    [[nodiscard]] double days_to_expiry() const noexcept { return dte_; }
    /// For tests: the strike on a row, in paise; 0 past the end.
    [[nodiscard]] double strike_at(int row) const {
        return row >= 0 && static_cast<std::size_t>(row) < soa_.n
                   ? soa_.strike[row] : 0.0;
    }
    [[nodiscard]] double spot_paise() const { return spot_paise_; }

Q_SIGNALS:
    /// A contract picked from the chain, with its spec. The terminal wires
    /// this straight to OrderTicket::set_contract.
    void contractPicked(unsigned token, const QString& symbol, qint64 lot,
                        qint64 tick_paise, const QString& exchange);

private:
    void load_underlying() {
        const QString dir = under_->currentData().toString();
        const bool bnf = dir == QStringLiteral("banknifty");
        spot_token_ = bnf ? 260105u : 256265u;   // looked up in P25-01 / P2-12e
        spot_live_ = false;
        spot_replay_ = false;

        chain_ = load_chain(QStringLiteral(ALTAIR_SOURCE_DIR "/data/instruments.csv"),
                            bnf ? "BANKNIFTY" : "NIFTY");

        // Spot: the last COMPLETED daily close on disk until the stream says
        // otherwise. The date is kept, because a chain priced off a close
        // from last week must say so.
        const UiStamped s = ui_load_stamped(
            spot_path(QStringLiteral(ALTAIR_DATASET_DIR), dir, "1d"));
        spot_paise_ = s.closes.empty() ? 0.0 : s.closes.back() * 100.0;
        spot_ts_ns_ = s.stamps_ns.empty() ? 0 : s.stamps_ns.back();

        // Volatility, and where it came from -- see the header note.
        vol_ = 0.0;
        vol_src_.clear();
        if (!bnf) {
            const UiStamped vx = ui_load_stamped(spot_path(
                QStringLiteral(ALTAIR_DATASET_DIR), QStringLiteral("indiavix"),
                "1d"));
            if (!vx.closes.empty() && vx.closes.back() > 0.0) {
                vol_ = vx.closes.back() / 100.0;
                vol_src_ = QStringLiteral("India VIX %1 (implied, 30-day, FLAT)")
                               .arg(vx.closes.back(), 0, 'f', 2);
            }
        } else if (s.closes.size() > 21) {
            double m = 0.0, v2 = 0.0;
            std::vector<double> r;
            for (std::size_t i = s.closes.size() - 20; i < s.closes.size(); ++i) {
                r.push_back(std::log(s.closes[i] / s.closes[i - 1]));
            }
            for (double x : r) { m += x; }
            m /= static_cast<double>(r.size());
            for (double x : r) { v2 += (x - m) * (x - m); }
            vol_ = std::sqrt(v2 / static_cast<double>(r.size() - 1))
                   * std::sqrt(252.0);
            vol_src_ = QStringLiteral("20-day REALISED %1% (no BANKNIFTY VIX)")
                           .arg(100.0 * vol_, 0, 'f', 2);
        }

        // Expiries that have not yet happened, nearest first.
        const QTimeZone ist(5 * 3600 + 30 * 60);
        const std::int64_t now_ns =
            QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        const QSignalBlocker block(expiry_);
        expiry_->clear();
        for (const auto& [exp_ns, strikes] : chain_.by_expiry) {
            if (exp_ns + kExpiryClockNs <= now_ns) { continue; }
            const QDate d =
                QDateTime::fromMSecsSinceEpoch(exp_ns / 1'000'000LL, ist).date();
            expiry_->addItem(d.toString(QStringLiteral("dd-MMM-yyyy")),
                             QVariant::fromValue<qlonglong>(exp_ns));
            if (expiry_->count() >= 8) { break; }
        }
        reprice();
    }

    void reprice() {
        grid_->setRowCount(0);
        rows_.clear();
        soa_.n = 0;
        dte_ = 0.0;

        if (!chain_.loaded) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>NO CHAIN — %1</span>")
                               .arg(chain_.error.toHtmlEscaped()));
            return;
        }
        if (expiry_->count() == 0) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>No unexpired contracts in the "
                "master. Refresh data/instruments.csv.</span>"));
            return;
        }
        if (spot_paise_ <= 0.0 || vol_ <= 0.0) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>No spot or no volatility on disk "
                "for this underlying — nothing can be priced.</span>"));
            return;
        }

        const std::int64_t exp_ns = expiry_->currentData().toLongLong();
        const auto it = chain_.by_expiry.find(exp_ns);
        if (it == chain_.by_expiry.end()) { return; }

        // THE VALUATION INSTANT IS THE PRICE'S OWN. A streamed spot is valued
        // at its tick's timestamp -- for a replay, the replayed moment. The
        // first version valued every spot at the wall clock and so priced a
        // 4 September replayed spot with 11 September's time to expiry: a
        // week of theta that did not exist (hard rule 7 -- time comes off the
        // tick). A close from disk has no intraday instant; it is valued NOW,
        // and the header says both "close dd-MMM" and "valued now".
        const std::int64_t wall_ns =
            QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
        if (spot_live_ && spot_replay_ && spot_ts_ns_ <= 0) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>A REPLAYED price arrived with no "
                "timestamp, so its time to expiry is unknown. Not priced.</span>"));
            return;
        }
        const bool from_tick = spot_live_ && spot_ts_ns_ > 0;
        val_ns_ = from_tick ? spot_ts_ns_ : wall_ns;
        val_from_tick_ = from_tick;

        // TIME TO EXPIRY, to 15:30 IST on the expiry date. The master stores
        // IST midnight (instruments/kite_dump.hpp is explicit that reading it
        // as UTC would land 5h30m early, which on expiry day is the
        // difference between a live contract and a dead one).
        const double years =
            static_cast<double>(exp_ns + kExpiryClockNs - val_ns_)
            / (365.0 * 86'400.0 * 1e9);
        if (!(years > 0.0)) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>This expiry had settled by the "
                "price's timestamp. Nothing to value.</span>"));
            return;
        }
        dte_ = years * 365.0;

        // FORWARD, not spot: Black-76 prices off the future, which is what an
        // index option settles against. No dividend yield -- NIFTY pays one,
        // so this forward is slightly RICH and calls slightly overpriced.
        const double F = spot_paise_ * std::exp(kRate * years);

        // The strikes in view: the nearest-to-forward, +/- width.
        std::vector<const ChainRow*> all;
        for (const auto& [k, row] : it->second) { all.push_back(&row); }
        std::size_t atm = 0;
        double best = 1e300;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const double d = std::fabs(static_cast<double>(all[i]->strike) - F);
            if (d < best) { best = d; atm = i; }
        }
        const std::size_t w = static_cast<std::size_t>(width_->value());
        const std::size_t lo = atm > w ? atm - w : 0;
        const std::size_t hi = std::min(all.size(), atm + w + 1);

        // RULE 11: refuse a window wider than the SoA can hold, never
        // truncate it. width_ is capped at 60 so this is unreachable today,
        // and the check is what keeps it unreachable if the cap moves.
        if (hi - lo > kChainCap) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>%1 strikes requested; the chain "
                "holds %2. Narrow the window.</span>").arg(hi - lo).arg(kChainCap));
            return;
        }

        soa_.n = hi - lo;
        for (std::size_t i = lo; i < hi; ++i) {
            rows_.push_back(*all[i]);
            soa_.strike[i - lo] = static_cast<double>(all[i]->strike);
        }
        ChainInputs in;
        in.forward = F;
        in.years = years;
        in.vol = vol_;
        in.rate = kRate;
        const auto priced = price_chain(soa_, in);
        if (!priced) {
            head_->setText(QStringLiteral(
                "<span style='color:#E06C5B'>The chain could not be priced — "
                "bad input.</span>"));
            return;
        }

        paint(F, years, atm - lo);
    }

    void paint(double F, double years, std::size_t atm_row) {
        const QTimeZone ist(5 * 3600 + 30 * 60);
        const double dte = years * 365.0;
        head_->setText(QStringLiteral(
            "<b>%1</b>&nbsp; spot <b>%2</b> <span style='color:#8A93A2'>(%3)</span>"
            " &nbsp;·&nbsp; forward <b>%4</b> &nbsp;·&nbsp; vol <b>%5</b>"
            " &nbsp;·&nbsp; <b>%6</b> days to expiry"
            " <span style='color:#8A93A2'>(valued %8)</span>"
            " &nbsp;·&nbsp; rate %7%")
            .arg(under_->currentText())
            .arg(spot_paise_ / 100.0, 0, 'f', 2)
            .arg(spot_live_ ? (spot_replay_ ? QStringLiteral("REPLAY")
                                            : QStringLiteral("LIVE"))
                            : QStringLiteral("close ")
                                  + QDateTime::fromMSecsSinceEpoch(
                                        spot_ts_ns_ / 1'000'000LL, ist)
                                        .toString(QStringLiteral("dd-MMM")))
            .arg(F / 100.0, 0, 'f', 2)
            .arg(vol_src_.toHtmlEscaped())
            .arg(dte, 0, 'f', 2)
            .arg(100.0 * kRate, 0, 'f', 2)
            .arg(val_from_tick_
                     ? QStringLiteral("at tick ")
                           + QDateTime::fromMSecsSinceEpoch(
                                 val_ns_ / 1'000'000LL, ist)
                                 .toString(QStringLiteral("HH:mm dd-MMM"))
                     : QStringLiteral("now")));

        badge_->setText(QStringLiteral(
            "<span style='background:#6B4A12;color:#F4C95D;padding:2px 8px;"
            "font-weight:bold'>&nbsp;MODEL — NOT MARKET QUOTES&nbsp;</span>"));

        const QColor itm(QStringLiteral("#1F2A1F"));
        const QColor otm(QStringLiteral("#15181D"));
        const QColor atm_bg(QStringLiteral("#3A3212"));
        const QColor call_fg(QStringLiteral("#7FD17F"));
        const QColor put_fg(QStringLiteral("#F07A6A"));
        const QColor strike_fg(QStringLiteral("#FFFFFF"));

        grid_->setRowCount(static_cast<int>(soa_.n));
        for (std::size_t i = 0; i < soa_.n; ++i) {
            const int r = static_cast<int>(i);
            const bool at = i == atm_row;
            const bool call_itm = soa_.strike[i] < F;
            const bool put_itm = soa_.strike[i] > F;

            const auto put = [&](int col, const QString& text, const QColor& fg,
                                 const QColor& bg, bool bold = false) {
                auto* it = new QTableWidgetItem(text);
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                it->setForeground(QBrush(fg));
                it->setBackground(QBrush(at ? atm_bg : bg));
                if (bold || at) {
                    QFont f = it->font();
                    f.setBold(true);
                    it->setFont(f);
                }
                grid_->setItem(r, col, it);
            };

            const QColor cbg = call_itm ? itm : otm;
            const QColor pbg = put_itm ? itm : otm;
            // UNITS FOR A TRADER, from the raw paise-per-unit the kernel
            // returns: theta per DAY in rupees, vega per ONE VOL POINT in
            // rupees, gamma per ONE RUPEE of the underlying.
            put(kC_Theta, QString::number(soa_.call_theta[i] / 365.0 / 100.0, 'f', 2), call_fg, cbg);
            put(kC_Vega, QString::number(soa_.vega[i] * 0.01 / 100.0, 'f', 2), call_fg, cbg);
            put(kC_Gamma, QString::number(soa_.gamma[i] * 100.0, 'f', 5), call_fg, cbg);
            put(kC_Delta, QString::number(soa_.call_delta[i], 'f', 3), call_fg, cbg);
            put(kC_Iv, QString::number(100.0 * vol_, 'f', 1), QColor("#8A93A2"), cbg);
            put(kC_Px, rows_[i].has_ce ? QString::number(soa_.call_px[i] / 100.0, 'f', 2)
                                       : QStringLiteral("—"),
                call_fg, cbg, true);

            auto* sk = new QTableWidgetItem(
                QString::number(soa_.strike[i] / 100.0, 'f', 0));
            sk->setTextAlignment(Qt::AlignCenter);
            sk->setForeground(QBrush(strike_fg));
            sk->setBackground(QBrush(at ? atm_bg : QColor("#232830")));
            QFont sf = sk->font();
            sf.setBold(true);
            sk->setFont(sf);
            grid_->setItem(r, kStrike, sk);

            put(kP_Px, rows_[i].has_pe ? QString::number(soa_.put_px[i] / 100.0, 'f', 2)
                                       : QStringLiteral("—"),
                put_fg, pbg, true);
            put(kP_Iv, QString::number(100.0 * vol_, 'f', 1), QColor("#8A93A2"), pbg);
            put(kP_Delta, QString::number(soa_.put_delta[i], 'f', 3), put_fg, pbg);
            put(kP_Gamma, QString::number(soa_.gamma[i] * 100.0, 'f', 5), put_fg, pbg);
            put(kP_Vega, QString::number(soa_.vega[i] * 0.01 / 100.0, 'f', 2), put_fg, pbg);
            put(kP_Theta, QString::number(soa_.put_theta[i] / 365.0 / 100.0, 'f', 2), put_fg, pbg);
        }
        if (atm_row < soa_.n) {
            grid_->scrollToItem(grid_->item(static_cast<int>(atm_row), kStrike),
                                QAbstractItemView::PositionAtCenter);
        }
        foot_->setText(QStringLiteral(
            "%1 strikes · lot %2 · tick %3 paise · exchange %4 · "
            "click a CALL or PUT cell to load that contract into the Order "
            "ticket (F1 buy, F2 sell). OI, bid/ask and a real smile appear "
            "when a live option subscription exists.")
            .arg(soa_.n)
            .arg(rows_.empty() ? 0 : rows_.front().lot)
            .arg(rows_.empty() ? 0 : rows_.front().tick)
            .arg(rows_.empty() ? QStringLiteral("?") : rows_.front().exchange));
    }

    void on_click(int row, int col) {
        if (row < 0 || row >= static_cast<int>(rows_.size())) { return; }
        const ChainRow& r = rows_[static_cast<std::size_t>(row)];
        const bool call = col < kStrike;
        const bool put = col > kStrike;
        if (!call && !put) { return; }
        const std::uint32_t tok = call ? r.ce_token : r.pe_token;
        const std::string& sym = call ? r.ce_symbol : r.pe_symbol;
        if (tok == 0 || r.lot <= 0 || r.tick <= 0 || r.exchange.isEmpty()) {
            return;   // nothing listed on this side, or no spec: do not guess
        }
        Q_EMIT contractPicked(tok, QString::fromStdString(sym), r.lot, r.tick,
                              r.exchange);
    }

    /// 15:30 IST after the midnight the master stores. NSE index options
    /// expire at the close.
    static constexpr std::int64_t kExpiryClockNs =
        (15LL * 3600LL + 30LL * 60LL) * 1'000'000'000LL;
    /// The risk-free rate the model uses, SHOWN in the header rather than
    /// buried. Near the RBI repo rate; a percent either way moves a
    /// short-dated option's price by very little and its rho by exactly that.
    static constexpr double kRate = 0.065;

    QComboBox* under_ = nullptr;
    QComboBox* expiry_ = nullptr;
    QSpinBox* width_ = nullptr;
    QLabel* badge_ = nullptr;
    QLabel* head_ = nullptr;
    QLabel* foot_ = nullptr;
    QTableWidget* grid_ = nullptr;

    Chain chain_;
    ChainSoA soa_;                     ///< 18 KB, a member: no heap on reprice
    std::vector<ChainRow> rows_;
    double spot_paise_ = 0.0;
    std::int64_t spot_ts_ns_ = 0;
    bool spot_live_ = false;
    bool spot_replay_ = false;
    std::int64_t val_ns_ = 0;          ///< the instant the chain is valued at
    bool val_from_tick_ = false;       ///< ... and whether a tick supplied it
    double dte_ = 0.0;
    unsigned spot_token_ = 0;
    double vol_ = 0.0;
    QString vol_src_;
};

}  // namespace altair::ui
