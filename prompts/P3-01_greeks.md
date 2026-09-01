# P3-01 — `analytics/greeks`: Black-76 price and first-order greeks

> Phase 3 · Card 1 of 10 · Status: **DONE** 2026-09-01
> Depends on: P0-01 (`units`)
> Feeds: P3-02 (higher greeks), P3-04 (IV solver), P3-05 (SVI), P10 (hedge book)

## 1. FETCH OR COMPUTE — SETTLED

**Kite exposes no greeks, no IV and no vol.** Verified against the whole
gokiteconnect v4.4.2 surface: zero matches for delta/gamma/vega/theta/rho/
implied anywhere, and the richest payload it returns carries price, quantity,
OI, OHLC and five levels of depth. Zerodha's *web* option chain shows greeks
via Sensibull — a separate product, not the API.

They would be computed here even if the API offered them:

* **Rule 6** — backtest and live share one code path. Fetched live, computed in
  backtest, and the two diverge; the backtest becomes a lie. Nobody stores
  historical greeks per tick.
* **Rule 10** — a fetched greek carries an unknown IV model, rate and dividend
  assumption. It cannot enter the reproducibility tuple.
* **Latency** — retail APIs cap at 10–50 ms. Portfolio greeks re-evaluate the
  whole chain per tick.
* **Hypotheticals have no API.** "What is my delta if I sell this strike?"

## 2. THE SEVEN DECISIONS

**D1 — Black-76 on a FORWARD, not Black-Scholes on spot.** NIFTY and BANKNIFTY
options are European and cash-settled on the index; the forward is what prices
them. Taking it as an *input* avoids embedding a dividend-yield model this card
has no business owning. One fewer estimated parameter is one fewer place to be
quietly wrong.

**D2 — `norm_cdf` is built on `erfc`, not `erf`.** For a deep out-of-the-money
strike the textbook `0.5·(1+erf(d₁/√2))` returns **exactly 0.0** — every
significant digit lost to cancellation — where the erfc form returns 2.48e-32.
A delta of 1e-32 versus 0 is the difference between a hedge existing and not.

**D3 — Greeks are returned as RAW derivatives, never desk conventions.** Vega
per unit sigma, theta per year. "Vega per vol point" and "theta per day" are
named helpers, because a scaling applied twice or not at all is where greek
bugs hide.

**D4 — T ≤ 0 and σ ≤ 0 are REFUSED, not given limit values.** Gamma at the
money at expiry is genuinely infinite, and a large number there would size a
position on infinity. `intrinsic()` handles expiry separately: at T = 0 an
option is an exercise decision, not a derivative.

**D5 — Gamma and vega are computed once, outside the call/put branch.** They
are right-independent — a call and a put on one strike differ by a forward,
which has neither convexity nor vol sensitivity. Computing them once makes the
identity impossible to break by editing one side.

**D6 — rho is −T·V.** Under Black-76 the *only* rate dependence is the discount
factor. The Black-Scholes-on-spot form would double-count the drift.

**D7 — `parity_residual` is exposed, not hidden in a test.** It is a runtime
invariant worth checking on a live surface: a residual that drifts means the
forward in use is not the one the market is pricing.

## 3. FILE MANIFEST

```
CREATE   analytics/greeks.hpp
CREATE   analytics/tests/test_greeks.cpp
CREATE   analytics/CMakeLists.txt
MODIFY   CMakeLists.txt        (add_subdirectory)
```

## 4. REVIEW RECORD

Reviewed 2026-09-01. MSVC, `/W4`, zero warnings, 35/35 ctest, 44 checks.

**Two independent kinds of check, because they catch different mistakes.**
Reference values computed in Python catch a transcription slip. **Finite
differences against the price function catch a wrong FORMULA** — a dropped term
or flipped sign that a reference value computed from the same wrong formula
would happily confirm. Delta, gamma, vega, theta and rho are each checked
against a central difference of the price.

Theta's sign gets its own assertion: it is −∂V/∂T, and getting that backwards
gives a long option positive time decay, which no long option has.

Measured on a realistic NIFTY ATM 30-day call at 15 vol: **Rs 451.21, delta
0.537, vega Rs 27.26 per vol point, theta −Rs 6.73 per day.** Sizes a desk
would recognise.

Gate 8: every greek's unit is documented and they differ from one another —
delta dimensionless, gamma per paise, vega paise per unit vol, theta paise per
year. Gate 7: doubles throughout, which rule 3 permits in analytics and bans in
the ledger; anything that becomes a hedge quantity must return through integer
paise.
