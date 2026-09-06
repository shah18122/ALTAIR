// analytics/tests/test_garch.cpp -- P14-01/02/03.
//
// THE HORSE RACE IS THE CARD, NOT THE MODEL.
//
// QUANTLAB's Phase 5 ran EWMA against the GARCH family on its own data and
// EWMA won on QLIKE. That is a finding Altair inherits as a HYPOTHESIS, not as
// a fact: it was measured on FX and US indices, and whether it holds on 36
// years of NIFTY is a different question with a different answer. Building
// GARCH and having it lose to the twenty-line EWMA already in this tree is a
// legitimate and expected outcome, and the point of the exercise.
//
// Everything here is out-of-sample: parameters are fitted on the first half
// and scored on the second. An in-sample QLIKE comparison would favour the
// model with more parameters every time, which is arithmetic, not evidence.

#include <analytics/garch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

std::vector<double> load_closes(const std::string& path) {
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        double close = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 4 && !cell.empty()) { close = std::atof(cell.c_str()); }
            ++col;
        }
        if (close > 0.0) { out.push_back(close); }
    }
    return out;
}

/// RiskMetrics EWMA: v_t = lam * v_{t-1} + (1-lam) * r^2_{t-1}.
/// The GARCH special case omega = 0, alpha = 1-lam, beta = lam -- which is
/// exactly why it is the right control.
void ewma_filter(const std::vector<double>& r, double lam, double v0,
                 std::vector<double>& out) {
    out.assign(r.size(), v0);
    double v = v0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        out[i] = v;
        v = lam * v + (1.0 - lam) * r[i] * r[i];
    }
}

} // namespace

int main() {
    using altair::GarchParams;
    using altair::fit_garch;
    using altair::garch_error_text;
    using altair::garch_filter;
    using altair::proxy_mse;
    using altair::qlike;

    std::printf("P14-01/02/03 GARCH, GJR, and the volatility horse race\n");

    const std::string path =
        std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d/all.csv";
    const auto closes = load_closes(path);
    if (closes.size() < 2000) {
        std::printf("  SKIP: %s has %zu closes; need 2000+\n",
                    path.c_str(), closes.size());
        return 0;
    }
    std::vector<double> r;
    r.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        r.push_back(std::log(closes[i] / closes[i - 1]));
    }
    std::printf("  %zu daily log returns\n", r.size());

    // ---- WALK-FORWARD, NOT IN-SAMPLE --------------------------------------
    const std::size_t split = r.size() / 2;
    const std::vector<double> tr(r.begin(), r.begin() + static_cast<long>(split));
    const std::vector<double> te(r.begin() + static_cast<long>(split), r.end());
    std::printf("  fitted on the first %zu, scored on the last %zu\n\n",
                tr.size(), te.size());

    // ---- 1. GARCH(1,1) ----------------------------------------------------
    const auto g = fit_garch(tr.data(), tr.size(), false);
    check(g.has_value(), "GARCH(1,1) fits");
    if (!g) {
        std::printf("    %s\n", garch_error_text(g.error()));
        return 1;
    }
    std::printf("  GARCH(1,1)  omega %.3e  alpha %.4f  beta %.4f\n",
                g->p.omega, g->p.alpha, g->p.beta);
    std::printf("              persistence %.4f, half-life %.1f days,"
                " uncond vol %.2f%%/yr\n",
                g->p.persistence(), g->p.half_life(),
                100.0 * std::sqrt(g->p.unconditional_variance() * 252.0));

    check(g->p.stationary(),
          "and it is STATIONARY -- alpha + beta < 1, so the variance has a "
          "finite level to revert to and a multi-step forecast does not "
          "diverge");
    check(g->p.persistence() > 0.85,
          "with the high persistence every equity series shows: volatility "
          "clusters, which is the entire reason the model exists");

    // ---- 2. GJR: IS THERE A LEVERAGE EFFECT? ------------------------------
    const auto gjr = fit_garch(tr.data(), tr.size(), true);
    check(gjr.has_value(), "GJR-GARCH fits");
    if (gjr) {
        std::printf("\n  GJR-GARCH   alpha %.4f  beta %.4f  gamma %.4f"
                    " (se %.4f)\n",
                    gjr->p.alpha, gjr->p.beta, gjr->p.gamma, gjr->gamma_se);
        const double t_stat =
            gjr->gamma_se > 0.0 ? gjr->p.gamma / gjr->gamma_se : 0.0;
        std::printf("              gamma / se = %.2f\n", t_stat);
        // The point estimate is read THROUGH its error, which is the whole
        // discipline: a gamma of 0.05 with an se of 0.04 is not asymmetry.
        if (std::fabs(t_stat) > 2.0) {
            std::printf("              -> a leverage effect IS measurable: a "
                        "fall raises tomorrow's\n                 expected "
                        "variance more than a rise of the same size\n");
        } else {
            std::printf("              -> gamma is NOT distinguishable from "
                        "zero on this sample.\n                 No measurable "
                        "asymmetry, which is a real answer\n");
        }
        check(gjr->log_likelihood >= g->log_likelihood - 1e-9,
              "GJR nests GARCH, so its likelihood cannot be WORSE -- if it "
              "were, the optimiser failed rather than the model");
    }

    // ---- 3. THE RACE ------------------------------------------------------
    //
    // Out-of-sample, on the held-back half.
    {
        std::vector<double> v_garch(te.size()), v_gjr(te.size()), v_ewma;
        const auto f1 = garch_filter(te.data(), te.size(), g->p, v_garch.data());
        check(f1.has_value(), "the GARCH filter runs on the held-out half");

        double tv = 0.0;
        for (const double x : tr) { tv += x * x; }
        const double v0 = tv / static_cast<double>(tr.size());
        ewma_filter(te, 0.94, v0, v_ewma);          // RiskMetrics lambda

        std::vector<double> v_const(te.size(), v0);

        const auto q_g = qlike(v_garch.data(), te.data(), te.size());
        const auto q_e = qlike(v_ewma.data(), te.data(), te.size());
        const auto q_c = qlike(v_const.data(), te.data(), te.size());
        const auto m_g = proxy_mse(v_garch.data(), te.data(), te.size());
        const auto m_e = proxy_mse(v_ewma.data(), te.data(), te.size());
        const auto m_c = proxy_mse(v_const.data(), te.data(), te.size());

        double q_j = 0.0, m_j = 0.0;
        bool have_j = false;
        if (gjr) {
            if (garch_filter(te.data(), te.size(), gjr->p, v_gjr.data())) {
                const auto a = qlike(v_gjr.data(), te.data(), te.size());
                const auto b = proxy_mse(v_gjr.data(), te.data(), te.size());
                if (a && b) { q_j = *a; m_j = *b; have_j = true; }
            }
        }

        std::printf("\n  OUT-OF-SAMPLE, %zu days\n", te.size());
        std::printf("    %-24s %12s %14s\n", "", "QLIKE", "proxy MSE");
        std::printf("    %-24s %12.5f %14.3e\n", "constant (train var)",
                    q_c ? *q_c : 0.0, m_c ? *m_c : 0.0);
        std::printf("    %-24s %12.5f %14.3e\n", "EWMA (lambda 0.94)",
                    q_e ? *q_e : 0.0, m_e ? *m_e : 0.0);
        std::printf("    %-24s %12.5f %14.3e\n", "GARCH(1,1)",
                    q_g ? *q_g : 0.0, m_g ? *m_g : 0.0);
        if (have_j) {
            std::printf("    %-24s %12.5f %14.3e\n", "GJR-GARCH", q_j, m_j);
        }
        std::printf("    (QLIKE: lower is better. It punishes UNDER-predicted "
                    "variance hardest,\n     which is the right asymmetry for "
                    "anything that sizes a position.)\n");

        check(q_g && q_e && q_c, "every model scores");
        if (q_g && q_e && q_c) {
            check(*q_g < *q_c && *q_e < *q_c,
                  "both conditional models beat a CONSTANT variance -- if "
                  "they did not, volatility clustering would not be present "
                  "and none of this machinery would be worth running");

            std::printf("\n  VERDICT\n");
            if (*q_e <= *q_g) {
                std::printf("    EWMA WINS ON QLIKE, replicating QUANTLAB's "
                            "Phase 5 result on Indian\n    daily data. GARCH "
                            "has three fitted parameters against EWMA's one\n"
                            "    fixed constant and does not buy an "
                            "out-of-sample forecast with them.\n    The "
                            "twenty-line estimator already in analytics/ "
                            "stays the default.\n");
                if (m_g && m_e && *m_g < *m_e) {
                    // THE TWO LOSSES DISAGREE, and the header predicted this
                    // case: GARCH wins on proxy MSE and loses on QLIKE.
                    //
                    // Not a contradiction -- the proxy. MSE scores against the
                    // squared return, an unbiased but violently noisy
                    // estimator of variance, so it rewards a model that tracks
                    // the noise. QLIKE is robust to that and punishes
                    // under-prediction hardest. For sizing a position, being
                    // wrong about a quiet day costs little and being wrong
                    // about a violent one costs everything, so QLIKE is the
                    // loss that matches the consequence -- and it is the one
                    // this verdict follows.
                    std::printf("\n    Note: GARCH wins on proxy MSE (%.3e "
                                "vs %.3e) and loses on QLIKE.\n"
                                "    The squared-return proxy is unbiased but "
                                "very noisy, so MSE rewards\n    tracking the "
                                "noise. QLIKE is robust to it and punishes "
                                "under-predicted\n    variance hardest, which "
                                "is the asymmetry a position size actually\n"
                                "    faces. The disagreement is information, "
                                "not a tie.\n",
                                *m_g, *m_e);
                }
            } else {
                std::printf("    GARCH WINS ON QLIKE, which does NOT "
                            "replicate QUANTLAB's Phase 5\n    finding. Two "
                            "readings, and the difference matters: either "
                            "NIFTY's\n    variance dynamics differ from the "
                            "FX and US series it tested, or\n    something "
                            "here is fitted where it should not be. Before "
                            "this is\n    used, it needs a second horizon and "
                            "a rolling refit.\n");
            }
        }
    }

    // ---- 4. THE CONSTRAINT IS REFUSED, NOT CLAMPED ------------------------
    {
        GarchParams bad;
        bad.omega = 1e-6;
        bad.alpha = 0.20;
        bad.beta = 0.85;              // 1.05 persistence
        check(!bad.stationary(), "alpha + beta = 1.05 is not stationary");
        std::vector<double> v(10);
        const auto f = garch_filter(te.data(), 10, bad, v.data());
        check(!f && f.error() == altair::GarchError::NotStationary,
              "and the filter REFUSES it rather than clamping to 0.999 -- a "
              "clamped fit returns numbers from a model that does not exist, "
              "and they look exactly like a volatility forecast");

        GarchParams neg;
        neg.omega = -1e-6;
        neg.alpha = 0.1;
        neg.beta = 0.8;
        check(!garch_filter(te.data(), 10, neg, v.data()).has_value(),
              "a negative omega is refused too: it admits a negative variance");
    }

    check(!fit_garch(te.data(), 10, false).has_value(),
          "and ten observations is not enough to fit three parameters, which "
          "is refused rather than fitted with a wide shrug");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
