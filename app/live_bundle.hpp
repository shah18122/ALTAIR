// app/live_bundle.hpp -- what a live session was built from, in one file.
//
// A decision can only be reproduced from the exact inputs that made it. The
// bundle names them: a digest of every history series the engine read (count,
// first and last stamp, and an FNV-1a digest of every value), the universe,
// the charges file, the ledger it resumed from, the options it ran with, and
// for the direction models: the features, the scaler, what each fit chose,
// its parameters where they print, a fingerprint of the fitted function (its
// call on every training row), the walk-forward calibration and the learned
// gain and loss. Its own digest -- of everything except when it was written --
// is the bundle's id: two runs with the same id were built from the same
// inputs and fitted the same models.
//
// Written by altair_live_engine to data/live/bundles/<day>/bundle-<id>.json,
// with oos-<id>.csv: every out-of-sample walk-forward call behind the
// calibration. The tape (live/tape.hpp) carries the bundle, and a replay
// refuses to run on inputs whose bundle differs.

#pragma once

#include <app/data_audit.hpp>
#include <app/live_direction.hpp>
#include <live/fingerprint.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace altair::live_bundle {

namespace da = altair::data_audit;
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// JSON string escaping for the few characters our texts can hold.
[[nodiscard]] inline std::string esc(const std::string& s) {
    std::string o;
    for (const char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) o.push_back(' ');
        else o.push_back(c);
    }
    return o;
}
[[nodiscard]] inline std::string str(const std::string& s) { return "\"" + esc(s) + "\""; }
/// A number that reads back bit for bit; null for NaN/inf.
[[nodiscard]] inline std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[40];
    std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}

[[nodiscard]] inline std::string bars_digest(const std::vector<da::AuditBar>& v) {
    live::Fingerprint fp;
    for (const auto& b : v) fp.i64(b.t).f64(b.o).f64(b.h).f64(b.l).f64(b.c);
    return "{\"bars\": " + std::to_string(v.size()) + ", \"first\": " + std::to_string(v.empty() ? 0 : v.front().t)
         + ", \"last\": " + std::to_string(v.empty() ? 0 : v.back().t) + ", \"digest\": \"" + fp.hex() + "\"}";
}
[[nodiscard]] inline std::string closes_digest(const std::map<std::int64_t, double>& m) {
    live::Fingerprint fp;
    for (const auto& [d, c] : m) fp.i64(d).f64(c);
    return "{\"days\": " + std::to_string(m.size()) + ", \"first\": " + std::to_string(m.empty() ? 0 : m.begin()->first)
         + ", \"last\": " + std::to_string(m.empty() ? 0 : m.rbegin()->first) + ", \"digest\": \"" + fp.hex() + "\"}";
}
[[nodiscard]] inline std::string text_digest(const std::string& s) {
    return "{\"bytes\": " + std::to_string(s.size()) + ", \"digest\": \"" + live::Fingerprint{}.text(s).hex() + "\"}";
}
/// The whole file, or empty when it cannot be read.
[[nodiscard]] inline std::string file_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

/// The direction models' part: everything the 10:15 decision is made from.
[[nodiscard]] inline std::string direction_json(const live_direction::DirectionShared& s) {
    std::string o = "{\"ok\": " + std::string(s.ok ? "true" : "false") + ", \"why\": " + str(s.why)
                  + ", \"history_days\": " + std::to_string(s.history_days) + ", \"fitted_rows\": " + std::to_string(s.fitted_rows)
                  + ", \"track_digest\": " + str(s.track_fingerprint) + ", \"gate_z\": " + num(s.gate_z)
                  + ", \"cost_other_bp\": " + num(s.other_cost_bp) + ", \"features\": [";
    for (std::size_t j = 0; j < s.feature_names.size(); ++j) {
        o += (j ? ", " : "") + std::string("{\"name\": ") + str(s.feature_names[j]) + ", \"mean\": "
           + num(j < s.scaler_mean.size() ? s.scaler_mean[j] : kNaN) + ", \"sd\": " + num(j < s.scaler_sd.size() ? s.scaler_sd[j] : kNaN) + "}";
    }
    o += "], \"models\": [";
    for (std::size_t m = 0; m <= s.names.size() && m < s.cal.size(); ++m) {
        const bool vote = m == s.names.size();
        const auto pay = s.payoff[m].estimate();
        o += (m ? ",\n    " : "\n    ") + std::string("{\"name\": ") + str(vote ? "Vote" : s.names[m]);
        if (!vote) {
            o += ", \"fit\": " + str(m < s.fit_note.size() ? s.fit_note[m] : "") + ", \"params\": "
               + str(m < s.fit_params.size() ? s.fit_params[m] : "") + ", \"fingerprint\": "
               + str(m < s.fit_fingerprint.size() ? s.fit_fingerprint[m] : "");
        }
        o += ", \"walk_forward\": {\"scored\": " + std::to_string(s.scored[m]) + ", \"hit_rate\": " + num(s.accuracy[m])
           + ", \"calibration\": " + str(s.cal[m].state()) + ", \"gain_sigmas\": " + num(pay.gain) + ", \"loss_sigmas\": "
           + num(pay.loss) + ", \"gain_se\": " + num(pay.gain_se) + ", \"loss_se\": " + num(pay.loss_se) + "}}";
    }
    return o + "\n  ]}";
}

/// Every out-of-sample call behind the calibration, oldest first.
[[nodiscard]] inline bool write_oos_csv(const std::filesystem::path& path, const live_direction::DirectionShared& s) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "decision_time,model,call,p_up,q_calibrated,return_to_1520,right\n";
    char b[64];
    for (const auto& c : s.oos) {
        const std::string name = c.model < s.names.size() ? s.names[c.model] : "Vote";
        f << c.t << ",\"" << name << "\"," << (c.dir > 0 ? "UP" : "DOWN") << ',';
        if (std::isfinite(c.p_up)) { std::snprintf(b, sizeof b, "%.6f", c.p_up); f << b; }
        f << ',';
        if (std::isfinite(c.q_cal)) { std::snprintf(b, sizeof b, "%.6f", c.q_cal); f << b; }
        std::snprintf(b, sizeof b, ",%.8f,%d\n", c.ret, c.right ? 1 : 0);
        f << b;
    }
    f.flush();
    return static_cast<bool>(f);
}

} // namespace altair::live_bundle
