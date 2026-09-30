// desktop/data/master_lookup.hpp -- token -> instrument profile, from the
// Kite master.
//
// P11Q-11.
//
// The watchlist showed a token, a name and whatever the user typed. Everything
// that makes an instrument identifiable -- segment, expiry, strike, whether it
// is a call or a put -- was in `data/instruments.csv` and nowhere the UI could
// reach it.
//
// LOADED ONCE, NOT PER ROW.
//
// The master is 8.8 MB and 108,411 rows. Scanning it on every watchlist add is
// a visible freeze at the tenth instrument, and the freeze arrives late enough
// that it reads as a different bug. So the index is built on first use and
// held: one pass, then a hash lookup.
//
// AND IT RESOLVES THROUGH THE REAL PARSER.
//
// `instruments/kite_dump.hpp`, sink-templated, so the strike, lot size, tick
// size and expiry arrive as a `ContractSpec` from the same code the engine
// loads specs with. Rule 1: none of them is a literal, and ROADMAP §6 says why
// -- NSE has revised lot sizes mid-series.
//
// WHAT IT CANNOT SUPPLY, AND SAYS SO: A QUOTE.
//
// The master is reference data. It has no bid, no ask, no last price and no
// book -- those come from a live subscription (P2-02's decoder is built and
// tested; nothing subscribes yet). A watchlist column for them exists and
// renders as absent rather than as zero, because a bid of 0.00 is a price and
// "no quote" is not.

#pragma once

#include <instruments/kite_dump.hpp>

#include <QHash>
#include <QString>

#include <cstring>
#include <fstream>
#include <string>

namespace altair::ui {

/// What the master knows about one instrument.
struct InstrumentProfile {
    bool found = false;
    QString symbol;
    QString underlying;
    QString segment;
    QString opt_type;        ///< "CE", "PE", or empty
    std::int64_t strike = 0;         ///< paise, 0 when not an option
    std::int64_t lot_size = 0;
    std::int64_t tick_paise = 0;
    std::int64_t expiry_ns = 0;      ///< 0 when the contract has no expiry
    /// The exchange an order for this contract is sent to: NSE, NFO, BSE, BFO,
    /// CDS, BCD or MCX. Derived from the spec's exchange AND segment -- never
    /// from the symbol's spelling. See kite_exchange_name().
    QString kite_exchange;
};

/// The Kite exchange code for a contract.
///
/// THIS REPLACES A GUESS THAT WAS WRONG FOR EVERY OPTION. order_ticket.hpp
/// decided the exchange with `symbol.endsWith("FUT") ? "NFO" : "NSE"`, so an
/// option -- NIFTY2690824000CE ends in CE, not FUT -- was written to the
/// intent queue as NSE, which is the cash segment. data/order_intents.jsonl
/// already held one such line. oms/ would have routed it to a segment where
/// the contract does not exist.
///
/// Exchange and segment both come from the master, so this is a lookup and
/// not an inference.
[[nodiscard]] inline QString kite_exchange_name(Exchange ex, Segment seg) {
    const bool bse = ex == Exchange::BSE;
    switch (seg) {
    case Segment::Cash:      return bse ? QStringLiteral("BSE") : QStringLiteral("NSE");
    case Segment::Fut:
    case Segment::Opt:       return bse ? QStringLiteral("BFO") : QStringLiteral("NFO");
    case Segment::Currency:  return bse ? QStringLiteral("BCD") : QStringLiteral("CDS");
    case Segment::Commodity: return QStringLiteral("MCX");
    }
    return QString();   // unknown: empty, so a caller must refuse rather than send
}

[[nodiscard]] inline QString segment_name(Segment s) {
    switch (s) {
    case Segment::Cash:      return QStringLiteral("CASH");
    case Segment::Fut:       return QStringLiteral("FUT");
    case Segment::Opt:       return QStringLiteral("OPT");
    case Segment::Currency:  return QStringLiteral("CUR");
    case Segment::Commodity: return QStringLiteral("COM");
    }
    return QStringLiteral("?");
}

/// One pass over the master, held for the process's life.
class MasterIndex {
public:
    /// True when the master was found and parsed. False is not an error the
    /// caller should hide: without it every profile is empty and the
    /// watchlist must say so rather than showing blank cells that look like
    /// "no expiry".
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const QString& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(by_token_.size());
    }

    void load(const QString& path) {
        if (loaded_ || tried_) { return; }
        tried_ = true;
        std::ifstream f(path.toStdString(), std::ios::binary);
        if (!f) {
            error_ = QStringLiteral("cannot open %1").arg(path);
            return;
        }
        const std::string csv((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (csv.empty()) {
            error_ = QStringLiteral("%1 is empty").arg(path);
            return;
        }
        const auto sink = [this](const ContractSpec& s) noexcept {
            InstrumentProfile p;
            p.found = true;
            p.symbol = QString::fromUtf8(s.symbol);
            p.underlying = QString::fromUtf8(s.underlying);
            p.segment = segment_name(s.segment);
            p.opt_type = s.opt_type == OptionType::CE ? QStringLiteral("CE")
                       : s.opt_type == OptionType::PE ? QStringLiteral("PE")
                                                      : QString();
            p.strike = s.strike.raw();
            p.lot_size = s.lot_size.raw();
            p.tick_paise = s.tick_size.raw();
            p.expiry_ns = s.expiry.ns_since_epoch();
            p.kite_exchange = kite_exchange_name(s.exchange, s.segment);
            // ---- QUALIFY THE ENUM. THERE ARE TWO OF THEM. ---------------
            //
            // `altair::FeedSource` (instruments/contract_spec.hpp) is
            // {Kite = 0, Xts = 1} and sizes `token[kFeedSourceCount]`, which
            // is two elements. `altair::ui::FeedSource`
            // (desktop/feed_status.hpp) is {Unspecified, Replay, Kite, Xts}
            // and its Kite is 2.
            //
            // This file is in namespace altair::ui, so an UNQUALIFIED
            // FeedSource::Kite picks the UI one and indexes token[2] -- one
            // past the end of a two-element array. It compiled, it did not
            // crash, and it silently indexed 3 contracts out of 108,411
            // because it was reading whatever followed the array.
            //
            // The static_assert below is the part that matters: it fails the
            // BUILD if the qualified value ever moves out of range, so this
            // cannot come back as another quiet wrong number.
            static_assert(static_cast<std::size_t>(altair::FeedSource::Kite)
                              < kFeedSourceCount,
                          "the Kite token slot must be inside token[]");
            by_token_.insert(
                s.token[static_cast<std::size_t>(altair::FeedSource::Kite)], p);
            // GETS screens look contracts up by name: FYERS and Kite spell
            // derivative trading symbols the same way, and the exchange code
            // keeps an NSE cash line apart from its BSE twin.
            by_symbol_.insert(p.kite_exchange + QLatin1Char(':') + p.symbol, p);
            return true;
        };
        const auto rep = detail::load_kite_dump_into(csv.data(), csv.size(),
                                                     sink, Timestamp{0});
        if (!rep) {
            error_ = QStringLiteral("the master did not parse");
            return;
        }
        loaded_ = true;
    }

    [[nodiscard]] InstrumentProfile find(std::uint32_t token) const {
        const auto it = by_token_.find(token);
        return it == by_token_.end() ? InstrumentProfile{} : *it;
    }

    /// By Kite exchange code and trading symbol, e.g. ("NFO",
    /// "NIFTY26SEP25000CE") or ("NSE", "SBIN").
    [[nodiscard]] InstrumentProfile find_symbol(const QString& kite_exchange,
                                                const QString& symbol) const {
        const auto it = by_symbol_.find(kite_exchange + QLatin1Char(':') + symbol);
        return it == by_symbol_.end() ? InstrumentProfile{} : *it;
    }

private:
    QHash<std::uint32_t, InstrumentProfile> by_token_;
    QHash<QString, InstrumentProfile> by_symbol_;
    bool loaded_ = false;
    bool tried_ = false;
    QString error_;
};

} // namespace altair::ui
