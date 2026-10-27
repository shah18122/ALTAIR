// app/regimes_main.cpp -- altair_regimes: the HMM's stress probability, day by
// day, walk-forward, from FYERS bars (models/hmm_regime.hpp).
//
// Reads one FYERS folder of bars (1d or intraday; a day's close is its last
// bar's), takes close-to-close log returns, and writes one row per day once
// `--min-history` returns exist: P(stress), P(stress next), the two states'
// daily sigma, the stress state's expected dwell, the day the fit used data
// up to, and how many Baum-Welch restarts agreed. The row's `reason` says in
// words what the regime is and why.
//
// The output is an INPUT, not a trade: altair_as_mm scales its risk aversion
// with yesterday's P(stress) (--regimes), altair_trader joins the same
// regime as a daily input, and a straddle gate can stand aside in stress.
//
// --leak-check reruns with every return after two cut days replaced, and
// needs every row up to each cut to come out bit for bit the same. Exit 3 if
// one does not.

#include <app/data_audit.hpp>
#include <app/fyers_source.hpp>
#include <models/hmm_regime.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace da = altair::data_audit;

void usage(const char* exe) {
    std::printf(
        "usage: %s --fyers DIR --out FILE [--tf 1d|1m|5m|15m|60m] [--min-history N] [--refit-every N]\n"
        "          [--window N] [--leak-check] [--not-fyers]\n"
        "  --fyers        a folder of FYERS bars (SOURCE.txt naming FYERS, or under a folder named fyers)\n"
        "  --tf           the bars' timeframe (default 1d)\n"
        "  --out          the CSV to write (default data/verified/regimes/<folder>.csv)\n"
        "  --min-history  returns before the first fit (default 500)\n"
        "  --refit-every  days between refits (default 21)\n"
        "  --window       fit on the last N returns (default 0: every return so far)\n"
        "  --leak-check   rerun with another future after two cuts; exit 3 if any earlier row changes\n"
        "  --not-fyers    run on a folder that is not FYERS: a smoke test, stamped NOT FYERS, never a result\n",
        exe);
}

std::string date_text(std::int64_t day) {
    const auto c = da::audit_civil_from_days(day);
    char b[16];
    std::snprintf(b, sizeof b, "%04lld-%02u-%02u", static_cast<long long>(c.y), c.m, c.d);
    return b;
}

} // namespace

int main(int argc, char** argv) {
    fs::path dir, out;
    std::string tf_name = "1d";
    altair::RegimeParams p;
    bool leak = false, not_fyers = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--fyers" && has) dir = argv[++i];
        else if (a == "--out" && has) out = argv[++i];
        else if (a == "--tf" && has) tf_name = argv[++i];
        else if (a == "--min-history" && has) p.min_history = static_cast<std::size_t>(std::max(100LL, std::atoll(argv[++i])));
        else if (a == "--refit-every" && has) p.refit_every = static_cast<std::size_t>(std::max(1LL, std::atoll(argv[++i])));
        else if (a == "--window" && has) p.window = static_cast<std::size_t>(std::max(0LL, std::atoll(argv[++i])));
        else if (a == "--leak-check") leak = true;
        else if (a == "--not-fyers") not_fyers = true;
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (dir.empty()) { usage(argv[0]); return 2; }
    const bool fyers = altair::app::is_fyers_folder(dir);
    if (!fyers && !not_fyers) {
        std::printf("%s is not marked as FYERS data (no SOURCE.txt naming FYERS, no 'fyers' folder in its path).\n"
                    "ALTAIR runs on FYERS data only. Fetch it with altair_fyers_history, or pass --not-fyers for a smoke test.\n",
                    dir.string().c_str());
        return 2;
    }
    const auto tf_opt = da::audit_tf_from_name(tf_name);
    if (!tf_opt) { std::printf("unknown --tf %s (1m, 5m, 15m, 60m or 1d)\n", tf_name.c_str()); return 2; }
    const int tf = *tf_opt;
    const auto s = da::load_audit_dir(dir, tf);
    std::vector<std::int64_t> t;
    std::vector<double> c;
    for (const auto& b : s.bars) { t.push_back(b.t); c.push_back(b.c); }
    std::vector<std::int64_t> day;
    std::vector<double> ret;
    altair::daily_log_returns(t, c, day, ret);
    std::printf("altair_regimes: %zu bars, %zu daily returns from %s%s\n", s.bars.size(), ret.size(),
                dir.string().c_str(), fyers ? "" : " (NOT FYERS: smoke test only)");
    const auto rows = altair::walk_forward_regimes(day, ret, p);
    if (rows.empty()) {
        std::printf("fewer than %zu returns: no regime yet\n", p.min_history);
        return 1;
    }

    int rc = 0;
    if (leak) {
        std::size_t diff = 0;
        for (const std::size_t cut : {ret.size() * 2 / 3, ret.size() - 30}) {
            if (cut < p.min_history || cut + 1 >= ret.size()) continue;
            auto other = ret;
            for (std::size_t i = cut + 1; i < other.size(); ++i) other[i] = -2.0 * other[i] + 0.01;
            const auto b = altair::walk_forward_regimes(day, other, p);
            for (std::size_t i = 0; i < rows.size() && i < b.size() && rows[i].day <= day[cut]; ++i)
                if (rows[i].p_stress != b[i].p_stress || rows[i].p_stress_next != b[i].p_stress_next) ++diff;
        }
        std::printf("leak check: %zu rows differ up to the cuts (%s)\n", diff, diff == 0 ? "pass" : "FAIL");
        if (diff != 0) rc = 3;
    }

    if (out.empty()) out = fs::path("data/verified/regimes") / (dir.filename().string() + ".csv");
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    std::ofstream o(out, std::ios::binary);
    if (!o) { std::printf("cannot write %s\n", out.string().c_str()); return 1; }
    o << "date,p_stress,p_stress_next,sigma_calm,sigma_stress,dwell_stress_days,fitted_on,agreeing_restarts,restarts,regime,source,reason\n";
    for (const auto& r : rows) {
        const bool st = r.p_stress > 0.5;
        char reason[256];
        std::snprintf(reason, sizeof reason,
            "%s: P(stress) %.2f; stress state %.2f%% a day vs calm %.2f%%, stays about %.0f days; fit on data to %s, %zu of %zu restarts agreed",
            st ? "stress" : "calm", r.p_stress, 100.0 * r.sigma_stress, 100.0 * r.sigma_calm, r.dwell_stress,
            date_text(r.fitted_on).c_str(), r.agreeing, r.restarts);
        char line[512];
        std::snprintf(line, sizeof line, "%s,%.6f,%.6f,%.6f,%.6f,%.2f,%s,%zu,%zu,%s,%s,\"%s\"\n",
                      date_text(r.day).c_str(), r.p_stress, r.p_stress_next, r.sigma_calm, r.sigma_stress,
                      r.dwell_stress, date_text(r.fitted_on).c_str(), r.agreeing, r.restarts, st ? "stress" : "calm",
                      fyers ? "FYERS" : "NOT FYERS", reason);
        o << line;
    }
    const auto& last = rows.back();
    std::printf("%zu rows to %s; latest %s: P(stress) %.2f, next %.2f\n", rows.size(), out.string().c_str(),
                date_text(last.day).c_str(), last.p_stress, last.p_stress_next);
    return rc;
}
