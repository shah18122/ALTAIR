# P2-06 / P2-07 — `feed/tick_store`: the session tape, writer and reader

> Phase 2 · Cards 6 and 7 of 14 · Status: **DONE** 2026-08-31
> Depends on: P2-01 (`Tick`, `DepthUpdate`)

## 1. WHY THE TWO CARDS ARE ONE FILE

A format written without a reader that proves it round-trips is a format nobody
can trust. The first time it is read back will be the day a backtest disagrees
with a session, and nobody will be able to say which is wrong. Writer and
reader ship together, with the round-trip as the headline test.

**Rule 6 is the point of this file.** The replay feed and the live feed must
emit the same struct into the same pipeline, so the store persists `Tick` and
`DepthUpdate` exactly as P2-01 defined them, byte for byte.

## 2. THE SEVEN DECISIONS

**D1 — The file records TWO versions.** `store_version` describes the file
layout; `wire_version` describes the structs inside it. A file can outlive
several struct versions only if both are written down, so both are.

**D2 — The struct SIZES are recorded too, and checked on open.**
A file whose `Tick` was 64 bytes cannot be read by a build whose `Tick` is 72.
Reinterpreting those bytes would produce **plausible garbage**, which is the
worst outcome available — worse than a crash, because it looks like data.
`BadWireVersion` is returned before a single record is read.

**D3 — A truncated tail is reported, and the good records survive.**
A crash mid-write leaves a partial record. The reader hands back every complete
record before the cut and *then* says `Truncated`. Treating the partial tail as
a clean EOF would silently shorten the session; refusing the whole file would
throw away a day of tape for the sake of one record.

**D4 — An unknown record tag is fatal, not skippable.**
Unlike a truncated tail, an unrecognised tag means the framing itself is lost:
the reader does not know how many bytes follow, so everything after it is
unaddressable.

**D5 — Records are self-describing by a one-byte tag**, so the tick/depth
interleaving is preserved exactly. The order between the two streams is
information, not an artefact of how they were stored.

**D6 — `flush()` is separate from `close()`**, so the recorder can bound a
crash to seconds of tape rather than the session.

**D7 — Zero records is a valid session**, not an error. A header-only store
opens and reports a clean end of file.

## 3. FILE MANIFEST

```
CREATE   feed/tick_store.hpp
CREATE   feed/tests/test_tick_store.cpp
MODIFY   feed/CMakeLists.txt
```

## 4. REVIEW RECORD

Reviewed 2026-08-31. MSVC, `/W4`, zero warnings, 30/30 ctest, 33 checks.

The round-trip test writes 500 ticks and 500 depth updates, reads them back,
and `memcmp`s every one — **padding included**. Zero differences. That is rule
6 made checkable rather than asserted.

Gate 6: **65.0 bytes per tick**, and the test asserts the total is exactly
`header + N x (1 + sizeof(Tick))`. No hidden framing, so a session's tape size
is predictable from its tick count rather than discovered when a disk fills.

Gate 7: no arithmetic on money — the store moves bytes. But the failure mode is
financial and severe: a tape that reads back subtly wrong makes every backtest
built on it wrong in the same direction, and nothing downstream would notice.
D2's size check is the guard, and it refuses rather than reinterprets.
