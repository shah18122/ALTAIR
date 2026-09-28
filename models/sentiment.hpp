// models/sentiment.hpp -- point-in-time, auditable finance sentiment baseline.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace altair {
enum class SentimentError : std::uint8_t { EmptyText, NotAvailable, NoVocabulary };
struct SentimentDocument {
    std::uint64_t published_ns = 0;
    std::uint64_t available_ns = 0;
    std::uint64_t entity = 0;
    std::string text;
};
struct SentimentScore {
    std::uint64_t entity = 0;
    std::uint64_t available_ns = 0;
    double score = 0.0;
    std::size_t matched_tokens = 0;
};

class FinanceSentiment {
public:
    FinanceSentiment() {
        weights_ = {{"beat",1},{"growth",1},{"upgrade",1},{"profit",1},
                    {"strong",0.7},{"miss",-1},{"loss",-1},{"downgrade",-1},
                    {"fraud",-1.5},{"default",-1.5},{"weak",-0.7}};
    }
    [[nodiscard]] std::expected<SentimentScore, SentimentError>
    score(const SentimentDocument& document, std::uint64_t decision_ns) const {
        if (document.text.empty()) return std::unexpected(SentimentError::EmptyText);
        if (document.available_ns > decision_ns)
            return std::unexpected(SentimentError::NotAvailable);
        if (weights_.empty()) return std::unexpected(SentimentError::NoVocabulary);
        std::string token;
        bool negate = false;
        double sum = 0.0;
        std::size_t matched = 0;
        auto flush = [&]() {
            if (token.empty()) return;
            if (token == "not" || token == "no" || token == "never") negate = true;
            else if (const auto it = weights_.find(token); it != weights_.end()) {
                sum += negate ? -it->second : it->second;
                ++matched; negate = false;
            } else negate = false;
            token.clear();
        };
        for (const unsigned char ch : document.text) {
            if (std::isalnum(ch)) token.push_back(static_cast<char>(std::tolower(ch)));
            else flush();
        }
        flush();
        return SentimentScore{document.entity, document.available_ns,
            matched ? sum / std::sqrt(static_cast<double>(matched)) : 0.0, matched};
    }
private:
    std::unordered_map<std::string,double> weights_;
};
} // namespace altair
