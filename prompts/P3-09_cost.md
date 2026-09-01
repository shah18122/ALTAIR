# P3-09 — `risk/cost`: the cost calculator

> Phase 3 · Card 9 of 10 · Status: **DONE** 2026-09-01
> Depends on: P0-01 (`units`), P1-01 (`ContractSpec`)
> Feeds: every strategy, every backtest, Phase 5's arbitrage disproofs

## 1. WHY IT COMES FIRST IN PHASE 3

CLAUDE.md rule 5: *"Every signal is priced net of full cost before it exists.
No strategy sees a pre-cost number."* Nothing in Phases 5–6 can be built
correctly until this exists — every arbitrage and every forecast is a claim
about **net** edge, and a pre-cost claim is not a claim about anything.

## 2. THE SEVEN DECISIONS

**D1 — Option turnover is PREMIUM, not notional.** The factor-of-500 trap. A
NIFTY 25000 call at Rs 50 has premium turnover Rs 50 × lot and notional
Rs 25'000 × lot. Charging notional kills every options strategy before it is
written; charging notional where premium was meant understates a live bill by
the same factor. The calculator takes the **traded price**, which for an option
*is* the premium, so it computes premium turnover by construction.

**D2 — The SIDE is load-bearing.** STT is sell-side for intraday, futures and
options — but **both sides for delivery**. Stamp duty is buy-side only. DP is
delivery-sell only. A calculator that applied one rule everywhere would double
or halve the largest line on the bill.

**D3 — Exact integer arithmetic, and this is where the P0-01 debt is paid.**
Rates are integers scaled by 1e9; every product goes through a **128-bit**
intermediate. Turnover of Rs 1000 crore is 1e12 paise, which times a rate of
1.0 is 1e21 — two orders of magnitude past int64. That is precisely the
"session-accumulated turnover" the carried debt named. `apply_bps`'s
`long double` path is deliberately **not** used.

**D4 — GST excludes STT and stamp duty.** GST is not levied on a tax. Including
them inflates every bill by roughly the STT again at 18%, which on options is
the difference between an edge and a loss.

**D5 — Itemised, never a single number.** A cost you cannot decompose is a cost
you cannot audit, dispute, or explain to a strategy that just lost money.

**D6 — The schedule follows the TRADE DATE, never today.** A March backtest
must be charged March's rates. The upper bound is **closed**, because circulars
read "effective from D1 to D2 inclusive".

**D7 — `schedule_verified` rides on every result.** `charges.toml` still says
`last_verified = "UNVERIFIED"`. A P&L figure whose cost basis was never checked
against a circular should say so rather than look like any other number.

## 3. FILE MANIFEST

```
CREATE   risk/cost.hpp
CREATE   risk/tests/test_cost.cpp
CREATE   risk/CMakeLists.txt
MODIFY   CMakeLists.txt        (add_subdirectory)
```

## 4. REVIEW RECORD

Reviewed 2026-09-01. MSVC, `/W4`, zero warnings, 34/34 ctest, 46 checks.

Measured on a real NIFTY futures lot (65 × Rs 24'080 = Rs 15.65 lakh turnover):
**round trip Rs 930.56, or 5.95 bps.** That is the number a signal must beat,
and rule 5 says it must be netted *before* the signal exists rather than
subtracted from a backtest afterwards.

**The test caught its own author.** Test 1 asserted the notional/premium STT
ratio was exactly 500. It is 499.5: premium STT is 325'000 × 0.0015 = 487.5,
which rounds to 488. The rounding lives at the small end — which is exactly
where it should, since the premium charge is the one that must be right to the
paisa. The assertion was wrong, not the arithmetic.

### Not in scope, deliberately

Exercise and assignment are charged differently (STT on an exercised option is
on **intrinsic value**, not premium) and are a separate card. This prices a
trade — a buy or a sell of one contract.

### Still open

`charges.toml` is `UNVERIFIED`. The structure is right and the arithmetic is
tested; the *numbers* need checking against live NSE/BSE circulars, and only
Smit can sign that off. The file's own header says a 3 bps error turns a
profitable arbitrage into a losing one — against a measured round trip of 5.95
bps, that is half the cost base.
