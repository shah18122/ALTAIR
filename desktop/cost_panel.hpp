// desktop/cost_panel.hpp -- what a round trip actually costs.
//
// P11Q-05a.
//
// RULE 5 IS THE PROJECT'S CENTRAL DISCIPLINE AND IT HAD NO WINDOW.
//
// "Every signal is priced net of full cost before it exists. No strategy sees
// a pre-cost number." The calculator that enforces it has been correct since
// P3-09 and invisible ever since -- and a discipline nobody can see is one
// that gets argued with. This panel prices a trade end to end and shows the
// itemisation, because a cost you cannot decompose is a cost you cannot
// dispute.
//
// EVERY RATE COMES FROM config/charges.toml. NONE IS A LITERAL.
//
// Rule 1's reasoning applied to charges: a hard-coded rate in the UI is a
// number that disagrees with the engine the moment a circular changes, and it
// disagrees silently. P3-09b made the file loadable; this is its first
// consumer. Where the loader is not compiled in -- the default preset has no
// toml++ -- the panel says so and shows NOTHING, because an invented charge is
// worse than no charge when rule 5 is what everything downstream trusts.
//
// AND IT SHOWS THE SAME TRADE UNDER BOTH SCHEDULES.
//
// CLAUDE.md: "STT rose on 2026-04-01 (futures 0.02 -> 0.05%, options 0.10 ->
// 0.15% sell-side premium). Every pre-April backtest is optimistic until
// re-run." That sentence is a warning nobody acts on until they see the two
// numbers side by side, so the panel puts them there and prints the
// difference. The schedules are chosen by the TRADE DATE, never by today's --
// D6, and the reason the backtester can reach March at all.
//
// BPS OF WHAT, EXACTLY.
//
// The cost is shown against PREMIUM turnover for options and NOTIONAL for
// everything else, because that is what the charge is levied on. Quoting an
// option's cost as bps of notional would divide by the strike and make an
// 82 bps round trip look like 0.16 bps -- a factor of 500 in the direction
// that makes every options strategy look viable. The basis is printed next to
// the number for exactly that reason.

#pragma once

#include <risk/cost.hpp>

#if ALTAIR_HAVE_CHARGES_TOML
#include <risk/charges_toml.hpp>
#endif

#include <QComboBox>
#include <QDateEdit>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace altair::ui {

/// Days from 1970-01-01, proleptic Gregorian. Same closed form as the loader's.
[[nodiscard]] constexpr std::int64_t cost_days_from_civil(std::int64_t y,
                                                          unsigned m,
                                                          unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

class CostPanel final : public QWidget {
    Q_OBJECT

public:
    explicit CostPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);

        auto* head = new QLabel(
            QStringLiteral("<h3>Cost — what a round trip actually costs</h3>"),
            this);
        v->addWidget(head);

        note_ = new QLabel(this);
        note_->setWordWrap(true);
        note_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note_);

        auto* form = new QHBoxLayout;
        segment_ = new QComboBox(this);
        segment_->addItem(QStringLiteral("Options"),
                          static_cast<int>(Segment::Opt));
        segment_->addItem(QStringLiteral("Futures"),
                          static_cast<int>(Segment::Fut));
        segment_->addItem(QStringLiteral("Cash intraday"),
                          static_cast<int>(Segment::Cash));
        form->addWidget(new QLabel(QStringLiteral("Segment"), this));
        form->addWidget(segment_);

        qty_ = new QSpinBox(this);
        qty_->setRange(1, 1'000'000);
        qty_->setValue(75);
        form->addWidget(new QLabel(QStringLiteral("Qty"), this));
        form->addWidget(qty_);

        // Rupees in, paise everywhere after. The only place a rupee figure is
        // handled is this box and the formatter -- rule 3.
        price_ = new QSpinBox(this);
        price_->setRange(1, 100'000'000);
        price_->setValue(50);
        price_->setPrefix(QStringLiteral("Rs "));
        form->addWidget(new QLabel(QStringLiteral("Price"), this));
        form->addWidget(price_);
        form->addStretch();
        v->addLayout(form);

        out_ = new QPlainTextEdit(this);
        out_->setReadOnly(true);
        out_->setStyleSheet(QStringLiteral(
            "background:#12161A;color:#D6DBDF;font-family:Consolas,monospace;"));
        v->addWidget(out_, 1);

        connect(segment_, &QComboBox::currentIndexChanged, this,
                [this](int) { recompute(); });
        connect(qty_, &QSpinBox::valueChanged, this,
                [this](int) { recompute(); });
        connect(price_, &QSpinBox::valueChanged, this,
                [this](int) { recompute(); });

        load();
        recompute();
    }

private:
    void load() {
#if ALTAIR_HAVE_CHARGES_TOML
        const auto rep = load_charges_file(ALTAIR_CHARGES_TOML, schedules_);
        if (!rep) {
            note_->setText(
                QStringLiteral("<span style='color:#C0392B'>config/charges.toml "
                               "did not load: %1</span>")
                    .arg(QString::fromLatin1(charges_error_text(rep.error()))));
            return;
        }
        loaded_ = true;
        verified_ = rep->verified;
        note_->setText(QStringLiteral(
            "%1 effective-dated schedules from <code>config/charges.toml</code>."
            " Every rate below is read from that file; none is a literal. "
            "<b style='color:%2'>last_verified: %3</b> — %4")
            .arg(rep->schedules)
            .arg(verified_ ? QStringLiteral("#1B8A4B")
                           : QStringLiteral("#C0392B"))
            .arg(verified_ ? QStringLiteral("set") : QStringLiteral("UNVERIFIED"))
            .arg(verified_
                     ? QStringLiteral("checked against a circular.")
                     : QStringLiteral("nothing here has been checked against "
                                      "an NSE circular or a broker schedule. "
                                      "A 3 bps error turns a profitable "
                                      "arbitrage into a losing one.")));
#else
        note_->setText(QStringLiteral(
            "<span style='color:#B9770B'>The charge-schedule loader is not in "
            "this build.</span> P3-09b's reader needs <code>toml++</code>, "
            "which only the <code>vcpkg</code> preset provides. Rather than "
            "show a plausible cost from a hard-coded rate, this panel shows "
            "nothing: rule 5 is what every strategy downstream trusts, and an "
            "invented charge is worse than no charge. Configure with "
            "<code>cmake --preset vcpkg</code>."));
#endif
    }

    /// A trade at 09:15 on the given date.
    [[nodiscard]] static Timestamp at_0915(std::int64_t y, unsigned m,
                                           unsigned d) noexcept {
        return Timestamp{cost_days_from_civil(y, m, d) * 86'400'000'000'000LL
                         + (9 * 3600LL + 15 * 60LL) * 1'000'000'000LL};
    }

    void recompute() {
        if (!loaded_ || schedules_.empty()) {
            out_->setPlainText(QString());
            return;
        }
        const auto seg = static_cast<Segment>(segment_->currentData().toInt());

        Trade t{};
        t.segment = seg;
        t.exchange = Exchange::NSE;
        t.qty = Qty{qty_->value()};
        t.price = Price{static_cast<std::int64_t>(price_->value()) * 100};
        t.delivery = false;

        // BROKERAGE IS COMMERCIAL, NOT REGULATORY, AND IS NOT IN charges.toml.
        //
        // `BrokerageRule`'s own comment says why: it changes without a
        // circular, so it does not belong in an effective-dated regulatory
        // schedule. That makes it the ONE set of numbers on this panel that is
        // a literal, and the header line says which is which rather than
        // letting a reader assume everything came from the file.
        //
        // AND THE FIRST VERSION WAS WRONG IN THE FLATTERING DIRECTION. It
        // applied "Rs 20 or 0.03%, whichever is LOWER" to options. That is the
        // intraday-equity and futures term; OPTIONS are a flat Rs 20 per
        // order. On Rs 3,750 of premium the lower rule gives Rs 1.13 against a
        // real Rs 20, so the round trip read Rs 9.66 / 25.8 bps when it is
        // Rs 47.40 / 126.4 bps -- out by a factor of five, in the direction
        // that makes an options strategy look viable. Caught on screen.
        BrokerageRule br{};
        br.flat_per_order = Notional{2'000};   // Rs 20
        if (seg == Segment::Opt) {
            br.pct = 0;                        // flat, no percentage variant
            br.take_lower = false;
        } else {
            br.pct = rate_from(0.0003L);       // or 0.03%
            br.take_lower = true;
        }

        QString o;
        o += QStringLiteral(
            "THE SAME TRADE UNDER BOTH SCHEDULES, chosen by TRADE DATE (D6)\n"
            "%1 x Rs %2, NSE\n"
            "brokerage %3  <- COMMERCIAL, a literal here; every other rate "
            "below comes from charges.toml\n\n")
                 .arg(qty_->value()).arg(price_->value())
                 .arg(seg == Segment::Opt
                          ? QStringLiteral("flat Rs 20/order (options)")
                          : QStringLiteral("Rs 20 or 0.03%, whichever lower"));

        // Two dates that straddle the 2026-04-01 STT rise. Fixed, because the
        // point is the comparison, not a date picker.
        struct When { const char* label; std::int64_t y; unsigned m, d; };
        const When whens[] = {{"2026-03-31  (pre-Budget STT)", 2026, 3, 31},
                              {"2026-06-01  (post-Budget STT)", 2026, 6, 1}};

        std::int64_t round_trip[2] = {0, 0};
        std::int64_t turnover[2] = {0, 0};
        int n = 0;

        for (const When& w : whens) {
            const Timestamp ts = at_0915(w.y, w.m, w.d);
            const ChargeSchedule* s =
                schedule_for(schedules_.data(), schedules_.size(), ts);
            if (s == nullptr) {
                o += QStringLiteral("  %1  NO SCHEDULE COVERS THIS DATE\n")
                         .arg(QString::fromLatin1(w.label));
                ++n;
                continue;
            }
            Trade buy = t;  buy.side = Side::Buy;  buy.trade_ts = ts;
            Trade sell = t; sell.side = Side::Sell; sell.trade_ts = ts;
            const auto cb = compute_cost(buy, *s, br);
            const auto cs = compute_cost(sell, *s, br);
            if (!cb || !cs) {
                o += QStringLiteral("  %1  refused: no charge line for this "
                                    "segment\n")
                         .arg(QString::fromLatin1(w.label));
                ++n;
                continue;
            }
            o += QStringLiteral("  %1\n").arg(QString::fromLatin1(w.label));
            o += QStringLiteral("                    BUY        SELL\n");
            o += line(QStringLiteral("brokerage"), cb->brokerage, cs->brokerage);
            o += line(QStringLiteral("STT/CTT"), cb->stt, cs->stt);
            o += line(QStringLiteral("exchange"), cb->exchange_txn,
                      cs->exchange_txn);
            o += line(QStringLiteral("SEBI"), cb->sebi, cs->sebi);
            o += line(QStringLiteral("stamp"), cb->stamp, cs->stamp);
            o += line(QStringLiteral("IPFT"), cb->ipft, cs->ipft);
            o += line(QStringLiteral("GST"), cb->gst, cs->gst);
            o += line(QStringLiteral("DP"), cb->dp, cs->dp);
            o += QStringLiteral("    %1  %2  %3\n")
                     .arg(QStringLiteral("TOTAL"), -16)
                     .arg(rupees(cb->total), 10)
                     .arg(rupees(cs->total), 10);

            const std::int64_t rt = cb->total.raw() + cs->total.raw();
            round_trip[n] = rt;
            turnover[n] = cb->turnover.raw();
            const double bps = cb->turnover.raw() > 0
                                   ? 10'000.0 * static_cast<double>(rt)
                                         / static_cast<double>(
                                               cb->turnover.raw())
                                   : 0.0;
            o += QStringLiteral(
                     "    ROUND TRIP        %1   = %2 bps of %3 turnover "
                     "(Rs %4)\n\n")
                     .arg(rupees(Notional{rt}), 10)
                     .arg(bps, 0, 'f', 1)
                     .arg(cb->basis == TurnoverBasis::Premium
                              ? QStringLiteral("PREMIUM")
                              : QStringLiteral("notional"))
                     .arg(static_cast<double>(cb->turnover.raw()) / 100.0, 0,
                          'f', 2);
            ++n;
        }

        if (round_trip[0] > 0 && round_trip[1] > 0) {
            const std::int64_t d = round_trip[1] - round_trip[0];
            const double pct = 100.0 * static_cast<double>(d)
                             / static_cast<double>(round_trip[0]);
            o += QStringLiteral(
                     "  THE 2026-04-01 RISE COSTS THIS TRADE %1 MORE PER ROUND "
                     "TRIP, %2%.\n")
                     .arg(rupees(Notional{d})).arg(pct, 0, 'f', 1);
            o += QStringLiteral(
                "  CLAUDE.md: \"every pre-April backtest is optimistic until "
                "re-run\".\n  That is this number, per round trip, compounded "
                "over every trade a backtest took.\n");
        }

        if (!verified_) {
            o += QStringLiteral(
                "\n  UNVERIFIED. charges.toml has never been checked against a "
                "circular, so\n  every figure above is what the file says, not "
                "what the exchange charges.\n  CostBreakdown carries that flag "
                "through to anything that reads it.");
        }
        out_->setPlainText(o);
    }

    [[nodiscard]] static QString rupees(Notional n) {
        return QStringLiteral("%1").arg(
            static_cast<double>(n.raw()) / 100.0, 0, 'f', 2);
    }
    [[nodiscard]] static QString line(const QString& name, Notional b,
                                      Notional s) {
        // A zero here is a real zero -- the charge does not apply on that side
        // -- not an absent value, so it prints as 0.00 rather than blank. That
        // is the opposite of the tick grid's rule, and deliberately: there,
        // absence means "no tick yet"; here, every component was computed.
        return QStringLiteral("    %1  %2  %3\n")
            .arg(name, -16).arg(rupees(b), 10).arg(rupees(s), 10);
    }

    QLabel* note_ = nullptr;
    QComboBox* segment_ = nullptr;
    QSpinBox* qty_ = nullptr;
    QSpinBox* price_ = nullptr;
    QPlainTextEdit* out_ = nullptr;
    std::vector<ChargeSchedule> schedules_;
    bool loaded_ = false;
    bool verified_ = false;
};

} // namespace altair::ui
