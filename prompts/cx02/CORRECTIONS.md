# CX-02 correction cards for the P0 contracts (C21-001 … C21-004)

In four places the P0 task card itself specified the defect, and the implementation followed the card faithfully (G21, `CX01C_FINDINGS.md` theme "defects that originate in the cards"). A fix that changed only the code would leave each card contradicting the code, and the next implementer who read the card would put the defect back.

These cards follow `prompts/PROTOCOL.md` §7. They amend the named contract items and nothing else. The original card files are left as written, and this file is the amendment of record. Each card's implementation lands as the CX-02 card named under FIX.

---

```
CORRECTION to P0-01 (core/types).  Finding C21-001 (code defect C01-001).

WHAT FAILED
  Gate 5b -- bounds refuse. Gate 7 -- numerical.

WHAT IS WRONG
  Behavioural item 9 lists round_to_tick's errors as DivideByZero (tick == 0)
  and NotRepresentable (tick < 0) only. The return type already carries
  ArithError::Overflow, but no item requires it. core/types/units.hpp:350-372
  computes quot = v / t, steps quot by one for Up/Down/Nearest, and returns
  Price{quot * t} unchecked. Near Price::max()/min() that multiply is signed
  overflow -- undefined behaviour, not a wrong number.

WHY IT MATTERS
  Item 4 requires notional_of to check BEFORE any signed overflow and "never
  rely on wraparound". round_to_tick is the same class of function and was
  exempted by omission. A plausible-looking price produced by UB is the rule-11
  failure shape.

FIX (lands in CX02-D1)
  Item 9 gains: "returns Overflow if the rounded multiple of tick is outside
  int64_t. The check happens before the multiply: for t > 0, quot * t overflows
  iff quot > INT64_MAX / t or quot < INT64_MIN / t."
  Acceptance tests added to test_units:
    round_to_tick(Price::max(), Price{5}, Up).error() == ArithError::Overflow
    round_to_tick(Price::min(), Price{5}, Down).error() == ArithError::Overflow
    round_to_tick(Price{max - max % 5}, Price{5}, Up) == Price{max - max % 5}
Change ONLY item 9 and its tests. Everything else in P0-01 passed review.
```

---

```
CORRECTION to P0-05b (core/mem arena + pool).  Finding C21-002 (code defect C01-004).

WHAT FAILED
  Gate 5b -- bounds refuse. Gate 8 -- conservation (blocks in use).

WHAT IS WRONG
  Behavioural item 12 specifies release(nullptr) (no-op) and release of a
  non-owned pointer (counted in foreign_releases()). It is silent on releasing
  an OWNED block that is ALREADY FREE. pool.hpp:105-118 then pushes that block
  onto the free list a second time -- the list self-loops, the same block is
  handed out twice -- and --in_use_ underflows an unsigned counter. Item 11 of
  the contract also never forbids copying, so Pool and Arena copy implicitly,
  and two copies hand out the same memory.

WHY IT MATTERS
  A double release is a caller bug, and the pool is the one place that can see
  it. Handing the same block to two owners is silent memory corruption on the
  hot path, surfacing "thousands of allocations later" -- the exact failure
  item 11's comment gives as the reason owns() refuses mid-block pointers.

FIX (lands in CX02-D2)
  Item 12 gains: "release of an owned block that is currently free changes
  nothing and increments double_releases(). in_use() never decreases below
  zero. Detection may use an O(1) tag in the free block, confirmed by a walk of
  the free list bounded by capacity(); a block smaller than two pointers is
  confirmed by the walk alone." New observer: double_releases().
  New item 13: "Pool and Arena are non-copyable and movable; a moved-from pool
  owns nothing."
  Acceptance tests:
    test_pool_double_release_is_counted_and_harmless
      (acquire a; release a; release a -> double_releases()==1, in_use()==0,
       next two acquires return DIFFERENT blocks)
    test_pool_and_arena_are_not_copyable
      (static_assert(!std::is_copy_constructible_v<Pool>) and likewise for
       copy-assign and for Arena)
Change ONLY items 11-13, the observer list and these tests.
```

---

```
CORRECTION to P0-06b (core/lockfree seqlock) and P0-08a (core/config store).
Finding C21-003 (code defects C02-002, C02-003, C02-004).

WHAT FAILED
  Gate 8 -- physics/concurrency: a data race is undefined behaviour.

WHAT IS WRONG
  Items 6-7 prescribe the textbook seqlock. The writer assigns the payload as a
  plain object between an odd and even sequence store, and the reader COPIES
  the payload with a plain read before anything confirms no write is in
  flight. For any payload that is not a set of atomics, that copy races the
  write: UB under the C++ memory model, whatever the sequence check says
  afterwards. Item 8 then REQUIRES an unbounded retry loop, and item 10 makes
  a failed read leave `out` indeterminate. atomic_thread_fence is an MSVC STL
  facility with no GCC/Clang equivalent (LEAD_VERIFICATION V-03), so the item-6
  "required" fence is not even portable. P0-08a's ConfigStore publishes
  through this primitive and does not require version to rise (C02-004).

WHY IT MATTERS
  ConfigStore is how risk limits and strategy parameters reach the hot path.
  "Torn value detected and retried" is only true if the torn copy was never
  UB to make. And TSan cannot run on this box (V-10), so nothing here would
  report the race.

FIX (lands in CX02-C1)
  Replace items 6-8 and 10 for any payload that is not trivially a set of
  atomics with SLOT PUBLICATION:
    SnapshotSlots<T, kReaders>: kSlots = kReaders + 2 slots of T.
    publish(T): writes ONLY a slot that is neither the published slot nor held
      by any reader, then atomically swaps the published index. A slot a
      reader might be copying is never written.
    read(): a reader stores a hold on the slot it is about to copy, RE-LOADS
      the published index (seq_cst on both sides -- the Dekker handshake), and
      copies only if unchanged; otherwise releases and retries, at most
      kReadRetries times, then returns unexpected(Contended). No torn value
      exists to return, so a bound is safe where item 8's was not.
    NoFreeSlot is unreachable: the published slot plus at most one hold per
      reader occupy kReaders + 1 < kSlots slots. Refused anyway, not assumed.
    Payload copies happen only on slots no writer touches; no fence, so no
    MSVC-only facility.
  P0-08a gains: publish() refuses a version that does not rise
  (StoreError::StaleVersion), and a reader never observes version decrease.
  Acceptance tests (deterministic interleavings through a test-only hook
  template parameter, not timing):
    test_writer_never_writes_a_held_slot
    test_reader_retries_when_published_moves_between_hold_and_recheck
    test_contended_read_is_refused_not_torn
    test_publish_refuses_non_rising_version
    test_no_free_slot_is_unreachable_at_kreaders_holds
  Evidence limit, stated in the card: ASan is not race evidence. The proof is
  the protocol plus the forced interleavings; a TSan run needs Linux and is
  recorded as UNVERIFIED until one exists.
Change ONLY those items and the ConfigStore publish contract.
```

---

```
CORRECTION to P0-09a (core/invariant).  Finding C21-004 (code defects C02-001, C12-002).

WHAT FAILED
  Gate 8 -- conservation invariants.

WHAT IS WRONG
  The architect's note and item 6 define check() as
      fills + costs + (cash - initial_cash - adjustments) == 0
  over the ledger's OWN accumulators, and on_fill updates all three together.
  So check() is true by construction, and cannot fail on any accounting error
  a caller can make -- only on a cash movement that bypassed on_fill, which
  the class makes impossible. Item 3 and the overflow test also say a REFUSED
  fill "is not a breach". A refused fill is one that happened at the venue and
  was not booked: the books now disagree with the world, silently.
  CLAUDE.md's physics section describes this check as the invariant that trips
  the kill switch every tick, and nothing outside tests calls it (C12-002).

WHY IT MATTERS
  A conservation law checked against itself proves arithmetic, not
  conservation. The checkbox reads as protection it does not give, and the one
  real failure it could see -- an unbooked fill -- is waved through.

FIX (lands in CX02-E1)
  Item 6 is renamed for what it is: check() verifies INTERNAL CONSISTENCY,
  and its doc comment says so.
  New item 13: reconcile(Notional external_cash, Qty external_position)
  compares against an INDEPENDENT source -- the paper venue's or the broker's
  statement -- with == on paise and no tolerance, and latches
  Breach::ExternalMismatch on any difference.
  Item 3 and item 4 amended: an on_fill refused for a negative cost or an
  overflow still returns the error and mutates nothing, AND trips the ledger
  with Breach::RefusedFill, because the fill it describes happened.
  The overflow acceptance test's "a rejected fill is not a breach" line is
  replaced by "a rejected fill latches RefusedFill".
  Wiring (CX-03, not this card): the engine calls check_and_trip() every tick
  and reconcile() on every venue statement, and a latched ledger trips the
  risk/ KillSwitch.
  Acceptance tests:
    test_reconcile_detects_an_unbooked_fill
    test_reconcile_detects_a_booked_fill_the_venue_never_made
    test_refused_fill_latches
Change ONLY items 3, 4, 6, 13 and those tests.
```
