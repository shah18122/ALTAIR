// instruments/fyers_identity.hpp -- FYERS token to canonical contract
// identity.  Tokens are provider-local; this map never joins on a token or
// symbol alone and never invents an NSE/BSE/segment classification.
#pragma once

#include "contract_spec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair::instruments {

inline constexpr std::size_t kFyersIdentityCapacity = kMaxInstruments;

enum class FyersIdentityError : std::uint8_t {
    BadToken,
    BadSymbol,
    BadUnderlying,
    InvalidExchange,
    InvalidSegment,
    InvalidOption,
    Full,
    DuplicateToken,
    IdentityCollision,
    NotFound
};

/// Fields that define the instrument itself. `token` is deliberately outside
/// this key: FYERS may use a different token for the same contract after a
/// master refresh, while NSE and BSE versions of a symbol must never merge.
struct FyersCanonicalKey {
    char underlying[kMaxUnderlyingLen + 1]{};
    Exchange exchange{Exchange::NSE};
    Segment segment{Segment::Cash};
    OptionType option{OptionType::None};
    Timestamp expiry{};
    Price strike{};
};

struct FyersInstrumentRecord {
    std::uint64_t fy_token{};
    const char* symbol{};
    const char* underlying{};
    Exchange exchange{Exchange::NSE};
    Segment segment{Segment::Cash};
    OptionType option{OptionType::None};
    Timestamp expiry{};
    Price strike{};
};

struct FyersIdentityBinding {
    FyersCanonicalKey key{};
    std::uint64_t fy_token{};
    InstrumentId instrument{InstrumentId::Invalid};
};

[[nodiscard]] inline bool fyers_key_equal(const FyersCanonicalKey& a,
                                          const FyersCanonicalKey& b) noexcept {
    return a.exchange == b.exchange && a.segment == b.segment
        && a.option == b.option && a.expiry == b.expiry
        && a.strike == b.strike
        && std::strcmp(a.underlying, b.underlying) == 0;
}

/// Fixed-capacity mapping built before the feed starts. A repeated canonical
/// key is accepted only when it resolves to the same internal InstrumentId;
/// a token collision or a key bound to another ID is refused.
class FyersIdentityMap {
public:
    [[nodiscard]] std::expected<void, FyersIdentityError>
    bind(const FyersInstrumentRecord& record, InstrumentId id) noexcept {
        if (record.fy_token == 0) return std::unexpected(FyersIdentityError::BadToken);
        if (id == InstrumentId::Invalid)
            return std::unexpected(FyersIdentityError::NotFound);
        if (record.symbol == nullptr || record.symbol[0] == '\0'
            || detail::spec_str_len(record.symbol, kMaxSymbolLen) > kMaxSymbolLen)
            return std::unexpected(FyersIdentityError::BadSymbol);
        if (record.underlying == nullptr || record.underlying[0] == '\0'
            || detail::spec_str_len(record.underlying, kMaxUnderlyingLen)
                   > kMaxUnderlyingLen)
            return std::unexpected(FyersIdentityError::BadUnderlying);
        if (static_cast<std::uint8_t>(record.exchange) > static_cast<std::uint8_t>(Exchange::BSE))
            return std::unexpected(FyersIdentityError::InvalidExchange);
        if (static_cast<std::uint8_t>(record.segment) > static_cast<std::uint8_t>(Segment::Commodity))
            return std::unexpected(FyersIdentityError::InvalidSegment);
        if (record.option != OptionType::None && record.segment != Segment::Opt)
            return std::unexpected(FyersIdentityError::InvalidOption);

        FyersCanonicalKey key{};
        // Copy the validated length only. record.underlying is a C string, not
        // a kMaxUnderlyingLen+1 array: copying sizeof(key.underlying) read past
        // the end of every shorter name (ASan: global-buffer-overflow).
        const std::size_t underlying_len =
            detail::spec_str_len(record.underlying, kMaxUnderlyingLen);
        std::memcpy(key.underlying, record.underlying, underlying_len);
        key.underlying[underlying_len] = '\0';
        key.exchange = record.exchange;
        key.segment = record.segment;
        key.option = record.option;
        key.expiry = record.expiry;
        key.strike = record.strike;

        for (std::size_t i = 0; i < count_; ++i) {
            if (bindings_[i].instrument == id
                && !fyers_key_equal(bindings_[i].key, key))
                return std::unexpected(FyersIdentityError::IdentityCollision);
            if (bindings_[i].fy_token == record.fy_token) {
                if (!fyers_key_equal(bindings_[i].key, key)
                    || bindings_[i].instrument != id)
                    return std::unexpected(FyersIdentityError::DuplicateToken);
                return {};
            }
            if (fyers_key_equal(bindings_[i].key, key)) {
                if (bindings_[i].instrument != id)
                    return std::unexpected(FyersIdentityError::IdentityCollision);
                // Keep the old token alias. A master refresh may issue a new
                // token, but silently forgetting the old one would let a
                // delayed/stale token be rebound to another contract.
                if (bindings_[i].fy_token == record.fy_token) return {};
            }
        }
        if (count_ >= kFyersIdentityCapacity)
            return std::unexpected(FyersIdentityError::Full);
        bindings_[count_++] = FyersIdentityBinding{key, record.fy_token, id};
        return {};
    }

    [[nodiscard]] std::expected<InstrumentId, FyersIdentityError>
    id_of(std::uint64_t token) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (bindings_[i].fy_token == token) return bindings_[i].instrument;
        return std::unexpected(FyersIdentityError::NotFound);
    }

    [[nodiscard]] std::expected<std::uint64_t, FyersIdentityError>
    token_of(const FyersCanonicalKey& key, InstrumentId id) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (bindings_[i].instrument == id && fyers_key_equal(bindings_[i].key, key))
                return bindings_[i].fy_token;
        return std::unexpected(FyersIdentityError::NotFound);
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    std::array<FyersIdentityBinding, kFyersIdentityCapacity> bindings_{};
    std::size_t count_{};
};

} // namespace altair::instruments
