// risk/factor_risk.hpp -- M18 factor exposure and variance decomposition.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

enum class FactorRiskError : std::uint8_t {
    BadShape,
    NonFinite,
    NegativeVariance,
    NonSymmetricCovariance
};

struct FactorRiskDecomposition {
    std::vector<double> exposure;
    std::vector<double> factor_contribution;
    double factor_variance = 0.0;
    double idiosyncratic_variance = 0.0;
    double total_variance = 0.0;
};

/// Decompose portfolio variance under r=Bf+epsilon.
/// holdings are signed currency notionals; beta is asset-major [asset,factor];
/// factor_covariance is [factor,factor], and idiosyncratic_variance is per asset.
[[nodiscard]] inline std::expected<FactorRiskDecomposition, FactorRiskError>
factor_risk(std::span<const double> holdings, std::span<const double> beta,
            std::size_t factors, std::span<const double> factor_covariance,
            std::span<const double> idiosyncratic_variance) {
    const std::size_t assets = holdings.size();
    if (assets == 0 || factors == 0 || beta.size() != assets * factors
        || factor_covariance.size() != factors * factors
        || idiosyncratic_variance.size() != assets)
        return std::unexpected(FactorRiskError::BadShape);
    for (std::size_t i = 0; i < factors; ++i) {
        for (std::size_t j = 0; j < factors; ++j) {
            const double a = factor_covariance[i * factors + j];
            const double b = factor_covariance[j * factors + i];
            if (!std::isfinite(a)) return std::unexpected(FactorRiskError::NonFinite);
            if (std::fabs(a - b) > 1e-12 * (1.0 + std::fabs(a) + std::fabs(b)))
                return std::unexpected(FactorRiskError::NonSymmetricCovariance);
        }
        if (factor_covariance[i * factors + i] < 0.0)
            return std::unexpected(FactorRiskError::NegativeVariance);
    }
    FactorRiskDecomposition out;
    out.exposure.assign(factors, 0.0);
    out.factor_contribution.assign(factors, 0.0);
    for (std::size_t asset = 0; asset < assets; ++asset) {
        if (!std::isfinite(holdings[asset])
            || !std::isfinite(idiosyncratic_variance[asset]))
            return std::unexpected(FactorRiskError::NonFinite);
        if (idiosyncratic_variance[asset] < 0.0)
            return std::unexpected(FactorRiskError::NegativeVariance);
        for (std::size_t factor = 0; factor < factors; ++factor) {
            const double loading = beta[asset * factors + factor];
            if (!std::isfinite(loading))
                return std::unexpected(FactorRiskError::NonFinite);
            out.exposure[factor] += holdings[asset] * loading;
        }
        out.idiosyncratic_variance += holdings[asset] * holdings[asset]
                                      * idiosyncratic_variance[asset];
    }
    for (std::size_t i = 0; i < factors; ++i) {
        double marginal = 0.0;
        for (std::size_t j = 0; j < factors; ++j)
            marginal += factor_covariance[i * factors + j] * out.exposure[j];
        out.factor_contribution[i] = out.exposure[i] * marginal;
        out.factor_variance += out.factor_contribution[i];
    }
    if (out.factor_variance < -1e-9)
        return std::unexpected(FactorRiskError::NegativeVariance);
    if (out.factor_variance < 0.0) out.factor_variance = 0.0;
    out.total_variance = out.factor_variance + out.idiosyncratic_variance;
    return out;
}

} // namespace altair
