# CX-01 Engine Audit

Read-only audit in progress. Sources are validated against CX01_MANIFEST.json before reading. Coverage JSON records only completely received and inspected ranges. No source modifications, builds, tests, broker calls or account operations performed by this reader.

## Confirmed code-level findings (not runtime reproduced)

- **E001 (P1), core/types/units.hpp:372:** round_to_tick performs unchecked quotient-times-tick after rounding; `Price::max(), Price{2}, Up` yields unrepresentable +2^63 instead of Overflow. Add int64 extrema and all modes to unit tests.
- **E002 (P1), core/types/units.hpp:322–328:** on MSVC long double has double precision; converting INT64_MAX rounds the comparison bound to +2^63, so apply_bps can cast +2^63 to int64. Test near2^63 under UBSan and Windows target; compare to exact exclusive floating bound before conversion.
- **E003 (P1), core/lockfree/seqlock.hpp:52,71:** concurrent plain payload write/read is a C++ data race; post-copy sequence equality does not remove conflicting accesses. `load` lines87–92 additionally spins without deadline. Existing stress counts tuples on x86 and is not a memory-model proof. Validate replacement publication using TSAN, delayed writers/readers and bounded stale-or-retry behavior.
- **E004 (P2), core/lockfree/mpsc_ring.hpp:65–87,94–109:** a producer stopped after reservation at74 stalls ordered consumption of later published records; documented lock-free progress claim is too strong. CAS retry is unbounded. Pause reservation holder in a deterministic test and measure availability/tail bounds; compare per-producer SPSC design if required.
- **E005 (P2), core/lockfree/tests/test_spsc_ring.cpp:224; test_mpsc_seqlock.cpp:345; core/mem/tests/test_arena_pool.cpp:307:** several latency budgets only print OVER; these do not affect failure count/exit status. Functional CTest pass is not a pass for all latency gates.
- **E006 (P2), core/mem/pool.hpp:105–117:** duplicate release of an owned block passes `owns`, makes a self-referential free list, and underflows in_use; copying Pool also duplicates allocator bookkeeping. No allocation-state precondition/guard is documented. Add duplicate/never-acquired release and copy-trait tests; define debug diagnostics vs hot-path preconditions explicitly.

## Test hazards and boundaries

Early scan, not full test review: price_bus tests bind loopback only; warm_restart/order_intent/tick_store use fixed-name temporary files. Do not run duplicate suites concurrently. emit_vectors may overwrite its target. No external operations performed.

