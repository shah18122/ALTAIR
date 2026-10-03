// app/charges_check.hpp -- the paper engine's expenses, checked against a
// broker's contract note, head by head.
//
// Every paper net in this tree is priced by app/demo_costs.hpp through
// config/charges.toml. A schedule is only VERIFIED when a real contract note
// agrees with it, so this file takes a note's trades, prices each one exactly
// as the paper engine would, and hands both sides to the reconciler
// (app/note_reconcile.hpp): row by row, head by head, signed engine minus
// broker.
//
// THE NOTE, NORMALISED. Brokers print notes differently (PDF, XLSX, CSV); this
// reads one CSV shape, which a note is copied into by hand or by script --
//   order_id,date,symbol,segment,side,qty,price,brokerage,stt,exchange_txn,sebi,stamp,ipft,gst
// with an optional `time` (HH:MM:SS, IST). segment: FUT, OPT or CASH; side:
// BUY or SELL; price and charges in rupees. Columns may come in any order.
//
// ONE ROW PER ORDER. A note lists each fill; brokerage is charged per order.
// Rows sharing an order id are summed (quantity, turnover and every head) and
// priced as one order at its average price. Charges computed per fill and per
// order differ only by rounding, which --tolerance-paise allows for, per head.
//
// The schedule is used whatever its `verified` flag says: testing it is the
// point. Nothing here is a broker's statement of record: the note is.

#pragma once

#include <app/demo_costs.hpp>
#include <app/note_reconcile.hpp>
#include <live/universe.hpp>
#include <risk/cost.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace altair::charges_check {

struct NoteOrder {
    std::string order_id, symbol;
    std::int64_t day = 0;              ///< IST day
    std::int64_t second = 12 * 3600;   ///< IST second of the day (noon when the note gives no time)
    Segment segment = Segment::Fut;
    Side side = Side::Buy;
    std::int64_t qty = 0;
    double turnover = 0.0;             ///< rupees
    double brokerage = 0, stt = 0, exchange_txn = 0, sebi = 0, stamp = 0, ipft = 0, gst = 0;   ///< rupees
    std::size_t rows = 0;              ///< note rows summed into this order
};

/// Parse a normalised note. Rows that cannot be read are listed in `errors`
/// (line number and why), never skipped quietly.
[[nodiscard]] inline std::vector<NoteOrder> parse_note(const std::string& text, std::vector<std::string>& errors) {
    std::vector<NoteOrder> out;
    std::map<std::string, std::size_t> by_id;
    std::istringstream in(text);
    std::string line;
    std::map<std::string, std::size_t> col;
    if (!std::getline(in, line)) { errors.push_back("the note is empty"); return out; }
    {
        std::size_t k = 0;
        std::string name;
        std::istringstream h(line);
        while (std::getline(h, name, ',')) {
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            col[name] = k++;
        }
    }
    for (const char* need : {"order_id", "date", "symbol", "segment", "side", "qty", "price", "brokerage", "stt", "exchange_txn",
                             "sebi", "stamp", "ipft", "gst"})
        if (col.count(need) == 0) errors.push_back(std::string("the header has no column ") + need);
    if (!errors.empty()) return out;
    std::size_t lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> c;
        {
            std::string f;
            std::istringstream r(line);
            while (std::getline(r, f, ',')) c.push_back(f);
            if (!line.empty() && line.back() == ',') c.emplace_back();
        }
        const auto get = [&](const char* k) -> std::string { const auto i = col.at(k); return i < c.size() ? c[i] : std::string(); };
        const auto money = [&](const char* k, double& v) {
            const std::string s = get(k);
            char* end = nullptr;
            v = std::strtod(s.c_str(), &end);
            return !s.empty() && end != nullptr && *end == '\0' && std::isfinite(v) && v >= 0.0;
        };
        const std::string why_prefix = "line " + std::to_string(lineno) + ": ";
        NoteOrder o;
        o.order_id = get("order_id");
        o.symbol = get("symbol");
        o.day = live::parse_day(get("date"));
        const std::string seg = get("segment"), side = get("side");
        o.segment = seg == "FUT" ? Segment::Fut : seg == "OPT" ? Segment::Opt : Segment::Cash;
        o.side = side == "SELL" ? Side::Sell : Side::Buy;
        o.qty = std::atoll(get("qty").c_str());
        double price = 0.0;
        if (o.order_id.empty() || o.day == 0 || (seg != "FUT" && seg != "OPT" && seg != "CASH") || (side != "BUY" && side != "SELL")
            || o.qty <= 0 || !money("price", price) || !(price > 0.0)) {
            errors.push_back(why_prefix + "order id, date, segment (FUT/OPT/CASH), side (BUY/SELL), qty or price unreadable");
            continue;
        }
        if (!money("brokerage", o.brokerage) || !money("stt", o.stt) || !money("exchange_txn", o.exchange_txn) || !money("sebi", o.sebi)
            || !money("stamp", o.stamp) || !money("ipft", o.ipft) || !money("gst", o.gst)) {
            errors.push_back(why_prefix + "a charge head is missing or not a non-negative number");
            continue;
        }
        if (col.count("time") != 0) {
            const std::string t = get("time");
            if (t.size() >= 8) o.second = std::atoi(t.substr(0, 2).c_str()) * 3600 + std::atoi(t.substr(3, 2).c_str()) * 60 + std::atoi(t.substr(6, 2).c_str());
        }
        o.turnover = price * static_cast<double>(o.qty);
        o.rows = 1;
        const auto it = by_id.find(o.order_id);
        if (it == by_id.end()) {
            by_id.emplace(o.order_id, out.size());
            out.push_back(o);
            continue;
        }
        NoteOrder& a = out[it->second];
        if (a.symbol != o.symbol || a.side != o.side || a.segment != o.segment || a.day != o.day) {
            errors.push_back(why_prefix + "order " + o.order_id + " appears with a different symbol, side, segment or date");
            continue;
        }
        a.qty += o.qty; a.turnover += o.turnover; a.brokerage += o.brokerage; a.stt += o.stt; a.exchange_txn += o.exchange_txn;
        a.sebi += o.sebi; a.stamp += o.stamp; a.ipft += o.ipft; a.gst += o.gst; ++a.rows;
        a.second = std::min(a.second, o.second);
    }
    return out;
}

[[nodiscard]] inline Notional paise(double rupees) { return Notional{static_cast<std::int64_t>(std::llround(rupees * 100.0))}; }

struct CheckResult {
    Report report;
    std::size_t unpriced = 0;          ///< orders the schedule could not price (no schedule for the date)
    std::int64_t tolerance_paise = 0;
};

/// Price every order as the paper engine would and reconcile it with the note.
/// A head within `tolerance_paise` of the note's is taken as agreeing.
[[nodiscard]] inline CheckResult check(const std::vector<NoteOrder>& orders, const std::vector<ChargeSchedule>& schedules,
                                       std::int64_t tolerance_paise = 0) {
    CheckResult r;
    r.tolerance_paise = tolerance_paise;
    std::vector<TradeRecord> engine, broker;
    for (const auto& o : orders) {
        TradeRecord b;
        b.order_id = o.order_id; b.symbol = o.symbol; b.side = o.side; b.qty = Qty{o.qty};
        const double avg = o.turnover / static_cast<double>(o.qty);
        b.price = Price{static_cast<std::int64_t>(std::llround(avg * 100.0))};
        b.charges = ChargeHeads{paise(o.brokerage), paise(o.stt), paise(o.exchange_txn), paise(o.sebi), paise(o.stamp), paise(o.ipft), paise(o.gst)};
        broker.push_back(b);
        const auto c = demo_costs::fill(o.segment, o.side, static_cast<double>(o.qty), avg, o.day * 86400 + o.second, schedules);
        if (!c.priced) { ++r.unpriced; continue; }   // left out: the reconciler reports it BROKER-ONLY
        TradeRecord e = b;
        e.charges = ChargeHeads{paise(c.brokerage), paise(c.stt), paise(c.exchange), paise(c.sebi), paise(c.stamp), paise(c.ipft), paise(c.gst)};
        const auto within = [tolerance_paise](Notional& mine, Notional theirs) {
            const std::int64_t d = mine.raw() - theirs.raw();
            if ((d < 0 ? -d : d) <= tolerance_paise) mine = theirs;
        };
        within(e.charges.brokerage, b.charges.brokerage); within(e.charges.stt, b.charges.stt);
        within(e.charges.exchange_txn, b.charges.exchange_txn); within(e.charges.sebi, b.charges.sebi);
        within(e.charges.stamp, b.charges.stamp); within(e.charges.ipft, b.charges.ipft); within(e.charges.gst, b.charges.gst);
        engine.push_back(e);
    }
    r.report = reconcile(engine, broker);
    return r;
}

} // namespace altair::charges_check
