#include <models/classical.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
}

int main() {
    using namespace altair;

    // Logistic regression: a linearly separable, deliberately unbalanced set.
    {
        const std::vector<double> x{
            -3, -2, -2, -1, -1, -3, -1, -1,
             1,  1,  1,  3,  2,  1,  3,  2};
        const std::vector<std::uint8_t> y{0, 0, 0, 0, 1, 1, 1, 1};
        LogisticParams p; p.epochs = 900; p.learning_rate = 0.2; p.l2 = 1e-5;
        const auto m = LogisticRegression::fit(x, 8, 2, y, p);
        check(m.has_value(), "logistic regression fits finite binary data");
        if (m) {
            check(m->probability(std::array<double, 2>{2.5, 1.0}) > 0.9
                      && m->probability(std::array<double, 2>{-2.5, -1.0}) < 0.1,
                  "logistic regression separates the two classes");
            check(!LogisticRegression::fit(x, 8, 2,
                    std::vector<std::uint8_t>{0, 0, 0, 2, 1, 1, 1, 1}, p),
                  "logistic regression rejects labels outside {0,1}");
        }
    }

    // Random forest: shallow bootstrapped trees still learn a nonlinear XOR.
    {
        std::vector<double> x;
        std::vector<double> y;
        for (int a = -4; a <= 4; ++a) for (int b = -4; b <= 4; ++b) {
            x.push_back(static_cast<double>(a)); x.push_back(static_cast<double>(b));
            y.push_back((a < 0) == (b < 0) ? 1.0 : 0.0);
        }
        ForestParams p; p.trees = 48; p.max_depth = 4; p.min_leaf = 2; p.seed = 42;
        const auto m = RandomForest::fit(x, y.size(), 2, y, p);
        check(m && m->tree_count() == p.trees, "random forest builds deterministic trees");
        if (m) {
            const double a = m->predict(std::array<double, 2>{-3.0, -2.0});
            const double b = m->predict(std::array<double, 2>{-3.0,  2.0});
            check(a > 0.6 && b < 0.4, "random forest captures a nonlinear class boundary");
        }
        auto invalid = y;
        invalid[0] = 2.0;
        check(!RandomForest::fit(x, y.size(), 2, invalid, p),
              "classification forest rejects labels outside {0,1}");
    }

    // M01 SVM: linear separation and the nonlinear XOR that requires RBF.
    {
        const std::vector<double> linear_x{
            -2, -1, -1, -2, -2, -2, 1, 2, 2, 1, 2, 2};
        const std::vector<std::int8_t> linear_y{-1, -1, -1, 1, 1, 1};
        SvmParams p; p.kernel = SvmKernel::Linear; p.c = 10.0;
        const auto linear = SupportVectorMachine::fit(
            linear_x, linear_y.size(), 2, linear_y, p);
        check(linear && linear->classify(std::array<double, 2>{-3.0, -1.0}) == -1
                     && linear->classify(std::array<double, 2>{3.0, 1.0}) == 1,
              "linear C-SVM separates a published-style two-class fixture");

        const std::vector<double> xor_x{-1,-1, -1,1, 1,-1, 1,1};
        const std::vector<std::int8_t> xor_y{1, -1, -1, 1};
        p.kernel = SvmKernel::Rbf; p.gamma = 1.0; p.c = 100.0;
        const auto rbf = SupportVectorMachine::fit(xor_x, 4, 2, xor_y, p);
        bool exact = rbf.has_value();
        if (rbf) for (std::size_t i = 0; i < 4; ++i)
            exact = exact && rbf->classify(std::span<const double>(
                xor_x.data() + i * 2, 2)) == xor_y[i];
        check(exact, "RBF C-SVM represents XOR while the kernel contract stays explicit");
        check(!SupportVectorMachine::fit(linear_x, linear_y.size(), 2,
                  std::vector<std::int8_t>{-1,-1,0,1,1,1}, p),
              "SVM refuses labels outside {-1,+1}");
    }

    // M02 k-NN: train-fold scaling, exact neighbours and deterministic ties.
    {
        const std::vector<double> x{0,0, 0,2, 10,10, 10,12};
        const std::vector<double> y{0,0,1,1};
        const auto cls = KnnModel::fit(x, 4, 2, y, 3, true);
        check(cls && cls->predict(std::array<double,2>{0.2,1.0}) == 0.0
                  && cls->predict(std::array<double,2>{9.8,11.0}) == 1.0,
              "k-NN classification uses exact scaled neighbour distances");
        const auto one = KnnModel::fit(x, 4, 2, y, 1, true);
        const auto ns = one ? one->neighbours(std::array<double,2>{0.0,1.0})
                            : std::expected<std::vector<KnnNeighbour>, ClassicalError>{
                                  std::unexpected(ClassicalError::BadShape)};
        check(ns && ns->front().training_row == 0,
              "equal-distance k-NN ties resolve by stable training-row order");
        const auto reg = KnnModel::fit(x, 4, 2,
            std::vector<double>{1,3,9,11}, 2, false);
        const auto prediction = reg ? reg->predict(std::array<double,2>{0.0,1.0})
                                    : std::expected<double, ClassicalError>{
                                          std::unexpected(ClassicalError::BadShape)};
        check(prediction && std::fabs(*prediction - 2.0) < 1e-12,
              "k-NN regression averages the requested neighbours");
    }

    // PCA: the first component must explain the deliberately dominant factor.
    {
        std::vector<double> x;
        for (int i = -20; i <= 20; ++i) {
            const double f = static_cast<double>(i);
            x.insert(x.end(), {f, 2.0 * f + 0.01 * std::sin(f), -f});
        }
        const auto pca = fit_pca(x, 41, 3, 2);
        check(pca && pca->explained[0] > pca->explained[1] * 20.0,
              "PCA extracts the dominant cross-sectional factor");
        if (pca) check(pca->transform(std::array<double, 3>{1.0, 2.0, -1.0}).has_value(),
                        "PCA transforms a row with the fitted shape");
    }

    // M03: a linear autoencoder exactly reconstructs a rank-one manifold and
    // assigns a larger anomaly score to an off-manifold observation.
    {
        std::vector<double> x;
        for (int i = -30; i <= 30; ++i) {
            const double z = static_cast<double>(i) / 10.0;
            x.insert(x.end(), {z, 2.0 * z, -0.5 * z});
        }
        const auto ae = LinearAutoencoder::fit(x, 61, 3, 1);
        const auto ordinary = ae ? ae->reconstruction_error(
            std::array<double,3>{1.0, 2.0, -0.5})
            : std::expected<double, ClassicalError>{
                  std::unexpected(ClassicalError::BadShape)};
        const auto anomaly = ae ? ae->reconstruction_error(
            std::array<double,3>{1.0, -2.0, 3.0})
            : std::expected<double, ClassicalError>{
                  std::unexpected(ClassicalError::BadShape)};
        check(ordinary && anomaly && *ordinary < 1e-12 && *anomaly > 1.0,
              "linear autoencoder reconstructs its low-rank manifold and scores anomalies");
        const auto code = ae ? ae->encode(std::array<double,3>{1.0,2.0,-0.5})
                             : std::expected<std::vector<double>, ClassicalError>{
                                   std::unexpected(ClassicalError::BadShape)};
        check(code && code->size() == 1,
              "autoencoder exposes the requested bottleneck representation");
    }

    // VAR(1): a stable two-series system with known intercept and lag effect.
    {
        std::vector<double> x(240 * 2, 0.0);
        for (std::size_t t = 1; t < 240; ++t) {
            x[t * 2] = 0.5 + 0.7 * x[(t - 1) * 2];
            x[t * 2 + 1] = -0.25 + 0.4 * x[(t - 1) * 2 + 1];
        }
        const auto var = fit_var1(x, 240, 2);
        check(var.has_value(), "VAR(1) fits a stable multivariate series");
        if (var) {
            const auto next = var->predict(std::array<double, 2>{1.0, -1.0});
            check(next && std::fabs((*next)[0] - 1.2) < 1e-3
                      && std::fabs((*next)[1] + 0.65) < 1e-3,
                  "VAR(1) forecasts both series from one lag");
        }
    }

    std::printf("Classical models: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
