// models/fill_probability.hpp -- M21 calibrated resting-order fill model.
#pragma once

#include <models/classical.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

struct FillFeatures {
    double queue_ahead = 0.0;
    double depth_at_price = 0.0;
    double imbalance = 0.0;       // [-1,1], positive supports a buy fill
    double horizon_seconds = 0.0;
};

struct ReliabilityBin {
    std::size_t count = 0;
    double mean_probability = 0.0;
    double observed_frequency = 0.0;
};

class FillProbabilityModel {
public:
    [[nodiscard]] static std::expected<FillProbabilityModel, ClassicalError>
    fit(std::span<const FillFeatures> features,
        std::span<const std::uint8_t> filled) {
        if (features.size() < 20 || features.size() != filled.size())
            return std::unexpected(ClassicalError::TooFewSamples);
        FillProbabilityModel out;
        std::vector<double> raw(features.size() * kColumns);
        for (std::size_t i = 0; i < features.size(); ++i) {
            if (!valid(features[i])) return std::unexpected(ClassicalError::NonFinite);
            encode_raw(features[i], raw.data() + i * kColumns);
            for (std::size_t j = 0; j < kColumns; ++j)
                out.mean_[j] += raw[i * kColumns + j];
        }
        for (double& value : out.mean_) value /= static_cast<double>(features.size());
        for (std::size_t i = 0; i < features.size(); ++i)
            for (std::size_t j = 0; j < kColumns; ++j) {
                const double d = raw[i * kColumns + j] - out.mean_[j];
                out.scale_[j] += d * d;
            }
        for (double& value : out.scale_) {
            value = std::sqrt(value / static_cast<double>(features.size() - 1));
            if (!(value > 1e-12)) value = 1.0;
        }
        for (std::size_t i = 0; i < features.size(); ++i)
            for (std::size_t j = 0; j < kColumns; ++j)
                raw[i * kColumns + j] = (raw[i * kColumns + j] - out.mean_[j])
                                       / out.scale_[j];
        LogisticParams params;
        params.epochs = 1500; params.learning_rate = 0.08; params.l2 = 1e-3;
        const auto logistic = LogisticRegression::fit(
            raw, features.size(), kColumns, filled, params);
        if (!logistic) return std::unexpected(logistic.error());
        out.logistic_ = *logistic;
        return out;
    }

    [[nodiscard]] double probability(const FillFeatures& features) const noexcept {
        if (!valid(features)) return std::numeric_limits<double>::quiet_NaN();
        std::array<double, kColumns> row{};
        encode_raw(features, row.data());
        for (std::size_t j = 0; j < kColumns; ++j)
            row[j] = (row[j] - mean_[j]) / scale_[j];
        return logistic_.probability(row);
    }

    [[nodiscard]] static std::expected<std::vector<ReliabilityBin>, ClassicalError>
    reliability(std::span<const double> probability,
                std::span<const std::uint8_t> outcome,
                std::size_t bins = 10) {
        if (probability.size() != outcome.size() || probability.empty()
            || bins < 2 || bins > 100)
            return std::unexpected(ClassicalError::BadShape);
        std::vector<ReliabilityBin> out(bins);
        for (std::size_t i = 0; i < probability.size(); ++i) {
            if (!(probability[i] >= 0.0 && probability[i] <= 1.0)
                || outcome[i] > 1)
                return std::unexpected(ClassicalError::BadShape);
            const std::size_t bin = std::min(bins - 1,
                static_cast<std::size_t>(probability[i] * static_cast<double>(bins)));
            ++out[bin].count;
            out[bin].mean_probability += probability[i];
            out[bin].observed_frequency += static_cast<double>(outcome[i]);
        }
        for (auto& bin : out) if (bin.count != 0) {
            bin.mean_probability /= static_cast<double>(bin.count);
            bin.observed_frequency /= static_cast<double>(bin.count);
        }
        return out;
    }

private:
    static constexpr std::size_t kColumns = 4;
    [[nodiscard]] static bool valid(const FillFeatures& f) noexcept {
        return f.queue_ahead >= 0.0 && f.depth_at_price >= 0.0
            && f.imbalance >= -1.0 && f.imbalance <= 1.0
            && f.horizon_seconds > 0.0 && std::isfinite(f.queue_ahead)
            && std::isfinite(f.depth_at_price) && std::isfinite(f.imbalance)
            && std::isfinite(f.horizon_seconds);
    }
    static void encode_raw(const FillFeatures& f, double* out) noexcept {
        out[0] = std::log1p(f.queue_ahead);
        out[1] = std::log1p(f.depth_at_price);
        out[2] = f.imbalance;
        out[3] = std::log1p(f.horizon_seconds);
    }

    LogisticRegression logistic_;
    std::array<double, kColumns> mean_{};
    std::array<double, kColumns> scale_{};
};

} // namespace altair
