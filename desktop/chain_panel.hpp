// desktop/chain_panel.hpp -- the option chain, and the half of the butterfly
// that data cannot supply.
//
// P11Q-05d.
//
// The former Ratio Spread page's blocker said: "wiring it needs a live option chain --
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
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeZone>
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
            QStringLiteral("<h3>Pair Trading — choose two instruments</h3>"),
            this));

        auto* pair = new QHBoxLayout;
        pair->addWidget(new QLabel(QStringLiteral("Instrument A"), this));
        instrument_a_ = new QComboBox(this);
        add_pair_candidates(instrument_a_);
        pair->addWidget(instrument_a_, 1);
        pair->addWidget(new QLabel(QStringLiteral("Instrument B"), this));
        instrument_b_ = new QComboBox(this);
        add_pair_candidates(instrument_b_);
        instrument_b_->setCurrentIndex(1);
        pair->addWidget(instrument_b_, 1);
        swap_ = new QPushButton(QStringLiteral("Swap"), this);
        pair->addWidget(swap_);
        v->addLayout(pair);

        pair_note_ = new QLabel(this);
        pair_note_->setWordWrap(true);
        v->addWidget(pair_note_);

        auto* legs = new QHBoxLayout;
        side_a_ = new QComboBox(this);
        side_a_->setObjectName(QStringLiteral("pairSideA"));
        side_a_->addItems({QStringLiteral("BUY"), QStringLiteral("SELL")});
        ratio_a_ = new QSpinBox(this);
        ratio_a_->setObjectName(QStringLiteral("pairRatioA"));
        ratio_a_->setRange(1, 1000);
        ratio_a_->setValue(2);
        side_b_ = new QComboBox(this);
        side_b_->setObjectName(QStringLiteral("pairSideB"));
        side_b_->addItems({QStringLiteral("BUY"), QStringLiteral("SELL")});
        side_b_->setCurrentText(QStringLiteral("SELL"));
        ratio_b_ = new QSpinBox(this);
        ratio_b_->setObjectName(QStringLiteral("pairRatioB"));
        ratio_b_->setRange(1, 1000);
        ratio_b_->setValue(1);
        legs->addWidget(new QLabel(QStringLiteral("Leg A"), this));
        legs->addWidget(side_a_);
        legs->addWidget(ratio_a_);
        legs->addSpacing(18);
        legs->addWidget(new QLabel(QStringLiteral("Leg B"), this));
        legs->addWidget(side_b_);
        legs->addWidget(ratio_b_);
        legs->addStretch();
        v->addLayout(legs);

        auto* definition = new QLabel(QStringLiteral(
            "This is an instrument pair, not an option strategy. Quantities are "
            "relative units: the default means buy 2 NIFTY and sell 1 BANKNIFTY. "
            "Contract multipliers, hedge estimation, entry and exit rules remain "
            "separate validation steps."), this);
        definition->setWordWrap(true);
        definition->setStyleSheet(QStringLiteral(
            "background:#162027;color:#B8C5CC;padding:10px;border:1px solid #263842;"));
        v->addWidget(definition);
        v->addStretch(1);

        chain_path_ = QStringLiteral(ALTAIR_SOURCE_DIR "/data/instruments.csv");
        connect(instrument_a_, &QComboBox::currentIndexChanged, this,
                [this](int) { pair_changed(instrument_a_); });
        connect(instrument_b_, &QComboBox::currentIndexChanged, this,
                [this](int) { pair_changed(instrument_b_); });
        connect(swap_, &QPushButton::clicked, this, [this] {
            const QSignalBlocker a(instrument_a_);
            const QSignalBlocker b(instrument_b_);
            const int old_a = instrument_a_->currentIndex();
            instrument_a_->setCurrentIndex(instrument_b_->currentIndex());
            instrument_b_->setCurrentIndex(old_a);
            const QSignalBlocker sa(side_a_), sb(side_b_), ra(ratio_a_), rb(ratio_b_);
            const int old_side = side_a_->currentIndex();
            const int old_ratio = ratio_a_->value();
            side_a_->setCurrentIndex(side_b_->currentIndex());
            side_b_->setCurrentIndex(old_side);
            ratio_a_->setValue(ratio_b_->value());
            ratio_b_->setValue(old_ratio);
            update_pair_note();
        });
        connect(side_a_, &QComboBox::currentIndexChanged, this,
                [this](int) { update_pair_note(); });
        connect(side_b_, &QComboBox::currentIndexChanged, this,
                [this](int) { update_pair_note(); });
        connect(ratio_a_, &QSpinBox::valueChanged, this,
                [this](int) { update_pair_note(); });
        connect(ratio_b_, &QSpinBox::valueChanged, this,
                [this](int) { update_pair_note(); });
        update_pair_note();
    }

    [[nodiscard]] QString instrument_a() const {
        return instrument_a_ != nullptr ? instrument_a_->currentData().toString()
                                        : QString{};
    }
    [[nodiscard]] QString instrument_b() const {
        return instrument_b_ != nullptr ? instrument_b_->currentData().toString()
                                        : QString{};
    }
    [[nodiscard]] QString pair_status() const {
        return pair_note_ != nullptr ? pair_note_->text() : QString{};
    }
    [[nodiscard]] int ratio_a() const noexcept { return ratio_a_->value(); }
    [[nodiscard]] int ratio_b() const noexcept { return ratio_b_->value(); }
    [[nodiscard]] QString side_a() const { return side_a_->currentText(); }
    [[nodiscard]] QString side_b() const { return side_b_->currentText(); }

    /// Set one leg's observed price. `ts_ns` is UTC nanoseconds from the source
    /// event, never a wall-clock substitute. A zero timestamp is refused and
    /// leaves the previous leg state untouched. No value is forward-filled.
    bool set_pair_price(const QString& leg, std::int64_t price_paise,
                        bool replay, std::int64_t ts_ns) {
        if (price_paise <= 0 || ts_ns <= 0) return false;
        PairLegState* target = leg == QStringLiteral("A") ? &leg_a_ :
            leg == QStringLiteral("B") ? &leg_b_ : nullptr;
        if (target == nullptr) return false;
        target->price_paise = price_paise;
        target->timestamp_ns = ts_ns;
        target->source = replay ? PairSource::Replay : PairSource::Live;
        update_pair_note();
        return true;
    }

    /// Persist only canonical A/B identities. Price observations are session
    /// data and are intentionally not persisted as if they were current.
    void save_pair(QSettings& settings) const {
        settings.setValue(QStringLiteral("pairTrading/instrumentA"), instrument_a());
        settings.setValue(QStringLiteral("pairTrading/instrumentB"), instrument_b());
        settings.setValue(QStringLiteral("pairTrading/sideA"), side_a());
        settings.setValue(QStringLiteral("pairTrading/sideB"), side_b());
        settings.setValue(QStringLiteral("pairTrading/ratioA"), ratio_a());
        settings.setValue(QStringLiteral("pairTrading/ratioB"), ratio_b());
    }

    /// Restore canonical identities, refusing missing/unknown/duplicate values.
    /// Returns true only when both legs were applied.
    bool restore_pair(QSettings& settings) {
        const QString a = settings.value(QStringLiteral("pairTrading/instrumentA"))
                              .toString().trimmed().toUpper();
        const QString b = settings.value(QStringLiteral("pairTrading/instrumentB"))
                              .toString().trimmed().toUpper();
        const int ai = instrument_a_->findData(a);
        const int bi = instrument_b_->findData(b);
        if (ai < 0 || bi < 0 || a == b) return false;
        const QSignalBlocker block_a(instrument_a_);
        const QSignalBlocker block_b(instrument_b_);
        instrument_a_->setCurrentIndex(ai);
        instrument_b_->setCurrentIndex(bi);
        side_a_->setCurrentText(settings.value(QStringLiteral("pairTrading/sideA"),
                                               QStringLiteral("BUY")).toString());
        side_b_->setCurrentText(settings.value(QStringLiteral("pairTrading/sideB"),
                                               QStringLiteral("SELL")).toString());
        ratio_a_->setValue(settings.value(QStringLiteral("pairTrading/ratioA"), 2).toInt());
        ratio_b_->setValue(settings.value(QStringLiteral("pairTrading/ratioB"), 1).toInt());
        update_pair_note();
        return true;
    }

private:
    enum class PairSource : std::uint8_t { Unknown, Replay, Live };
    struct PairLegState {
        std::int64_t price_paise{};
        std::int64_t timestamp_ns{};
        PairSource source{PairSource::Unknown};
    };

    [[nodiscard]] static QString source_text(PairSource source) {
        switch (source) {
        case PairSource::Replay: return QStringLiteral("REPLAY");
        case PairSource::Live: return QStringLiteral("LIVE");
        case PairSource::Unknown: return QStringLiteral("MISSING");
        }
        return QStringLiteral("MISSING");
    }

    [[nodiscard]] QString aligned_data_status() const {
        if (leg_a_.source == PairSource::Unknown || leg_b_.source == PairSource::Unknown)
            return QStringLiteral("Awaiting both legs — no forward-fill is applied.");
        if (leg_a_.source != leg_b_.source)
            return QStringLiteral("MIXED SOURCE — A %1, B %2; comparison refused.")
                .arg(source_text(leg_a_.source), source_text(leg_b_.source));
        const std::int64_t delta = leg_a_.timestamp_ns > leg_b_.timestamp_ns
            ? leg_a_.timestamp_ns - leg_b_.timestamp_ns
            : leg_b_.timestamp_ns - leg_a_.timestamp_ns;
        if (delta > 5'000'000'000LL)
            return QStringLiteral("STALE SKEW — legs are %1 ms apart; comparison refused.")
                .arg(delta / 1'000'000LL);
        return QStringLiteral("Aligned %1 · A %2 @ %3 · B %4 @ %5")
            .arg(source_text(leg_a_.source))
            .arg(format_paise(leg_a_.price_paise))
            .arg(leg_a_.timestamp_ns / 1'000'000LL)
            .arg(format_paise(leg_b_.price_paise))
            .arg(leg_b_.timestamp_ns / 1'000'000LL);
    }

    static void add_pair_candidates(QComboBox* box) {
        // These are canonical UNDERLYING names, not broker tokens. The chain
        // loader resolves the selected name through the same instrument master
        // parser used by the rest of the engine; no token or contract is
        // fabricated by the UI.
        const struct Candidate { const char* name; const char* venue; } values[] = {
            {"NIFTY", "NSE:CASH"}, {"BANKNIFTY", "NSE:CASH"},
            {"CIPLA", "NSE:CASH"}, {"SUNPHARMA", "NSE:CASH"},
        };
        for (const auto& c : values) {
            box->addItem(QStringLiteral("%1 (%2)").arg(
                             QString::fromUtf8(c.name),
                             QString::fromUtf8(c.venue)),
                         QString::fromUtf8(c.name));
        }
    }

    void pair_changed(QComboBox* changed) {
        if (instrument_a_->currentData() == instrument_b_->currentData()) {
            const QSignalBlocker guard(changed);
            const int other = changed == instrument_a_
                ? instrument_b_->currentIndex() : instrument_a_->currentIndex();
            changed->setCurrentIndex(other == 0 ? 1 : 0);
            pair_note_->setText(QStringLiteral(
                "A and B must be different instruments. The duplicate choice "
                "was refused."));
            return;
        }
        update_pair_note();
    }

    void update_pair_note() {
        if (instrument_a_ == nullptr || instrument_b_ == nullptr) return;
        pair_note_->setText(QStringLiteral(
            "<b>%1 %2 × %3</b> versus <b>%4 %5 × %6</b>. Prices, timestamps and "
            "data source will be shown when both legs have admitted live or "
            "replay data. This quantity ratio is explicit; no hedge ratio or "
            "entry/exit threshold is inferred.<br><span style='color:#B9770B'>%7</span>")
            .arg(side_a(), QString::number(ratio_a()), instrument_a_->currentText(),
                 side_b(), QString::number(ratio_b()), instrument_b_->currentText(),
                 aligned_data_status()));
    }

    void reload_chain() {
        const QByteArray name = instrument_a_->currentData().toString().toUtf8();
        chain_ = load_chain(chain_path_, name.constData());
        expiry_->clear();
        t_->clearContents();
        populate();
    }
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
            "<b>%1 %2 option contracts</b> across %3 expiries, read from "
            "<code>data/instruments.csv</code> through "
            "<code>instruments/kite_dump.hpp</code> — the same parser the "
            "engine loads specs with. Every strike, lot size and tick below "
            "comes from the master; none is a literal (rule 1, and ROADMAP §6 "
            "says why: NSE has revised lot sizes mid-series).")
                .arg(instrument_a_->currentData().toString())
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
    QString chain_path_;
    QComboBox* instrument_a_ = nullptr;
    QComboBox* instrument_b_ = nullptr;
    QComboBox* side_a_ = nullptr;
    QComboBox* side_b_ = nullptr;
    QSpinBox* ratio_a_ = nullptr;
    QSpinBox* ratio_b_ = nullptr;
    QPushButton* swap_ = nullptr;
    QLabel* pair_note_ = nullptr;
    QLabel* head_ = nullptr;
    QComboBox* expiry_ = nullptr;
    QTableWidget* t_ = nullptr;
    QLabel* foot_ = nullptr;
    PairLegState leg_a_{};
    PairLegState leg_b_{};
};

} // namespace altair::ui
