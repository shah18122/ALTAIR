// app/quantum_smile_main.cpp -- altair_quantum_smile: Orrell's eq. 8 smile on
// real NIFTY / BANKNIFTY option closes.
//
// analytics/quantum_oscillator.hpp says volatility given a log move x over T is
//     sigma_x^2 = sigma^2 + x^2 / (2T).
// Read as an option smile, with x = ln(K / F), it predicts the wings from the
// floor with no free slope. Each bhavcopy day, for the first monthly expiry
// 14-60 days out: out-of-the-money closes (puts below the future, calls above)
// within 10 % of it, inverted under Black-76 on the same-expiry future. Then:
//   * flat         every strike at the ATM strike's IV (the no-smile baseline)
//   * Orrell ATM   sqrt(ATM^2 + k^2 / 2T): the paper's slope, anchored at ATM, nothing fitted
//   * Orrell fit   the same slope, sigma fitted to the day's smile
//   * free         a + b k^2 fitted: what slope the market wants (b x 2T = 1 is Orrell's)
// RMSE in vol points against the market, averaged over days.
//
// Read-only; writes nothing. Fetch the files with ops/fetch_bhavcopy.ps1.

#include <analytics/iv.hpp>
#include <analytics/quantum_oscillator.hpp>
#include <app/bhavcopy.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    fs::path dir = "data/bhavcopy";
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--bhavcopy-dir" && i + 1 < argc) { dir = argv[++i]; continue; }
        std::printf("usage: %s [--bhavcopy-dir DIR]   (default data/bhavcopy)\n", argv[0]);
        return a == "--help" || a == "-h" ? 0 : 2;
    }
    constexpr double kRate = 0.065;
    std::printf("Orrell eq. 8 smile on NSE option closes (%s)\n", dir.string().c_str());
    for (const std::string sym : {"NIFTY", "BANKNIFTY"}) {
        altair::bhavcopy::LoadReport rep;
        const auto days = altair::bhavcopy::load_dir(dir, sym, rep);
        if (days.empty()) { std::printf("  %s: no bhavcopy under %s\n", sym.c_str(), dir.string().c_str()); continue; }
        std::size_t n = 0, beats_flat = 0, atm_beats_flat = 0;
        double e_flat = 0, e_atm = 0, e_orr = 0, e_free = 0, e_lo_atm = 0, e_hi_atm = 0;
        std::vector<double> slope;
        for (const auto& [day, d] : days) {
            for (const auto& [e, chain] : d.opt) {
                if (e - day < 14 || e - day > 60) { continue; }
                const auto f = d.fut.find(e);
                if (f == d.fut.end() || !(f->second > 0.0)) { break; }
                const double F = f->second, T = static_cast<double>(e - day) / 365.0;
                std::vector<double> k, iv;
                double atm = std::nan(""), atm_gap = 1e300;
                for (const auto& [K, leg] : chain) {
                    const double x = std::log(K / F);
                    if (std::fabs(x) > 0.10) { continue; }
                    const bool call = K >= F;
                    const double px = call ? leg.call : leg.put;
                    if (!std::isfinite(px) || px < 0.5) { continue; }
                    const auto v = altair::implied_vol_black76(call ? altair::OptionRight::Call : altair::OptionRight::Put,
                                                               altair::Price{std::llround(px * 100.0)},
                                                               altair::Price{std::llround(F * 100.0)},
                                                               altair::Price{std::llround(K * 100.0)}, altair::Years{T}, kRate);
                    if (!v || !(v->vol.raw() > 0.01) || v->vol.raw() > 3.0) { continue; }
                    k.push_back(x);
                    iv.push_back(v->vol.raw());
                    if (std::fabs(x) < atm_gap) { atm_gap = std::fabs(x); atm = v->vol.raw(); }
                }
                if (k.size() < 6 || !std::isfinite(atm) || atm_gap > 0.01) { break; }
                const auto fit = altair::qosc::fit_smile(k, iv, T);
                double ef = 0, ea = 0, elo = 0, ehi = 0;
                std::size_t nlo = 0, nhi = 0;
                for (std::size_t i = 0; i < k.size(); ++i) {
                    const double pa = altair::qosc::smile_vol(atm, k[i], T);
                    ef += (iv[i] - atm) * (iv[i] - atm);
                    ea += (iv[i] - pa) * (iv[i] - pa);
                    if (k[i] < 0) { elo += iv[i] - pa; ++nlo; } else { ehi += iv[i] - pa; ++nhi; }
                }
                const double dn = static_cast<double>(k.size());
                ef = std::sqrt(ef / dn);
                ea = std::sqrt(ea / dn);
                e_flat += ef; e_atm += ea; e_orr += fit.rmse_orrell; e_free += fit.rmse_free;
                if (nlo) { e_lo_atm += elo / static_cast<double>(nlo); }
                if (nhi) { e_hi_atm += ehi / static_cast<double>(nhi); }
                beats_flat += fit.rmse_orrell < ef ? 1u : 0u;
                atm_beats_flat += ea < ef ? 1u : 0u;
                slope.push_back(fit.b * 2.0 * T);
                ++n;
                break;   // the first expiry in the window only
            }
        }
        if (n == 0) { std::printf("  %s: no day had a usable smile\n", sym.c_str()); continue; }
        std::sort(slope.begin(), slope.end());
        const double dn = static_cast<double>(n);
        std::printf("  %s: %zu days (%zu files)\n", sym.c_str(), n, rep.files);
        std::printf("    RMSE, vol points: flat %.2f  Orrell ATM-anchored %.2f  Orrell fitted %.2f  free slope %.2f\n",
                    100 * e_flat / dn, 100 * e_atm / dn, 100 * e_orr / dn, 100 * e_free / dn);
        std::printf("    Orrell ATM-anchored beats flat on %.1f %% of days; fitted on %.1f %%\n", 100.0 * static_cast<double>(atm_beats_flat) / dn,
                    100.0 * static_cast<double>(beats_flat) / dn);
        std::printf("    the market's slope as a multiple of Orrell's 1/(2T): median %.2f (quartiles %.2f, %.2f)\n",
                    slope[slope.size() / 2], slope[slope.size() / 4], slope[3 * slope.size() / 4]);
        std::printf("    ATM-anchored error by side (market minus model): puts %+.2f, calls %+.2f vol points -- the skew it lacks\n",
                    100 * e_lo_atm / dn, 100 * e_hi_atm / dn);
    }
    return 0;
}
