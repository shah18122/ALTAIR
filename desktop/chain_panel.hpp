// desktop/chain_panel.hpp -- the option chain, and the half of the butterfly
// that data cannot supply.
//
// P11Q-05d.
//
// The Ratio Spread page's blocker said: "wiring it needs a live option chain --
// an instrument set, a spot, and quotes for each strike. The demo generator
// emits four instruments and one strike, so there is no chain to price yet."
//
// P2-12e removed the first of those three. `data/instruments.csv` now holds
// 108,411 real contracts, and this page reads the chain out of it THROUGH THE
// REAL PARSER -- `instruments/kite_dump.hpp`, sink-templated, so the strike,
// the lot size, the tick size and the expiry arrive as a `ContractSpec` from
// the same code the engine uses. Rule 1 is satisfied by construction: nothing
// on this page is a literal, and the reason that matters is in ROADMAP §6 --
// NSE has revised lot sizes mid-series before.
//
// AND THE SECOND HALF STILL CANNOT BE SUPPLIED, FOR A REASON WORTH STATING.
//
// `strategies/parity.hpp::butterfly_margin` takes three `Touch`es, and a
// `Touch` is a BID AND AN ASK. It is computed "at the touch: buy the wings at
// the ask, sell the body at the bid". A historical candle carries a close,
// which is neither of those -- it is one trade, at one instant, on whichever
// side happened to lift.
//
// So the chain COULD be priced from the historical API, and it would be the
// same error P11Q-06 measured when it put bar closes through a tick pipeline
// and lost 37.8% of the range. The margin would come out plausible and mean
// nothing. What fills this is a live full-mode subscription (P2-02), not more
// history.
//
// WHAT THE CHAIN DOES SETTLE TODAY: WHETHER THE LADDER IS EVEN.
//
// `butterfly_margin` is spacing-weighted, and its header says "nothing here
// assumes a step". That is a defensive statement nobody could check without a
// real ladder. Now it can be checked, and the page shows the gaps.

#pragma once

#include <instruments/kite_dump.hpp>

#include "format.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <fstream>
#include "data/master_lookup.hpp"

#include <map>
#include <string>
#include <vector>

namespace altair::ui {

/// One strike's pair, as read from the master.
struct ChainRow {
    std::int64_t strike = 0;
    std::int64_t lot = 0;
    std::int64_t tick = 0;
    bool has_ce = false;
    bool has_pe = false;
    /// The broker tokens and trading symbols, so a click on a chain row can
    /// load the REAL contract into the order ticket. Read from the master by
    /// the same parser the engine uses -- never composed from strike and
    /// expiry, which is how a ticket ends up naming a contract that does not
    /// exist.
    std::uint32_t ce_token = 0;
    std::uint32_t pe_token = 0;
    std::string ce_symbol;
    std::string pe_symbol;
    /// NFO or BFO, from the spec. Never inferred from the symbol.
    QString exchange;
};

/// The chain for one underlying, grouped by expiry.
struct Chain {
    bool loaded = false;
    QString error;
    std::size_t contracts = 0;
    /// expiry (ns) -> strike -> row
    std::map<std::int64_t, std::map<std::int64_t, ChainRow>> by_expiry;
    /// Expiries whose strike ladder is NOT uniformly spaced.
    std::size_t uneven_expiries = 0;
    std::size_t expiries_seen = 0;
};

/// Read one underlying's options out of the Kite master.
///
/// Uses `detail::load_kite_dump_into` with a FILTERING SINK rather than a
/// SpecStore: the store is sized for a trading universe and this is 108,411
/// rows of which a few thousand are wanted. The sink is the documented
/// extension point (the P1-04 carried-debt note names it), so this is not a
/// second parser -- the strike, lot, tick and expiry below all come out of the
/// same code the engine loads specs with.
[[nodiscard]] inline Chain load_chain(const QString& path,
                                      const char* underlying) {
    Chain c;
    std::ifstream f(path.toStdString(), std::ios::binary);
    if (!f) {
        c.error = QStringLiteral("cannot open %1").arg(path);
        return c;
    }
    const std::string csv((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (csv.empty()) {
        c.error = QStringLiteral("%1 is empty").arg(path);
        return c;
    }

    const auto sink = [&c, underlying](const ContractSpec& s) noexcept {
        if (s.opt_type == OptionType::None) {
            return true;                       // not an option; keep going
        }
        if (std::strcmp(s.underlying, underlying) != 0) {
            return true;
        }
        auto& strikes = c.by_expiry[s.expiry.ns_since_epoch()];
        ChainRow& r = strikes[s.strike.raw()];
        r.strike = s.strike.raw();
        r.lot = s.lot_size.raw();
        r.tick = s.tick_size.raw();
        r.exchange = kite_exchange_name(s.exchange, s.segment);
        const std::uint32_t tok =
            s.token[static_cast<std::size_t>(FeedSource::Kite)];
        if (s.opt_type == OptionType::CE) {
            r.has_ce = true;
            r.ce_token = tok;
            r.ce_symbol = s.symbol;
        }
        if (s.opt_type == OptionType::PE) {
            r.has_pe = true;
            r.pe_token = tok;
            r.pe_symbol = s.symbol;
        }
        ++c.contracts;
        return true;
    };

    const auto rep = detail::load_kite_dump_into(csv.data(), csv.size(), sink,
                                                 Timestamp{0});
    if (!rep) {
        c.error = QStringLiteral("the master did not parse");
        return c;
    }
    c.loaded = true;
    // How many expiries have a NON-UNIFORM ladder. COMPUTED, because
    // `butterfly_margin` being spacing-weighted is only worth the complexity
    // if real ladders are actually uneven, and asserting that without counting
    // is the kind of claim this project keeps catching.
    for (const auto& [exp, strikes] : c.by_expiry) {
        std::int64_t first = 0, prev = 0;
        bool uneven = false, have = false;
        for (const auto& [k, row] : strikes) {
            if (have) {
                const std::int64_t g = k - prev;
                if (first == 0) { first = g; }
                else if (g != first) { uneven = true; }
            }
            prev = k;
            have = true;
        }
        ++c.expiries_seen;
        if (uneven) { ++c.uneven_expiries; }
    }
    return c;
}

class ChainPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ChainPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel(
            QStringLiteral("<h3>Ratio spread — the chain, and the half that is "
                           "missing</h3>"), this));

        head_ = new QLabel(this);
        head_->setWordWrap(true);
        v->addWidget(head_);

        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Expiry"), this));
        expiry_ = new QComboBox(this);
        row->addWidget(expiry_);
        row->addStretch();
        v->addLayout(row);

        t_ = new QTableWidget(0, 5, this);
        t_->setHorizontalHeaderLabels({QStringLiteral("Strike"),
                                       QStringLiteral("CE"),
                                       QStringLiteral("PE"),
                                       QStringLiteral("Lot"),
                                       QStringLiteral("Tick")});
        t_->verticalHeader()->setVisible(false);
        t_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t_->setAlternatingRowColors(true);
        t_->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(t_, 1);

        foot_ = new QLabel(this);
        foot_->setWordWrap(true);
        v->addWidget(foot_);

        chain_ = load_chain(QStringLiteral(ALTAIR_SOURCE_DIR
                                           "/data/instruments.csv"),
                            "NIFTY");
        connect(expiry_, &QComboBox::currentIndexChanged, this,
                [this](int) { show_expiry(); });
        populate();
    }

private:
    void populate() {
        if (!chain_.loaded) {
            head_->setText(QStringLiteral(
                "<b style='color:#B9770B'>No instrument master.</b> %1<br>"
                "Fetch it with <code>altair_kite_fetch "
                "--dump-instruments data/instruments.csv</code>.")
                    .arg(chain_.error));
            return;
        }
        head_->setText(QStringLiteral(
            "<b>%1 NIFTY option contracts</b> across %2 expiries, read from "
            "<code>data/instruments.csv</code> through "
            "<code>instruments/kite_dump.hpp</code> — the same parser the "
            "engine loads specs with. Every strike, lot size and tick below "
            "comes from the master; none is a literal (rule 1, and ROADMAP §6 "
            "says why: NSE has revised lot sizes mid-series).")
                .arg(chain_.contracts).arg(chain_.by_expiry.size()));

        for (const auto& [exp, strikes] : chain_.by_expiry) {
            const QDateTime d = QDateTime::fromMSecsSinceEpoch(
                exp / 1'000'000, QTimeZone::utc());
            expiry_->addItem(
                QStringLiteral("%1  (%2 strikes)")
                    .arg(d.toString(QStringLiteral("yyyy-MM-dd")))
                    .arg(strikes.size()),
                QVariant::fromValue(exp));
        }
        show_expiry();
    }

    void show_expiry() {
        if (!chain_.loaded || expiry_->currentIndex() < 0) {
            return;
        }
        const auto exp = expiry_->currentData().toLongLong();
        const auto it = chain_.by_expiry.find(exp);
        if (it == chain_.by_expiry.end()) {
            return;
        }
        const auto& strikes = it->second;

        t_->setRowCount(static_cast<int>(strikes.size()));
        int r = 0;
        std::vector<std::int64_t> ks;
        ks.reserve(strikes.size());
        for (const auto& [k, row] : strikes) {
            ks.push_back(k);
            auto put = [&](int col, const QString& s) {
                auto* i = new QTableWidgetItem(s);
                i->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                t_->setItem(r, col, i);
            };
            put(0, format_paise(row.strike));
            put(1, row.has_ce ? QStringLiteral("yes") : QStringLiteral("—"));
            put(2, row.has_pe ? QStringLiteral("yes") : QStringLiteral("—"));
            put(3, QString::number(row.lot));
            put(4, format_paise(row.tick));
            ++r;
        }
        t_->resizeColumnsToContents();

        // IS THE LADDER EVEN? `butterfly_margin` is spacing-weighted and its
        // header says "nothing here assumes a step" -- a defensive claim
        // nobody could check without a real ladder. Now it can be.
        std::size_t gaps = 0, uneven = 0;
        std::int64_t first_gap = 0;
        for (std::size_t i = 1; i < ks.size(); ++i) {
            const std::int64_t g = ks[i] - ks[i - 1];
            if (gaps == 0) { first_gap = g; }
            ++gaps;
            if (g != first_gap) { ++uneven; }
        }
        QString f;
        if (gaps > 0) {
            f += uneven == 0
                     ? QStringLiteral(
                           "The ladder is EVENLY spaced here — every gap is "
                           "%1. The weighting still earns its place: <b>%2 of "
                           "%3 expiries in this chain are NOT uniform</b>, and "
                           "the unweighted textbook form is wrong on every "
                           "one of them.")
                           .arg(format_paise(first_gap))
                           .arg(chain_.uneven_expiries)
                           .arg(chain_.expiries_seen)
                     : QStringLiteral(
                           "<b>The ladder is UNEVEN</b> — %1 of %2 gaps differ "
                           "from the first (%3). This is exactly why "
                           "<code>butterfly_margin</code> is spacing-weighted; "
                           "the unweighted textbook form is wrong on a ladder "
                           "like this.")
                           .arg(uneven).arg(gaps).arg(format_paise(first_gap));
        }
        f += QStringLiteral(
            "<br><br><b>No margin is computed, and it is not for want of a "
            "chain.</b> <code>butterfly_margin</code> takes three "
            "<code>Touch</code>es, and a Touch is a BID AND AN ASK — it prices "
            "\"buy the wings at the ask, sell the body at the bid\". A "
            "historical candle carries a CLOSE, which is neither: it is one "
            "trade, at one instant, on whichever side happened to lift. "
            "Pricing this chain from the historical API would be the same "
            "error P11Q-06 measured when bar closes went through a tick "
            "pipeline and 37.8% of the range vanished — the number would come "
            "out plausible and mean nothing. A live full-mode subscription "
            "(P2-02) is what fills this, not more history.");
        foot_->setText(f);
    }

    Chain chain_;
    QLabel* head_ = nullptr;
    QComboBox* expiry_ = nullptr;
    QTableWidget* t_ = nullptr;
    QLabel* foot_ = nullptr;
};

} // namespace altair::ui
