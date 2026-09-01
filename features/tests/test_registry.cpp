// P5-01 acceptance tests for features/registry.hpp.
//
// Test 1 is the card: two registries holding the SAME features in a different
// order must hash differently. A model's input is a positional vector, so
// those two registries are not interchangeable -- and a hash over an unordered
// set would say they are, silently, with the tensor the right shape and every
// value in range.
//
// No check description here may contain the substring FAIL.

#include <features/registry.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>

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

} // namespace

using namespace altair;

namespace {

FeatureSpec spec(const char* n, std::uint16_t v, HorizonBand lo,
                 HorizonBand hi, std::int64_t lookback_ns)
{
    FeatureSpec s{};
    s.name = n;
    s.version = v;
    s.min_band = lo;
    s.max_band = hi;
    s.lookback = Duration{lookback_ns};
    return s;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card. Order is part of the identity.
void the_hash_covers_the_order_not_just_the_set()
{
    std::printf("\n1 the_hash_covers_the_order_not_just_the_set\n");
    FeatureRegistry a;
    (void)a.add(spec("book_imbalance", 1, HorizonBand::Micro,
                     HorizonBand::Fast, 200'000'000));
    (void)a.add(spec("realised_vol", 2, HorizonBand::Fast,
                     HorizonBand::Swing, 300'000'000'000));
    (void)a.add(spec("spread_bps", 1, HorizonBand::Micro,
                     HorizonBand::Intraday, 1'000'000'000));

    // The SAME three features, registered in a different order.
    FeatureRegistry b;
    (void)b.add(spec("realised_vol", 2, HorizonBand::Fast,
                     HorizonBand::Swing, 300'000'000'000));
    (void)b.add(spec("spread_bps", 1, HorizonBand::Micro,
                     HorizonBand::Intraday, 1'000'000'000));
    (void)b.add(spec("book_imbalance", 1, HorizonBand::Micro,
                     HorizonBand::Fast, 200'000'000));

    const auto ha = a.seal();
    const auto hb = b.seal();
    check(ha && hb, "both registries seal");
    if (!ha || !hb) { return; }
    std::printf("    same three features, different order:\n");
    std::printf("      order A -> %016llx\n",
                static_cast<unsigned long long>(*ha));
    std::printf("      order B -> %016llx\n",
                static_cast<unsigned long long>(*hb));
    check(a.size() == b.size() && a.size() == 3,
          "both hold exactly the same three features");
    check(*ha != *hb,
          "and their hashes DIFFER -- a model's weights attach to an INDEX,"
          " not a name, so these two registries are not interchangeable");

    std::printf("    index 0 is \"%.*s\" in A and \"%.*s\" in B\n",
                static_cast<int>(a.name_of(0).size()), a.name_of(0).data(),
                static_cast<int>(b.name_of(0).size()), b.name_of(0).data());
    check(a.name_of(0) != b.name_of(0),
          "which is exactly the mistake: a model trained on A and served B"
          " reads realised volatility where it learned to read imbalance --"
          " the tensor is the right shape, every value is in range, nothing"
          " errors, and it scores confidently");

    // The same order twice must be IDENTICAL, or the hash is useless.
    FeatureRegistry c;
    (void)c.add(spec("book_imbalance", 1, HorizonBand::Micro,
                     HorizonBand::Fast, 200'000'000));
    (void)c.add(spec("realised_vol", 2, HorizonBand::Fast,
                     HorizonBand::Swing, 300'000'000'000));
    (void)c.add(spec("spread_bps", 1, HorizonBand::Micro,
                     HorizonBand::Intraday, 1'000'000'000));
    check(c.seal().value() == *ha,
          "while the same features in the same order hash IDENTICALLY -- the"
          " hash has to be stable across runs or a recorded feature_version"
          " means nothing next quarter");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_version_or_a_window_change_moves_the_hash()
{
    std::printf("\n2 a_version_or_a_window_change_moves_the_hash\n");
    auto build = [](std::uint16_t ver, std::int64_t lookback) {
        FeatureRegistry r;
        (void)r.add(spec("realised_vol", ver, HorizonBand::Fast,
                         HorizonBand::Swing, lookback));
        return r.seal().value();
    };
    const std::uint64_t base = build(1, 300'000'000'000);
    check(build(2, 300'000'000'000) != base,
          "bumping the VERSION changes the hash -- a model trained on v1 was"
          " fitted to v1, bug included, and must not silently be served v2");
    check(build(1, 600'000'000'000) != base,
          "changing the LOOKBACK changes it too: the same formula over a"
          " different window is a different feature");
    check(build(1, 300'000'000'000) == base,
          "and an unchanged spec reproduces the same hash");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// CLAUDE.md's Nyquist argument, enforced.
void a_feature_cannot_be_used_outside_its_horizon_band()
{
    std::printf("\n3 a_feature_cannot_be_used_outside_its_horizon_band\n");
    FeatureRegistry r;
    // Book imbalance decays in 10-200 ms. Micro and Fast only.
    const auto imb = r.add(spec("book_imbalance", 1, HorizonBand::Micro,
                                HorizonBand::Fast, 200'000'000));
    // Regime is a swing feature and means nothing at 100 ms.
    const auto reg = r.add(spec("regime_state", 1, HorizonBand::Intraday,
                                HorizonBand::Swing, 86'400'000'000'000LL));
    check(imb && reg, "both register");
    if (!imb || !reg) { return; }

    check(r.check_band(*imb, duration::millis(500)).has_value(),
          "imbalance is valid for a 500 ms forecast");
    check(r.check_band(*imb, duration::seconds(30)).has_value(),
          "and for 30 seconds");
    const auto far = r.check_band(*imb, duration::seconds(24 * 3600));
    check(!far && far.error() == RegistryError::OutOfBand,
          "but a 24-hour forecast CANNOT have it -- order-book imbalance"
          " decays in 10-200 ms, and a swing model fitted to it would"
          " backtest beautifully because the noise is in-sample too");

    check(!r.check_band(*reg, duration::millis(100)),
          "and regime is refused at 100 ms, for the mirror reason");
    check(r.check_band(*reg, duration::seconds(24 * 3600)).has_value(),
          "while being valid at a day");

    check(band_of(duration::millis(500)) == HorizonBand::Micro,
          "500 ms is Micro");
    check(band_of(duration::seconds(30)) == HorizonBand::Fast, "30 s is Fast");
    check(band_of(duration::seconds(600)) == HorizonBand::Intraday,
          "10 minutes -- the headline forecast horizon -- is Intraday");
    check(band_of(Duration{0}) == HorizonBand::Unset,
          "and a zero horizon is Unset rather than silently Micro");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_view_for_a_horizon_keeps_registry_order()
{
    std::printf("\n4 the_view_for_a_horizon_keeps_registry_order\n");
    FeatureRegistry r;
    (void)r.add(spec("book_imbalance", 1, HorizonBand::Micro,
                     HorizonBand::Fast, 200'000'000));       // idx 0
    (void)r.add(spec("realised_vol", 1, HorizonBand::Fast,
                     HorizonBand::Swing, 300'000'000'000));  // idx 1
    (void)r.add(spec("microprice", 1, HorizonBand::Micro,
                     HorizonBand::Micro, 50'000'000));       // idx 2
    (void)r.add(spec("regime_state", 1, HorizonBand::Intraday,
                     HorizonBand::Swing, 86'400'000'000'000LL));  // idx 3

    FeatureIndex out[8];
    const auto n = r.features_for(duration::seconds(30), out, 8);
    check(n.has_value(), "a 30-second horizon resolves");
    if (!n) { return; }
    std::printf("    30 s horizon admits %zu of 4:", *n);
    for (std::size_t i = 0; i < *n; ++i) {
        std::printf(" [%u]%.*s", out[i],
                    static_cast<int>(r.name_of(out[i]).size()),
                    r.name_of(out[i]).data());
    }
    std::printf("\n");
    check(*n == 2 && out[0] == 0 && out[1] == 1,
          "imbalance and realised vol, in REGISTRY ORDER and keeping their"
          " original indices -- renumbering would hand the model the right"
          " values in the wrong slots");

    const auto swing = r.features_for(duration::seconds(48 * 3600), out, 8);
    check(swing && *swing == 2 && out[0] == 1 && out[1] == 3,
          "a two-day horizon admits realised vol and regime, again by index");

    const auto tiny = r.features_for(duration::millis(10), out, 8);
    check(tiny && *tiny == 2 && out[0] == 0 && out[1] == 2,
          "and a 10 ms horizon admits imbalance and microprice");

    check(!r.features_for(duration::seconds(30), out, 1),
          "a buffer too small to hold the answer is refused, not truncated");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void a_sealed_registry_cannot_change_underneath_a_model()
{
    std::printf("\n5 a_sealed_registry_cannot_change_underneath_a_model\n");
    FeatureRegistry r;
    (void)r.add(spec("spread_bps", 1, HorizonBand::Micro,
                     HorizonBand::Intraday, 1'000'000'000));
    check(!r.feature_version(),
          "an UNSEALED registry has no feature_version -- it can still change,"
          " and a recorded hash that no longer describes it is worse than"
          " none at all");

    const auto h = r.seal();
    check(h.has_value() && r.sealed(), "sealing produces the hash");
    check(r.feature_version().value() == *h,
          "which is the feature_version rule 10's tuple carries");

    const auto late = r.add(spec("realised_vol", 1, HorizonBand::Fast,
                                 HorizonBand::Swing, 300'000'000'000));
    check(!late && late.error() == RegistryError::Sealed,
          "and nothing further can be registered -- a feature added"
          " mid-session would change the input vector under a model that is"
          " already serving");
    check(r.size() == 1, "the registry is unchanged");
    check(r.seal().value() == *h, "and re-sealing returns the same hash");

    FeatureRegistry empty;
    check(!empty.seal(),
          "an EMPTY registry cannot be sealed -- a hash over nothing would"
          " still look like a valid feature_version in the audit tuple");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void malformed_specs_are_refused()
{
    std::printf("\n6 malformed_specs_are_refused\n");
    FeatureRegistry r;
    auto err = [&](FeatureSpec s) {
        const auto v = r.add(s);
        return v ? RegistryError::NotFound : v.error();
    };
    check(err(spec("", 1, HorizonBand::Micro, HorizonBand::Fast, 1))
          == RegistryError::BadName, "an empty name is refused");
    check(err(spec("x", 0, HorizonBand::Micro, HorizonBand::Fast, 1))
          == RegistryError::BadVersion,
          "version 0 is refused -- versions start at 1, so a zeroed spec is"
          " invalid rather than being version zero of something");
    check(err(spec("x", 1, HorizonBand::Unset, HorizonBand::Fast, 1))
          == RegistryError::BadBand,
          "an Unset band is refused, so a zeroed spec cannot register");
    check(err(spec("x", 1, HorizonBand::Swing, HorizonBand::Micro, 1))
          == RegistryError::BadBand, "and an inverted band range");
    check(err(spec("x", 1, HorizonBand::Micro, HorizonBand::Fast, 0))
          == RegistryError::BadLookback,
          "a zero lookback is refused -- every feature reads something");

    (void)r.add(spec("dup", 3, HorizonBand::Micro, HorizonBand::Fast, 1));
    check(err(spec("dup", 3, HorizonBand::Micro, HorizonBand::Fast, 1))
          == RegistryError::Duplicate,
          "the same name AND version twice is a duplicate");
    check(r.add(spec("dup", 4, HorizonBand::Micro, HorizonBand::Fast, 1))
              .has_value(),
          "but the same name at a NEW version is a different feature and is"
          " allowed -- that is what versioning is for");
    check(!r.find("dup", 5), "a version that was never registered is not found");
    check(r.find("dup", 3).value() == 0 && r.find("dup", 4).value() == 1,
          "and both versions resolve to their own index");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void the_warmup_is_the_longest_lookback()
{
    std::printf("\n7 the_warmup_is_the_longest_lookback\n");
    FeatureRegistry r;
    (void)r.add(spec("microprice", 1, HorizonBand::Micro, HorizonBand::Micro,
                     50'000'000));                    // 50 ms
    (void)r.add(spec("realised_vol", 1, HorizonBand::Fast, HorizonBand::Swing,
                     300'000'000'000));               // 5 minutes
    (void)r.add(spec("spread_bps", 1, HorizonBand::Micro,
                     HorizonBand::Intraday, 1'000'000'000));   // 1 s
    std::printf("    warmup is %.1f s -- the longest lookback in the set\n",
                static_cast<double>(r.warmup().raw()) / 1e9);
    check(r.warmup().raw() == 300'000'000'000,
          "the warmup is the LONGEST lookback, not the shortest and not the"
          " mean -- no vector can be emitted until every feature in it has"
          " enough history");
    check(r.warmup().raw() > r.lookback_of(0).raw(),
          "so a fast feature does not let the session start early: a feature"
          " computed on a short window is not the feature the model was"
          " trained on");
}

} // namespace

int main()
{
    std::printf("altair features registry tests\n");
    the_hash_covers_the_order_not_just_the_set();
    a_version_or_a_window_change_moves_the_hash();
    a_feature_cannot_be_used_outside_its_horizon_band();
    the_view_for_a_horizon_keeps_registry_order();
    a_sealed_registry_cannot_change_underneath_a_model();
    malformed_specs_are_refused();
    the_warmup_is_the_longest_lookback();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
