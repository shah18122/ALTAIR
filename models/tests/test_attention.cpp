// P8-06, P8-07 and P8-08 acceptance tests.
//
// Test 1 is the card: attention without a causal mask reads the answer. The
// test hides a target in position t+1 and measures how much of it an unmasked
// model recovers.
//
// Test 2: "same" padding centres the kernel, so half a dilated convolution's
// receptive field is the future.
//
// Test 3: a softmax over features is not feature importance.
//
// Test 4: a cross-sectional model needs cross-sectional normalisation.
//
// No check description here may contain the substring FAIL.

#include <models/attention.hpp>
#include <models/mlp.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

constexpr std::size_t kT = 16;
// Sixteen dimensions, not four. With kD = 4 the dot product between two
// random keys has a standard deviation comparable to the deliberate match, so
// the softmax never concentrates and the leak is diluted by noise rather than
// by the mask. Random keys separate as the dimension grows, which is the
// ordinary reason attention is run wide.
constexpr std::size_t kD = 16;

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void unmasked_attention_reads_the_answer()
{
    std::printf("\n1 unmasked_attention_reads_the_answer\n");
    // A sequence where the value at position t+1 IS the thing a forecast at
    // position t is supposed to predict. Query and key are made to match at
    // the neighbouring position, so attention will find it if it is allowed.
    static double qd[kT * kD], kd[kT * kD], vd[kT * kD];
    static double om[kT * kD], ou[kT * kD], sc[kT * kT];
    Lcg g{0xA77E7};
    for (std::size_t t = 0; t < kT; ++t) {
        for (std::size_t d = 0; d < kD; ++d) {
            const double base = g.normal();
            qd[t * kD + d] = base;
            kd[t * kD + d] = base;      // key at t matches query at t
        }
    }
    // THE ANSWER lives in v at each position: v[t] is the target for t-1.
    for (std::size_t t = 0; t < kT; ++t) {
        for (std::size_t d = 0; d < kD; ++d) {
            vd[t * kD + d] = static_cast<double>(t);
        }
    }
    // Make position t's query match position t+1's key exactly, so unmasked
    // attention puts nearly all its weight on the future neighbour.
    for (std::size_t t = 0; t + 1 < kT; ++t) {
        for (std::size_t d = 0; d < kD; ++d) {
            qd[t * kD + d] = kd[(t + 1) * kD + d] * 8.0;
        }
    }

    Matrix q{qd, kT, kD}, k{kd, kT, kD}, v{vd, kT, kD};
    Matrix masked{om, kT, kD}, unmasked{ou, kT, kD};
    check(attention(q, k, v, masked, sc, Causality::Masked).has_value()
          && attention(q, k, v, unmasked, sc, Causality::Unmasked).has_value(),
          "both attentions run");

    double leak = 0.0, honest = 0.0;
    std::size_t n = 0;
    for (std::size_t t = 0; t + 1 < kT; ++t) {
        const double target = static_cast<double>(t + 1);
        leak += std::fabs(unmasked.at(t, 0) - target);
        honest += std::fabs(masked.at(t, 0) - target);
        ++n;
    }
    leak /= static_cast<double>(n);
    honest /= static_cast<double>(n);

    std::printf("    v[t] holds the target for position t-1, and each query is"
                " built to match the\n    NEXT position's key:\n"
                "      UNMASKED: mean |output - the future value|  %8.4f\n"
                "      MASKED  : mean |output - the future value|  %8.4f\n",
                leak, honest);
    check(leak < 0.5,
          "unmasked attention recovers the future value almost exactly -- it"
          " simply attends to the position holding it");
    check(honest > 4.0 * leak,
          "while the causal mask leaves the model no better than its own past,"
          " which is the only information it will have in production");
    std::printf("    -> nothing about the shapes objects. The tensors are the"
                " right size, the softmax\n       sums to one, the loss falls,"
                " and the validation score is excellent because\n       the"
                " validation set has the same structure. It fails only where"
                " t+1 has not\n       happened yet.\n");

    // The mask is applied BEFORE the softmax, not after.
    Matrix m2{om, kT, kD};
    (void)attention(q, k, v, m2, sc, Causality::Masked);
    double row_sum = 0.0;
    for (std::size_t j = 0; j < kT; ++j) { row_sum += sc[3 * kT + j]; }
    check(near(row_sum, 1.0, 1e-9),
          "the masked attention row still sums to one -- the future positions"
          " are set to -infinity BEFORE the softmax, not zeroed after it");
    double future_weight = 0.0;
    for (std::size_t j = 4; j < kT; ++j) { future_weight += sc[3 * kT + j]; }
    check(future_weight < 1e-12,
          "and carry no weight at all. Zeroing after the softmax would leave"
          " them in the normalising sum, so every VISIBLE weight would be"
          " wrong even though no future value was read directly");

    check(attention(q, k, v, masked, sc, Causality::Unspecified).error()
          == AttentionError::NoCausality,
          "and attention with no causality chosen is refused: on a sentence"
          " unmasked is right, on a time series it is a look-ahead bug that"
          " improves every offline metric");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void same_padding_centres_the_kernel_on_the_future()
{
    std::printf("\n2 same_padding_centres_the_kernel_on_the_future\n");
    constexpr std::size_t T = 24, C = 1, F = 1, K = 5, Dl = 2;
    static double xd[T * C], wd[F * C * K], od[T * F];
    for (std::size_t i = 0; i < T * C; ++i) { xd[i] = 0.0; }
    for (std::size_t i = 0; i < F * C * K; ++i) { wd[i] = 1.0; }

    // Perturb ONE input and see which outputs move. That is the receptive
    // field, measured rather than derived.
    const std::size_t spike = 12;
    xd[spike] = 1.0;
    Matrix x{xd, T, C}, w{wd, F, C * K}, out{od, T, F};

    check(dilated_conv(x, w, nullptr, K, Dl, out, Padding::CausalLeft)
              .has_value(), "the causal convolution runs");
    std::size_t causal_before = 0, causal_after = 0;
    for (std::size_t t = 0; t < T; ++t) {
        if (out.at(t, 0) == 0.0) { continue; }
        if (t < spike) { ++causal_before; } else if (t > spike) { ++causal_after; }
    }

    check(dilated_conv(x, w, nullptr, K, Dl, out, Padding::Same).has_value(),
          "and so does the same-padded one");
    std::size_t same_before = 0, same_after = 0;
    for (std::size_t t = 0; t < T; ++t) {
        if (out.at(t, 0) == 0.0) { continue; }
        if (t < spike) { ++same_before; } else if (t > spike) { ++same_after; }
    }

    std::printf("    kernel %zu, dilation %zu; a spike at input %zu moves these"
                " outputs:\n"
                "      CausalLeft : %zu before the spike, %zu after\n"
                "      Same       : %zu before the spike, %zu after   <- the"
                " ones after it are the future\n",
                K, Dl, spike, causal_before, causal_after,
                same_before, same_after);
    check(causal_before == 0,
          "under causal padding NO output before the spike moves -- an input"
          " cannot affect an earlier output");
    check(same_before > 0,
          "under SAME padding several earlier outputs move, which means those"
          " outputs are functions of an input that had not arrived yet");
    check(forward_reach(K, Dl, Padding::Same) == 4
          && forward_reach(K, Dl, Padding::CausalLeft) == 0,
          "and the forward reach is (k-1)/2 * dilation = 4 samples for same"
          " padding and exactly zero for causal, which is the whole difference"
          " expressed as a number a test can assert on");
    std::printf("    -> and 'same' is the argument every library defaults to,"
                " because it is the right\n       answer for images. It keeps"
                " the output length equal to the input length,\n       which is"
                " what the name refers to -- not to causality.\n");

    check(dilated_conv(x, w, nullptr, K, Dl, out, Padding::Unspecified).error()
          == AttentionError::BadKernel,
          "a convolution with no padding chosen is refused");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_softmax_over_features_is_not_feature_importance()
{
    std::printf("\n3 a_softmax_over_features_is_not_feature_importance\n");
    // Two features that matter, and two that do not, with IDENTICAL gate
    // logits in each case.
    const double logits[2] = {0.2, 0.0};
    const double decisive[2] = {5.0, 4.0};
    const double useless[2] = {0.001, 0.0008};

    const auto d = variable_selection(decisive, logits, 2);
    const auto u = variable_selection(useless, logits, 2);
    check(d.has_value() && u.has_value(), "both selections compute");
    if (!d || !u) { return; }

    std::printf("    identical gate logits {%.1f, %.1f} over two very different"
                " feature sets:\n"
                "      decisive features {%.3f, %.3f}: weights {%.3f, %.3f},"
                " output magnitude %.4f\n"
                "      useless  features {%.3f, %.3f}: weights {%.3f, %.3f},"
                " output magnitude %.4f\n",
                logits[0], logits[1], decisive[0], decisive[1],
                d->weight[0], d->weight[1], d->output_magnitude,
                useless[0], useless[1], u->weight[0], u->weight[1],
                u->output_magnitude);

    check(near(d->weight[0], u->weight[0], 1e-12),
          "the softmax weights are IDENTICAL, because they depend only on the"
          " gate logits and not on whether the features carry anything");
    check(d->output_magnitude > 1000.0 * u->output_magnitude,
          "while the output magnitudes differ by three orders of magnitude --"
          " which is the only one of the two numbers that distinguishes"
          " 'feature 1 matters most' from 'neither matters'");
    std::printf("    -> so the selection reports BOTH. A caller reading only"
                " the weights learns which\n       feature won a contest, not"
                " whether the contest was about anything.\n");

    const double even[3] = {0.0, 0.0, 0.0};
    const double vals[3] = {1.0, 1.0, 1.0};
    const auto flat = variable_selection(vals, even, 3);
    check(flat.has_value() && near(flat->concentration(), 1.0, 1e-9),
          "and a concentration of 1.0 says the softmax split evenly and"
          " selected nothing at all, which a maximum weight of 0.333 does not"
          " make obvious on its own");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_cross_sectional_model_needs_cross_sectional_normalisation()
{
    std::printf("\n4 a_cross_sectional_model_needs_cross_sectional"
                "_normalisation\n");
    constexpr std::size_t kNames = 50;
    double today[kNames], ranks_before[kNames], ranks_after[kNames];
    double ts_z_before[kNames], ts_z_after[kNames];
    Lcg g{0xC1055};
    for (std::size_t i = 0; i < kNames; ++i) {
        today[i] = g.normal();
        // A per-instrument time-series z-score: each name against its OWN
        // history, which here is standard normal, so the z-score is the value.
        ts_z_before[i] = today[i];
    }
    check(cross_sectional_rank(today, kNames, ranks_before).has_value(),
          "the cross-section ranks");

    // A MARKET-WIDE SHOCK: everything falls by the same amount. Nothing has
    // changed relative to anything.
    const double shock = -3.0;
    double after[kNames];
    for (std::size_t i = 0; i < kNames; ++i) {
        after[i] = today[i] + shock;
        ts_z_after[i] = after[i];
    }
    check(cross_sectional_rank(after, kNames, ranks_after).has_value(),
          "and so does the shocked cross-section");

    double rank_move = 0.0, z_move = 0.0;
    for (std::size_t i = 0; i < kNames; ++i) {
        rank_move += std::fabs(ranks_after[i] - ranks_before[i]);
        z_move += std::fabs(ts_z_after[i] - ts_z_before[i]);
    }
    rank_move /= kNames;
    z_move /= kNames;

    std::printf("    %zu instruments, then a market-wide %.1f-sigma shock that"
                " moves every one of\n    them by exactly the same amount:\n"
                "      mean |change| in TIME-SERIES z-score      %.4f\n"
                "      mean |change| in CROSS-SECTIONAL rank     %.4f\n",
                kNames, shock, z_move, rank_move);
    check(near(rank_move, 0.0, 1e-12),
          "not one cross-sectional rank moves -- which is the truth, since"
          " nothing changed relative to anything");
    check(z_move > 2.0,
          "while every time-series z-score moves by the full shock, so a model"
          " fed them sees fifty strong sell signals on a day when the"
          " cross-section is unchanged");
    std::printf("    -> P8-02's scaler is correct everywhere else in this"
                " directory and is the wrong\n       tool here. Nothing in a"
                " shape check distinguishes them.\n");

    // Ties get a midrank rather than an arbitrary order.
    double flat[4] = {2.0, 2.0, 2.0, 2.0};
    double fr[4];
    check(cross_sectional_rank(flat, 4, fr).has_value(), "a flat cross-section ranks");
    check(near(fr[0], 0.0, 1e-12) && near(fr[3], 0.0, 1e-12),
          "and identical values all map to zero via the midrank, rather than"
          " to whatever the iteration order produced");
    check(cross_sectional_rank(flat, 1, fr).error()
          == MlpError::TooFewInstruments,
          "a universe of one has no cross-section and is refused");
}

} // namespace

int main()
{
    std::printf("altair attention, temporal convolution and MLP tests\n");
    unmasked_attention_reads_the_answer();
    same_padding_centres_the_kernel_on_the_future();
    a_softmax_over_features_is_not_feature_importance();
    a_cross_sectional_model_needs_cross_sectional_normalisation();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
