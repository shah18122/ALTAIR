#include <instruments/fyers_identity.hpp>

#include <cstdio>

int main() {
    using namespace altair;
    using namespace altair::instruments;
    const auto rec = [](std::uint64_t token, Exchange ex, Timestamp expiry) {
        return FyersInstrumentRecord{token, "NIFTY", "NIFTY", ex,
                                     Segment::Fut, OptionType::None, expiry, Price{0}};
    };
    FyersIdentityMap map;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    constexpr std::uint64_t kRealWidthToken = 101126092968390ULL;
    check(map.bind(rec(kRealWidthToken, Exchange::NSE, Timestamp{2026}), InstrumentId{7}).has_value(),
          "a valid FYERS record binds to the canonical internal ID");
    check(map.id_of(kRealWidthToken).value_or(InstrumentId::Invalid) == InstrumentId{7},
          "a real 15-digit FYERS token resolves without truncation");
    check(map.bind(rec(202, Exchange::NSE, Timestamp{2026}), InstrumentId{7}).has_value(),
          "a refreshed provider token can rebind the same canonical contract");
    check(map.bind(rec(kRealWidthToken, Exchange::BSE, Timestamp{2026}), InstrumentId{8}).error()
              == FyersIdentityError::DuplicateToken,
          "a token reused for another exchange is refused");
    check(map.bind(rec(303, Exchange::BSE, Timestamp{2026}), InstrumentId{8}).has_value(),
          "the same symbol on BSE remains a distinct canonical contract");
    check(map.bind(rec(404, Exchange::NSE, Timestamp{2027}), InstrumentId{7}).error()
              == FyersIdentityError::IdentityCollision,
          "one internal ID cannot silently absorb a different expiry");
    FyersInstrumentRecord bad = rec(505, Exchange::NSE, Timestamp{2026});
    bad.option = OptionType::CE;
    check(map.bind(bad, InstrumentId{9}).error() == FyersIdentityError::InvalidOption,
          "an option classification on a non-option segment is refused");
    return failures == 0 ? 0 : 1;
}
