// P8-04 and P8-05 acceptance tests.
//
// Test 1 is the card: the forget-gate bias decides whether the cell has any
// long-range memory at all, and zero is what you get by not choosing.
//
// Test 2: the GRU's update gate plays the forget gate's role with the OPPOSITE
// sign, so copying the LSTM's +1 shortens memory instead of lengthening it.
//
// Test 3: order matters -- a reversed sequence must give a different answer,
// or the recurrence is not recurring.
//
// No check description here may contain the substring FAIL.

#include <models/recurrent.hpp>

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

constexpr std::size_t kH = 16;
constexpr std::size_t kX = 4;
constexpr std::size_t kSeq = 12;
constexpr std::size_t kRows = 1500;

double xs[kRows * kX], ys[kRows], ws[kRows], preds[kRows];
std::size_t bars[kRows];
LabelWindow wins[kRows];

/// A series where the label DOES depend on the features, so a working model
/// can beat the mean and a broken one cannot.
Dataset make_learnable(std::uint64_t seed)
{
    Lcg g{seed};
    double state = 0.0;
    for (std::size_t i = 0; i < kRows; ++i) {
        for (std::size_t j = 0; j < kX; ++j) { xs[i * kX + j] = g.normal(); }
        state = 0.85 * state + xs[i * kX];      // a persistent driver
        ys[i] = state + 0.3 * g.normal();
        ws[i] = 1.0;
        bars[i] = i;
        wins[i] = LabelWindow{i, i + 1};
    }
    Dataset d{};
    d.x = Matrix{xs, kRows, kX};
    d.y = ys;
    d.weight = ws;
    d.bar = bars;
    d.window = wins;
    d.rows = kRows;
    return d;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void the_forget_gate_bias_decides_whether_there_is_memory()
{
    std::printf("\n1 the_forget_gate_bias_decides_whether_there_is_memory\n");
    LstmCell<kH, kX> zero, one, two;
    zero.init(1234, 0.0);       // what you get by not choosing
    one.init(1234, 1.0);        // the convention
    two.init(1234, 2.0);

    std::printf("    forget bias -> retention per step -> memory half-life ->"
                " fraction left after 20 steps\n");
    double left_zero = 0.0, left_one = 0.0;
    for (const auto* p : {&zero, &one, &two}) {
        const double r = p->initial_retention();
        const double hl = p->memory_half_life();
        const double after20 = std::pow(r, 20.0);
        std::printf("      %+.1f            %.4f              %6.2f steps  "
                    "        %.3e\n",
                    std::log(r / (1.0 - r)), r, hl, after20);
        if (near(r, 0.5, 1e-9)) { left_zero = after20; }
        if (near(r, sigmoid(1.0), 1e-9)) { left_one = after20; }
    }

    check(near(zero.initial_retention(), 0.5, 1e-12),
          "a ZERO forget bias puts the gate at sigmoid(0) = 0.5, so the cell"
          " keeps half its contents every single step before it has learned"
          " anything");
    check(near(zero.memory_half_life(), 1.0, 1e-9),
          "which is a memory half-life of exactly ONE step");
    check(one.memory_half_life() > 2.0,
          "a bias of 1 more than doubles it");
    check(two.memory_half_life() > 2.0 * one.memory_half_life(),
          "and a bias of 2 more than doubles it again -- the half-life grows"
          " faster than the bias, because the gate is a logistic");
    std::printf("    -> after 20 steps a zero-bias cell retains %.1e of what it"
                " held and a unit-bias\n       cell retains %.1e -- %.0fx more."
                " The multiplicative path through the forget\n       gate is"
                " the ONLY route a gradient has across many steps, so a network"
                " that\n       starts at 0.5 has to climb out of the vanishing"
                " regime before it can begin\n       learning what to remember,"
                " and usually does not.\n",
                left_zero, left_one, left_one / left_zero);
    check(left_one > 100.0 * left_zero,
          "two orders of magnitude, from one initialisation constant nobody"
          " sets deliberately");

    // The bias is a required argument, so it cannot be forgotten -- there is
    // no init() overload without it.
    LstmCell<kH, kX> c;
    c.init(1, 1.0);
    LstmState s{};
    double x[kX] = {1.0, 0.0, 0.0, 0.0};
    c.step(x, s);
    bool finite = true;
    for (std::size_t i = 0; i < kH; ++i) {
        if (!std::isfinite(s.h[i]) || !std::isfinite(s.c[i])) { finite = false; }
    }
    check(finite, "and one step produces a finite state");
    bool bounded = true;
    for (std::size_t i = 0; i < kH; ++i) {
        if (std::fabs(s.h[i]) > 1.0) { bounded = false; }
    }
    check(bounded,
          "with the hidden output inside [-1, 1], because it is a tanh through"
          " an output gate -- an LSTM whose h escapes that range has a gate"
          " wired to the wrong activation");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void the_gru_update_gate_has_the_opposite_sign()
{
    std::printf("\n2 the_gru_update_gate_has_the_opposite_sign\n");
    GruCell<kH, kX> copied, correct;
    copied.init(99, +1.0);      // the LSTM's convention, copied across
    correct.init(99, -1.0);     // what actually lengthens a GRU's memory

    std::printf("    h = (1-z)*h + z*n, so RETAINING means a SMALL z:\n"
                "      update bias +1.0 (LSTM's convention copied):"
                " retention %.4f, half-life %.2f steps\n"
                "      update bias -1.0 (correct for a GRU)       :"
                " retention %.4f, half-life %.2f steps\n",
                copied.initial_retention(), copied.memory_half_life(),
                correct.initial_retention(), correct.memory_half_life());
    check(correct.memory_half_life() > copied.memory_half_life(),
          "a NEGATIVE update bias lengthens a GRU's memory and a positive one"
          " shortens it -- the opposite of the LSTM, because the GRU's gate"
          " multiplies the CANDIDATE rather than the carried state");
    check(copied.memory_half_life() < 1.0,
          "so copying the LSTM's +1 across gives a half-life under one step:"
          " the exact opposite of the intent, from a change that looks like"
          " consistency");

    // The GRU has no cell state at all, which is the structural difference.
    GruCell<kH, kX> g;
    g.init(7, -1.0);
    double h[kH] = {};
    double x[kX] = {1.0, 0.0, 0.0, 0.0};
    g.step(x, h);
    bool bounded = true;
    for (std::size_t i = 0; i < kH; ++i) {
        if (std::fabs(h[i]) > 1.0) { bounded = false; }
    }
    check(bounded,
          "a GRU's hidden state stays in [-1, 1] -- it is a convex combination"
          " of the previous state and a tanh, so it cannot leave the range the"
          " previous state was in");
    std::printf("    -> and there is no second vector: an LSTM's cell state is"
                " protected by the output\n       gate and can hold a value"
                " while emitting nothing about it. A GRU's hidden\n       state"
                " is both memory and output, so remembering forces it to speak."
                " That is\n       the whole difference, and it bites exactly on"
                " a value carried across a gap\n       without influencing the"
                " outputs in between.\n");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void order_matters_and_the_readout_learns()
{
    std::printf("\n3 order_matters_and_the_readout_learns\n");
    const Dataset d = make_learnable(0x0EDE12A1);

    RecurrentReadout<LstmCell<kH, kX>, kH, kX, kSeq> lstm{1.0, 1e-3};
    lstm.reset(2026);

    // The SAME inputs in reverse order must give a different encoding. If they
    // do not, the loop is overwriting its state instead of carrying it -- a
    // recurrence that is not recurring, which passes every shape check.
    double h_fwd[kH], h_rev[kH];
    lstm.encode(d, 200, h_fwd);
    // Encode a window whose contents are the same rows in the opposite order
    // by walking a reversed copy.
    static double rev[kRows * kX];
    for (std::size_t i = 0; i < kRows; ++i) {
        for (std::size_t j = 0; j < kX; ++j) {
            rev[i * kX + j] = xs[(kRows - 1 - i) * kX + j];
        }
    }
    Dataset r = d;
    r.x = Matrix{rev, kRows, kX};
    lstm.encode(r, kRows - 1 - (200 - (kSeq - 1)), h_rev);
    double diff = 0.0;
    for (std::size_t i = 0; i < kH; ++i) {
        diff += std::fabs(h_fwd[i] - h_rev[i]);
    }
    std::printf("    the same %zu rows encoded forwards and backwards differ by"
                " %.4f in total\n", kSeq, diff);
    check(diff > 1e-6,
          "reversing the sequence changes the encoding -- a cell whose output"
          " is order-invariant is not recurring, and nothing about its shapes"
          " would say so");

    // And it trains: on a series whose label really does depend on a
    // persistent driver, the readout beats predicting the mean.
    Splits s{};
    s.train = Block{0, 900};
    s.validation = Block{900, 1200};
    s.test = Block{1200, kRows};
    TrainConfig c{};
    c.lr0 = 0.01;
    c.max_epochs = 3;
    c.schedule = LrSchedule::Constant;
    c.seed = 2026;

    Trainer t;
    static double best[kH + 1];
    const auto res = t.run(lstm, d, s, c, preds, best);
    check(res.has_value(), "the readout trains");
    if (!res) { return; }

    // The baseline: predict the training mean everywhere.
    double m = 0.0;
    for (std::size_t i = s.train.start; i < s.train.end; ++i) { m += d.y[i]; }
    m /= static_cast<double>(s.train.size());
    double base = 0.0;
    for (std::size_t i = s.validation.start; i < s.validation.end; ++i) {
        base += (d.y[i] - m) * (d.y[i] - m);
    }
    base /= static_cast<double>(s.validation.size());

    std::printf("    validation MSE: model %.4f, predict-the-mean baseline"
                " %.4f  (%.0f%% of baseline)\n",
                res->best_validation_loss, base,
                100.0 * res->best_validation_loss / base);
    check(res->best_validation_loss < base,
          "the LSTM readout beats the predict-the-mean baseline on a series"
          " whose label genuinely depends on a persistent driver -- so the"
          " forward pass, the encoding and the ridge solve are all doing"
          " something, not merely running");
    std::printf("    -> the recurrent weights are FIXED at their seeded"
                " initialisation and only the\n       linear readout is"
                " solved. That is echo-state / reservoir computing: real,\n"
                "       named, trains end to end, and honestly weaker than"
                " BPTT. Autograd is what a\n       LibTorch backend behind this"
                " same interface brings.\n");

    // A GRU over the same data, same interface.
    RecurrentReadout<GruCell<kH, kX>, kH, kX, kSeq> gru{-1.0, 1e-3};
    Trainer t2;
    const auto gres = t2.run(gru, d, s, c, preds, best);
    check(gres.has_value() && gres->best_validation_loss < base,
          "and a GRU over the identical data and the identical interface also"
          " beats the baseline");
    if (gres) {
        std::printf("    GRU validation MSE %.4f against the LSTM's %.4f\n",
                    gres->best_validation_loss, res->best_validation_loss);
    }
}

} // namespace

int main()
{
    std::printf("altair LSTM and GRU tests\n");
    the_forget_gate_bias_decides_whether_there_is_memory();
    the_gru_update_gate_has_the_opposite_sign();
    order_matters_and_the_readout_learns();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
