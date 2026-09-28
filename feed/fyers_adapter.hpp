// feed/fyers_adapter.hpp -- transport-neutral core for the official FYERS
// data WebSocket SDK callback. Socket ownership stays in the helper process.
#pragma once

#include <feed/fyers_decoder.hpp>
#include <lockfree/spsc_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace altair {

enum class FyersDataMode : std::uint8_t { SymbolUpdate, DepthUpdate };
enum class FyersEventKind : std::uint8_t { Trade, Quote, Index, Depth };

template <typename T>
struct FeedEnvelope {
    T value{};
    std::uint64_t epoch{};
    FyersEventKind kind{FyersEventKind::Quote};
};

struct FyersSubscription {
    static constexpr std::size_t kSymbolBytes = 64;
    std::array<char, kSymbolBytes> symbol{};
    FyersDataMode mode{FyersDataMode::SymbolUpdate};
    bool active{};
};

/// The official C SDK owns TLS, binary HSM decoding and reconnect. Its JSON
/// callback enters here with the epoch returned by connected(). This class is
/// fixed-capacity and allocation-free after construction; it refuses overload
/// instead of blocking the socket callback or overwriting unread data.
template <std::size_t MaxSubscriptions = 5000,
          std::size_t TickCapacity = 1024,
          std::size_t DepthCapacity = 256>
class FyersDataAdapter {
public:
    struct Stats {
        std::uint64_t messages{};
        std::uint64_t trades{};
        std::uint64_t quotes{};
        std::uint64_t indices{};
        std::uint64_t depths{};
        std::uint64_t decode_errors{};
        std::uint64_t stale_epoch{};
        std::uint64_t duplicate_sequence{};
        std::uint64_t sequence_gaps{};
        std::uint64_t sequence_unavailable{};
        std::uint64_t backpressure_drops{};
        std::uint64_t reconnects{};
    };

    explicit FyersDataAdapter(const SpecStore& specs, std::uint8_t channel = 1) noexcept
        : specs_(specs), channel_(channel >= 1 && channel <= 30 ? channel : 1) {}

    [[nodiscard]] bool subscribe(std::string_view symbol, FyersDataMode mode) noexcept {
        if (!valid_symbol(symbol)) return false;
        for (auto& item : subscriptions_) {
            if (item.active && same_symbol(item, symbol) && item.mode == mode) return true;
        }
        for (auto& item : subscriptions_) {
            if (!item.active) {
                std::memcpy(item.symbol.data(), symbol.data(), symbol.size());
                item.symbol[symbol.size()] = '\0';
                item.mode = mode;
                item.active = true;
                ++subscription_count_;
                subscriptions_dirty_ = true;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool unsubscribe(std::string_view symbol, FyersDataMode mode) noexcept {
        for (auto& item : subscriptions_) {
            if (item.active && same_symbol(item, symbol) && item.mode == mode) {
                item = {};
                --subscription_count_;
                subscriptions_dirty_ = true;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t subscription_count() const noexcept { return subscription_count_; }
    [[nodiscard]] const FyersSubscription* subscription_at(std::size_t index) const noexcept {
        std::size_t seen = 0;
        for (const auto& item : subscriptions_) {
            if (item.active && seen++ == index) return &item;
        }
        return nullptr;
    }
    [[nodiscard]] std::uint8_t channel() const noexcept { return channel_; }
    [[nodiscard]] bool subscriptions_dirty() const noexcept { return subscriptions_dirty_; }
    void subscriptions_sent() noexcept { subscriptions_dirty_ = false; }

    [[nodiscard]] std::uint64_t connected() noexcept {
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) return 0;
        if (epoch_ != 0) ++stats_.reconnects;
        ++epoch_;
        connected_ = true;
        subscriptions_dirty_ = subscription_count_ != 0;
        have_message_seq_ = false;
        return epoch_;
    }

    void disconnected(std::uint64_t callback_epoch) noexcept {
        if (callback_epoch == epoch_) connected_ = false;
    }
    [[nodiscard]] bool connected_state() const noexcept { return connected_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

    [[nodiscard]] bool on_message(std::string_view json, Timestamp received,
                                  std::uint64_t callback_epoch) noexcept {
        if (!connected_ || callback_epoch == 0 || callback_epoch != epoch_) {
            ++stats_.stale_epoch;
            return false;
        }
        ++stats_.messages;
        const auto seq_text = fyers_detail::value(json, "seq");
        std::uint32_t provider_seq = 0;
        if (fyers_detail::parse_u32(seq_text, provider_seq)) {
            if (have_message_seq_) {
                if (provider_seq <= last_message_seq_) {
                    ++stats_.duplicate_sequence;
                    return false;
                }
                if (provider_seq > last_message_seq_ + 1u) ++stats_.sequence_gaps;
            }
            have_message_seq_ = true;
            last_message_seq_ = provider_seq;
        } else {
            ++stats_.sequence_unavailable;
        }

        Tick tick{};
        DepthUpdate depth{};
        const auto decoded = decode_fyers_message(json, specs_, received,
            decoder_seq_, &tick, 1, &depth, 1);
        if (!decoded) { ++stats_.decode_errors; return false; }
        if (decoded->ticks != 0) {
            FyersEventKind kind = FyersEventKind::Quote;
            const auto type = fyers_detail::value(json, "type");
            if (type == "if") { kind = FyersEventKind::Index; ++stats_.indices; }
            else if (!fyers_detail::value(json, "last_traded_qty").empty()) {
                kind = FyersEventKind::Trade; ++stats_.trades;
            } else { ++stats_.quotes; }
            if (!ticks_.try_push({tick, epoch_, kind})) {
                ++stats_.backpressure_drops;
                return false;
            }
            return true;
        }
        if (decoded->depths != 0) {
            if (!depths_.try_push({depth, epoch_, FyersEventKind::Depth})) {
                ++stats_.backpressure_drops;
                return false;
            }
            ++stats_.depths;
            return true;
        }
        return true; // valid update for an unknown/blocked instrument was counted by decoder
    }

    [[nodiscard]] bool try_pop(FeedEnvelope<Tick>& value) noexcept {
        return ticks_.try_pop(value);
    }
    [[nodiscard]] bool try_pop(FeedEnvelope<DepthUpdate>& value) noexcept {
        return depths_.try_pop(value);
    }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] static bool valid_symbol(std::string_view symbol) noexcept {
        if (symbol.empty() || symbol.size() >= FyersSubscription::kSymbolBytes) return false;
        for (const unsigned char c : symbol)
            if (c < 0x21 || c > 0x7e || c == '"' || c == '\\') return false;
        return true;
    }
    [[nodiscard]] static bool same_symbol(const FyersSubscription& item,
                                          std::string_view symbol) noexcept {
        return std::string_view{item.symbol.data()} == symbol;
    }

    const SpecStore& specs_;
    std::array<FyersSubscription, MaxSubscriptions> subscriptions_{};
    SpscRing<FeedEnvelope<Tick>, TickCapacity> ticks_{};
    SpscRing<FeedEnvelope<DepthUpdate>, DepthCapacity> depths_{};
    Stats stats_{};
    std::size_t subscription_count_{};
    std::uint64_t epoch_{};
    std::uint32_t decoder_seq_{};
    std::uint32_t last_message_seq_{};
    std::uint8_t channel_{};
    bool connected_{};
    bool subscriptions_dirty_{};
    bool have_message_seq_{};
};

} // namespace altair
