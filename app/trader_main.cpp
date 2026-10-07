// app/trader_main.cpp -- altair_trader: one model over every model's output,
// trained and traded day by day, walk-forward, up to the last close in the
// data.
//
// WHAT IT READS. The forecast curriculum's out-of-sample record
// (altair_forecast_curriculum writes <logs>/<track>.csv): on every daily
// track -- NIFTY, BANKNIFTY, NIFTY futures, INDIA VIX, each also with the
// VIX forecast fed in -- every base model's call at each close for the next
// close, made before that close was known. Each (track, model) is one input:
// its conviction 2 * p_up - 1, or +-1 from a bare call; 0 where the model had
// not started. "Coin flip" is left out: it is random by construction.
//
// WHAT IT DOES, EACH DAY, IN ORDER (models/meta_trader.hpp):
//   1. yesterday's outcome is now known: score yesterday's call, learn the row,
//      refit (ridge with forgetting, several penalties scored on the calls they
//      made, the best one's call used);
//   2. today's call: the expected move to the next close, in bp, per index;
//   3. trade one lot of the index future at the close, model-driven only:
//        flat -> enter when |expected move| beats the round trip's expenses
//                (both fills priced by config/charges.toml, in bp);
//        held -> keep while the expected move still points the held way;
//                exit when it turns, and enter the other way only if that
//                move beats the expenses.
//      No threshold is tuned: the expenses are the exchange's and the broker's,
//      the move is the model's.
//
// WHAT IT WRITES (<out>, default data/verified/altair_trader):
//   trades.csv   one row per trip: dates, side, lots, prices, days held, rolls,
//                gross, every expense head, net, the call that opened it and
//                why it closed
//   days.csv     every day per index: the call, its error, the position, the
//                day's P&L, the running net
//   weights.csv  the latest weight on every input: which models it trusts
//   summary.txt  trips, win rate, gross/expenses/net, Sharpe, Sortino,
//                drawdown, direction accuracy against the best single model
//
// LIMITS, SAID: index futures are priced on the index close (the futures'
// basis is left out); a trip held over a month end pays a roll (two more
// fills); a trip open at the end of the data is closed at the last close.
// The base models are refitted by the curriculum in doubling stages, not
// daily; this model is refitted daily.

#include <app/demo_costs.hpp>
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
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace dc = altair::demo_costs;
using altair::meta::MetaCall;
using altair::meta::MetaConfig;
using altair::meta::MetaModel;

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

constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<std::int64_t>(doe) - 719468;
}
std::int64_t day_of(const std::string& iso) {
    if (iso.size() < 10) return 0;
    return days_from_civil(std::atoi(iso.substr(0, 4).c_str()), static_cast<unsigned>(std::atoi(iso.substr(5, 2).c_str())),
                           static_cast<unsigned>(std::atoi(iso.substr(8, 2).c_str())));
}
constexpr std::int64_t kCloseSec = 15 * 3600 + 30 * 60;

/// One track's rows by date: per model, the conviction; per date, the closes.
struct Track {
    std::string name;
    std::map<std::string, std::map<std::string, double>> conviction;   ///< date -> model -> conviction
    std::map<std::string, std::pair<double, double>> closes;           ///< date -> (last, next)
};

bool read_track(const fs::path& path, Track& t, std::string& err) {
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
        const std::string date = f[static_cast<std::size_t>(c_time)].substr(0, 10);
        const std::string& model = f[static_cast<std::size_t>(c_model)];
        if (date.size() != 10 || model == "Coin flip") continue;
        const double last = std::atof(f[static_cast<std::size_t>(c_last)].c_str());
        const double next = std::atof(f[static_cast<std::size_t>(c_next)].c_str());
        if (last > 0.0 && next > 0.0) t.closes[date] = {last, next};
        const std::string& p = f[static_cast<std::size_t>(c_p)];
        const std::string& call = f[static_cast<std::size_t>(c_call)];
        double c = 0.0;
        if (!p.empty()) c = 2.0 * std::atof(p.c_str()) - 1.0;
        else if (call == "UP") c = 1.0;
        else if (call == "DOWN") c = -1.0;
        if (std::isfinite(c)) t.conviction[date][model] = std::clamp(c, -1.0, 1.0);
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

struct Trip {
    std::string index, entry_date, exit_date;
    int side = 0;
    long long qty = 0;
    double entry = 0, exit = 0;
    int days = 0, rolls = 0;
    double mu_in = 0, cost_bp_in = 0;
    std::string why_out;
    dc::Costs costs;
    bool priced = true;
};

struct DayRow {
    std::string index, date;
    bool ready = false;
    double mu = 0, se = 0, lambda = 0, y = 0, cost_bp = 0;
    int pos = 0;
    double pnl = 0, cum = 0;
};

std::string fmt(double v, int dp = 2) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", dp, v);
    return b;
}

void usage(const char* exe) {
    std::printf(
        "usage: %s [--logs DIR] [--out DIR] [--dataset DIR] [--lots N] [--unverified-costs]\n"
        "          [--half-life DAYS] [--min-days N]\n"
        "  --logs      the forecast curriculum's forecast_log/ (default data/verified/forecast_log)\n"
        "  --out       where to write (default data/verified/altair_trader)\n"
        "  --dataset   for the session after the last logged close (default dataset)\n"
        "  --lots      lots per trade (default 1)\n"
        "  --unverified-costs  price expenses while config/charges.toml is unverified\n",
        exe);
}

} // namespace

int main(int argc, char** argv) {
    fs::path logs = "data/verified/forecast_log", out = "data/verified/altair_trader", dataset = "dataset";
    long long lots = 1;
    bool unverified = false;
    MetaConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--logs" && has) logs = argv[++i];
        else if (a == "--out" && has) out = argv[++i];
        else if (a == "--dataset" && has) dataset = argv[++i];
        else if (a == "--lots" && has) lots = std::max(1LL, std::atoll(argv[++i]));
        else if (a == "--half-life" && has) cfg.half_life_days = std::max(5.0, std::atof(argv[++i]));
        else if (a == "--min-days" && has) cfg.min_days = static_cast<std::size_t>(std::max(20LL, std::atoll(argv[++i])));
        else if (a == "--unverified-costs") unverified = true;
        else { usage(argv[0]); return 2; }
    }

    // ---- expenses --------------------------------------------------------------------
    std::vector<altair::ChargeSchedule> schedules;
    const auto rep = altair::load_charges_file("config/charges.toml", schedules);
    bool costs_allowed = false;
    std::string cost_tag;
    if (!rep) {
        std::printf("config/charges.toml did not load: %s -- gross P&L only, and no trade can beat unknown expenses\n",
                    altair::charges_error_text(rep.error()));
        cost_tag = "refused";
    } else if (rep->verified) {
        costs_allowed = true;
        cost_tag = "verified";
    } else if (unverified) {
        for (auto& s : schedules) s.verified = true;
        costs_allowed = true;
        cost_tag = "UNVERIFIED";
    } else {
        std::printf("config/charges.toml is UNVERIFIED: pass --unverified-costs to price expenses anyway\n");
        cost_tag = "refused";
    }
    if (!costs_allowed) return 2;   // the decision needs the expenses: no costs, no trades

    // ---- the inputs: every daily track's every model ---------------------------------
    std::vector<Track> tracks;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs, ec)) {
        const std::string n = e.path().filename().string();
        if (e.path().extension() != ".csv" || n.find("_daily") == std::string::npos) continue;
        Track t;
        std::string err;
        if (!read_track(e.path(), t, err)) { std::printf("  %s\n", err.c_str()); continue; }
        tracks.push_back(std::move(t));
    }
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.name < b.name; });
    if (tracks.empty()) {
        std::printf("no daily forecast logs in %s: run altair_forecast_curriculum first\n", logs.string().c_str());
        return 2;
    }
    std::vector<std::pair<std::string, std::string>> inputs;   // (track, model)
    {
        std::set<std::pair<std::string, std::string>> seen;
        for (const auto& t : tracks)
            for (const auto& [d, m] : t.conviction)
                for (const auto& [model, c] : m) seen.insert({t.name, model});
        inputs.assign(seen.begin(), seen.end());
    }
    std::printf("altair_trader: %zu inputs from %zu daily tracks in %s\n", inputs.size(), tracks.size(), logs.string().c_str());
    const auto features_on = [&](const std::string& date) {
        std::vector<double> x(inputs.size(), 0.0);
        std::size_t k = 0;
        for (const auto& [tn, model] : inputs) {
            for (const auto& t : tracks) {
                if (t.name != tn) continue;
                const auto d = t.conviction.find(date);
                if (d != t.conviction.end()) {
                    const auto m = d->second.find(model);
                    if (m != d->second.end()) x[k] = m->second;
                }
                break;
            }
            ++k;
        }
        return x;
    };

    // ---- what it trades ------------------------------------------------------------------
    struct Arm { std::string index, track, fut_name, data_dir; long long lot; };
    std::vector<Arm> arms{
        {"NIFTY", "nifty_daily", "NIFTY", "nifty", lot_from_master("data/instruments.csv", "NIFTY", 65)},
        {"BANKNIFTY", "banknifty_daily", "BANKNIFTY", "banknifty", lot_from_master("data/instruments.csv", "BANKNIFTY", 30)},
    };

    std::vector<Trip> trips;
    std::vector<DayRow> days;
    std::map<std::string, std::vector<double>> last_weights;
    struct Acc { std::size_t called = 0, right = 0, traded = 0, traded_right = 0; };
    /// Buy and hold one lot over the same called days, rolled at month ends:
    /// what the timing has to beat.
    struct Hold { std::string from, to; double gross = 0, expenses = 0; int rolls = 0; };
    std::map<std::string, Hold> hold;
    std::map<std::string, Acc> acc;
    std::map<std::string, std::map<std::string, std::pair<std::size_t, std::size_t>>> single;   // index -> model -> (right, n)

    for (const auto& arm : arms) {
        const Track* target = nullptr;
        for (const auto& t : tracks) if (t.name == arm.track) target = &t;
        if (target == nullptr || target->closes.empty()) {
            std::printf("  %s: no %s.csv in the logs, not traded\n", arm.index.c_str(), arm.track.c_str());
            continue;
        }
        std::vector<std::string> dates;
        for (const auto& [d, c] : target->closes) dates.push_back(d);
        const std::string after_last = last_dataset_date(dataset / "spot" / arm.data_dir / "1d" / "all.csv");
        MetaModel model(inputs.size(), cfg);
        const auto price_fill = [&](bool buy, double px, const std::string& date) {
            return dc::fill(altair::Segment::Fut, buy ? altair::Side::Buy : altair::Side::Sell,
                            static_cast<double>(arm.lot * lots), px, day_of(date) * 86'400 + kCloseSec, schedules);
        };
        const auto round_trip_bp = [&](double px, const std::string& date) {
            const dc::Costs b = price_fill(true, px, date), s = price_fill(false, px, date);
            if (!b.priced || !s.priced) return std::numeric_limits<double>::infinity();
            return (b.total + s.total) / (px * static_cast<double>(arm.lot * lots)) * 1e4;
        };
        int pos = 0;
        Trip open;
        double cum = 0.0;
        for (std::size_t i = 0; i < dates.size(); ++i) {
            const std::string& date = dates[i];
            const auto closes = target->closes.at(date);   // not a structured binding: lambdas below use them
            const double last = closes.first, next = closes.second;
            const std::string next_date = i + 1 < dates.size() ? dates[i + 1]
                                        : (!after_last.empty() && after_last > date ? after_last : "next session");
            const double y = (next / last - 1.0) * 1e4;
            const MetaCall c = model.predict(features_on(date));
            const double cost_bp = round_trip_bp(last, date);
            DayRow row;
            row.index = arm.index;
            row.date = date;
            row.ready = c.ready;
            row.mu = c.mu;
            row.se = c.se;
            row.lambda = c.lambda;
            row.y = y;
            row.cost_bp = cost_bp;
            // The best single model on the same days, for comparison.
            if (c.ready) {
                Hold& h = hold[arm.index];
                const long long q = arm.lot * lots;
                if (h.from.empty()) { h.from = date; h.expenses += price_fill(true, last, date).total; }
                h.to = next_date;
                h.gross += static_cast<double>(q) * (next - last);
                if (next_date.size() == 10 && next_date.substr(0, 7) != date.substr(0, 7)) {
                    ++h.rolls;
                    h.expenses += price_fill(false, next, next_date).total + price_fill(true, next, next_date).total;
                }
                if (i + 1 == dates.size()) h.expenses += price_fill(false, next, next_date == "next session" ? date : next_date).total;
                const auto d = target->conviction.find(date);
                if (d != target->conviction.end())
                    for (const auto& [m, conv] : d->second)
                        if (conv != 0.0) {
                            auto& s = single[arm.index][m];
                            ++s.second;
                            s.first += (conv > 0) == (y > 0) ? 1 : 0;
                        }
                auto& a = acc[arm.index];
                ++a.called;
                a.right += (c.mu > 0) == (y > 0) ? 1 : 0;
            }
            // ---- the decision at today's close -----------------------------------------------
            const auto close_trip = [&](const std::string& why) {
                open.exit_date = date;
                open.exit = last;
                open.why_out = why;
                const dc::Costs f = price_fill(open.side < 0, last, date);
                open.costs.add(f);
                open.priced = open.priced && f.priced;
                trips.push_back(open);
                pos = 0;
            };
            if (pos != 0) {
                if (!c.ready || c.mu * pos <= 0.0)
                    close_trip(!c.ready ? "no call" : "the expected move turned against the position");
            }
            if (pos == 0 && c.ready && std::fabs(c.mu) > cost_bp) {
                pos = c.mu > 0 ? 1 : -1;
                open = Trip{};
                open.index = arm.index;
                open.entry_date = date;
                open.side = pos;
                open.qty = arm.lot * lots;
                open.entry = last;
                open.mu_in = c.mu;
                open.cost_bp_in = cost_bp;
                const dc::Costs f = price_fill(pos > 0, last, date);
                open.costs.add(f);
                open.priced = f.priced;
                auto& a = acc[arm.index];
                ++a.traded;
                a.traded_right += (pos > 0) == (y > 0) ? 1 : 0;
            } else if (pos != 0) {
                auto& a = acc[arm.index];
                ++a.traded;
                a.traded_right += (pos > 0) == (y > 0) ? 1 : 0;
            }
            // ---- the session to the next close -----------------------------------------------
            if (pos != 0) {
                ++open.days;
                // A month end inside the holding: the future is rolled, two more fills.
                if (next_date.size() == 10 && next_date.substr(0, 7) != date.substr(0, 7)) {
                    ++open.rolls;
                    const dc::Costs r1 = price_fill(pos < 0, next, next_date), r2 = price_fill(pos > 0, next, next_date);
                    open.costs.add(r1);
                    open.costs.add(r2);
                    open.priced = open.priced && r1.priced && r2.priced;
                }
                row.pnl = static_cast<double>(pos) * static_cast<double>(open.qty) * (next - last);
            }
            row.pos = pos;
            cum += row.pnl;
            row.cum = cum;
            days.push_back(row);
            model.learn(y);
            // The last close in the data: a trip still open is closed there.
            if (i + 1 == dates.size() && pos != 0) {
                open.exit_date = next_date;
                open.exit = next;
                open.why_out = "end of the data (closed at the last close)";
                const dc::Costs f = price_fill(open.side < 0, next, next_date == "next session" ? date : next_date);
                open.costs.add(f);
                open.priced = open.priced && f.priced;
                trips.push_back(open);
                pos = 0;
            }
        }
        last_weights[arm.index] = model.weights();
    }

    // ---- write ------------------------------------------------------------------------
    fs::create_directories(out, ec);
    {
        std::ofstream f(out / "trades.csv");
        f << "date,index,side,lots,qty,entry_date,entry,exit_date,exit,days,rolls,gross,expenses,net,"
             "brokerage,stt,exchange_txn,sebi,stamp,ipft,gst,expected_bp,cost_bp,why_in,why_out,costs\n";
        for (const auto& t : trips) {
            const double gross = static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry);
            f << t.exit_date << ',' << t.index << ',' << (t.side > 0 ? "long" : "short") << ',' << lots << ',' << t.qty << ','
              << t.entry_date << ',' << fmt(t.entry) << ',' << t.exit_date << ',' << fmt(t.exit) << ',' << t.days << ','
              << t.rolls << ',' << fmt(gross) << ',';
            if (t.priced) {
                f << fmt(t.costs.total) << ',' << fmt(gross - t.costs.total) << ',' << fmt(t.costs.brokerage) << ','
                  << fmt(t.costs.stt) << ',' << fmt(t.costs.exchange) << ',' << fmt(t.costs.sebi) << ',' << fmt(t.costs.stamp)
                  << ',' << fmt(t.costs.ipft) << ',' << fmt(t.costs.gst);
            } else {
                f << ",,,,,,,,";
            }
            f << ',' << fmt(t.mu_in, 1) << ',' << fmt(t.cost_bp_in, 2) << ",\"expected " << fmt(t.mu_in, 1)
              << " bp over a " << fmt(t.cost_bp_in, 2) << " bp round trip\",\"" << t.why_out << "\"," << cost_tag << '\n';
        }
    }
    {
        std::ofstream f(out / "days.csv");
        f << "date,index,ready,expected_bp,error_bp,lambda,move_bp,cost_bp,position,pnl,cum_gross\n";
        for (const auto& d : days)
            f << d.date << ',' << d.index << ',' << (d.ready ? 1 : 0) << ',' << fmt(d.mu, 2) << ',' << fmt(d.se, 1) << ','
              << fmt(d.lambda, 0) << ',' << fmt(d.y, 1) << ',' << fmt(d.cost_bp, 2) << ',' << d.pos << ',' << fmt(d.pnl)
              << ',' << fmt(d.cum) << '\n';
    }
    {
        std::ofstream f(out / "weights.csv");
        f << "index,track,model,weight\n";
        for (const auto& [index, w] : last_weights) {
            if (w.empty()) continue;
            f << index << ",intercept,,"<< fmt(w[0], 3) << '\n';
            std::vector<std::size_t> order(inputs.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return std::fabs(w[a + 1]) > std::fabs(w[b + 1]); });
            for (const std::size_t k : order)
                f << index << ',' << inputs[k].first << ",\"" << inputs[k].second << "\"," << fmt(w[k + 1], 3) << '\n';
        }
    }

    // ---- summary -----------------------------------------------------------------------
    std::ostringstream s;
    s << "altair_trader -- one model over " << inputs.size() << " model outputs (" << tracks.size()
      << " daily tracks), refitted every day, walk-forward\n"
      << "expenses: config/charges.toml (" << cost_tag << "); one round trip in bp is the bar every entry must clear\n\n";
    for (const auto& arm : arms) {
        double gross = 0, exp = 0;
        std::size_t n = 0, wins = 0, losses = 0;
        double win_sum = 0, loss_sum = 0;
        for (const auto& t : trips) {
            if (t.index != arm.index) continue;
            const double g = static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry);
            const double net = g - t.costs.total;
            gross += g;
            exp += t.costs.total;
            ++n;
            if (net > 0) { ++wins; win_sum += net; } else { ++losses; loss_sum += net; }
        }
        const Acc& a = acc[arm.index];
        s << arm.index << " (lot " << arm.lot << " x " << lots << ")\n"
          << "  trips " << n << ", won " << wins << ", lost " << losses
          << (n ? ", win rate " + fmt(100.0 * static_cast<double>(wins) / static_cast<double>(n), 1) + " %" : std::string()) << '\n'
          << "  gross " << fmt(gross) << "  expenses " << fmt(exp) << "  net " << fmt(gross - exp) << '\n';
        for (const int side : {1, -1}) {
            double sn = 0;
            std::size_t k = 0;
            for (const auto& t : trips)
                if (t.index == arm.index && t.side == side) {
                    ++k;
                    sn += static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry) - t.costs.total;
                }
            s << "  " << (side > 0 ? "long " : "short") << " trips " << k << ", net " << fmt(sn) << '\n';
        }
        if (const auto h = hold.find(arm.index); h != hold.end())
            s << "  buy and hold one lot " << h->second.from << " to " << h->second.to << " (" << h->second.rolls
              << " rolls): gross " << fmt(h->second.gross) << ", expenses " << fmt(h->second.expenses) << ", net "
              << fmt(h->second.gross - h->second.expenses) << " -- what the timing has to beat\n";
        if (wins && losses) s << "  average win " << fmt(win_sum / static_cast<double>(wins)) << ", average loss "
                              << fmt(loss_sum / static_cast<double>(losses)) << ", profit factor " << fmt(win_sum / -loss_sum) << '\n';
        if (a.called)
            s << "  direction: " << fmt(100.0 * static_cast<double>(a.right) / static_cast<double>(a.called), 1) << " % right on "
              << a.called << " called days; " << (a.traded ? fmt(100.0 * static_cast<double>(a.traded_right) / static_cast<double>(a.traded), 1) : std::string("-"))
              << " % on the " << a.traded << " days held\n";
        std::string best;
        double best_acc = 0.0;
        std::size_t best_n = 0;
        for (const auto& [m, rn] : single[arm.index])
            if (rn.second >= a.called / 2 && rn.second > 0) {
                const double ac = static_cast<double>(rn.first) / static_cast<double>(rn.second);
                if (ac > best_acc) { best_acc = ac; best = m; best_n = rn.second; }
            }
        if (!best.empty())
            s << "  best single model on the same days (chosen after the fact, so flattered): " << best << " "
              << fmt(100.0 * best_acc, 1) << " % on " << best_n << " days\n";
        // Daily net: each trip's net booked on its exit day; Sharpe and Sortino over the days traded.
        std::map<std::string, double> by_day;
        for (const auto& t : trips)
            if (t.index == arm.index)
                by_day[t.exit_date] += static_cast<double>(t.side) * static_cast<double>(t.qty) * (t.exit - t.entry) - t.costs.total;
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
        if (const auto w = last_weights[arm.index]; !w.empty()) {
            std::vector<std::size_t> order(inputs.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return std::fabs(w[x + 1]) > std::fabs(w[y + 1]); });
            s << "  trusts most now (bp per unit of conviction):";
            for (std::size_t k = 0; k < order.size() && k < 5; ++k)
                s << (k ? "; " : " ") << inputs[order[k]].second << " on " << inputs[order[k]].first << " " << fmt(w[order[k] + 1], 1);
            s << '\n';
        }
        s << '\n';
    }
    s << "Files: trades.csv (one row per trip, every expense head), days.csv (the call every day), weights.csv\n";
    const std::string text = s.str();
    std::ofstream(out / "summary.txt") << text;
    std::fputs(text.c_str(), stdout);
    std::printf("wrote %s\n", out.string().c_str());
    return 0;
}
