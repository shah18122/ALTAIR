// live/bank_pairs.hpp -- bank-stock pairs reversion, paper: one model per pair of
// ICICIBANK, KOTAKBANK, AXISBANK, SBIN, INDUSINDBK (10 pairs), traded in stock futures.
//
// WHERE IT COMES FROM. research/equity/PAIRS_RESULTS.md: rule P6 on 63 same-sector pairs failed
// its pre-registered bar (monthly t 1.3 dev, 0.85 in 2023), but the bank pairs were positive on
// every split (+61 bp a trade 2020+2022, +51 bp 2023, after 10 bp). That is exploratory, so this
// model is PAPER, a forward test with the rule unchanged: judge it on ~30 paper trades against
// /mnt/project-files/pairs/bank_pairs_trade_log.csv before anyone switches it to LIVE.
//
// THE RULE (unchanged from research/equity/pairs.py):
//   s = log(close A / close B), one value per session (the stock's last trade at ~15:29).
//   z = (s_today - mean of the last 20) / their sd (sample).
//   Flat and |z| > 2 at a close: next morning at 09:20 sell the rich leg's future and buy the
//   cheap one's, rupee-matched by whole lots. In a pair: exit at 09:20 after a close with
//   |z| < 0.5, or after the 11th close since entry (10 sessions held). No re-entry the morning
//   of an exit. Skipped when either leg moved more than 20 % overnight in the last 19 closes
//   (a split or bonus in unadjusted prices).
//
// CLOSES. BankPairsCloses keeps every leg's session closes: FYERS daily candles from
// data/pairs/<sym>/1d (ops/linux/fetch.py universe --go) as the warm-up, overwritten by the
// stock's own last trade the engine sees from 15:25 on each day, saved to
// <live dir>/bank_pairs/closes.csv so a restart keeps them. Every decision, with its z and why,
// goes to <live dir>/bank_pairs/decisions.csv.
#pragma once

#include <live/engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace altair::live {

inline const std::vector<std::string> kBankPairStocks = {"ICICIBANK", "KOTAKBANK", "AXISBANK", "SBIN", "INDUSINDBK"};

struct BankPairRule {
    int lookback = 20;
    double entry_z = 2.0, exit_z = 0.5;
    int max_closes = 11;            ///< exit after the 11th close since entry (10 sessions held)
    double corp_move = 0.20;        ///< |overnight log move| treated as a corporate action
    int decide_minute = 9 * 60 + 20, last_decide_minute = 10 * 60;   ///< waits out a stale feed until 10:00
};

/// z of the last value against the last `n` values (sample sd); NaN when there are too few.
[[nodiscard]] inline double bank_pair_z(const std::vector<double>& s, int n = 20) {
    if (n < 2 || s.size() < static_cast<std::size_t>(n)) return std::numeric_limits<double>::quiet_NaN();
    double m = 0.0;
    for (std::size_t i = s.size() - n; i < s.size(); ++i) m += s[i];
    m /= n;
    double v = 0.0;
    for (std::size_t i = s.size() - n; i < s.size(); ++i) v += (s[i] - m) * (s[i] - m);
    const double sd = std::sqrt(v / (n - 1));
    return sd > 0.0 ? (s.back() - m) / sd : std::numeric_limits<double>::quiet_NaN();
}

/// What to do this morning, from the closes up to yesterday. Pure, so tests and the replay
/// check it against research/equity/pairs.py.
struct BankPairAction {
    enum Kind { None, Enter, Exit } kind = None;
    int side_a = 0;          ///< Enter: +1 buy A / sell B, -1 sell A / buy B
    double z = std::numeric_limits<double>::quiet_NaN();
    std::string why;
};

/// `ca`, `cb`: the two legs' closes, oldest first, same days, up to yesterday.
/// `in_pair`: holding the pair; `closes_since_entry`: closes from the entry day on.
/// `exited_today`: an exit was made this morning (no re-entry the same morning).
[[nodiscard]] inline BankPairAction bank_pair_decide(const std::vector<double>& ca, const std::vector<double>& cb,
                                                     bool in_pair, int closes_since_entry, const BankPairRule& r = {}) {
    BankPairAction a;
    std::vector<double> s;
    s.reserve(ca.size());
    for (std::size_t i = 0; i < ca.size() && i < cb.size(); ++i)
        s.push_back(ca[i] > 0.0 && cb[i] > 0.0 ? std::log(ca[i] / cb[i]) : std::numeric_limits<double>::quiet_NaN());
    a.z = bank_pair_z(s, r.lookback);
    char b[160];
    if (in_pair) {
        if (closes_since_entry >= r.max_closes) {
            a.kind = BankPairAction::Exit;
            std::snprintf(b, sizeof b, "held %d closes (max %d), z %+.2f", closes_since_entry, r.max_closes, a.z);
        } else if (std::isfinite(a.z) && std::fabs(a.z) < r.exit_z) {
            a.kind = BankPairAction::Exit;
            std::snprintf(b, sizeof b, "spread back: |z| %.2f < %.2f", std::fabs(a.z), r.exit_z);
        } else {
            std::snprintf(b, sizeof b, "holding: z %+.2f, %d of %d closes", a.z, closes_since_entry, r.max_closes);
        }
        a.why = b;
        return a;
    }
    if (!std::isfinite(a.z)) { a.why = "need " + std::to_string(r.lookback) + " closes of both legs"; return a; }
    // corporate action in the window: the 19 overnight moves inside the last 20 closes
    for (std::size_t i = s.size() - static_cast<std::size_t>(r.lookback) + 1; i < s.size(); ++i)
        for (const auto* c : {&ca, &cb})
            if (std::fabs(std::log((*c)[i] / (*c)[i - 1])) > r.corp_move) { a.why = "split/bonus in the window: skipped"; return a; }
    if (std::fabs(a.z) <= r.entry_z) {
        std::snprintf(b, sizeof b, "flat: |z| %.2f <= %.2f", std::fabs(a.z), r.entry_z);
        a.why = b;
        return a;
    }
    a.kind = BankPairAction::Enter;
    a.side_a = a.z > 0.0 ? -1 : 1;   // A rich against B: sell A, buy B
    std::snprintf(b, sizeof b, "z %+.2f beyond %.1f: %s", a.z, r.entry_z, a.side_a < 0 ? "A rich, sell A / buy B" : "A cheap, buy A / sell B");
    a.why = b;
    return a;
}

/// Session closes of the pair legs: FYERS daily warm-up, then the engine's own last trades.
class BankPairsCloses final : public LiveModel {
public:
    using Series = std::map<std::int64_t, double>;   ///< day -> close

    BankPairsCloses(std::map<std::string, Series> warm, std::string dir) : c_(std::move(warm)), dir_(std::move(dir)) {
        load_own();
    }

    [[nodiscard]] std::string name() const override { return "Bank pairs closes"; }
    [[nodiscard]] std::string family() const override { return "pairs"; }

    void on_minute(LiveEngine& e, int m) override {
        if (m < 15 * 60 + 25) return;
        bool any = false;
        for (const auto& sym : kBankPairStocks) {
            const LiveInstrument* eq = equity(e, sym);
            const double px = eq ? e.ltp(eq->token) : 0.0;
            if (px > 0.0) { c_[sym][e.today()] = px; own_[sym][e.today()] = px; any = true; }
        }
        if (any) save_own();
    }

    [[nodiscard]] LiveModelView view(const LiveEngine&) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = "recording";
        std::string f;
        for (const auto& sym : kBankPairStocks) {
            const auto it = c_.find(sym);
            f += sym + " " + std::to_string(it == c_.end() ? 0 : it->second.size()) + "  ";
        }
        v.signal = f;
        v.reason = "Closes per stock (FYERS daily + own 15:25+ last trades). 20 are needed.";
        return v;
    }

    /// Both legs' closes on the days both have, before `before`, oldest first; and those days.
    void pair(const std::string& a, const std::string& b, std::int64_t before, std::vector<double>& ca,
              std::vector<double>& cb, std::vector<std::int64_t>& days) const {
        ca.clear(); cb.clear(); days.clear();
        const auto ia = c_.find(a), ib = c_.find(b);
        if (ia == c_.end() || ib == c_.end()) return;
        for (const auto& [d, pa] : ia->second) {
            if (d >= before) break;
            const auto j = ib->second.find(d);
            if (j == ib->second.end()) continue;
            ca.push_back(pa); cb.push_back(j->second); days.push_back(d);
        }
    }

    [[nodiscard]] static const LiveInstrument* equity(const LiveEngine& e, const std::string& sym) {
        for (const auto& in : e.universe())
            if (in.kind == LiveKind::Equity && in.symbol == sym && in.fyers.rfind("NSE:", 0) == 0) return &in;
        return nullptr;
    }

private:
    [[nodiscard]] std::filesystem::path own_path() const { return std::filesystem::path(dir_) / "closes.csv"; }
    void load_own() {
        if (dir_.empty()) return;
        std::ifstream in(own_path());
        std::string line;
        while (std::getline(in, line)) {
            const auto c1 = line.find(','), c2 = line.find(',', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos || line[0] < '0' || line[0] > '9') continue;
            const std::int64_t d = std::atoll(line.substr(0, c1).c_str());
            const std::string sym = line.substr(c1 + 1, c2 - c1 - 1);
            const double px = std::atof(line.c_str() + c2 + 1);
            if (d > 0 && px > 0.0) { c_[sym][d] = px; own_[sym][d] = px; }
        }
    }
    void save_own() const {
        if (dir_.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        const auto tmp = own_path().string() + ".tmp";
        {
            std::ofstream o(tmp, std::ios::trunc);
            o << "day_index,symbol,close\n";
            for (const auto& [sym, ser] : own_)
                for (const auto& [d, px] : ser) o << d << ',' << sym << ',' << px << '\n';
        }
        std::filesystem::rename(tmp, own_path(), ec);
    }

    std::map<std::string, Series> c_, own_;
    std::string dir_;
};

/// One pair, traded in the two stocks' futures, carried overnight. Paper unless switched.
class LiveBankPairModel final : public LiveModel {
public:
    LiveBankPairModel(std::string a, std::string b, const BankPairsCloses* closes, std::string dir, BankPairRule r = {})
        : a_(std::move(a)), b_(std::move(b)), closes_(closes), dir_(std::move(dir)), r_(r) {}

    [[nodiscard]] std::string name() const override { return "Bank pair " + a_ + "/" + b_; }
    [[nodiscard]] std::string family() const override { return "pairs"; }
    [[nodiscard]] bool carry() const override { return true; }

    void on_minute(LiveEngine& e, int m) override {
        if (decided_day_ == e.today() || m < r_.decide_minute || m > r_.last_decide_minute) return;
        if (e.stale() || e.paused()) { note_ = "waiting for a fresh feed to decide"; return; }
        decided_day_ = e.today();
        std::vector<double> ca, cb;
        std::vector<std::int64_t> days;
        closes_->pair(a_, b_, e.today(), ca, cb, days);
        const bool in = e.book().engaged(name());
        int since = 0;
        if (in) {
            std::int64_t entry_day = e.today();
            for (const auto& p : e.book().positions())
                if (p.model == name()) entry_day = std::min(entry_day, live_day_of(live_ist_minute_index(p.decided_ns ? p.decided_ns : p.entry_ns)));
            for (const auto d : days) since += d >= entry_day ? 1 : 0;
        }
        const auto act = bank_pair_decide(ca, cb, in, since, r_);
        z_ = act.z;
        std::string what = "none";
        if (act.kind == BankPairAction::Exit) {
            e.book().close_if([this](const LivePosition& p) { return p.model == name(); }, e.clock_ns(), act.why);
            what = "EXIT";
        } else if (act.kind == BankPairAction::Enter) {
            what = enter(e, act) ? "ENTRY" : "ENTRY refused";
        }
        note_ = act.why + (what == "ENTRY refused" ? " -- " + refused_ : "");
        e.note_decision(name(), what + ": " + note_);
        log(e, act, what);
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = e.book().flat(name()) ? "watching" : "in position";
        v.signal = std::isfinite(z_) ? "z " + live_fmt::num(z_) : "no z yet";
        v.reason = note_.empty() ? "Decides at 09:20 on yesterday's closes: in at |z| > 2, out at |z| < 0.5 or after 10 sessions. Paper forward test (research/equity/PAIRS_RESULTS.md)." : note_;
        return v;
    }

private:
    bool enter(LiveEngine& e, const BankPairAction& act) {
        const LiveInstrument* fa = e.carry_future(a_);
        const LiveInstrument* fb = e.carry_future(b_);
        if (fa == nullptr || fb == nullptr) { refused_ = "no future of " + std::string(fa ? b_ : a_) + " in the feed"; return false; }
        const double pa = e.mid(fa->token), pb = e.mid(fb->token);
        if (!(pa > 0.0) || !(pb > 0.0)) { refused_ = "a future has no price"; return false; }
        // rupee-matched by whole lots: the bigger lot is one, the other the nearest whole number (>= 1)
        const double na = pa * static_cast<double>(fa->lot), nb = pb * static_cast<double>(fb->lot);
        std::int64_t la = 1, lb = 1;
        if (na >= nb) lb = std::max<std::int64_t>(1, std::llround(na / nb));
        else la = std::max<std::int64_t>(1, std::llround(nb / na));
        const std::string why = name() + " " + act.why;
        std::string w;
        if (!e.book().open(name(), *fa, act.side_a, la, e.clock_ns(), why, true, &w)
            || !e.book().open(name(), *fb, -act.side_a, lb, e.clock_ns(), why, true, &w)) {
            e.book().close_if([this](const LivePosition& p) { return p.model == name(); }, e.clock_ns(), "leg refused");
            refused_ = w.empty() ? "a leg was refused" : w;
            return false;
        }
        char b[160];
        std::snprintf(b, sizeof b, " (%lld lot(s) %s Rs %.0f vs %lld lot(s) %s Rs %.0f)", static_cast<long long>(la), a_.c_str(),
                      na * la, static_cast<long long>(lb), b_.c_str(), nb * lb);
        sizing_ = b;
        return true;
    }

    void log(const LiveEngine& e, const BankPairAction& act, const std::string& what) const {
        if (dir_.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        const auto path = std::filesystem::path(dir_) / "decisions.csv";
        const bool fresh = !std::filesystem::exists(path);
        std::ofstream o(path, std::ios::app);
        if (fresh) o << "day,pair,z,action,side_a,reason\n";
        std::string why = act.why + (what == "ENTRY" ? sizing_ : what == "ENTRY refused" ? " -- " + refused_ : "");
        for (auto& ch : why) if (ch == ',' || ch == '\n') ch = ';';
        o << day_text(e.today()) << ',' << a_ << '/' << b_ << ',' << live_fmt::num(act.z, 3) << ',' << what << ','
          << act.side_a << ',' << why << '\n';
    }

    std::string a_, b_;
    const BankPairsCloses* closes_;
    std::string dir_;
    BankPairRule r_;
    std::int64_t decided_day_ = -1;
    double z_ = std::numeric_limits<double>::quiet_NaN();
    std::string note_, refused_, sizing_;
};

} // namespace altair::live
