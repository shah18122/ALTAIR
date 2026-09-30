// app/forecast_curriculum_main.cpp -- altair_forecast_curriculum.
//
// Walks every forecaster in models/ through the doubling curriculum
// (models/curriculum.hpp) on the dataset: learn 3 days, forecast the next 3,
// keep the record, refit on 6, forecast 6, ... to the end of the data. Seven
// tracks: NIFTY, BANKNIFTY, NIFTY futures and INDIA VIX next-day direction;
// NIFTY, BANKNIFTY and INDIA VIX next-hour direction.
//
// Offline and read-only: it reads dataset/ and writes
//   <out>/forecast_curriculum.xlsx   Summary, Data, Method, one learning-curve
//                                    sheet per track
//   <out>/forecast_curriculum.txt    the summary as text
//   <out>/forecast_log/<track>.csv   every forecast and whether it was right

#include <app/forecast_tracks.hpp>
#include <app/xlsx_writer.hpp>
#include <models/curriculum.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;
using altair::CurriculumRun;
using altair::CurriculumSummary;
using altair::CurriculumTrack;
using altair::xlsx::XlsxCell;
using altair::xlsx::XlsxSheet;

struct TrackResult {
    CurriculumTrack track;
    ft::TrackInfo info;
    CurriculumRun run;
    bool ok = false;
    std::string error;
    double seconds = 0.0;
    std::vector<CurriculumSummary> summary;   ///< [model]
};

/// `v` with `decimals` places; `sign` adds a + to positive numbers.
std::string fixed(double v, int decimals, bool sign = false) {
    char buf[64];
    std::snprintf(buf, sizeof buf, sign ? "%+.*f" : "%.*f", decimals, v);
    return buf;
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) { s.append(width - s.size(), ' '); }
    return s;
}

std::string slug(const std::string& name) {
    std::string s;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!s.empty() && s.back() != '_') {
            s += '_';
        }
    }
    return s;
}

std::string day_text(std::int64_t t) { return da::format_audit_time(t, true); }

/// One plain sentence on what a model's record means.
std::string verdict(const CurriculumSummary& s, std::size_t tests) {
    if (s.all.scored() < 100) { return "too few forecasts to judge"; }
    if (s.p_adjusted < 0.05) {
        if (s.accuracy < 0.5) { return "reliably WRONG (significant) - worse than a coin"; }
        if (s.p_constant_adjusted < 0.05) { return "beats a coin AND the best constant call (significant)"; }
        return s.z_vs_constant > 2.0
                   ? "beats a coin (significant); its edge over the best constant call is not significant after "
                         + std::to_string(tests) + " tests"
                   : "beats a coin (significant) but not the best constant call";
    }
    if (s.p_vs_half < 0.05) {
        return "above a coin by chance-level evidence (not significant after " + std::to_string(tests) + " tests)";
    }
    return "indistinguishable from a coin";
}

double pct(double v) { return std::isfinite(v) ? 100.0 * v : v; }

XlsxSheet summary_sheet(const std::vector<TrackResult>& results, std::size_t tests) {
    XlsxSheet sh;
    sh.name = "Summary";
    sh.freeze_rows = 1;
    const char* head[] = {"Track", "Model", "Family", "Forecasts", "Abstained", "Coverage %", "Right", "Wrong", "No direction", "Flat",
                          "Accuracy %", "95% low %", "95% high %", "z vs coin", "p vs coin",
                          "p (Bonferroni)", "Up-rate on same bars %", "vs best constant call (pts)", "p vs constant (Bonferroni)",
                          "Brier", "RMSE bp", "Random walk RMSE bp", "Skill vs RW %", "Price verdict",
                          "Trades (clear cost)", "Trade hit %", "Net bp / trade", "Net t-stat", "Net bp total",
                          "Final-stage accuracy %", "Fit s", "Verdict"};
    std::vector<XlsxCell> h;
    for (const char* c : head) { h.push_back(XlsxCell::str(c, true)); }
    sh.rows.push_back(h);
    sh.widths = {24, 24, 11, 10, 10, 10, 9, 9, 10, 7, 10, 9, 9, 9, 9, 11, 12, 12, 12, 8, 9, 11, 10, 22, 11, 10, 10, 9, 11, 11, 8, 52};
    for (const auto& r : results) {
        if (!r.ok) {
            sh.rows.push_back({XlsxCell::str(r.track.name), XlsxCell::str("-"), XlsxCell::str("-"),
                               XlsxCell::str("not run: " + r.error)});
            continue;
        }
        std::vector<std::size_t> order(r.summary.size());
        for (std::size_t m = 0; m < order.size(); ++m) { order[m] = m; }
        std::stable_sort(order.begin(), order.end(), [&r](std::size_t a, std::size_t b) {
            const double x = r.summary[a].all.scored() > 0 ? r.summary[a].accuracy : -1.0;
            const double y = r.summary[b].all.scored() > 0 ? r.summary[b].accuracy : -1.0;
            return x > y;
        });
        const auto& last = r.run.stages.back();
        for (const std::size_t m : order) {
            const auto& s = r.summary[m];
            const auto fin = altair::curriculum_tally(r.track, r.run, m, last.test_begin, last.test_end);
            const bool scored = s.all.scored() > 0;
            const double nan = std::numeric_limits<double>::quiet_NaN();
            sh.rows.push_back({
                XlsxCell::str(r.track.name), XlsxCell::str(r.run.models[m]), XlsxCell::str(r.run.families[m]),
                XlsxCell::num(static_cast<double>(s.all.forecasts)), XlsxCell::num(static_cast<double>(s.all.abstained)),
                XlsxCell::num(s.all.forecasts + s.all.abstained > 0
                                  ? 100.0 * static_cast<double>(s.all.forecasts)
                                        / static_cast<double>(s.all.forecasts + s.all.abstained)
                                  : nan),
                XlsxCell::num(static_cast<double>(s.all.right)), XlsxCell::num(static_cast<double>(s.all.wrong)),
                XlsxCell::num(static_cast<double>(s.all.no_direction)), XlsxCell::num(static_cast<double>(s.all.flat)),
                XlsxCell::num(scored ? pct(s.accuracy) : nan, true),
                XlsxCell::num(scored ? pct(s.lo95) : nan), XlsxCell::num(scored ? pct(s.hi95) : nan),
                XlsxCell::num(scored ? s.z_vs_half : nan), XlsxCell::num(scored ? s.p_vs_half : nan),
                XlsxCell::num(scored ? s.p_adjusted : nan), XlsxCell::num(scored ? pct(s.up_rate) : nan),
                XlsxCell::num(scored ? 100.0 * (s.accuracy - s.best_constant) : nan),
                XlsxCell::num(scored ? s.p_constant_adjusted : nan),
                XlsxCell::num(s.all.brier()),
                XlsxCell::num(s.have_price ? s.price.rmse_model_bps : nan),
                XlsxCell::num(s.have_price ? s.price.rmse_naive_bps : nan),
                XlsxCell::num(s.have_price ? 100.0 * s.price.skill : nan),
                XlsxCell::str(s.have_price ? altair::verdict_text(s.verdict) : "-"),
                XlsxCell::num(r.track.tradable ? static_cast<double>(s.all.trades) : nan),
                XlsxCell::num(s.all.trades > 0 ? 100.0 * static_cast<double>(s.all.trade_wins)
                                                     / static_cast<double>(s.all.trades) : nan),
                XlsxCell::num(s.all.trades > 0 ? s.all.net_bp / static_cast<double>(s.all.trades) : nan),
                XlsxCell::num(s.all.net_t()),
                XlsxCell::num(s.all.trades > 0 ? s.all.net_bp : nan),
                XlsxCell::num(fin.scored() > 0 ? pct(fin.accuracy()) : nan),
                XlsxCell::num(std::round(r.run.fit_seconds[m] * 10.0) / 10.0),
                XlsxCell::str(verdict(s, tests)),
            });
        }
        sh.rows.push_back({});
    }
    return sh;
}

XlsxSheet track_sheet(const TrackResult& r) {
    XlsxSheet sh;
    sh.name = r.track.name;
    const auto& run = r.run;
    const std::size_t models = run.models.size();
    const auto& t = r.track;
    sh.rows.push_back({XlsxCell::str(t.name + " - " + t.horizon + " direction, doubling curriculum", true)});
    sh.rows.push_back({XlsxCell::str(std::to_string(t.rows()) + " decisions over " + std::to_string(t.days())
                                     + " trading days, " + day_text(t.t.front()) + " to " + day_text(t.t.back())
                                     + ". Each stage learns every day before it and forecasts the block; "
                                       "the model is frozen inside a block and refitted after it.")});
    sh.rows.push_back({});
    const auto header = [&](const char* title) {
        std::vector<XlsxCell> h{XlsxCell::str(title, true), XlsxCell::str("Learned days", true),
                                XlsxCell::str("Forecast days", true), XlsxCell::str("Learned", true),
                                XlsxCell::str("Forecast", true)};
        for (const auto& m : run.models) { h.push_back(XlsxCell::str(m, true)); }
        sh.rows.push_back(h);
    };
    const auto stage_cells = [&](const altair::CurriculumStage& st) {
        return std::vector<XlsxCell>{
            XlsxCell::num(static_cast<double>(st.index)), XlsxCell::num(st.train_days), XlsxCell::num(st.test_days),
            XlsxCell::str(day_text(t.t.front()) + " to " + day_text(t.t[st.train_rows - 1])),
            XlsxCell::str(day_text(t.t[st.test_begin]) + " to " + day_text(t.t[st.test_end - 1]))};
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();

    header("Accuracy % in the stage");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) {
            const auto k = altair::curriculum_tally(t, run, m, st.test_begin, st.test_end);
            row.push_back(XlsxCell::num(k.scored() > 0 ? pct(k.accuracy()) : nan));
        }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    header("Accuracy % so far");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) {
            const auto k = altair::curriculum_tally(t, run, m, run.first_row, st.test_end);
            row.push_back(XlsxCell::num(k.scored() > 0 ? pct(k.accuracy()) : nan));
        }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    header("Right / scored in the stage");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) {
            const auto k = altair::curriculum_tally(t, run, m, st.test_begin, st.test_end);
            row.push_back(XlsxCell::str(k.forecasts == 0 ? std::string{"abstained"}
                                                         : std::to_string(k.right) + " / " + std::to_string(k.scored())));
        }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    header("What the stage's fit chose");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) { row.push_back(XlsxCell::str(run.notes[m][st.index])); }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    sh.rows.push_back({XlsxCell::str("Track record going into each stage", true), XlsxCell::str("Champion", true),
                       XlsxCell::str("Hedge's top weights", true)});
    for (const auto& st : run.stages) {
        const int c = run.champion[st.index];
        const auto& w = run.hedge_weight[st.index];
        std::vector<std::size_t> idx(w.size());
        for (std::size_t m = 0; m < idx.size(); ++m) { idx[m] = m; }
        std::stable_sort(idx.begin(), idx.end(), [&w](std::size_t a, std::size_t b) { return w[a] > w[b]; });
        std::string top;
        for (std::size_t k = 0; k < idx.size() && k < 3 && w[idx[k]] > 0.0; ++k) {
            if (!top.empty()) { top += ", "; }
            top += run.models[idx[k]] + " " + fixed(100.0 * w[idx[k]], 0) + "%";
        }
        sh.rows.push_back({XlsxCell::str("stage " + std::to_string(st.index)),
                           XlsxCell::str(c >= 0 ? run.models[static_cast<std::size_t>(c)] : "none yet"),
                           XlsxCell::str(top.empty() ? "none yet" : top)});
    }
    sh.widths = {26, 12, 13, 26, 26};
    for (std::size_t m = 0; m < models; ++m) { sh.widths.push_back(14); }
    return sh;
}

XlsxSheet data_sheet(const std::vector<TrackResult>& results) {
    XlsxSheet sh;
    sh.name = "Data";
    sh.freeze_rows = 1;
    const char* head[] = {"Track", "Source", "Files", "Rows read", "Parse errors", "Duplicates", "Conflicts (kept first)",
                          "Seconds floored", "Bars after cleaning", "Bad prices dropped", "OHLC repaired",
                          "Short sessions excluded", "Bars before INDIA VIX", "Rows without VIX",
                          "Rows without spot", "Rows without VIX forecast", "Roll outcomes excluded", "Expiries", "Basis on expiry bp",
                          "Basis jump next day bp", "Warm-up bars", "Decisions", "Trading days",
                          "First decision", "Last decision", "Features", "Cost"};
    std::vector<XlsxCell> h;
    for (const char* c : head) { h.push_back(XlsxCell::str(c, true)); }
    sh.rows.push_back(h);
    for (const auto& r : results) {
        const auto& i = r.info;
        const auto n = [](std::size_t v) { return XlsxCell::num(static_cast<double>(v)); };
        std::string features;
        for (const auto& f : r.track.feature_names) { features += (features.empty() ? "" : ", ") + f; }
        sh.rows.push_back({XlsxCell::str(r.track.name), XlsxCell::str(i.source), n(i.files), n(i.rows_read),
                           n(i.parse_errors), n(i.duplicates), n(i.conflicts), n(i.seconds_floored), n(i.bars),
                           n(i.bad_price), n(i.ohlc_repaired), n(i.short_days), n(i.before_vix), n(i.no_vix),
                           n(i.no_spot), n(i.no_vix_forecast), n(i.roll_excluded), n(i.expiries), XlsxCell::num(i.basis_on_expiry_bp),
                           XlsxCell::num(i.basis_jump_after_bp), n(i.warmup), n(i.rows),
                           XlsxCell::num(i.days), XlsxCell::str(i.first), XlsxCell::str(i.last),
                           XlsxCell::str(features), XlsxCell::str(i.cost_note)});
    }
    sh.widths = {24, 28, 7, 10, 8, 10, 10, 9, 10, 9, 9, 10, 10, 9, 9, 10, 10, 8, 9, 9, 9, 10, 9, 26, 26, 90, 40};
    return sh;
}

XlsxSheet method_sheet(const std::vector<std::string>& lines) {
    XlsxSheet sh;
    sh.name = "Method";
    for (std::size_t k = 0; k < lines.size(); ++k) { sh.rows.push_back({XlsxCell::str(lines[k], k == 0)}); }
    sh.widths = {160};
    return sh;
}

void write_log(const fs::path& dir, const TrackResult& r) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (slug(r.track.name) + ".csv");
    std::ofstream out(file.string() + ".tmp", std::ios::binary | std::ios::trunc);
    out << "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n";
    const auto& t = r.track;
    const auto& run = r.run;
    char buf[320];
    for (std::size_t i = run.first_row; i < t.rows(); ++i) {
        const std::size_t st = run.stage_of[i - run.first_row];
        const int moved = t.ret(i) > 0.0 ? 1 : (t.ret(i) < 0.0 ? -1 : 0);
        const std::string when = da::format_audit_time(t.t[i], false);
        for (std::size_t m = 0; m < run.models.size(); ++m) {
            const auto& c = run.calls[m][i - run.first_row];
            if (!c.made) { continue; }   // abstentions are in the workbook, stage by stage
            const char* result = moved == 0 ? "FLAT" : (c.dir == moved ? "RIGHT" : "WRONG");
            const double net = altair::curriculum_trade_bp(t, c, i);
            // Quoted: "ARMA(1,1)" carries a comma.
            std::snprintf(buf, sizeof buf, "%s,%zu,%d,\"%s\",%.2f,%.2f,%.2f,", when.c_str(), st,
                          run.stages[st].train_days, run.models[m].c_str(), t.anchor[i], t.actual[i],
                          t.anchor[i] * std::exp(c.mu));
            out << buf;
            if (std::isfinite(c.p_up)) { out << fixed(c.p_up, 4); }
            out << ',' << (c.dir > 0 ? "UP" : (c.dir < 0 ? "DOWN" : "NONE")) << ','
                << (moved > 0 ? "UP" : (moved < 0 ? "DOWN" : "FLAT")) << ',' << result << ',';
            if (std::isfinite(net)) { out << fixed(net, 2); }
            out << '\n';
        }
    }
    out.close();
    if (!out) {
        std::printf("  could not write %s\n", file.string().c_str());
        fs::remove(file.string() + ".tmp", ec);
        return;
    }
    fs::rename(file.string() + ".tmp", file, ec);
}

void usage(const char* exe) {
    std::printf(
        "  Train every forecaster on a doubling curriculum and keep its record.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--first-days N] [--cap-days N]\n"
        "       [--other-cost-bp X] [--only TEXT] [--no-log]\n\n"
        "    --dataset DIR      default dataset\n"
        "    --out DIR          default data/verified\n"
        "    --first-days N     days learned before the first forecast (default 3)\n"
        "    --cap-days N       stop doubling once a block exceeds N days; 0 = pure doubling (default)\n"
        "    --other-cost-bp X  round-trip charges besides STT, incl. one tick of slippage (default 1.3)\n"
        "    --only TEXT        run only tracks whose name contains TEXT (e.g. daily, NIFTY)\n"
        "    --no-log           skip the per-forecast CSV logs\n", exe);
}

bool parse_int(std::string_view v, long& out) {
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = "dataset";
    fs::path out = "data/verified";
    long first_days = 3, cap_days = 0;
    double other_cost = 1.3;
    std::string only;
    bool log = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--only" && has) { only = argv[++i]; continue; }
        if (a == "--no-log") { log = false; continue; }
        if (a == "--first-days" && has) {
            if (!parse_int(argv[++i], first_days) || first_days < 1 || first_days > 1000) { usage(argv[0]); return 2; }
            continue;
        }
        if (a == "--cap-days" && has) {
            if (!parse_int(argv[++i], cap_days) || cap_days < 0 || cap_days > 100000) { usage(argv[0]); return 2; }
            continue;
        }
        if (a == "--other-cost-bp" && has) {
            const std::string v{argv[++i]};
            char* end = nullptr;
            other_cost = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(other_cost >= 0.0) || other_cost > 1000.0) { usage(argv[0]); return 2; }
            continue;
        }
        usage(argv[0]);
        return 2;
    }

    const auto t_start = std::chrono::steady_clock::now();
    std::printf("Forecast curriculum over %s\n", root.string().c_str());

    // Every series once; each is cleaned against the info of the track that owns it.
    ft::TrackInfo i_nifty_d, i_bank_d, i_fut_d, i_vix_d, i_nifty_h, i_bank_h, i_vix_h;
    const auto load = [&root](const char* rel, int tf, ft::TrackInfo& info) {
        auto bars = ft::load_bars(root / rel, tf, info);
        ft::clean_bars(bars, info);
        return bars;
    };
    const auto nifty_d = load("spot/nifty/1d", da::kDailyTf, i_nifty_d);
    const auto bank_d = load("spot/banknifty/1d", da::kDailyTf, i_bank_d);
    const auto vix_d = load("spot/indiavix/1d", da::kDailyTf, i_vix_d);
    const auto fut_d = load("fut/nifty/1d", da::kDailyTf, i_fut_d);
    const auto nifty_h = load("spot/nifty/60m", 60, i_nifty_h);
    const auto bank_h = load("spot/banknifty/60m", 60, i_bank_h);
    const auto vix_h = load("spot/indiavix/60m", 60, i_vix_h);
    if (nifty_d.empty() || vix_d.empty() || nifty_h.empty() || vix_h.empty()) {
        std::printf("  dataset not found or empty under %s\n", root.string().c_str());
        return 1;
    }

    std::vector<TrackResult> results;
    const auto add = [&](CurriculumTrack t, ft::TrackInfo info, const char* cost) {
        if (!only.empty() && t.name.find(only) == std::string::npos) { return; }
        info.cost_note = cost;
        TrackResult r;
        r.track = std::move(t);
        r.info = std::move(info);
        results.push_back(std::move(r));
    };
    const std::string cost_text = "2 bp STT to 2026-03-31, 5 bp from 2026-04-01, + " + fixed(other_cost, 1)
                                  + " bp charges and one tick";
    // Built first, then added: the builders fill the info they are handed, and
    // an argument list does not order its evaluation.
    auto t_nifty_d = ft::build_daily({"NIFTY daily", "NIFTY", &nifty_d, &vix_d, nullptr, true, other_cost, nullptr}, i_nifty_d);
    auto t_bank_d = ft::build_daily({"BANKNIFTY daily", "BANKNIFTY", &bank_d, &vix_d, nullptr, true, other_cost, nullptr}, i_bank_d);
    auto t_fut_d = ft::build_daily({"NIFTY FUT daily", "NIFTY FUT", &fut_d, &vix_d, &nifty_d, true, other_cost, nullptr}, i_fut_d);
    auto t_vix_d = ft::build_daily({"INDIA VIX daily", "INDIA VIX", &vix_d, nullptr, nullptr, false, other_cost, nullptr}, i_vix_d);
    auto t_nifty_h = ft::build_hourly({"NIFTY hourly", "NIFTY", &nifty_h, &vix_h, true, other_cost, nullptr}, i_nifty_h);
    auto t_bank_h = ft::build_hourly({"BANKNIFTY hourly", "BANKNIFTY", &bank_h, &vix_h, true, other_cost, nullptr}, i_bank_h);
    auto t_vix_h = ft::build_hourly({"INDIA VIX hourly", "INDIA VIX", &vix_h, nullptr, false, other_cost, nullptr}, i_vix_h);
    // INDIA VIX daily runs first: its forecasts feed the "+ VIX fc" tracks.
    add(std::move(t_vix_d), i_vix_d, "not tradable");
    add(std::move(t_nifty_d), i_nifty_d, cost_text.c_str());
    add(std::move(t_bank_d), i_bank_d, cost_text.c_str());
    add(std::move(t_fut_d), i_fut_d, cost_text.c_str());
    add(std::move(t_nifty_h), i_nifty_h, cost_text.c_str());
    add(std::move(t_bank_h), i_bank_h, cost_text.c_str());
    add(std::move(t_vix_h), i_vix_h, "not tradable");

    altair::CurriculumOptions opt;
    opt.first_days = static_cast<std::int32_t>(first_days);
    opt.step_cap_days = static_cast<std::int32_t>(cap_days);
    std::size_t tests = 0;
    const auto run_one = [&](TrackResult& r) {
        const auto t0 = std::chrono::steady_clock::now();
        auto models = altair::curriculum_default_models();
        auto run = altair::curriculum_run(r.track, models, opt);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!run) {
            r.error = altair::curriculum_error_text(run.error());
            std::printf("  %-24s not run: %s\n", r.track.name.c_str(), r.error.c_str());
            return;
        }
        r.run = std::move(*run);
        r.ok = true;
        tests += r.run.models.size();
        std::printf("  %-24s %6zu decisions, %5d days, %2zu stages, %zu look-ahead refusals, %.0f s\n",
                    r.track.name.c_str(), r.track.rows(), r.track.days(), r.run.stages.size(),
                    r.run.lookahead_refusals, r.seconds);
        std::fflush(stdout);
    };
    for (auto& r : results) { run_one(r); }

    // The VIX model's out-of-sample forecasts -- Hedge's, which picks models by
    // their record in finished stages, not by hindsight -- as an input to the
    // index models. Run beside the originals so the two can be compared.
    ft::VixForecast vix_fc;
    for (const auto& r : results) {
        if (!r.ok || r.track.name != "INDIA VIX daily") { continue; }
        const auto h = std::find(r.run.models.begin(), r.run.models.end(), std::string{"Hedge"});
        const auto m = static_cast<std::size_t>(h - r.run.models.begin());
        for (std::size_t i = r.run.first_row; m < r.run.models.size() && i < r.track.rows(); ++i) {
            const auto& c = r.run.calls[m][i - r.run.first_row];
            if (c.made && std::isfinite(c.p_up)) { vix_fc[da::audit_day(r.track.t[i])] = c.p_up; }
        }
    }
    if (!vix_fc.empty()) {
        ft::TrackInfo f_nifty_d = i_nifty_d, f_bank_d = i_bank_d, f_fut_d = i_fut_d, f_nifty_h = i_nifty_h,
                      f_bank_h = i_bank_h;
        for (ft::TrackInfo* i : {&f_nifty_d, &f_bank_d, &f_fut_d, &f_nifty_h, &f_bank_h}) {
            // The builder recounts what it drops; the loader's counts stay.
            i->short_days = i->before_vix = i->no_vix = i->no_spot = i->roll_excluded = i->expiries = i->warmup = 0;
        }
        auto v_nifty_d = ft::build_daily({"NIFTY daily + VIX fc", "NIFTY", &nifty_d, &vix_d, nullptr, true, other_cost, &vix_fc}, f_nifty_d);
        auto v_bank_d = ft::build_daily({"BANKNIFTY daily + VIX fc", "BANKNIFTY", &bank_d, &vix_d, nullptr, true, other_cost, &vix_fc}, f_bank_d);
        auto v_fut_d = ft::build_daily({"NIFTY FUT daily + VIX fc", "NIFTY FUT", &fut_d, &vix_d, &nifty_d, true, other_cost, &vix_fc}, f_fut_d);
        auto v_nifty_h = ft::build_hourly({"NIFTY hourly + VIX fc", "NIFTY", &nifty_h, &vix_h, true, other_cost, &vix_fc}, f_nifty_h);
        auto v_bank_h = ft::build_hourly({"BANKNIFTY hourly + VIX fc", "BANKNIFTY", &bank_h, &vix_h, true, other_cost, &vix_fc}, f_bank_h);
        const std::size_t before = results.size();
        add(std::move(v_nifty_d), f_nifty_d, cost_text.c_str());
        add(std::move(v_bank_d), f_bank_d, cost_text.c_str());
        add(std::move(v_fut_d), f_fut_d, cost_text.c_str());
        add(std::move(v_nifty_h), f_nifty_h, cost_text.c_str());
        add(std::move(v_bank_h), f_bank_h, cost_text.c_str());
        for (std::size_t k = before; k < results.size(); ++k) { run_one(results[k]); }
    } else if (only.empty() || std::string{"daily + VIX fc hourly"}.find(only) != std::string::npos) {
        std::printf("  (no INDIA VIX daily forecasts: the '+ VIX fc' tracks were not built)\n");
    }

    for (auto& r : results) {
        if (!r.ok) { continue; }
        for (std::size_t m = 0; m < r.run.models.size(); ++m) {
            r.summary.push_back(altair::curriculum_summary(r.track, r.run, m, tests));
        }
    }

    std::ostringstream text;
    text << "Forecast curriculum: learn 3 days, forecast, keep score, refit on 6, 12, 24 ... days.\n"
         << "Accuracy = right / (right + wrong); unchanged bars and abstentions are not scored.\n"
         << "Significance is Bonferroni-corrected over " << tests << " model-track tests.\n";
    for (const auto& r : results) {
        if (!r.ok) { continue; }
        text << "\n" << r.track.name << " (" << r.track.rows() << " decisions, " << r.run.stages.size()
             << " stages, " << r.track.days() << " days)\n";
        std::vector<std::size_t> order(r.summary.size());
        for (std::size_t m = 0; m < order.size(); ++m) { order[m] = m; }
        std::stable_sort(order.begin(), order.end(), [&r](std::size_t a, std::size_t b) {
            const double x = r.summary[a].all.scored() > 0 ? r.summary[a].accuracy : -1.0;
            const double y = r.summary[b].all.scored() > 0 ? r.summary[b].accuracy : -1.0;
            return x > y;
        });
        for (const std::size_t m : order) {
            const auto& s = r.summary[m];
            if (s.all.scored() == 0) {
                text << "  " << pad(r.run.models[m], 24) << "  never forecast\n";
                continue;
            }
            text << "  " << pad(r.run.models[m], 24) << pad(fixed(pct(s.accuracy), 2) + "%", 7)
                 << "  [" << fixed(pct(s.lo95), 1) << ", " << fixed(pct(s.hi95), 1) << "]"
                 << "  n=" << s.all.scored() << "  up-rate " << fixed(pct(s.up_rate), 1) + "%";
            if (s.have_price) { text << "  skill vs RW " << fixed(100.0 * s.price.skill, 2, true) + "%"; }
            if (s.all.trades > 0) {
                text << "  net " << fixed(s.all.net_bp / static_cast<double>(s.all.trades), 2, true) << " bp/trade (t "
                     << fixed(s.all.net_t(), 1, true) << ")";
            }
            text << "  -- " << verdict(s, tests) << "\n";
        }
    }

    const std::vector<std::string> method = {
        "How the curriculum works",
        "Every model walks the same doubling schedule over trading days: learn days [0, 3) and forecast days [3, 6); "
        "then learn [0, 6) and forecast [6, 12); then 12, 24, 48 ... until the data ends. The last block is what is left.",
        "Inside a block the model is frozen: it never sees the outcomes it is forecasting. After the block it is refitted "
        "from scratch on everything seen so far. That refit is the 'update'.",
        "A call is RIGHT when the next close moved the called way, WRONG otherwise (a call with no direction is WRONG and also "
        "counted under 'No direction'; an exact 50/50 is broken by the expected return). An unchanged close is neither. A model "
        "that cannot fit yet (too few rows for its method) ABSTAINS, and abstentions are listed per stage, not scored.",
        "Daily: decide at 15:30 from that day's close; outcome = next trading day's close. Hourly: decide at the close of "
        "each of a full day's first six hourly bars (10:15 ... 15:15); outcome = the next hourly close. No overnight hour.",
        "Features are standardised on the training window only (models/dataset.hpp Scaler). Classical models tune one "
        "hyper-parameter on the last quarter of their training window. Ensembles, all from finished stages only: Vote "
        "(majority of the learning models), Champion (best record so far), Hedge (exponential weights on the record), "
        "Stack (a logistic regression on every model's past out-of-sample calls: whom to trust, invert or ignore), "
        "Stack (confident third) (the Stack only in its most confident third), Consensus 75% (the Vote only when three "
        "quarters of at least six callers agree). A filtered ensemble abstains otherwise: read its accuracy with its Coverage.",
        "Baselines: Coin flip (seeded), Always majority (the training up-rate), Momentum and Mean reversion (repeat or reverse "
        "the last move). 'Up-rate on same bars' is what calling UP every time would have scored; the best constant call is "
        "max(up-rate, 1 - up-rate) -- always down on INDIA VIX, which falls more often than it rises.",
        "Significance: z and two-sided p against 50 %, and one-sided p against the best constant call, both "
        "Bonferroni-corrected over every model on every track. Skill vs RW: "
        "1 - RMSE(model price) / RMSE(last price), from models/forecast_scorecard.hpp; the price verdict needs 200 "
        "forecasts and a paired t beyond 2.",
        "Trades: a call is acted on only when its expected move exceeds the round-trip cost of that day (futures STT 2 bp, "
        "5 bp from 2026-04-01, plus other charges and one tick). Net bp = direction x move - cost. VIX is not tradable.",
        "Data rules (see the Data sheet): seconds floored, repeated stamps keep the first bar, impossible OHLC widened and "
        "counted, hourly only on full 09:15-15:15 days, NIFTY futures daily only with roll-crossing outcomes excluded, "
        "rows without a same-time INDIA VIX bar dropped, history before INDIA VIX starts (2015) not used.",
        "Limits: the transformer is trained by finite differences (the repository's reference backend) with 400 SGD steps "
        "a stage, so it is the least-trained network. The SVM learns from its window's latest 1,500 rows. LSTM and GRU "
        "use analytic backpropagation through time.",
    };

    std::error_code ec;
    fs::create_directories(out, ec);
    std::vector<XlsxSheet> sheets{summary_sheet(results, tests), data_sheet(results), method_sheet(method)};
    for (const auto& r : results) {
        if (r.ok) { sheets.push_back(track_sheet(r)); }
    }
    const auto book = altair::xlsx::build_workbook(sheets);
    if (!book) {
        std::printf("  could not build the workbook\n");
        return 1;
    }
    {
        std::ofstream f(out / "forecast_curriculum.xlsx", std::ios::binary | std::ios::trunc);
        f.write(book->data(), static_cast<std::streamsize>(book->size()));
        if (!f) { std::printf("  could not write %s\n", (out / "forecast_curriculum.xlsx").string().c_str()); return 1; }
    }
    {
        std::ofstream f(out / "forecast_curriculum.txt", std::ios::binary | std::ios::trunc);
        f << text.str();
    }
    if (log) {
        for (const auto& r : results) {
            if (r.ok) { write_log(out / "forecast_log", r); }
        }
    }
    std::size_t refusals = 0;
    for (const auto& r : results) { refusals += r.ok ? r.run.lookahead_refusals : 0; }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("%s\n  wrote %s (%.0f s)\n", text.str().c_str(), (out / "forecast_curriculum.xlsx").string().c_str(), total);
    if (log) { std::printf("  wrote per-forecast logs under %s\n", (out / "forecast_log").string().c_str()); }
    bool all_ok = !results.empty();
    for (const auto& r : results) { all_ok = all_ok && r.ok; }
    return all_ok && refusals == 0 ? 0 : 1;
}
