// P1-06 acceptance tests for instruments/reconcile.hpp.
// Plain main() (Catch2 blocked on vcpkg — see LEDGER blocker #7).
//
// Reconciler and SpecStore are multi-megabyte by design and must not go on the
// stack; every test holds them as file-scope objects.

#include <instruments/reconcile.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

constexpr Timestamp kSnap{1787888700000000000LL};   // 2026-08-28 09:15 IST
constexpr Timestamp kOlder{1787802300000000000LL};  // a day earlier
constexpr Timestamp kExpiry{1790226000000000000LL};

// One source's view of a contract. Everything a test varies is a parameter, so
// a test reads as the difference between the sources rather than as setup.
ContractSpec make(SpecSource src, const char* underlying, std::int64_t lot,
                  std::int64_t tick, std::uint32_t token,
                  std::int64_t scale = 100, bool stale = false,
                  Timestamp snap = kSnap, std::uint64_t hash = 0)
{
    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.lot_size = LotSize{lot};
    s.tick_size = Price{tick};
    s.strike = Price{2'500'000};
    s.price_scale = scale;
    s.expiry = kExpiry;
    s.valid_from = kSnap;
    s.valid_to = Timestamp::max();
    s.source_hash = (hash != 0) ? hash : (0xAB00u + static_cast<std::uint64_t>(src));
    s.snapshot_at = snap;
    s.exchange = Exchange::NSE;
    s.segment = Segment::Opt;
    s.opt_type = OptionType::CE;
    s.source = src;
    s.stale = stale;
    // The symbol deliberately encodes the SOURCE, so a reconciler that keyed on
    // the symbol instead of D1's tuple would fail to join and these tests would
    // catch it rather than pass by luck.
    std::snprintf(s.symbol, sizeof(s.symbol), "%s25SEP25000CE_%u", underlying,
                  static_cast<unsigned>(src));
    std::snprintf(s.underlying, sizeof(s.underlying), "%s", underlying);
    if (src == SpecSource::KiteDump) {
        s.token[static_cast<std::size_t>(FeedSource::Kite)] = token;
    } else if (src == SpecSource::XtsMaster) {
        s.token[static_cast<std::size_t>(FeedSource::Xts)] = token;
    }
    return s;
}

Reconciler g_rec;
SpecStore  g_store;
SpecStore  g_store2;   // ~1.7 MB — a local here would overflow the 1 MB stack

const ContractVerdict* only(Reconciler& r)
{
    const auto v = r.verdict_at(0);
    return v ? *v : nullptr;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void three_sources_agree()
{
    std::printf("\n1 three_sources_agree\n");
    g_rec.clear();
    check(g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0)).has_value(),
          "NSE master added");
    check(g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111)).has_value(),
          "Kite added");
    check(g_rec.add(make(SpecSource::XtsMaster, "NIFTY", 75, 5, 22222)).has_value(),
          "XTS added");

    const ReconcileReport r = g_rec.reconcile();
    check(r.contracts == 1, "three sources, ONE contract — the tuple joined them");
    check(r.agreed == 1, "Agreed");
    check(r.blocked == 0, "nothing blocked");
    check(r.primary_source_seen, "a primary was loaded");

    const ContractVerdict* v = only(g_rec);
    check(v != nullptr && v->verdict == ReconcileVerdict::Agreed, "verdict Agreed");
    check(v != nullptr && v->present_mask
              == ((1u << 0) | (1u << 2) | (1u << 3)), "three presence bits");
    check(v != nullptr && !verdict_blocks(v->verdict), "does not block");
    check(v != nullptr
              && v->merged.token[static_cast<std::size_t>(FeedSource::Kite)] == 11111,
          "merged carries the Kite token");
    check(v != nullptr
              && v->merged.token[static_cast<std::size_t>(FeedSource::Xts)] == 22222,
          "merged carries the XTS token — this IS the token map");
    check(v != nullptr && v->merged.source == SpecSource::NseMaster,
          "the exchange master wins precedence, not the broker");
    check(v != nullptr && v->merged.lot_size.raw() == 75, "lot size 75");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void lot_size_conflict_blocks()
{
    std::printf("\n2 lot_size_conflict_blocks\n");
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 50, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111));

    const ReconcileReport r = g_rec.reconcile();
    check(r.conflicts == 1, "one conflict");
    check(r.blocked == 1, "and it blocks");
    check(r.agreed == 0, "nothing agreed");

    const ContractVerdict* v = only(g_rec);
    check(v != nullptr && v->verdict == ReconcileVerdict::ValueConflict, "ValueConflict");
    check(v != nullptr && v->field == ConflictField::LotSize, "field is LotSize");
    check(v != nullptr && verdict_blocks(v->verdict), "verdict_blocks true");
    check(v != nullptr
              && v->values[static_cast<std::size_t>(SpecSource::NseMaster)] == 50,
          "NSE said 50");
    check(v != nullptr
              && v->values[static_cast<std::size_t>(SpecSource::KiteDump)] == 75,
          "Kite said 75");
    check(v != nullptr
              && v->conflict_mask
                     == (1u << static_cast<std::size_t>(SpecSource::KiteDump)),
          "the broker is flagged as differing from the winning exchange value");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void tick_and_scale_conflicts()
{
    std::printf("\n3 tick_and_scale_conflicts\n");

    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 10, 11111));
    (void)g_rec.reconcile();
    const ContractVerdict* v = only(g_rec);
    check(v != nullptr && v->field == ConflictField::TickSize,
          "a tick-only conflict reports TickSize");
    check(v != nullptr && v->values[2] == 10, "and carries the differing value 10");

    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0, 100));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111, 10'000'000));
    (void)g_rec.reconcile();
    v = only(g_rec);
    check(v != nullptr && v->field == ConflictField::PriceScale,
          "a scale-only conflict reports PriceScale");
    check(v != nullptr && v->values[2] == 10'000'000,
          "the currency-derivative scale, five decimals finer than a paisa");

    // Both lot AND tick differ. Requirement 5 fixes the order: lot is reported.
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 50, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 10, 11111));
    (void)g_rec.reconcile();
    v = only(g_rec);
    check(v != nullptr && v->field == ConflictField::LotSize,
          "lot and tick both differ -> LotSize wins, proving the req-5 ordering");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// D7, and the subtlest rule in the card. With only the Kite parser built,
// EVERY contract lacks a primary; blocking the universe for that is correct
// and useless.
void missing_primary_only_when_a_primary_was_loaded()
{
    std::printf("\n4 missing_primary_only_when_a_primary_was_loaded\n");

    g_rec.clear();
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111));
    (void)g_rec.add(make(SpecSource::KiteDump, "BANKNIFTY", 15, 5, 22222));
    ReconcileReport r = g_rec.reconcile();
    check(!r.primary_source_seen, "no primary was loaded anywhere in the run");
    check(r.missing_primary == 0, "so NOTHING is blocked for missing one");
    check(r.single_source == 2, "both are SingleSource — accepted, uncorroborated");
    check(r.blocked == 0, "today's Kite-only world still starts");

    // Now a run where a primary DOES exist — for one contract but not the other.
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111));
    (void)g_rec.add(make(SpecSource::KiteDump, "BANKNIFTY", 15, 5, 22222));
    r = g_rec.reconcile();
    check(r.primary_source_seen, "a primary WAS loaded this run");
    check(r.missing_primary == 1, "so the contract lacking one is now an anomaly");
    check(r.blocked == 1, "and it blocks");
    check(r.agreed == 1, "the corroborated one still passes");

    for (std::size_t i = 0; i < g_rec.size(); ++i) {
        const ContractVerdict* vv = *g_rec.verdict_at(i);
        if (std::strncmp(vv->merged.underlying, "BANKNIFTY", 9) == 0) {
            check(vv->verdict == ReconcileVerdict::MissingPrimary,
                  "BANKNIFTY is the one missing its primary");
        }
    }
}

// ── 5 ────────────────────────────────────────────────────────────────────
void no_broker_blocks()
{
    std::printf("\n5 no_broker_blocks\n");
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111));
    (void)g_rec.add(make(SpecSource::NseMaster, "FINNIFTY", 65, 5, 0));

    const ReconcileReport r = g_rec.reconcile();
    check(r.no_broker == 1, "the exchange lists it but no broker carries it");
    check(r.blocked == 1, "so it cannot be routed, and it blocks");
    check(r.agreed == 1, "the routable one is unaffected");

    for (std::size_t i = 0; i < g_rec.size(); ++i) {
        const ContractVerdict* vv = *g_rec.verdict_at(i);
        if (std::strncmp(vv->merged.underlying, "FINNIFTY", 8) == 0) {
            check(vv->verdict == ReconcileVerdict::NoBroker, "FINNIFTY is the unroutable one");
            check(vv->merged.token[0] == 0 && vv->merged.token[1] == 0,
                  "and it has no broker token at all");
        }
    }
}

// ── 6 ────────────────────────────────────────────────────────────────────
// D8 and rule 10. Permute, do not assert on one order.
void order_independence()
{
    std::printf("\n6 order_independence\n");
    const ContractSpec a = make(SpecSource::NseMaster, "NIFTY", 75, 5, 0, 100,
                                false, kSnap, 0x1111);
    const ContractSpec b = make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111, 100,
                                false, kSnap, 0x2222);
    const ContractSpec c = make(SpecSource::XtsMaster, "NIFTY", 75, 5, 22222, 100,
                                false, kSnap, 0x3333);
    const ContractSpec* perm[6][3] = {
        {&a, &b, &c}, {&a, &c, &b}, {&b, &a, &c},
        {&b, &c, &a}, {&c, &a, &b}, {&c, &b, &a},
    };

    ContractSpec reference{};
    int identical = 0;
    for (std::size_t p = 0; p < 6; ++p) {
        g_rec.clear();
        for (std::size_t k = 0; k < 3; ++k) {
            (void)g_rec.add(*perm[p][k]);
        }
        (void)g_rec.reconcile();
        const ContractVerdict* v = only(g_rec);
        if (v == nullptr) {
            continue;
        }
        if (p == 0) {
            reference = v->merged;
            identical = 1;
        } else if (std::memcmp(&reference, &v->merged, sizeof(ContractSpec)) == 0) {
            ++identical;
        }
    }
    // Assert on the invariant, and CONSTRUCT the sample size — a loop that
    // silently ran zero times must not be able to pass this.
    check(identical == 6,
          "all 6 permutations produce a BYTE-IDENTICAL merged spec");
    check(reference.source_hash != 0, "and the merged hash is actually populated");
    check(reference.token[0] == 11111 && reference.token[1] == 22222,
          "with both tokens, whichever order the sources arrived in");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void merge_provenance()
{
    std::printf("\n7 merge_provenance\n");
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0, 100,
                         /*stale=*/false, kSnap));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111, 100,
                         /*stale=*/true, kOlder));
    (void)g_rec.reconcile();
    const ContractVerdict* v = only(g_rec);

    check(v != nullptr && v->merged.stale,
          "one stale source makes the merge stale — the weakest link governs");
    check(v != nullptr && v->merged.snapshot_at == kOlder,
          "snapshot_at is the OLDEST contributing snapshot, not the freshest");
    check(v != nullptr && v->merged.source == SpecSource::NseMaster,
          "source is the precedence winner");
    check(v != nullptr && v->merged.id == InstrumentId::Invalid,
          "id is left Invalid — the store assigns it");

    // Re-adding the same (key, source) must be idempotent, not a phantom
    // conflict against itself.
    const std::uint64_t before = v->merged.source_hash;
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111, 100,
                         true, kOlder));
    const ReconcileReport r2 = g_rec.reconcile();
    check(r2.contracts == 1, "re-adding a source does not create a second contract");
    check(r2.conflicts == 0, "and does not conflict with itself");
    check(only(g_rec)->merged.source_hash == before,
          "the merged hash is unchanged — last-writer-wins, not accumulate");

    // Requirement 10: reconcile() twice gives the same answer.
    const ReconcileReport r3 = g_rec.reconcile();
    check(r3.contracts == r2.contracts && r3.agreed == r2.agreed
              && r3.blocked == r2.blocked,
          "reconcile() is idempotent");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void apply_to_store_blocks()
{
    std::printf("\n8 apply_to_store_blocks\n");
    g_rec.clear();
    // agreed
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111));
    // conflicting
    (void)g_rec.add(make(SpecSource::NseMaster, "BANKNIFTY", 15, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "BANKNIFTY", 30, 5, 33333));

    const ReconcileReport r = g_rec.reconcile();
    check(r.agreed == 1 && r.conflicts == 1, "one agreed, one conflicting");

    const auto blocked = apply_to_store(g_rec, g_store);
    check(blocked.has_value(), "apply_to_store succeeded");
    check(blocked.has_value() && *blocked == r.blocked,
          "and blocked exactly as many as the report said");
    check(g_store.size() == 2,
          "BOTH are in the store — a blocked contract is added, never dropped");
    check(g_store.blocked_count() == 1, "one of them is blocked");

    const auto good = g_store.id_of(FeedSource::Kite, 11111);
    check(good.has_value(), "the agreed contract resolves by Kite token");
    check(good.has_value() && g_store.current(*good).has_value(),
          "and its spec is readable");

    const auto bad = g_store.id_of(FeedSource::Kite, 33333);
    check(bad.has_value(),
          "the BLOCKED contract still resolves — the tick is not an unknown token");
    const auto spec = g_store.current(*bad);
    check(!spec.has_value() && spec.error() == SpecError::Blocked,
          "and reading it returns Blocked, which is loud — not NotFound, "
          "which would look like an unknown instrument (D6, rule 9)");
}

// ── 9 ────────────────────────────────────────────────────────────────────
// The guard that stops a forgotten reconcile() from reading as an all-clear.
// ReconcileVerdict::Agreed is ordinal 0, so a default-constructed ContractVerdict claims
// agreement. Without the dirty flag this whole function passes.
void unreconciled_never_reads_as_agreed()
{
    std::printf("\n9 unreconciled_never_reads_as_agreed\n");
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 50, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 11111));

    check(!g_rec.reconciled(), "after add(), the reconciler reports itself dirty");
    const auto early = g_rec.verdict_at(0);
    check(!early.has_value(), "a verdict read before reconcile() is REFUSED");
    check(!early.has_value() && early.error() == ReconcileError::NotReconciled,
          "and says NotReconciled — it does not hand back a zeroed 'Agreed'");

    (void)g_rec.reconcile();
    check(g_rec.reconciled(), "after reconcile() it is clean");
    check(g_rec.verdict_at(0).has_value(), "and the verdict is readable");
    check((*g_rec.verdict_at(0))->verdict == ReconcileVerdict::ValueConflict,
          "and it is the conflict, not the default Agreed");

    // An add AFTER a reconcile must invalidate the answer, not serve a stale one.
    (void)g_rec.add(make(SpecSource::XtsMaster, "NIFTY", 75, 5, 22222));
    check(!g_rec.reconciled(), "an add() after reconcile() re-dirties it");
    const auto stale = g_rec.verdict_at(0);
    check(!stale.has_value() && stale.error() == ReconcileError::NotReconciled,
          "so the stale verdict is refused rather than silently served");

    // apply_to_store removes the failure mode instead of reporting it.
    g_rec.clear();
    (void)g_rec.add(make(SpecSource::NseMaster, "NIFTY", 75, 5, 0));
    (void)g_rec.add(make(SpecSource::KiteDump, "NIFTY", 75, 5, 44444));
    check(!g_rec.reconciled(), "deliberately NOT reconciled before applying");
    const auto n = apply_to_store(g_rec, g_store2);
    check(n.has_value() && *n == 0, "apply_to_store reconciles for itself");
    check(g_store2.size() == 1, "and the contract still lands in the store");
}

// ── error paths ──────────────────────────────────────────────────────────
void error_paths()
{
    std::printf("\nerror paths\n");
    g_rec.clear();
    ContractSpec s = make(SpecSource::KiteDump, "NIFTY", 75, 5, 1);
    s.underlying[0] = '\0';
    const auto e1 = g_rec.add(s);
    check(!e1.has_value() && e1.error() == ReconcileError::BadSymbol,
          "an empty underlying is BadSymbol");

    s = make(SpecSource::KiteDump, "NIFTY", 75, 5, 1);
    s.source = static_cast<SpecSource>(9);
    const auto e2 = g_rec.add(s);
    check(!e2.has_value() && e2.error() == ReconcileError::BadSource,
          "an out-of-range SpecSource is BadSource, checked BEFORE it indexes");

    check(g_rec.size() == 0, "and neither rejected add changed anything");

    const auto e3 = g_rec.verdict_at(0);
    check(!e3.has_value() && e3.error() == ReconcileError::OutOfRange,
          "verdict_at past the end is OutOfRange");
}

} // namespace

int main()
{
    std::printf("altair instruments reconcile tests\n");
    three_sources_agree();
    lot_size_conflict_blocks();
    tick_and_scale_conflicts();
    missing_primary_only_when_a_primary_was_loaded();
    no_broker_blocks();
    order_independence();
    merge_provenance();
    apply_to_store_blocks();
    unreconciled_never_reads_as_agreed();
    error_paths();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
