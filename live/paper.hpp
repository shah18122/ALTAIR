// live/paper.hpp -- the live models' paper book.
//
// Every signal a live model acts on becomes a paper position here, priced the
// way a market order would have been: a buy at the best ASK, a sell at the
// best BID, from the quote in force at that moment -- not at the last trade,
// and never at a mid nobody could deal at. When there is no quote the last
// trade is used and the fill says so.
//
// EXPENSES ARE CHARGED ON EVERY FILL through the caller's cost function (the
// CLI prices them from config/charges.toml through risk/cost.hpp, and marks
// them UNVERIFIED while that file is). A round trip records gross P&L, the
// expenses of both fills, net P&L, the model that took it, and why it was
// opened and closed.
//
// THERE IS NO ORDER PATH. This book is arithmetic on prices; nothing here
// reaches a broker, and live/ links no broker and no OMS.
//
// A position is per (model, instrument). Intraday positions are squared off
// by the engine at 15:20; positional ones (pairs, stat-arb) are carried and
// written to disk so the next session resumes them.

#pragma once

#include <live/universe.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace altair::live {

/// What the paper book needs to know about the market right now.
struct LiveTop {
    std::int64_t ltp = 0, bid = 0, ask = 0;   ///< paise; 0 when absent
};

/// Rupees of expenses for one fill: (instrument, buy?, quantity, price in
/// rupees, fill time ns). NaN when it cannot be priced.
using LiveCostFn = std::function<double(const LiveInstrument&, bool, double, double, std::int64_t)>;

struct LivePaperFill {
    std::string model;
    std::uint32_t token = 0;
    std::string symbol;
    int side = 0;              ///< +1 buy, -1 sell
    std::int64_t qty = 0;      ///< units (lots x lot size)
    double price = 0.0;        ///< rupees
    double expenses = 0.0;     ///< rupees; NaN when unpriced
    std::int64_t ns = 0;
    std::string reason;
    bool at_quote = true;      ///< false: no bid/ask, filled at the last trade
};

struct LivePaperTrade {
    std::string model, symbol;
    std::uint32_t token = 0;
    int side = 0;              ///< +1 long, -1 short
    std::int64_t qty = 0;
    std::int64_t entry_ns = 0, exit_ns = 0;
    double entry = 0.0, exit = 0.0;   ///< rupees
    double gross = 0.0, expenses = 0.0, net = 0.0;
    std::string why_in, why_out;
};

struct LivePosition {
    std::string model;
    LiveInstrument inst;
    int side = 0;
    std::int64_t qty = 0;
    double entry = 0.0;
    std::int64_t entry_ns = 0;
    double entry_expenses = 0.0;
    std::string why_in;
    bool carry = false;        ///< held overnight (pairs, stat-arb)
};

class LivePaperBook {
public:
    using TopFn = std::function<LiveTop(std::uint32_t)>;

    LivePaperBook(TopFn top, LiveCostFn cost) : top_(std::move(top)), cost_(std::move(cost)) {}

    /// Open (or add to nothing: one position per model and instrument).
    /// Returns false, with `why` set, when it cannot fill.
    bool open(const std::string& model, const LiveInstrument& in, int side, std::int64_t lots, std::int64_t ns,
              const std::string& reason, bool carry, std::string* why = nullptr) {
        if (side == 0 || lots <= 0) { if (why) *why = "nothing to do"; return false; }
        const auto key = std::make_pair(model, in.token);
        if (positions_.count(key) != 0) { if (why) *why = "already in a position"; return false; }
        LivePaperFill f;
        if (!fill(model, in, side, lots * (in.lot > 0 ? in.lot : 1), ns, reason, f, why)) return false;
        LivePosition p;
        p.model = model; p.inst = in; p.side = side; p.qty = f.qty; p.entry = f.price; p.entry_ns = ns;
        p.entry_expenses = f.expenses; p.why_in = reason; p.carry = carry;
        positions_.emplace(key, p);
        return true;
    }

    /// Close `model`'s position in `token`, if any.
    bool close(const std::string& model, std::uint32_t token, std::int64_t ns, const std::string& reason) {
        const auto it = positions_.find(std::make_pair(model, token));
        if (it == positions_.end()) return false;
        const LivePosition& p = it->second;
        LivePaperFill f;
        std::string why;
        if (!fill(model, p.inst, -p.side, p.qty, ns, reason, f, &why)) return false;
        LivePaperTrade t;
        t.model = model; t.symbol = p.inst.symbol; t.token = token; t.side = p.side; t.qty = p.qty;
        t.entry_ns = p.entry_ns; t.exit_ns = ns; t.entry = p.entry; t.exit = f.price;
        t.gross = static_cast<double>(p.side) * (f.price - p.entry) * static_cast<double>(p.qty);
        t.expenses = p.entry_expenses + f.expenses;
        t.net = t.gross - t.expenses;
        t.why_in = p.why_in; t.why_out = reason;
        trades_.push_back(t);
        positions_.erase(it);
        return true;
    }

    /// Close every position matching `pred`.
    template <class Pred>
    void close_if(Pred&& pred, std::int64_t ns, const std::string& reason) {
        std::vector<std::pair<std::string, std::uint32_t>> keys;
        for (const auto& [k, p] : positions_) if (pred(p)) keys.push_back(k);
        for (const auto& k : keys) (void)close(k.first, k.second, ns, reason);
    }

    /// Restore a carried position from disk, without a fill.
    void restore(const LivePosition& p) { positions_.emplace(std::make_pair(p.model, p.inst.token), p); }

    [[nodiscard]] const LivePosition* position(const std::string& model, std::uint32_t token) const {
        const auto it = positions_.find(std::make_pair(model, token));
        return it == positions_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::vector<LivePosition> positions() const {
        std::vector<LivePosition> v;
        for (const auto& [k, p] : positions_) v.push_back(p);
        return v;
    }
    [[nodiscard]] bool flat(const std::string& model) const {
        for (const auto& [k, p] : positions_) if (k.first == model) return false;
        return true;
    }
    [[nodiscard]] const std::vector<LivePaperTrade>& trades() const noexcept { return trades_; }
    [[nodiscard]] const std::vector<LivePaperFill>& fills() const noexcept { return fills_; }

    /// Mark-to-market of an open position: what closing it now would fetch,
    /// at the side of the book it would close against.
    [[nodiscard]] double unrealised(const LivePosition& p) const {
        const LiveTop t = top_(p.inst.token);
        const std::int64_t px = p.side > 0 ? (t.bid > 0 ? t.bid : t.ltp) : (t.ask > 0 ? t.ask : t.ltp);
        if (px <= 0) return std::numeric_limits<double>::quiet_NaN();
        return static_cast<double>(p.side) * (static_cast<double>(px) / 100.0 - p.entry) * static_cast<double>(p.qty);
    }

private:
    bool fill(const std::string& model, const LiveInstrument& in, int side, std::int64_t qty, std::int64_t ns,
              const std::string& reason, LivePaperFill& f, std::string* why) {
        const LiveTop t = top_(in.token);
        std::int64_t px = side > 0 ? t.ask : t.bid;
        bool at_quote = px > 0;
        if (!at_quote) px = t.ltp;
        if (px <= 0) { if (why) *why = "no price for " + in.symbol; return false; }
        f.model = model; f.token = in.token; f.symbol = in.symbol; f.side = side; f.qty = qty;
        f.price = static_cast<double>(px) / 100.0; f.ns = ns; f.reason = reason; f.at_quote = at_quote;
        f.expenses = cost_ ? cost_(in, side > 0, static_cast<double>(qty), f.price, ns)
                           : std::numeric_limits<double>::quiet_NaN();
        fills_.push_back(f);
        return true;
    }

    TopFn top_;
    LiveCostFn cost_;
    std::map<std::pair<std::string, std::uint32_t>, LivePosition> positions_;
    std::vector<LivePaperTrade> trades_;
    std::vector<LivePaperFill> fills_;
};

// ---- carried positions on disk ---------------------------------------------

inline constexpr const char* kLivePositionsHeader =
    "model,token,symbol,side,qty,entry,entry_ns,entry_expenses,carry,why_in";

[[nodiscard]] inline bool live_write_positions(const std::string& path, const std::vector<LivePosition>& ps) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << kLivePositionsHeader << '\n';
        char num[64];
        for (const auto& p : ps) {
            std::snprintf(num, sizeof num, "%.2f", p.entry);
            f << p.model << ',' << p.inst.token << ',' << p.inst.symbol << ',' << p.side << ',' << p.qty << ','
              << num << ',' << p.entry_ns << ',';
            std::snprintf(num, sizeof num, "%.2f", p.entry_expenses);
            f << num << ',' << (p.carry ? 1 : 0) << ',' << p.why_in << '\n';
        }
        if (!f) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/// Positions back from disk, resolved against today's universe by token. A
/// position whose instrument is no longer streamed is returned in `orphans`.
[[nodiscard]] inline std::vector<LivePosition> live_read_positions(const std::string& path,
                                                                   const std::vector<LiveInstrument>& universe,
                                                                   std::vector<std::string>& orphans) {
    std::vector<LivePosition> out;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line.rfind("model,", 0) != 0) return out;
    while (std::getline(in, line)) {
        const auto c = split_csv(line);
        if (c.size() < 10) continue;
        const auto tok = static_cast<std::uint32_t>(std::strtoul(c[1].c_str(), nullptr, 10));
        const LiveInstrument* found = nullptr;
        for (const auto& i : universe) if (i.token == tok) { found = &i; break; }
        if (found == nullptr) { orphans.push_back(c[0] + " " + c[2]); continue; }
        LivePosition p;
        p.model = c[0]; p.inst = *found; p.side = std::atoi(c[3].c_str()); p.qty = std::atoll(c[4].c_str());
        p.entry = std::atof(c[5].c_str()); p.entry_ns = std::atoll(c[6].c_str());
        p.entry_expenses = std::atof(c[7].c_str()); p.carry = c[8] == "1"; p.why_in = c[9];
        if (p.side != 0 && p.qty > 0) out.push_back(p);
    }
    return out;
}

} // namespace altair::live
