// live/paper.hpp -- the live models' paper book.
//
// EXECUTABLE, OR NOT AT ALL. A model's decision becomes a WORKING ORDER, and
// an order fills only the way a market order could have:
//   * against a QUOTE THAT IS FRESH for that instrument (stamped within
//     `max_quote_age_ns` of the fill, on the feed's clock) -- a quote from
//     minutes ago is not a price anyone could deal at, however healthy the
//     rest of the feed looks;
//   * at the touch, a buy at the ASK and a sell at the BID, walking the
//     five-level book when one is fresh -- never at a mid, and NEVER AT THE
//     LAST TRADE: a fill with no executable quote does not happen;
//   * for no more than the size displayed: the rest of the order keeps
//     working against later quotes (a partial fill);
//   * no sooner than `latency_ns` after the decision.
// An ENTRY that is not filled within `entry_timeout_ns` is cancelled; a
// partly filled one keeps what filled, and a model's other legs from the same
// decision are unwound, so one filled leg is never left unhedged. An EXIT never
// expires: once submitted it is an obligation that keeps working until it
// fills, however long the market stays unquoted -- and it says so.
//
// EXPENSES ARE CHARGED ON EVERY FILL through the caller's cost function (the
// CLI prices them from config/charges.toml through risk/cost.hpp). Unpriced is
// NaN all the way through -- a round trip with an unpriced fill has NO net.
// A partly filled order pays brokerage on each fill, which overstates it.
//
// THERE IS NO ORDER PATH. This book is arithmetic on prices; nothing here
// reaches a broker, and live/ links no broker and no OMS.
//
// A position is per (model, instrument). Intraday positions are squared off
// by the engine at 15:20; positional ones (pairs, stat-arb) are carried. Every
// fill is an event the CLI journals, and a restart rebuilds the book by
// replaying them (live_replay_journal).

#pragma once

#include <live/universe.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace altair::live {

inline constexpr std::size_t kLiveDepth = 5;

struct LiveLevel {
    std::int64_t px = 0;    ///< paise
    std::int64_t qty = 0;   ///< units
};

/// What the paper book needs to know about one instrument's market right now.
struct LiveTop {
    std::int64_t ltp = 0, bid = 0, ask = 0;   ///< paise; 0 when absent
    std::int64_t bid_qty = 0, ask_qty = 0;    ///< units at the touch; 0 = none shown
    std::int64_t quote_ns = 0;                ///< feed time the bid/ask was stamped; 0 = never quoted
    std::int64_t book_ns = 0;                 ///< feed time of the five-level book; 0 = none
    std::uint16_t levels = 0;
    LiveLevel bids[kLiveDepth]{}, asks[kLiveDepth]{};
};

/// How orders meet the market. Feed time throughout.
struct LiveExecPolicy {
    std::int64_t latency_ns = 250'000'000;          ///< decision to order at the market
    std::int64_t max_quote_age_ns = 10'000'000'000; ///< older than this, a quote is not executable
    std::int64_t entry_timeout_ns = 60'000'000'000; ///< an entry not filled by then is cancelled
};

/// Rupees of expenses for one fill: (instrument, buy?, quantity, price in
/// rupees, fill time ns). NaN when it cannot be priced.
using LiveCostFn = std::function<double(const LiveInstrument&, bool, double, double, std::int64_t)>;

/// What an opening order asks of the pre-trade risk check.
struct LiveRiskRequest {
    std::string model;
    const LiveInstrument* inst = nullptr;
    int side = 0;
    std::int64_t qty = 0;
    double price = 0.0;      ///< rupees, the touch it would deal at now
    bool carry = false;
    std::int64_t ns = 0;
};
/// A refusal reason, or nullopt to allow.
using LiveRiskFn = std::function<std::optional<std::string>(const LiveRiskRequest&)>;

enum class LiveFillRole : std::uint8_t { Open, Close };

struct LivePaperFill {
    std::string model;
    std::uint32_t token = 0;
    std::string symbol;
    int side = 0;              ///< +1 buy, -1 sell
    std::int64_t qty = 0;      ///< units (lots x lot size)
    double price = 0.0;        ///< rupees, volume-weighted over the levels taken
    double expenses = 0.0;     ///< rupees; NaN when unpriced
    std::int64_t ns = 0;       ///< when it filled
    std::int64_t submit_ns = 0;///< when the order was decided
    std::string reason;
    LiveFillRole role = LiveFillRole::Open;
    bool carry = false;
    bool partial = false;      ///< the order still had quantity working after this fill
    bool at_quote = true;      ///< always: there is no last-trade fallback any more
};

struct LivePaperTrade {
    std::string model, symbol;
    std::uint32_t token = 0;
    int side = 0;              ///< +1 long, -1 short
    std::int64_t qty = 0;
    std::int64_t entry_ns = 0, exit_ns = 0;
    double entry = 0.0, exit = 0.0;   ///< rupees
    double gross = 0.0, expenses = 0.0, net = 0.0;   ///< expenses and net NaN when any fill was unpriced
    std::string why_in, why_out;
};

enum class LivePosState : std::uint8_t { Opening, Open, Closing };

[[nodiscard]] inline const char* live_state_text(LivePosState s) noexcept {
    switch (s) {
    case LivePosState::Opening: return "opening";
    case LivePosState::Open: return "open";
    case LivePosState::Closing: return "closing";
    }
    return "?";
}

struct LivePosition {
    std::string model;
    LiveInstrument inst;
    int side = 0;
    std::int64_t qty = 0;          ///< filled units; 0 while an entry has not filled
    double entry = 0.0;            ///< rupees, volume-weighted; 0 until a fill
    std::int64_t entry_ns = 0;     ///< the first fill (the decision, until then)
    double entry_expenses = 0.0;
    std::string why_in;
    bool carry = false;            ///< held overnight (pairs, stat-arb)
    LivePosState state = LivePosState::Open;
    std::int64_t want_qty = 0;     ///< what the entry asked for
    std::int64_t decided_ns = 0;   ///< the decision; legs of one decision share it
    std::int64_t exit_since_ns = 0;///< when the exit was submitted (0: none working)
    std::string exit_reason;
    [[nodiscard]] bool filled() const noexcept { return qty > 0; }
};

class LivePaperBook {
public:
    using TopFn = std::function<LiveTop(std::uint32_t)>;

    LivePaperBook(TopFn top, LiveCostFn cost, LiveExecPolicy policy = {})
        : top_(std::move(top)), cost_(std::move(cost)), pol_(policy) {}

    void set_policy(const LiveExecPolicy& p) { pol_ = p; }
    [[nodiscard]] const LiveExecPolicy& policy() const noexcept { return pol_; }
    void set_risk(LiveRiskFn fn) { risk_ = std::move(fn); }

    /// Submit an entry: one position per model and instrument. Refused, with
    /// `why`, when there is no fresh executable quote now or the risk check
    /// says no; otherwise it works until filled or timed out.
    bool open(const std::string& model, const LiveInstrument& in, int side, std::int64_t lots, std::int64_t ns,
              const std::string& reason, bool carry, std::string* why = nullptr) {
        if (side == 0 || lots <= 0) { if (why) *why = "nothing to do"; return false; }
        const auto key = std::make_pair(model, in.token);
        if (positions_.count(key) != 0) { if (why) *why = "already in a position"; return false; }
        const std::int64_t qty = lots * (in.lot > 0 ? in.lot : 1);
        const LiveTop t = top_(in.token);
        const std::int64_t touch = side > 0 ? t.ask : t.bid;
        if (!fresh(t.quote_ns, ns) || touch <= 0) {
            if (why) *why = "no fresh quote for " + in.symbol + age_text(t.quote_ns, ns);
            return false;
        }
        if (risk_) {
            LiveRiskRequest r{model, &in, side, qty, static_cast<double>(touch) / 100.0, carry, ns};
            if (const auto no = risk_(r)) { if (why) *why = "risk: " + *no; return false; }
        }
        LivePosition p;
        p.model = model; p.inst = in; p.side = side; p.qty = 0; p.entry = 0.0; p.entry_ns = ns;
        p.entry_expenses = 0.0; p.why_in = reason; p.carry = carry; p.state = LivePosState::Opening;
        p.want_qty = qty; p.decided_ns = ns;
        positions_.emplace(key, p);
        ++version_;
        Order o;
        o.id = next_id_++; o.key = key; o.side = side; o.remaining = qty; o.submit_ns = ns;
        o.due_ns = ns + pol_.latency_ns; o.expire_ns = ns + pol_.entry_timeout_ns; o.reason = reason; o.exit = false;
        orders_.emplace(o.id, o);
        if (pol_.latency_ns <= 0) execute(o.id, ns);
        return true;
    }

    /// Close `model`'s position in `token`: cancel an unfilled entry, or submit
    /// an exit that works until it fills. False when there is no position.
    bool close(const std::string& model, std::uint32_t token, std::int64_t ns, const std::string& reason) {
        const auto it = positions_.find(std::make_pair(model, token));
        if (it == positions_.end()) return false;
        LivePosition& p = it->second;
        if (p.state == LivePosState::Closing) return true;   // already working
        ++version_;
        if (p.state == LivePosState::Opening) {
            drop_order_for(it->first, false);
            if (!p.filled()) {
                cancelled_.push_back(model + " " + p.inst.symbol + ": entry cancelled before it filled (" + reason + ")");
                positions_.erase(it);
                return true;
            }
            p.state = LivePosState::Open;   // keep what filled, then exit it
        }
        p.state = LivePosState::Closing;
        p.exit_since_ns = ns;
        p.exit_reason = reason;
        Order o;
        o.id = next_id_++; o.key = it->first; o.side = -p.side; o.remaining = p.qty; o.submit_ns = ns;
        o.due_ns = ns + pol_.latency_ns; o.expire_ns = 0; o.reason = reason; o.exit = true;
        orders_.emplace(o.id, o);
        if (pol_.latency_ns <= 0) execute(o.id, ns);
        return true;
    }

    /// Close every position matching `pred` that is not already closing.
    template <class Pred>
    void close_if(Pred&& pred, std::int64_t ns, const std::string& reason) {
        std::vector<std::pair<std::string, std::uint32_t>> keys;
        for (const auto& [k, p] : positions_) if (p.state != LivePosState::Closing && pred(p)) keys.push_back(k);
        for (const auto& k : keys) (void)close(k.first, k.second, ns, reason);
    }

    /// The market moved for `token` at `ns`: work its due orders.
    void on_market(std::uint32_t token, std::int64_t ns) {
        std::vector<int> ids;
        for (const auto& [id, o] : orders_) if (o.key.second == token && o.due_ns <= ns) ids.push_back(id);
        for (const int id : ids) execute(id, ns);
    }

    /// The clock reached `ns`: work every due order against the quotes in
    /// force, and cancel entries past their timeout.
    void on_clock(std::int64_t ns) {
        std::vector<int> ids;
        for (const auto& [id, o] : orders_) if (o.due_ns <= ns) ids.push_back(id);
        for (const int id : ids) execute(id, ns);
        std::vector<int> expired;
        for (const auto& [id, o] : orders_) if (!o.exit && o.expire_ns > 0 && ns >= o.expire_ns) expired.push_back(id);
        for (const int id : expired) expire_entry(id, ns);
    }

    /// Restore a position from the journal, without a fill.
    void restore(const LivePosition& p) {
        LivePosition q = p;
        q.state = LivePosState::Open;
        q.want_qty = q.qty;
        positions_.emplace(std::make_pair(q.model, q.inst.token), q);
        ++version_;
    }

    /// Changes with every accepted order, cancel, fill, expiry and restore:
    /// the CLI asks it to learn whether a call changed the book.
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    [[nodiscard]] const LivePosition* position(const std::string& model, std::uint32_t token) const {
        const auto it = positions_.find(std::make_pair(model, token));
        return it == positions_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::vector<LivePosition> positions() const {
        std::vector<LivePosition> v;
        for (const auto& [k, p] : positions_) v.push_back(p);
        return v;
    }
    /// Positions with filled quantity: what is actually held (and journaled).
    [[nodiscard]] std::vector<LivePosition> held() const {
        std::vector<LivePosition> v;
        for (const auto& [k, p] : positions_) if (p.filled()) v.push_back(p);
        return v;
    }
    [[nodiscard]] bool flat(const std::string& model) const {
        for (const auto& [k, p] : positions_) if (k.first == model) return false;
        return true;
    }
    /// True while `model` holds or is entering a position it is not already
    /// exiting: what a model asks before deciding to enter again.
    [[nodiscard]] bool engaged(const std::string& model) const {
        for (const auto& [k, p] : positions_) if (k.first == model && p.state != LivePosState::Closing) return true;
        return false;
    }
    [[nodiscard]] std::size_t working_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] const std::vector<LivePaperTrade>& trades() const noexcept { return trades_; }
    [[nodiscard]] const std::vector<LivePaperFill>& fills() const noexcept { return fills_; }
    /// Entries cancelled or timed out, in words, since the last call.
    [[nodiscard]] std::vector<std::string> take_cancelled() { return std::exchange(cancelled_, {}); }

    /// Mark-to-market of a filled position: what closing it now would fetch,
    /// at the side of the book it would close against. NaN with no fresh quote.
    [[nodiscard]] double unrealised(const LivePosition& p, std::int64_t now_ns) const {
        if (!p.filled()) return 0.0;
        const LiveTop t = top_(p.inst.token);
        const std::int64_t px = p.side > 0 ? t.bid : t.ask;
        if (px <= 0 || !fresh(t.quote_ns, now_ns)) return std::numeric_limits<double>::quiet_NaN();
        return static_cast<double>(p.side) * (static_cast<double>(px) / 100.0 - p.entry) * static_cast<double>(p.qty);
    }

    [[nodiscard]] bool fresh(std::int64_t quote_ns, std::int64_t now_ns) const noexcept {
        return quote_ns > 0 && now_ns - quote_ns <= pol_.max_quote_age_ns;
    }

private:
    struct Order {
        int id = 0;
        std::pair<std::string, std::uint32_t> key;
        int side = 0;
        std::int64_t remaining = 0, filled = 0;
        double value = 0.0;            ///< rupees x units filled
        double expenses = 0.0;         ///< NaN once any fill is unpriced
        std::int64_t submit_ns = 0, due_ns = 0, expire_ns = 0;
        std::string reason;
        bool exit = false;
    };

    [[nodiscard]] static std::string age_text(std::int64_t quote_ns, std::int64_t now_ns) {
        if (quote_ns <= 0) return " (never quoted)";
        char b[64];
        std::snprintf(b, sizeof b, " (last quote %.1f s old)", static_cast<double>(now_ns - quote_ns) / 1e9);
        return b;
    }

    void drop_order_for(const std::pair<std::string, std::uint32_t>& key, bool exit) {
        for (auto it = orders_.begin(); it != orders_.end();) {
            if (it->second.key == key && it->second.exit == exit) it = orders_.erase(it);
            else ++it;
        }
    }

    /// Take what the book shows for `side`, up to `want`: (units, rupees x units).
    [[nodiscard]] std::pair<std::int64_t, double> take(const LiveTop& t, int side, std::int64_t want, std::int64_t now) const {
        std::int64_t got = 0;
        double value = 0.0;
        const auto eat = [&](std::int64_t px, std::int64_t qty) {
            if (px <= 0 || qty <= 0 || got >= want) return;
            const std::int64_t n = std::min(qty, want - got);
            got += n;
            value += static_cast<double>(px) / 100.0 * static_cast<double>(n);
        };
        if (t.levels > 0 && fresh(t.book_ns, now)) {
            for (std::size_t k = 0; k < t.levels && k < kLiveDepth; ++k) {
                const LiveLevel& l = side > 0 ? t.asks[k] : t.bids[k];
                eat(l.px, l.qty);
            }
            if (got > 0) return {got, value};
        }
        if (fresh(t.quote_ns, now)) eat(side > 0 ? t.ask : t.bid, side > 0 ? t.ask_qty : t.bid_qty);
        return {got, value};
    }

    void execute(int id, std::int64_t now) {
        const auto oit = orders_.find(id);
        if (oit == orders_.end()) return;
        Order& o = oit->second;
        if (now < o.due_ns) return;
        const auto pit = positions_.find(o.key);
        if (pit == positions_.end()) { orders_.erase(oit); return; }
        LivePosition& p = pit->second;
        const LiveTop t = top_(p.inst.token);
        const auto [got, value] = take(t, o.side, o.remaining, now);
        if (got <= 0) return;   // nothing executable now: keeps working
        const double px = value / static_cast<double>(got);
        LivePaperFill f;
        f.model = p.model; f.token = p.inst.token; f.symbol = p.inst.symbol; f.side = o.side; f.qty = got;
        f.price = px; f.ns = now; f.submit_ns = o.submit_ns; f.reason = o.reason;
        f.role = o.exit ? LiveFillRole::Close : LiveFillRole::Open; f.carry = p.carry;
        f.expenses = cost_ ? cost_(p.inst, o.side > 0, static_cast<double>(got), px, now)
                           : std::numeric_limits<double>::quiet_NaN();
        o.remaining -= got;
        o.filled += got;
        o.value += value;
        o.expenses += f.expenses;   // NaN sticks
        f.partial = o.remaining > 0;
        fills_.push_back(f);
        ++version_;
        if (!o.exit) {
            if (!p.filled()) p.entry_ns = now;
            p.qty = o.filled;
            p.entry = o.value / static_cast<double>(o.filled);
            p.entry_expenses = o.expenses;
            if (o.remaining == 0) { p.state = LivePosState::Open; orders_.erase(oit); }
            return;
        }
        if (o.remaining > 0) return;
        LivePaperTrade tr;
        tr.model = p.model; tr.symbol = p.inst.symbol; tr.token = p.inst.token; tr.side = p.side; tr.qty = p.qty;
        tr.entry_ns = p.entry_ns; tr.exit_ns = now; tr.entry = p.entry; tr.exit = o.value / static_cast<double>(o.filled);
        tr.gross = static_cast<double>(p.side) * (tr.exit - p.entry) * static_cast<double>(p.qty);
        tr.expenses = p.entry_expenses + o.expenses;
        tr.net = tr.gross - tr.expenses;   // NaN when any fill was unpriced
        tr.why_in = p.why_in; tr.why_out = o.reason;
        trades_.push_back(tr);
        orders_.erase(oit);
        positions_.erase(pit);
    }

    void expire_entry(int id, std::int64_t now) {
        const auto oit = orders_.find(id);
        if (oit == orders_.end()) return;
        const Order o = oit->second;
        orders_.erase(oit);
        ++version_;
        const auto pit = positions_.find(o.key);
        if (pit == positions_.end()) return;
        LivePosition& p = pit->second;
        char secs[32];
        std::snprintf(secs, sizeof secs, "%.0f s", static_cast<double>(pol_.entry_timeout_ns) / 1e9);
        if (p.filled()) {
            p.state = LivePosState::Open;   // keep what filled
            cancelled_.push_back(p.model + " " + p.inst.symbol + ": entry part-filled (" + std::to_string(p.qty) + " of "
                                 + std::to_string(p.want_qty) + ") when it timed out after " + secs);
            return;
        }
        const std::string model = p.model;
        const std::int64_t decided = p.decided_ns;
        cancelled_.push_back(model + " " + p.inst.symbol + ": entry unfilled after " + secs
                             + " -- no executable quote with size");
        positions_.erase(pit);
        // A leg of the same decision must not stay on alone.
        std::vector<std::uint32_t> siblings;
        for (const auto& [k, q] : positions_)
            if (k.first == model && q.decided_ns == decided && q.state != LivePosState::Closing) siblings.push_back(k.second);
        for (const std::uint32_t tok : siblings) (void)close(model, tok, now, "other leg unfilled");
    }

    TopFn top_;
    LiveCostFn cost_;
    LiveExecPolicy pol_;
    LiveRiskFn risk_;
    std::map<std::pair<std::string, std::uint32_t>, LivePosition> positions_;
    std::map<int, Order> orders_;
    std::vector<LivePaperTrade> trades_;
    std::vector<LivePaperFill> fills_;
    std::vector<std::string> cancelled_;
    int next_id_ = 1;
    std::uint64_t version_ = 0;
};

// ---- durable files ----------------------------------------------------------

/// Replace `path` with `tmp` in one step. std::filesystem::rename replaces an
/// existing file on every platform (POSIX rename; MoveFileEx with
/// REPLACE_EXISTING on Windows), so the old file is never deleted first and a
/// crash leaves one complete file or the other.
[[nodiscard]] inline bool live_replace_file(const std::string& tmp, const std::string& path) {
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

/// Append `rows` to a CSV, writing the header when the file is new. True only
/// when every byte reached the file (flushed and the stream still good).
[[nodiscard]] inline bool live_append_rows(const std::string& path, const char* header,
                                           const std::vector<std::string>& rows) {
    if (rows.empty()) return true;
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f) return false;
    if (fresh) f << header << '\n';
    for (const auto& r : rows) f << r << '\n';
    f.flush();
    return static_cast<bool>(f);
}

// ---- the fill journal ---------------------------------------------------------
//
// Every fill, one row, appended before it is reported anywhere else. The book
// is the journal replayed: opening fills add (volume-weighted entry), closing
// fills subtract, and what is left is what is held.

inline constexpr const char* kLiveJournalHeader =
    "ns,model,token,symbol,role,side,qty,price,expenses,carry,why,submit_ns";

[[nodiscard]] inline std::string live_journal_row(const LivePaperFill& f) {
    char px[64], ex[64];
    std::snprintf(px, sizeof px, "%.4f", f.price);
    if (std::isfinite(f.expenses)) std::snprintf(ex, sizeof ex, "%.4f", f.expenses); else ex[0] = '\0';
    std::string why = f.reason;
    for (char& c : why) if (c == ',' || c == '\n' || c == '"') c = ';';
    std::string sym = f.symbol;
    for (char& c : sym) if (c == ',') c = ';';
    std::string model = f.model;
    for (char& c : model) if (c == ',') c = ';';
    return std::to_string(f.ns) + ',' + model + ',' + std::to_string(f.token) + ',' + sym + ','
         + (f.role == LiveFillRole::Open ? "OPEN" : "CLOSE") + ',' + std::to_string(f.side) + ',' + std::to_string(f.qty)
         + ',' + px + ',' + ex + ',' + (f.carry ? "1" : "0") + ',' + why + ',' + std::to_string(f.submit_ns);
}

/// Rebuild the held positions from the journal. Instruments no longer streamed
/// come back in `orphans`; `rows` says how many fills were read.
[[nodiscard]] inline std::vector<LivePosition> live_replay_journal(const std::string& path,
                                                                   const std::vector<LiveInstrument>& universe,
                                                                   std::vector<std::string>& orphans,
                                                                   std::size_t* rows = nullptr) {
    struct Acc { LivePosition p; double value = 0.0; std::int64_t opened = 0, closed = 0; std::string symbol; };
    std::map<std::pair<std::string, std::uint32_t>, Acc> acc;
    std::ifstream in(path);
    std::string line;
    std::size_t n = 0;
    if (std::getline(in, line) && line.rfind("ns,", 0) == 0) {
        while (std::getline(in, line)) {
            const auto c = split_csv(line);
            if (c.size() < 11) continue;
            ++n;
            const auto tok = static_cast<std::uint32_t>(std::strtoul(c[2].c_str(), nullptr, 10));
            const std::int64_t qty = std::atoll(c[6].c_str());
            if (tok == 0 || qty <= 0) continue;
            auto& a = acc[{c[1], tok}];
            a.symbol = c[3];
            if (c[4] == "OPEN") {
                if (a.opened == a.closed) {   // a fresh round trip
                    a = Acc{};
                    a.symbol = c[3];
                    a.p.model = c[1];
                    a.p.side = std::atoi(c[5].c_str());
                    a.p.entry_ns = std::atoll(c[0].c_str());
                    a.p.carry = c[9] == "1";
                    a.p.why_in = c[10];
                    a.p.decided_ns = c.size() > 11 ? std::atoll(c[11].c_str()) : a.p.entry_ns;
                }
                a.opened += qty;
                a.value += std::atof(c[7].c_str()) * static_cast<double>(qty);
                a.p.entry_expenses += c[8].empty() ? std::numeric_limits<double>::quiet_NaN() : std::atof(c[8].c_str());
            } else {
                a.closed += qty;
            }
        }
    }
    if (rows) *rows = n;
    std::vector<LivePosition> out;
    for (auto& [k, a] : acc) {
        const std::int64_t held = a.opened - a.closed;
        if (held <= 0 || a.opened <= 0) continue;
        const LiveInstrument* found = nullptr;
        for (const auto& i : universe) if (i.token == k.second) { found = &i; break; }
        if (found == nullptr) { orphans.push_back(k.first + " " + a.symbol); continue; }
        LivePosition p = a.p;
        p.inst = *found;
        p.qty = held;
        p.entry = a.value / static_cast<double>(a.opened);
        p.state = LivePosState::Open;
        p.want_qty = held;
        out.push_back(p);
    }
    return out;
}

// ---- held positions, as a snapshot for people (the journal is the record) ----

inline constexpr const char* kLivePositionsHeader =
    "model,token,symbol,side,qty,entry,entry_ns,entry_expenses,carry,why_in";

[[nodiscard]] inline bool live_write_positions(const std::string& path, const std::vector<LivePosition>& ps) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << kLivePositionsHeader << '\n';
        char num[64];
        for (const auto& p : ps) {
            if (!p.filled()) continue;
            std::snprintf(num, sizeof num, "%.2f", p.entry);
            f << p.model << ',' << p.inst.token << ',' << p.inst.symbol << ',' << p.side << ',' << p.qty << ','
              << num << ',' << p.entry_ns << ',';
            std::snprintf(num, sizeof num, "%.2f", p.entry_expenses);
            f << num << ',' << (p.carry ? 1 : 0) << ',' << p.why_in << '\n';
        }
        f.flush();
        if (!f) return false;
    }
    return live_replace_file(tmp, path);
}

/// The snapshot back from disk (a tree from before the journal existed).
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
        p.decided_ns = p.entry_ns; p.want_qty = p.qty;
        if (p.side != 0 && p.qty > 0) out.push_back(p);
    }
    return out;
}

} // namespace altair::live
