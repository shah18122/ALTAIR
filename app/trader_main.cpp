// app/trader_main.cpp -- altair_trader: models over every model's output,
// trained and traded step by step, walk-forward, up to the last close in the
// data -- daily at the close and intraday at each hourly bar, on paper.
//
// WHAT IT READS.
//   * The forecast curriculum's out-of-sample record (altair_forecast_curriculum
//     writes <logs>/<track>.csv): every base model's call on every daily and
//     hourly track, made before its outcome was known. Each (track, model) is
//     one input: its conviction 2 * p_up - 1, or +-1 from a bare call; 0 where
//     the model had not started. "Coin flip" is left out.
//   * Implied and realised volatility (app/ivrv_feed.hpp) for NIFTY and
//     BANKNIFTY: realised from the dataset's five-minute bars, INDIA VIX, the
//     bhavcopy's 30-day ATM IV and, once recorded long enough, the FYERS
//     chain's. Each joins as of the decision, standardised on past steps only.
//   * The hidden Markov model's regime (models/hmm_regime.hpp): P(stress)
//     walk-forward on each index's daily closes, joined the same way.
//
// WHAT IT DOES, EACH STEP, IN ORDER (models/meta_learners.hpp):
//   1. the last step's outcome is now known: every candidate scores the call
//      it made, learns the row, and refits when due -- the ridge stack with
//      forgetting (models/meta_trader.hpp) every step; a temporal CNN over the
//      last ten steps of every input, and an analog search for the most
//      similar past moments, every quarter;
//   2. this step's call: the candidate with the least recent prequential
//      error -- "no change" among them -- gives the expected move, in bp;
//   3. trade one lot of the index future, model-driven only:
//        flat -> enter when |expected move| beats the round trip's expenses
//                (both fills priced by config/charges.toml, in bp);
//        held -> keep while the expected move still points the held way;
//                exit when it turns, and enter the other way only if that
//                move beats the expenses;
//        intraday -> no entry at the session's last hourly decision (15:15),
//                and anything held is closed there: nothing is carried overnight.
//      No threshold is tuned: the expenses are the exchange's and the broker's,
//      the move is the model's.
//
// --leak-check reruns every arm with everything after two cut points changed
// -- the logs' later calls flipped, every later price moved, the cut step's own
// outcome moved, the later volatility inputs moved -- and needs every call,
// every candidate's call and every position decided up to the cut to come out
// bit for bit the same. Exit 3 if one does not.
//
// WHAT IT WRITES (<out>, default data/verified/altair_trader):
//   trades.csv      one row per trip: dates, side, lots, prices, days held,
//                   rolls, gross, every expense head, net, the call that opened
//                   it, why it closed, the arm, the candidate that called it
//   days.csv        every step per arm: the candidate trading, the call, each
//                   candidate's call, the move, the position, the P&L
//   candidates.csv  every candidate on every arm: RMSE against no change on
//                   the same steps, direction, steps it traded, its last fit
//   analogs.csv     the latest step's ten most similar past moments, per arm
//   weights.csv     the ridge stack's latest weight on every input
//   summary.txt     trips, win rate, gross/expenses/net, Sharpe, Sortino,
//                   drawdown, buy and hold, the candidates, the leak check
//
// LIMITS, SAID: index futures are priced on the index (the futures' basis is
// left out); a daily trip held over a month end pays a roll (two more fills);
// a trip open at the end of the data is closed at the last close; intraday
// fills are at the hourly bar's close, not a quote. The base models are
// refitted by the curriculum in doubling stages; these are refitted here.

#include <app/demo_costs.hpp>
#include <app/forecast_tracks.hpp>
#include <app/ivrv_feed.hpp>
#include <models/curriculum_feeds.hpp>
#include <models/hmm_regime.hpp>
#include <models/meta_learners.hpp>
#include <models/meta_trader.hpp>
#include <risk/charges_toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace dc = altair::demo_costs;
namespace da = altair::data_audit;
namespace ft = altair::forecast_tracks;
namespace iv = altair::ivrv;
namespace mt = altair::meta;

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else q = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            q = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

constexpr std::int64_t kCloseSec = 15 * 3600 + 30 * 60;

/// When a step is decided, IST seconds: a daily key ("2026-09-24") at the
/// 15:30 close, an intraday key (a full time) at that time.
std::int64_t stamp_of(const std::string& key) {
    const auto t = da::parse_audit_time(key);
    if (!t) return 0;
    return key.size() == 10 ? da::audit_day(*t) * 86'400 + kCloseSec : *t;
}

/// One track's rows by step: per model, the conviction; per step, the prices.
struct Track {
    std::string name;
    std::map<std::string, std::map<std::string, double>> conviction;   ///< key -> model -> conviction
    std::map<std::string, std::pair<double, double>> closes;           ///< key -> (last, next)
};

bool read_track(const fs::path& path, bool intraday, Track& t, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path.string(); return false; }
    std::string line;
    if (!std::getline(in, line)) { err = path.string() + " is empty"; return false; }
    const auto head = split_csv(line);
    const auto col = [&](const char* n) -> int {
        for (std::size_t i = 0; i < head.size(); ++i) if (head[i] == n) return static_cast<int>(i);
        return -1;
    };
    const int c_time = col("time"), c_model = col("model"), c_last = col("last_price"), c_next = col("next_price"),
              c_p = col("p_up"), c_call = col("call");
    if (c_time < 0 || c_model < 0 || c_last < 0 || c_next < 0 || c_p < 0 || c_call < 0) {
        err = path.string() + " is not a forecast log (missing columns)";
        return false;
    }
    t.name = path.stem().string();
    while (std::getline(in, line)) {
        const auto f = split_csv(line);
        if (f.size() <= static_cast<std::size_t>(std::max({c_time, c_model, c_last, c_next, c_p, c_call}))) continue;
        const std::string& time = f[static_cast<std::size_t>(c_time)];
        const std::string key = intraday ? time : time.substr(0, 10);
        const std::string& model = f[static_cast<std::size_t>(c_model)];
        if (key.size() < 10 || model == "Coin flip") continue;
        const double last = std::atof(f[static_cast<std::size_t>(c_last)].c_str());
        const double next = std::atof(f[static_cast<std::size_t>(c_next)].c_str());
        if (last > 0.0 && next > 0.0) t.closes[key] = {last, next};
        const std::string& p = f[static_cast<std::size_t>(c_p)];
        const std::string& call = f[static_cast<std::size_t>(c_call)];
        double c = 0.0;
        if (!p.empty()) c = 2.0 * std::atof(p.c_str()) - 1.0;
        else if (call == "UP") c = 1.0;
        else if (call == "DOWN") c = -1.0;
        if (std::isfinite(c)) t.conviction[key][model] = std::clamp(c, -1.0, 1.0);   // RULE 11: a conviction is -1 .. 1 by definition
    }
    return true;
}

/// The last date in dataset/spot/<index>/1d/all.csv, for the session after
/// the logs' last close.
std::string last_dataset_date(const fs::path& file) {
    std::ifstream in(file);
    std::string line, last;
    while (std::getline(in, line)) if (line.size() >= 10 && line[0] >= '0' && line[0] <= '9') last = line.substr(0, 10);
    return last;
}

/// The nearest index future's lot in the Kite master; `fallback` without it.
long long lot_from_master(const fs::path& master, const std::string& name, long long fallback) {
    std::ifstream in(master);
    std::string line;
    std::string best_exp;
    long long lot = 0;
    while (std::getline(in, line)) {
        if (line.find(",FUT,NFO-FUT,") == std::string::npos) continue;
        const auto f = split_csv(line);
        if (f.size() < 12 || f[3] != name) continue;
        if (best_exp.empty() || f[5] < best_exp) { best_exp = f[5]; lot = std::atoll(f[8].c_str()); }
    }
    return lot > 0 ? lot : fallback;
}

/// Everything the arms read: the logs and the volatility inputs.
struct Data {
    std::vector<Track> daily, intraday;
    std::vector<altair::ModelFeed> vol_daily, vol_intraday;   ///< both indices' volatility inputs
};

/// What an arm trades and on which clock.
struct Arm {
    std::string index, track, fut_name, data_dir;
    long long lot = 0;
    bool intraday = false;
};

struct Trip {
    std::string index, entry_date, exit_date;
    int side = 0;
    long long qty = 0;
    double entry = 0, exit = 0;
    int days = 0, rolls = 0;
    double mu_in = 0, cost_bp_in = 0;
    std::string why_out, model_in;
    dc::Costs costs;
    bool priced = true;
};

struct StepRow {
    std::string index, key, chosen;
    double mu = 0, y = 0, cost_bp = 0;
    int pos = 0;
    double pnl = 0, cum = 0;
    std::vector<double> cand;   ///< every candidate's call
};

struct CandStat {
    std::string name, note;
    std::size_t n = 0, chosen = 0, called = 0, right = 0;
    double sse = 0, sse_zero = 0;
};

struct ArmOut {
    std::vector<StepRow> steps;
    std::vector<Trip> trips;
    std::vector<CandStat> cands;
    std::vector<std::pair<std::string, std::string>> inputs;   ///< (source, column)
    std::vector<double> ridge_w;
    std::vector<std::vector<std::string>> analogs;             ///< rows for analogs.csv
    // Daily arms: what the timing has to beat, and the best single model.
    std::string hold_from, hold_to;
    double hold_gross = 0, hold_exp = 0;
    int hold_rolls = 0;
    std::size_t called = 0, right = 0, traded = 0, traded_right = 0;
    std::string best_single;
    double best_single_acc = 0;
    std::size_t best_single_n = 0;
};

struct Settings {
    long long lots = 1;
    mt::MetaConfig ridge;
    std::vector<altair::ChargeSchedule> schedules;
    std::string after_last;   ///< per arm, filled by the caller
};

/// Prequential standardisation: each value against the mean and spread of
/// the values before it.
struct Running {
    double n = 0, mean = 0, m2 = 0;
    double z(double v) const {
        if (n < 20) return 0.0;
        const double sd = std::sqrt(m2 / (n - 1));
        return sd > 0 ? std::clamp((v - mean) / sd, -6.0, 6.0) : 0.0;   // RULE 11: a winsor, on purpose
    }
    void add(double v) { n += 1; const double d = v - mean; mean += d / n; m2 += d * (v - mean); }
};

ArmOut run_arm(const Arm& arm, const Data& data, const Settings& st) {
    ArmOut out;
    const auto& tracks = arm.intraday ? data.intraday : data.daily;
    const auto& vols = arm.intraday ? data.vol_intraday : data.vol_daily;
    const Track* target = nullptr;
    for (const auto& t : tracks) if (t.name == arm.track) target = &t;
    if (target == nullptr || target->closes.empty()) return out;

    // The inputs: every (track, model) of this clock, then every volatility column.
    {
        std::set<std::pair<std::string, std::string>> seen;
        for (const auto& t : tracks)
            for (const auto& [d, m] : t.conviction)
                for (const auto& [model, c] : m) seen.insert({t.name, model});
        out.inputs.assign(seen.begin(), seen.end());
    }
    const std::size_t n_conv = out.inputs.size();
    for (const auto& f : vols)
        for (const auto& nm : f.names) out.inputs.push_back({f.source, nm});
    std::vector<Running> vstat(out.inputs.size() - n_conv);
    std::vector<const Track*> by_input(n_conv, nullptr);
    for (std::size_t k = 0; k < n_conv; ++k)
        for (const auto& t : tracks) if (t.name == out.inputs[k].first) by_input[k] = &t;
    const auto features_on = [&](const std::string& key) {
        std::vector<double> x(out.inputs.size(), 0.0);
        for (std::size_t k = 0; k < n_conv; ++k) {
            const auto d = by_input[k]->conviction.find(key);
            if (d == by_input[k]->conviction.end()) continue;
            const auto m = d->second.find(out.inputs[k].second);
            if (m != d->second.end()) x[k] = m->second;
        }
        const std::int64_t t = stamp_of(key);
        std::size_t col = n_conv, vs = 0;
        for (const auto& f : vols) {
            const std::size_t r = altair::feed_detail::as_of(f, t);
            for (std::size_t c = 0; c < f.cols(); ++c, ++col, ++vs) {
                if (r >= f.rows()) continue;
                const double v = static_cast<double>(f.v[r * f.cols() + c]);
                x[col] = vstat[vs].z(v);
                vstat[vs].add(v);
            }
        }
        return x;
    };

    // The candidates. Steps are sessions on the daily clock and hourly
    // decisions (six a session) on the intraday one.
    const double per = arm.intraday ? 6.0 : 1.0;
    std::vector<std::unique_ptr<mt::Learner>> cands;
    cands.push_back(std::make_unique<mt::NoChange>());
    mt::MetaConfig rc = st.ridge;
    rc.min_days = static_cast<std::size_t>(static_cast<double>(rc.min_days) * per);
    rc.score_days = static_cast<std::size_t>(static_cast<double>(rc.score_days) * per);
    rc.half_life_days *= per;
    cands.push_back(std::make_unique<mt::RidgeLearner>(out.inputs.size(), rc));
    mt::TcnConfig tc;
    tc.min_steps = rc.min_days;
    tc.refit_every = static_cast<std::size_t>(63 * per);
    cands.push_back(std::make_unique<mt::MetaTcn>(tc));
    mt::AnalogConfig ac;
    ac.min_steps = rc.min_days;
    ac.refit_every = static_cast<std::size_t>(63 * per);
    cands.push_back(std::make_unique<mt::AnalogLearner>(ac));
    if (!arm.intraday) {
        // Shindhe & Gokhale (2026): LSTM, GRU, XGBoost (here the curriculum's
        // gradient boosting) and random forest on this arm's own track,
        // stacked by linear regression, 24 months before the first fit,
        // refit monthly. models/meta_learners.hpp says what is not copied.
        std::vector<std::size_t> cols;
        for (const char* m : {"LSTM", "GRU", "Gradient boosting", "Random forest"})
            for (std::size_t k = 0; k < n_conv; ++k)
                if (out.inputs[k].first == arm.track && out.inputs[k].second == m) cols.push_back(k);
        if (!cols.empty()) cands.push_back(std::make_unique<mt::GokhaleStack>(std::move(cols)));
    }
    mt::Selector sel(std::move(cands), rc.half_life_days, rc.score_days);
    out.cands.resize(sel.size());
    for (std::size_t k = 0; k < sel.size(); ++k) out.cands[k].name = sel.candidate(k).name();

    std::vector<std::string> keys;
    for (const auto& [k, c] : target->closes) keys.push_back(k);
    const auto price_fill = [&](bool buy, double px, std::int64_t at) {
        return dc::fill(altair::Segment::Fut, buy ? altair::Side::Buy : altair::Side::Sell,
                        static_cast<double>(arm.lot * st.lots), px, at, st.schedules);
    };
    const auto round_trip_bp = [&](double px, std::int64_t at) {
        const dc::Costs b = price_fill(true, px, at), s = price_fill(false, px, at);
        if (!b.priced || !s.priced) return std::numeric_limits<double>::infinity();
        return (b.total + s.total) / (px * static_cast<double>(arm.lot * st.lots)) * 1e4;
    };
    std::map<std::string, std::pair<std::size_t, std::size_t>> single;   // model -> (right, n)
    int pos = 0;
    Trip open;
    double cum = 0.0;
    const long long q = arm.lot * st.lots;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::string& key = keys[i];
        const auto closes = target->closes.at(key);
        const double last = closes.first, next = closes.second;
        const std::int64_t at = stamp_of(key);
        const std::string next_key = i + 1 < keys.size() ? keys[i + 1]
                                   : (!arm.intraday && !st.after_last.empty() && st.after_last > key ? st.after_last : "next session");
        const bool day_end = arm.intraday && (i + 1 == keys.size() || keys[i + 1].substr(0, 10) != key.substr(0, 10));
        const double y = (next / last - 1.0) * 1e4;
        const auto call = sel.predict(features_on(key));
        const bool ready = call.chosen != 0 && call.mu != 0.0;
        const double cost_bp = round_trip_bp(last, at);
        StepRow row;
        row.index = arm.index;
        row.key = key;
        row.chosen = sel.candidate(call.chosen).name();
        row.mu = call.mu;
        row.y = y;
        row.cost_bp = cost_bp;
        for (std::size_t k = 0; k < sel.size(); ++k) {
            row.cand.push_back(sel.last_prediction(k));
            auto& cs = out.cands[k];
            if (!sel.last_live(k)) continue;
            const double p = sel.last_prediction(k);
            ++cs.n;
            cs.sse += (p - y) * (p - y);
            cs.sse_zero += y * y;
            if (p != 0.0 && y != 0.0) { ++cs.called; cs.right += (p > 0) == (y > 0) ? 1 : 0; }
        }
        ++out.cands[call.chosen].chosen;
        if (ready) {
            ++out.called;
            out.right += (call.mu > 0) == (y > 0) ? 1 : 0;
        }
        // What the timing has to beat: holding one lot over every step the
        // arm could trade (from the end of the warm-up), and the best single
        // model's direction on the steps it was called.
        if (i >= rc.min_days) {
            if (!arm.intraday) {
                if (out.hold_from.empty()) { out.hold_from = key; out.hold_exp += price_fill(true, last, at).total; }
                out.hold_to = next_key;
                out.hold_gross += static_cast<double>(q) * (next - last);
                if (next_key.size() == 10 && next_key.substr(0, 7) != key.substr(0, 7)) {
                    ++out.hold_rolls;
                    const std::int64_t nat = stamp_of(next_key);
                    out.hold_exp += price_fill(false, next, nat).total + price_fill(true, next, nat).total;
                }
                if (i + 1 == keys.size()) out.hold_exp += price_fill(false, next, at).total;
                const auto d = target->conviction.find(key);
                if (d != target->conviction.end())
                    for (const auto& [m, conv] : d->second)
                        if (conv != 0.0) { auto& s1 = single[m]; ++s1.second; s1.first += (conv > 0) == (y > 0) ? 1 : 0; }
            }
        }
        // ---- the decision at this step -----------------------------------------------
        const auto close_trip = [&](const std::string& why) {
            open.exit_date = key;
            open.exit = last;
            open.why_out = why;
            const dc::Costs f = price_fill(open.side < 0, last, at);
            open.costs.add(f);
            open.priced = open.priced && f.priced;
            out.trips.push_back(open);
            pos = 0;
        };
        if (pos != 0) {
            if (day_end) close_trip("the session's last decision: nothing is carried overnight");
            else if (!ready || call.mu * pos <= 0.0) close_trip(!ready ? "no call" : "the expected move turned against the position");
        }
        if (pos == 0 && ready && !day_end && std::fabs(call.mu) > cost_bp) {
            pos = call.mu > 0 ? 1 : -1;
            open = Trip{};
            open.index = arm.index;
            open.entry_date = key;
            open.side = pos;
            open.qty = q;
            open.entry = last;
            open.mu_in = call.mu;
            open.cost_bp_in = cost_bp;
            open.model_in = row.chosen;
            const dc::Costs f = price_fill(pos > 0, last, at);
            open.costs.add(f);
            open.priced = f.priced;
        }
        if (pos != 0) { ++out.traded; out.traded_right += (pos > 0) == (y > 0) ? 1 : 0; }
        // ---- to the next step ---------------------------------------------------------
        if (pos != 0) {
            ++open.days;
            if (!arm.intraday && next_key.size() == 10 && next_key.substr(0, 7) != key.substr(0, 7)) {
                ++open.rolls;
                const std::int64_t nat = stamp_of(next_key);
                const dc::Costs r1 = price_fill(pos < 0, next, nat), r2 = price_fill(pos > 0, next, nat);
                open.costs.add(r1);
                open.costs.add(r2);
                open.priced = open.priced && r1.priced && r2.priced;
            }
            row.pnl = static_cast<double>(pos) * static_cast<double>(q) * (next - last);
        }
        row.pos = pos;
        cum += row.pnl;
        row.cum = cum;
        out.steps.push_back(std::move(row));
        sel.learn(y);
        if (i + 1 == keys.size() && pos != 0) {
            open.exit_date = next_key;
            open.exit = next;
            open.why_out = "end of the data (closed at the last close)";
            const dc::Costs f = price_fill(open.side < 0, next, at);
            open.costs.add(f);
            open.priced = open.priced && f.priced;
            out.trips.push_back(open);
            pos = 0;
        }
    }
    for (std::size_t k = 0; k < sel.size(); ++k) out.cands[k].note = sel.candidate(k).note();
    out.ridge_w = dynamic_cast<const mt::RidgeLearner&>(sel.candidate(1)).model().weights();
    // The latest step's most similar past moments.
    const auto& an = dynamic_cast<const mt::AnalogLearner&>(sel.candidate(3));
    auto nb = an.last_neighbours();
    std::sort(nb.begin(), nb.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    for (std::size_t k = 0; k < nb.size() && k < 10; ++k) {
        if (nb[k].step >= keys.size()) continue;
        char d[32], o[32];
        std::snprintf(d, sizeof d, "%.3f", nb[k].distance);
        std::snprintf(o, sizeof o, "%.1f", nb[k].outcome);
        out.analogs.push_back({arm.index, keys.back(), std::to_string(k + 1), keys[nb[k].step], d, o});
    }
    const std::size_t warm = keys.size() > rc.min_days ? keys.size() - rc.min_days : 0;
    for (const auto& [m, rn] : single)
        if (rn.second >= warm / 2 && rn.second > 0) {
            const double accy = static_cast<double>(rn.first) / static_cast<double>(rn.second);
            if (accy > out.best_single_acc) { out.best_single_acc = accy; out.best_single = m; out.best_single_n = rn.second; }
        }
    return out;
}

/// The same data with everything after `cut` changed: later calls flipped,
/// later prices moved, the cut step's own outcome moved, later volatility
/// inputs moved. A decision up to `cut` must not see any of it.
Data other_future(const Data& d, std::int64_t cut) {
    Data o = d;
    for (auto* ts : {&o.daily, &o.intraday})
        for (auto& t : *ts) {
            std::size_t k = 0;
            for (auto& [key, pr] : t.closes) {
                const std::int64_t at = stamp_of(key);
                if (at >= cut) pr.second *= std::exp(0.02 * std::sin(1.7 * static_cast<double>(k) + 0.4));
                if (at > cut) pr.first *= std::exp(0.02 * std::sin(1.1 * static_cast<double>(k)));
                ++k;
            }
            for (auto& [key, m] : t.conviction)
                if (stamp_of(key) > cut)
                    for (auto& [model, c] : m) c = -c;
        }
    for (auto* fs_ : {&o.vol_daily, &o.vol_intraday})
        for (auto& f : *fs_)
            for (std::size_t r = 0; r < f.rows(); ++r)
                if (f.t[r] > cut)
                    for (std::size_t c = 0; c < f.cols(); ++c) f.v[r * f.cols() + c] = -f.v[r * f.cols() + c] + 0.37F;
    return o;
}

std::string fmt(double v, int dp = 2) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", dp, v);
    return b;
}

void usage(const char* exe) {
    std::printf(
        "usage: %s [--logs DIR] [--out DIR] [--dataset DIR] [--bhavcopy DIR] [--recorded-iv DIR] [--lots N]\n"
        "          [--unverified-costs] [--half-life DAYS] [--min-days N] [--daily-only] [--no-ivrv] [--all-tracks]\n"
        "          [--leak-check]\n"
        "  --logs        the forecast curriculum's forecast_log/ (default data/verified/forecast_log)\n"
        "  --out         where to write (default data/verified/altair_trader)\n"
        "  --dataset     five-minute bars for realised vol, and the session after the last close (default dataset)\n"
        "  --bhavcopy    NSE F&O bhavcopy for the 30-day ATM IV (default data/bhavcopy)\n"
        "  --recorded-iv ATM IV recorded from the FYERS chain (default data/live/iv; altair_iv_record)\n"
        "  --lots        lots per trade (default 1)\n"
        "  --daily-only  skip the intraday (hourly) arms\n"
        "  --no-ivrv     leave the implied and realised volatility inputs out\n"
        "  --all-tracks  every logged track as input, the second-pass variants too (default: the base tracks)\n"
        "  --leak-check  rerun every arm with another future after two cuts; every decision up to each\n"
        "                cut must be the same (exit 3 if not)\n"
        "  --unverified-costs  price expenses while config/charges.toml is unverified\n",
        exe);
}

} // namespace

int main(int argc, char** argv) {
    fs::path logs = "data/verified/forecast_log", out = "data/verified/altair_trader", dataset = "dataset";
    fs::path bhav_dir = "data/bhavcopy", recorded_dir = "data/live/iv";
    bool unverified = false, daily_only = false, ivrv_on = true, leak_check = false, all_tracks = false;
    Settings st;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--logs" && has) logs = argv[++i];
        else if (a == "--out" && has) out = argv[++i];
        else if (a == "--dataset" && has) dataset = argv[++i];
        else if (a == "--bhavcopy" && has) bhav_dir = argv[++i];
        else if (a == "--recorded-iv" && has) recorded_dir = argv[++i];
        else if (a == "--lots" && has) st.lots = std::max(1LL, std::atoll(argv[++i]));
        else if (a == "--half-life" && has) st.ridge.half_life_days = std::max(5.0, std::atof(argv[++i]));
        else if (a == "--min-days" && has) st.ridge.min_days = static_cast<std::size_t>(std::max(20LL, std::atoll(argv[++i])));
        else if (a == "--unverified-costs") unverified = true;
        else if (a == "--daily-only") daily_only = true;
        else if (a == "--no-ivrv") ivrv_on = false;
        else if (a == "--leak-check") leak_check = true;
        else if (a == "--all-tracks") all_tracks = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }

    // ---- expenses --------------------------------------------------------------------
    const auto rep = altair::load_charges_file("config/charges.toml", st.schedules);
    std::string cost_tag;
    bool costs_allowed = false;
    if (!rep) {
        std::printf("config/charges.toml did not load: %s -- gross P&L only, and no trade can beat unknown expenses\n",
                    altair::charges_error_text(rep.error()));
    } else if (rep->verified) {
        costs_allowed = true;
        cost_tag = "verified";
    } else if (unverified) {
        for (auto& s : st.schedules) s.verified = true;
        costs_allowed = true;
        cost_tag = "UNVERIFIED";
    } else {
        std::printf("config/charges.toml is UNVERIFIED: pass --unverified-costs to price expenses anyway\n");
    }
    if (!costs_allowed) return 2;   // the decision needs the expenses: no costs, no trades

    // ---- the inputs ------------------------------------------------------------------
    Data data;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs, ec)) {
        const std::string n = e.path().filename().string();
        if (e.path().extension() != ".csv") continue;
        const std::string stem = e.path().stem().string();
        const bool daily = n.find("_daily") != std::string::npos;
        const bool hourly = n.find("_hourly") != std::string::npos;
        if (!daily && !(hourly && !daily_only)) continue;
        // The base tracks by default: the second-pass variants mostly repeat
        // their models' calls with other inputs, and every column costs the
        // daily refit its square.
        const bool base = stem.ends_with("_daily") || stem.ends_with("_hourly");
        if (!base && !all_tracks) continue;
        Track t;
        std::string err;
        if (!read_track(e.path(), hourly, t, err)) { std::printf("  %s\n", err.c_str()); continue; }
        (daily ? data.daily : data.intraday).push_back(std::move(t));
    }
    for (auto* v : {&data.daily, &data.intraday})
        std::sort(v->begin(), v->end(), [](const Track& a, const Track& b) { return a.name < b.name; });
    if (data.daily.empty()) {
        std::printf("no daily forecast logs in %s: run altair_forecast_curriculum first\n", logs.string().c_str());
        return 2;
    }
    std::string vol_text;
    if (ivrv_on) {
        ft::TrackInfo unused;
        auto vix = ft::load_bars(dataset / "spot/indiavix/1d", da::kDailyTf, unused);
        ft::clean_bars(vix, unused);
        for (const auto& [inst, dir] : {std::pair<std::string, std::string>{"NIFTY", "spot/nifty"}, {"BANKNIFTY", "spot/banknifty"}}) {
            ft::TrackInfo info;
            auto five = ft::load_bars(dataset / dir / "5m", 5, info);
            ft::clean_bars(five, info);
            if (five.empty()) continue;
            const auto s = iv::session_rv(five);
            data.vol_daily.push_back(iv::rv_daily_feed(inst + " RV", s, inst == "NIFTY" && !vix.empty() ? &vix : nullptr));
            data.vol_intraday.push_back(data.vol_daily.back());
            data.vol_intraday.push_back(iv::rv_intraday_feed(inst + " RV today", five, s));
            // The HMM's regime (models/hmm_regime.hpp): P(stress) walk-forward on
            // the daily closes, known at each close.
            {
                std::vector<std::int64_t> bt, rday;
                std::vector<double> bc, rret;
                for (const auto& b : five) { bt.push_back(b.t); bc.push_back(b.c); }
                altair::daily_log_returns(bt, bc, rday, rret);
                const auto reg = altair::walk_forward_regimes(rday, rret);
                if (!reg.empty()) {
                    data.vol_daily.push_back(altair::regime_daily_feed(inst + " regime", reg));
                    data.vol_intraday.push_back(data.vol_daily.back());
                    vol_text += "  " + inst + ": HMM regime on " + std::to_string(reg.size()) + " days\n";
                }
            }
            altair::bhavcopy::LoadReport br;
            const auto bhav = iv::iv_from_bhavcopy(altair::bhavcopy::load_dir(bhav_dir, inst, br));
            if (!bhav.empty()) {
                data.vol_daily.push_back(iv::iv_daily_feed(inst + " IV (bhavcopy)", bhav, s));
                data.vol_intraday.push_back(data.vol_daily.back());
            }
            const auto rec = iv::load_recorded_iv(recorded_dir, inst);
            const std::size_t sessions = iv::sessions_covered(rec);
            if (sessions >= 60) data.vol_intraday.push_back(iv::iv_intraday_feed(inst + " IV (FYERS)", rec, s));
            vol_text += "  " + inst + ": realised from " + std::to_string(five.size()) + " five-minute bars"
                      + (inst == "NIFTY" && !vix.empty() ? ", INDIA VIX" : "") + "; bhavcopy IV on " + std::to_string(bhav.size())
                      + " days; FYERS IV on " + std::to_string(sessions) + " sessions"
                      + (sessions >= 60 ? "" : " (joins at 60)") + "\n";
        }
    }
    std::printf("altair_trader: %zu daily and %zu hourly tracks from %s; %zu daily and %zu intraday volatility inputs\n%s",
                data.daily.size(), data.intraday.size(), logs.string().c_str(), data.vol_daily.size(), data.vol_intraday.size(),
                vol_text.c_str());

    // ---- the arms --------------------------------------------------------------------
    const long long nifty_lot = lot_from_master("data/instruments.csv", "NIFTY", 65);
    const long long bank_lot = lot_from_master("data/instruments.csv", "BANKNIFTY", 30);
    std::vector<Arm> arms{{"NIFTY", "nifty_daily", "NIFTY", "nifty", nifty_lot, false},
                          {"BANKNIFTY", "banknifty_daily", "BANKNIFTY", "banknifty", bank_lot, false}};
    if (!daily_only) {
        arms.push_back({"NIFTY intraday", "nifty_hourly", "NIFTY", "nifty", nifty_lot, true});
        arms.push_back({"BANKNIFTY intraday", "banknifty_hourly", "BANKNIFTY", "banknifty", bank_lot, true});
    }
    std::vector<ArmOut> results;
    std::vector<std::string> leak_lines;
    bool leak_clean = true;
    for (const auto& arm : arms) {
        Settings s = st;
        s.after_last = last_dataset_date(dataset / "spot" / arm.data_dir / "1d" / "all.csv");
        ArmOut r = run_arm(arm, data, s);
        if (r.steps.empty()) {
            std::printf("  %s: no %s.csv in the logs, not traded\n", arm.index.c_str(), arm.track.c_str());
            results.push_back(std::move(r));
            continue;
        }
        std::printf("  %s: %zu steps, %zu trips\n", arm.index.c_str(), r.steps.size(), r.trips.size());
        if (leak_check) {
            for (const double share : {0.5, 0.75}) {
                const std::size_t ci = static_cast<std::size_t>(share * static_cast<double>(r.steps.size()));
                const std::string& ck = r.steps[ci].key;
                const ArmOut o = run_arm(arm, other_future(data, stamp_of(ck)), s);
                std::size_t compared = 0, differ = 0;
                std::string first;
                for (std::size_t i = 0; i <= ci && i < o.steps.size(); ++i) {
                    const auto& a = r.steps[i];
                    const auto& b = o.steps[i];
                    ++compared;
                    if (a.key != b.key || a.chosen != b.chosen || a.mu != b.mu || a.pos != b.pos || a.cand != b.cand) {
                        if (differ++ == 0) first = a.key;
                    }
                }
                const bool clean = differ == 0 && compared == ci + 1;
                leak_clean = leak_clean && clean;
                leak_lines.push_back("  " + arm.index + " cut at " + ck + ": " + std::to_string(compared)
                                     + " decisions compared with another future after it -- "
                                     + (clean ? "all identical" : "LEAK, first at " + first));
                std::printf("%s\n", leak_lines.back().c_str());
            }
        }
        results.push_back(std::move(r));
    }

    // ---- write ------------------------------------------------------------------------
    fs::create_directories(out, ec);
    {
        std::ofstream f(out / "trades.csv");
        f << "date,index,side,lots,qty,entry_date,entry,exit_date,exit,days,rolls,gross,expenses,net,"
             "brokerage,stt,exchange_txn,sebi,stamp,ipft,gst,expected_bp,cost_bp,why_in,why_out,costs,arm,model\n";
        for (std::size_t a = 0; a < arms.size(); ++a)
            for (const auto& t : results[a].trips) {
                const double gross = static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry);
                f << t.exit_date << ',' << t.index << ',' << (t.side > 0 ? "long" : "short") << ',' << st.lots << ',' << t.qty
                  << ',' << t.entry_date << ',' << fmt(t.entry) << ',' << t.exit_date << ',' << fmt(t.exit) << ',' << t.days
                  << ',' << t.rolls << ',' << fmt(gross) << ',';
                if (t.priced) {
                    f << fmt(t.costs.total) << ',' << fmt(gross - t.costs.total) << ',' << fmt(t.costs.brokerage) << ','
                      << fmt(t.costs.stt) << ',' << fmt(t.costs.exchange) << ',' << fmt(t.costs.sebi) << ','
                      << fmt(t.costs.stamp) << ',' << fmt(t.costs.ipft) << ',' << fmt(t.costs.gst);
                } else {
                    f << ",,,,,,,,";
                }
                f << ',' << fmt(t.mu_in, 1) << ',' << fmt(t.cost_bp_in, 2) << ",\"" << t.model_in << ": expected "
                  << fmt(t.mu_in, 1) << " bp over a " << fmt(t.cost_bp_in, 2) << " bp round trip\",\"" << t.why_out << "\","
                  << cost_tag << ',' << (arms[a].intraday ? "intraday" : "daily") << ",\"" << t.model_in << "\"\n";
            }
    }
    {
        std::ofstream f(out / "days.csv");
        f << "date,index,model,expected_bp,move_bp,cost_bp,position,pnl,cum_gross";
        if (!results.empty())
            for (const auto& c : results.front().cands) f << ",\"" << c.name << " bp\"";
        f << '\n';
        for (const auto& r : results)
            for (const auto& d : r.steps) {
                f << d.key << ',' << d.index << ",\"" << d.chosen << "\"," << fmt(d.mu, 2) << ',' << fmt(d.y, 1) << ','
                  << fmt(d.cost_bp, 2) << ',' << d.pos << ',' << fmt(d.pnl) << ',' << fmt(d.cum);
                for (const double c : d.cand) f << ',' << fmt(c, 2);
                f << '\n';
            }
    }
    {
        std::ofstream f(out / "candidates.csv");
        f << "index,candidate,steps,rmse_bp,no_change_rmse_bp,skill_pct,direction_pct,called,steps_traded_on,last_fit\n";
        for (std::size_t a = 0; a < arms.size(); ++a)
            for (const auto& c : results[a].cands) {
                const double rm = c.n ? std::sqrt(c.sse / static_cast<double>(c.n)) : 0.0;
                const double rz = c.n ? std::sqrt(c.sse_zero / static_cast<double>(c.n)) : 0.0;
                f << arms[a].index << ",\"" << c.name << "\"," << c.n << ',' << fmt(rm, 2) << ',' << fmt(rz, 2) << ','
                  << (rz > 0 ? fmt(100.0 * (1.0 - rm / rz), 3) : std::string()) << ','
                  << (c.called ? fmt(100.0 * static_cast<double>(c.right) / static_cast<double>(c.called), 2) : std::string())
                  << ',' << c.called << ',' << c.chosen << ",\"" << c.note << "\"\n";
            }
    }
    {
        std::ofstream f(out / "analogs.csv");
        f << "index,latest_step,rank,similar_step,distance,move_after_bp\n";
        for (const auto& r : results)
            for (const auto& row : r.analogs)
                f << row[0] << ',' << row[1] << ',' << row[2] << ',' << row[3] << ',' << row[4] << ',' << row[5] << '\n';
    }
    {
        std::ofstream f(out / "weights.csv");
        f << "index,track,model,weight\n";
        for (std::size_t a = 0; a < arms.size(); ++a) {
            const auto& w = results[a].ridge_w;
            const auto& in = results[a].inputs;
            if (w.empty()) continue;
            f << arms[a].index << ",intercept,," << fmt(w[0], 3) << '\n';
            std::vector<std::size_t> order(in.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return std::fabs(w[x + 1]) > std::fabs(w[y + 1]); });
            for (const std::size_t k : order)
                f << arms[a].index << ',' << in[k].first << ",\"" << in[k].second << "\"," << fmt(w[k + 1], 3) << '\n';
        }
    }

    // ---- summary -----------------------------------------------------------------------
    std::ostringstream s;
    s << "altair_trader -- models over every model's output, refitted walk-forward, traded on paper\n"
      << "candidates each step: no change, the ridge stack, a temporal CNN, the analogs; the one with the least\n"
      << "recent prequential error trades. Expenses: config/charges.toml (" << cost_tag
      << "); one round trip in bp is the bar every entry must clear\n\n";
    for (std::size_t a = 0; a < arms.size(); ++a) {
        const auto& r = results[a];
        if (r.steps.empty()) continue;
        double gross = 0, exp = 0;
        std::size_t n = 0, wins = 0, losses = 0;
        double win_sum = 0, loss_sum = 0;
        for (const auto& t : r.trips) {
            const double g = static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry);
            const double net = g - t.costs.total;
            gross += g;
            exp += t.costs.total;
            ++n;
            if (net > 0) { ++wins; win_sum += net; } else { ++losses; loss_sum += net; }
        }
        s << arms[a].index << " (" << (arms[a].intraday ? "hourly decisions, flat every night" : "at each close")
          << ", lot " << arms[a].lot << " x " << st.lots << ", " << r.inputs.size() << " inputs)\n"
          << "  trips " << n << ", won " << wins << ", lost " << losses
          << (n ? ", win rate " + fmt(100.0 * static_cast<double>(wins) / static_cast<double>(n), 1) + " %" : std::string()) << '\n'
          << "  gross " << fmt(gross) << "  expenses " << fmt(exp) << "  net " << fmt(gross - exp) << '\n';
        for (const int side : {1, -1}) {
            double sn = 0;
            std::size_t k = 0;
            for (const auto& t : r.trips)
                if (t.side == side) { ++k; sn += static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry) - t.costs.total; }
            s << "  " << (side > 0 ? "long " : "short") << " trips " << k << ", net " << fmt(sn) << '\n';
        }
        std::map<std::string, std::size_t> by_model;
        for (const auto& t : r.trips) ++by_model[t.model_in];
        if (!by_model.empty()) {
            s << "  trips opened by:";
            for (const auto& [m, k] : by_model) s << ' ' << m << ' ' << k << ';';
            s << '\n';
        }
        if (!r.hold_from.empty())
            s << "  buy and hold one lot " << r.hold_from << " to " << r.hold_to << " (" << r.hold_rolls << " rolls): gross "
              << fmt(r.hold_gross) << ", expenses " << fmt(r.hold_exp) << ", net " << fmt(r.hold_gross - r.hold_exp)
              << " -- what the timing has to beat\n";
        if (wins && losses)
            s << "  average win " << fmt(win_sum / static_cast<double>(wins)) << ", average loss "
              << fmt(loss_sum / static_cast<double>(losses)) << ", profit factor " << fmt(win_sum / -loss_sum) << '\n';
        if (r.called)
            s << "  direction: " << fmt(100.0 * static_cast<double>(r.right) / static_cast<double>(r.called), 1) << " % right on "
              << r.called << " called steps; "
              << (r.traded ? fmt(100.0 * static_cast<double>(r.traded_right) / static_cast<double>(r.traded), 1) : std::string("-"))
              << " % on the " << r.traded << " steps held\n";
        if (!r.best_single.empty())
            s << "  best single model after the warm-up (chosen after the fact, so flattered): " << r.best_single << " "
              << fmt(100.0 * r.best_single_acc, 1) << " % on " << r.best_single_n << " days\n";
        std::map<std::string, double> by_day;
        for (const auto& t : r.trips)
            by_day[t.exit_date.substr(0, 10)] += static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry) - t.costs.total;
        std::vector<double> v;
        for (const auto& [d, x] : by_day) v.push_back(x);
        if (v.size() >= 2) {
            double mean = 0;
            for (double x : v) mean += x;
            mean /= static_cast<double>(v.size());
            double var = 0, down = 0;
            for (double x : v) { var += (x - mean) * (x - mean); down += x < 0 ? x * x : 0; }
            var /= static_cast<double>(v.size() - 1);
            down /= static_cast<double>(v.size());
            double peak = 0, cumn = 0, dd = 0;
            for (double x : v) { cumn += x; peak = std::max(peak, cumn); dd = std::max(dd, peak - cumn); }
            s << "  per exit day: Sharpe " << (var > 0 ? fmt(mean / std::sqrt(var) * std::sqrt(252.0)) : std::string("-"))
              << ", Sortino " << (down > 0 ? fmt(mean / std::sqrt(down) * std::sqrt(252.0)) : std::string("-"))
              << ", max drawdown " << fmt(dd) << " (annualised by sqrt(252) over exit days)\n";
        }
        s << "  candidates (prequential, each on the steps it was live):\n";
        for (const auto& c : r.cands) {
            const double rm = c.n ? std::sqrt(c.sse / static_cast<double>(c.n)) : 0.0;
            const double rz = c.n ? std::sqrt(c.sse_zero / static_cast<double>(c.n)) : 0.0;
            s << "    " << c.name << ": " << c.n << " steps, RMSE " << fmt(rm, 2) << " bp vs no change " << fmt(rz, 2)
              << " (skill " << (rz > 0 ? fmt(100.0 * (1.0 - rm / rz), 3) : std::string("-")) << " %)"
              << (c.called ? ", direction " + fmt(100.0 * static_cast<double>(c.right) / static_cast<double>(c.called), 1) + " % on "
                                 + std::to_string(c.called) : std::string())
              << ", traded on " << c.chosen << " steps" << (c.note.empty() ? "" : "; last fit: " + c.note) << '\n';
        }
        s << '\n';
    }
    if (leak_check) {
        s << "Leak check (--leak-check): every arm rerun with another future after two cuts --\n";
        for (const auto& l : leak_lines) s << l << '\n';
        s << (leak_clean ? "  every decision up to each cut identical: no call saw its future\n\n"
                         : "  A DECISION MOVED WITH THE FUTURE: a leak\n\n");
    }
    s << "Files: trades.csv, days.csv, candidates.csv, analogs.csv (the latest step's most similar past moments), weights.csv\n";
    const std::string text = s.str();
    std::ofstream(out / "summary.txt") << text;
    std::fputs(text.c_str(), stdout);
    std::printf("wrote %s\n", out.string().c_str());
    return leak_check && !leak_clean ? 3 : 0;
}
