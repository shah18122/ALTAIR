# P1-09 — `instruments/universe`: which contracts are allowed to exist

> Phase 1 · Card 9 · Status: **DONE** 2026-09-01
> Depends on: P1-01 · P1-06
> Feeds: P2-04 (session load), and everything that reads the spec store

## 1. WHY

The capacity debt, made unavoidable by real files. The live Kite dump is
**106'150 rows** and the NSE bhavcopy **30'488**, against a `SpecStore` of
**8'192**. Without a filter the first file loaded fills the store and the
second is refused entirely — which is exactly what happened the first time both
real files went through the pipeline together.

Worse than the refusal: **which 8'192 survived would depend on CSV row order**,
an ordering nobody chose and nobody could reproduce.

## 2. THE SIX DECISIONS

**D1 — It runs at the GATE, not at each door.**
`Reconciler::add` is the single point where a contract enters the system, so
the filter lives there. A filter applied by each loader is a filter one loader
can forget, and the one that forgets is the one that fills the store with
40'000 far-out-of-the-money options nobody will trade.

**D2 — It runs BEFORE anything is stored.** Filtering afterwards is not a
filter, it is a truncation.

**D3 — Cash is EXEMPT from both expiry rules.**
Cash carries `Timestamp::epoch()` as "no expiry", which is 1970. An expiry rule
applied blindly would call every cash instrument expired and drop the entire
cash universe in silence. The test asserts a derivative with a genuinely
ancient expiry is *still* rejected, so the exemption is `is_epoch()` and not
"old".

**D4 — A reference price of zero means NO strike filter, not "reject all".**
Futures-only names and cash have no strike. "We did not supply a reference"
must never silently mean "exclude everything".

**D5 — Rejections are counted BY REASON.**
"The universe came out empty" is undiagnosable without knowing which rule
emptied it. Six counters: underlying, segment, exchange, expired, horizon,
strike band.

**D6 — `OutOfUniverse` is not a failure.**
It is the filter working. A caller counts it apart from anything broken —
the same distinction P1-02c draws between a refused series and an unparseable
row.

## 3. FILE MANIFEST

```
CREATE   instruments/universe.hpp
CREATE   instruments/tests/test_universe.cpp
MODIFY   instruments/reconcile.hpp   (D11: the gate, and OutOfUniverse)
MODIFY   instruments/CMakeLists.txt
```

## 4. REVIEW RECORD

Reviewed 2026-09-01. MSVC, `/W4`, zero warnings, 33/33 ctest, 40 checks.

The strike band is what makes an options universe fit at all: the real Kite
dump carries **40'913 calls and 40'891 puts**, and all but a few hundred are
strikes nobody will trade today.

End to end on the real files, with a 45-day horizon and a Rs 1000 band:

```
Kite dump      106'150 rows
NSE bhavcopy    30'488 rows
universe        admitted 888, rejected 87'875
reconciled      444 contracts, 444 AGREED, 0 blocked
```

Gate 7: no arithmetic on money beyond a subtraction and an absolute value, both
in integer paise. Gate 8: `today` is a parameter, so the same universe on the
same date admits exactly the same set — rule 6 and rule 10 both need that, and
a filter that read a clock would silently change the tradable set between a
backtest and the session it claims to reproduce.
