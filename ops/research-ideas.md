# Research ideas: what physics and mathematics say about an edge

Written 2026-10-21 for Gautam Global LLP. Every idea below has:
- a source;
- what ALTAIR already has for it;
- how to test it here without fooling ourselves.

Some are measured. Those results are ALTAIR's own, from this repository's
code and data, and the run that produced them is named.

## First, the honest frame

**Who wins in Indian F&O, and why.**
- SEBI's study of FY22–FY24 found that 93% of individual F&O traders lost money.
- Proprietary traders and FPIs kept most of the profits, and 96–97% of their
  profit came from algorithmic trading
  ([SEBI, Sept 2024](https://www.sebi.gov.in/media-and-notifications/press-releases/sep-2024/updated-sebi-study-reveals-93-of-individual-traders-incurred-losses-in-equity-fando-between-fy22-and-fy24-aggregate-losses-exceed-1-8-lakh-crores-over-three-years_86906.html)).
- The firms that win do so by being co-located, cheaper and faster, and by
  making markets — not by predicting direction.

**Where a small desk can compete.** A one-desk shop on a home connection can't
out-latency them. It can win where they don't bother:
- capacity-limited trades;
- risk premia that need patience rather than speed;
- careful execution that loses less to the spread.

**What ALTAIR's own tests have found so far** — every one passed its leak
checks and placebos:
- **Price direction:** on index direction no model beats the random walk after
  costs (`ops/forecast-curriculum.md`). On a shuffled-returns placebo the best
  "skill" is +0.002% (section 28 of SMIT.txt).
- **The meta-trader:** altair_trader's CNN, analogs and selector mostly choose
  "no change", and that is the right call.
- **Arbitrage and option arbitrage:** both are real but small, and they live or
  die on legging and costs.

So the ideas below are ranked by where an edge can plausibly survive costs, not
by how clever they sound.

---

## 1. Volatility as a traded asset (the strongest lead)

### 1a. The variance risk premium — *have it, keep testing it*

**The idea.** Implied variance is, on average, above the variance that then
realises. Option sellers are paid for crash insurance (Bakshi & Kapadia 2003;
Carr & Wu 2009).

**India evidence:**
- NIFTY's implied vol sits above realised vol most of the time (Sankar,
  Ramachandran & Lukose; Chakrabarti & Kumar 2017).

**In ALTAIR:**
- `strategies/vol_premium.hpp` and `altair_vol_premium` sell the delta-hedged
  ATM straddle when implied beats a HAR forecast, on real bhavcopy closes.
- `app/ivrv_feed.hpp` provides realised variance from 5-minute bars, HAR, INDIA
  VIX and bhavcopy ATM IV.

**Next step.** Gate entries on a forecast of *vol of vol*, below. The losses of
a VRP seller come in the weeks when vol jumps.

### 1b. Vol-of-vol (volga) — *built this round, measured*

**The idea.** Vega-neutral is not vol-neutral: the 25-delta wings carry more
volga per unit of vega than the ATM option. Vol-of-vol risk carries a
**negative** premium, so options heavy in volga are dear (Huang, Schlag,
Shaliastovich & Thimme 2018; Park 2015 on VVIX).

**The trade.** Short the 25Δ strangle and long the ATM straddle, vega-neutral,
delta-hedged daily. Code: `strategies/vol_of_vol.hpp`, `altair_vol_of_vol`.

**Measured on NSE bhavcopy, 2015–2026, about 560 weekly rolls each**
(`altair_vol_of_vol --unverified-costs`):

| | Slippage per option fill | Gross | Net of expenses | t (net/trade) |
|---|---|---|---|---|
| NIFTY, every week | 0 pts | +₹3.06 lakh | −₹0.04 lakh | −0.03 |
| NIFTY, rich wings only | 0 pts | +₹3.36 lakh | +₹1.01 lakh | 1.09 |
| BANKNIFTY, rich wings only | 0 pts | +₹6.36 lakh | +₹3.57 lakh | 1.84 |
| NIFTY, every week | 1 pt | −₹10.1 lakh | −₹13.2 lakh | −9.6 |
| NIFTY, monthly hold | 0 pts | −₹0.10 lakh | −₹1.86 lakh | −1.20 |

**Reading:**
- There is a small gross premium for selling vol-of-vol. It concentrates in
  weeks when the wings are dear against their own recent median (the gate).
- It is **not significant**: t < 2 even before slippage, and after trying
  several variants.
- **Any realistic execution erases it.** Four legs, crossed at about 1 point a
  fill, cost more than the premium pays.
- The mirror trade (long vol-of-vol) loses exactly the gross the short makes,
  which checks the engine.

**Verdict.** Not tradable as built. It could become tradable only with
**passive** fills at mid. The 50-level book and the queue model in
`live/exec_study.hpp` can measure how often the wings fill passively — the
test to run next on recorded tapes.

### 1c. Vanna-volga smile pricing — *built this round*

**The idea** (Castagna & Mercurio, *Risk*, 2007).
- Price any strike from three liquid ones — 25Δ put, ATM, 25Δ call — by
  hedging its vega, vanna and volga with them.
- The fair IV of every other strike follows in closed form.
- Code: `analytics/vanna_volga.hpp`. Tests: the pillars are reproduced, a flat
  smile stays flat, the hedge carries all three Greeks, and it recovers a SABR
  smile to 0.004 vol points between the pillars.

**On screen:**
- **Option chain:** the **VV ±** column shows market IV minus vanna-volga IV,
  in vol points.
- **Greek Watch:** the **VV IV** and **Rich/cheap** columns.

**What it is and isn't.** It is a *consistency* check within one expiry: it
says which strikes are dear relative to the three most-traded. It isn't a
forecast, and the whole smile can still be too high (that is 1a's question).

**Next step.** Arbitrage-free smoothing (SVI; Gatheral & Jacquier 2014,
`analytics/svi_fit.hpp` exists), so "rich" can't be an artefact of a
non-arbitrage-free interpolation.

### 1d. Rough volatility — *a better vol forecast, cautiously*

**The idea** (Gatheral, Jaisson & Rosenbaum, arXiv 1410.3394). Log-volatility
behaves like fractional Brownian motion with Hurst H ≈ 0.1. Forecasts from it
beat GARCH-type models on many indices.

**Caution.** Cont & Das ([arXiv 2203.13820](https://arxiv.org/pdf/2203.13820))
argue that some measured roughness is microstructure noise in realised vol, not
the volatility process. No NIFTY-specific study turned up.

**Test.** Estimate H on NIFTY 5-minute realised variance:
- with noise-robust estimators (the GMM approach of
  [arXiv 2010.04610](https://arxiv.org/pdf/2010.04610));
- through the curriculum's walk-forward and leak checks;
- against HAR, which is already the bar to beat.

---

## 2. Order flow as a physical system

### 2a. Queue imbalance and multi-level OFI — *have the data now*

**The idea.**
- Price changes are linear in order-flow imbalance at the best levels, with a
  coefficient inversely proportional to depth (Cont, Kukanov & Stoikov,
  [arXiv 1011.6402](https://arxiv.org/pdf/1011.6402)).
- Multi-level OFI adds deeper levels (Xu, Gould & Howison, arXiv 1907.06230).
- Deep-learning OFI comes from Kolm, Turiel & Westray.

**India evidence:**
- On 195 NSE stocks, order flow predicts returns strongly for about five
  minutes, and the predictability perishes within thirty (Tripathi, Dixit &
  Vipul, *Finance Research Letters* 2021,
  [IDEAS](https://ideas.repec.org/a/eee/finlet/v41y2021ics1544612320316779.html)).
- Deeper NSE book levels add predictability beyond the top of book (Gupta &
  Kumar, NSE data 2014–18).

**In ALTAIR.** The 50-level FYERS book now follows the scrip on screen, and the
OFI/microprice research code exists.

**Test.** Record a few weeks of 50-level tapes on one future. Measure whether
OFI predicts the next mid move by more than the half-spread plus expenses. That
is the only threshold that matters.

### 2b. Hawkes processes — *self-exciting order flow*

**The idea.** Each trade raises the near-term rate of trades; the
"endogeneity" ratio tells how much of the activity is reflexive (Bacry,
Mastromatteo & Muzy, arXiv 1502.04592). It is useful for:
- spotting bursts before they cross the spread;
- timing passive orders.

**Test.** Fit a two-sided Hawkes model to the recorded trade tape (exponential
kernels are enough). Check whether its intensity forecasts the next minute's
volatility better than recent realised vol.

### 2c. The square-root impact law — *size orders correctly*

**The idea.** A metaorder of size Q moves the price by about Y·σ·√(Q/V).
- The exponent δ is close to 1/2 across markets.
- The law is explained by a V-shaped latent liquidity around the price (Tóth,
  Bouchaud et al., [arXiv 1105.1694](https://www.arxiv.org/pdf/1105.1694)).
- Donier et al. ([arXiv 1412.2152](https://arxiv.org/pdf/1412.2152)) argue the
  dependence may be logarithmic at the extremes.
- Durin, Rosenbaum & Szymanski separate two square-root laws: during execution,
  and for peak impact ([arXiv 2311.18283](https://arxiv.org/pdf/2311.18283)).

**Use in ALTAIR.** Cap every strategy's size by expected impact, not just by
lots. The arbitrage engine already sizes by the thinner leg; the next step is
an explicit √-law cost in `app/demo_costs.hpp`'s implicit section.

### 2d. The propagator model

**The idea.** Impact decays, and order signs are long-memory (Bouchaud, Gefen,
Potters & Wyart, cond-mat/0307332). For a desk this means:
- splitting an order does not reset its impact;
- the right schedule depends on the decay kernel.

**Test.** Measure the kernel on recorded tapes, then plug it into the router's
child-order spacing.

### 2e. HJB optimal execution — *built, checked against the paper*

**The idea.** Forsyth, "A Hamilton Jacobi Bellman approach to optimal trade
execution" ([preprint](https://cs.uwaterloo.ca/~paforsyt/optimal_trade.pdf),
Appl. Numer. Math. 61 (2011)). It drops Almgren-Chriss's three simplifications:
- the price is geometric, with permanent impact in its drift;
- temporary impact is nonlinear, `f(v) = [1+κs·sgn v]·exp(κt·sgn(v)|v|^β)`;
- the sale can react to the price path. It minimises the pre-commitment
  mean-variance objective `E[(B_L − γ/2)²]`, and sweeping γ gives the frontier.

**Built.** The solver is `oms/hjb_execution.hpp`. It uses the paper's
similarity reduction to two cash planes, semi-Lagrangian timestepping and
implicit diffusion. `altair_optimal_exec` prints the refinement ladder.

**Two changes from the paper, both stated in the header:**
- The mean and variance are interpolated, not the second moment. Interpolating
  V directly gave a riskless TWAP a standard deviation of 7.
- The control nodes are cubed toward zero. With a uniform set, nothing slower
  than "sell everything now" was admissible at the coarse levels.

**Result on Table 7.1 (γ = 200).**
- Level-2 objective: 1.177 against the paper's 1.176.
- Level 1: 1.467 against the paper's 1.319.
- E and sd move toward the paper's 99.295 and 0.7469.
- On the same grid, the HJB objective beats the best Almgren-Chriss trajectory:
  1.467 against 1.733 (kT = 32) at level 1.
- Table 8.1, as published, does not reproduce. With κt = 0.069 and β = 0.5,
  even a TWAP sells at 0.78 of the price, so the published E = 95.6 is out of
  reach. The units of v in that example are probably different. Until that is
  settled, the long preset is the table as printed, not a validated case.

**Not done.** Nothing in `oms/` slices a live order with this. The impact
parameters are the paper's normalised one-share values. Using it needs κt, κp
and β fitted from our own fills (`risk/slippage.hpp`), per instrument.

---

## 3. Statistics that refuse to be fooled

### 3a. Random-matrix cleaning of correlations

**The idea.** A sample correlation matrix of N stocks over T days is mostly
noise unless T ≫ N. Eigenvalues inside the Marchenko–Pastur band are noise.
Shrinking them (Bun, Bouchaud & Potters, arXiv 1610.08104) gives portfolios
that hold up out of sample.

**Use.** The NIFTY 50 stat-arb (`strategies/` Avellaneda–Lee) builds factors
from correlations. Clean them first, then rerun it walk-forward.

### 3b. Deflated Sharpe and the probability of backtest overfitting

**The idea.**
- After trying K variants, the best Sharpe is biased up. The deflated Sharpe
  ratio corrects for that (Bailey & López de Prado, SSRN 2460551).
- PBO measures how often the in-sample winner loses out of sample (SSRN 2326253).

**Use.** Every table in this repo that picks a "best" variant (forecast tracks,
tuner grids, the vol-of-vol gates above) should report a deflated Sharpe, not
the raw one. `live/report.hpp` already has Romano–Wolf; add DSR beside it.

### 3c. Ergodicity and the Kelly fraction

**The idea** (Peters, arXiv 0902.2965).
- A trader lives one path in time, not the ensemble average.
- A strategy with positive expected return can still have negative time-average
  growth if it is sized too big.
- Size at a fraction of Kelly, computed from the *net* edge and its uncertainty.

**Use.** Strategy caps in the LIVE dialog should come from a fractional-Kelly
calculation on the measured net edge. A strategy with no measured net edge gets
zero, which is what the readiness gates already enforce.

### 3d. Signatures

**The idea.** The path signature, a set of iterated integrals, is a universal
feature map of a price path. Linear functionals of it can express any
path-dependent strategy (Futter, Horvath & Wiese, arXiv 2308.15135).

**Caution.** It is a feature map, not an edge. Feed it to the curriculum like
any other input and it gets the same leak checks and placebo.

---

## 4. Market making, carefully

**The idea** (Avellaneda & Stoikov 2008; the queue-reactive model of Huang,
Lehalle & Rosenbaum, arXiv 1312.0563). Quote both sides around a reservation
price that leans against inventory, and earn the spread.

**The barrier.** Queue position. Co-located firms are ahead in the queue, so a
home-connection quote fills mostly when the price is about to run through it
(adverse selection).

**Test, before any real order:**
1. Use `live/exec_study.hpp`'s queue model on recorded 50-level tapes.
2. Measure the fill rate and the mark-out (P&L 1, 5 and 30 seconds after each
   fill).
3. A negative mark-out means no market making.

---

## 5. Things to stop doing

- **Predicting index direction at the bar level.**
  - Twelve model families and four ensembles have been tested; none beats the
    random walk after costs, and the placebo says that is not bad luck.
  - The models stay useful as **inputs** (regime, vol), not as signals.
- **Expiry-day gamma stories without data.** The searches found only practitioner
  blogs on NIFTY expiry-day gamma; the academic work is about volume and
  volatility on expiry days (an
  [IIMB study](https://repository.iimb.ac.in/handle/2074/21032?mode=full)). The
  expiry weekday also changed (NIFTY moved to Tuesday from September 2025), so
  any study has to split by regime.
- **Optimising on gross P&L.** Every result above changes sign once realistic
  slippage is charged. Costs come first, in the objective, not as an
  afterthought.

## The next three experiments, in order

1. **Record 50-level tapes for two weeks on one NIFTY future.** Measure OFI
   predictability against the half-spread (2a), and passive fill rates and
   mark-outs (section 4).
2. **Re-run vol-of-vol (1b) with passive fills at mid,** from those tapes'
   option quotes, and report a deflated Sharpe (3b).
3. **Rough-vol and HAR forecasts of next-week realised vol (1d)** as the gate
   for the VRP seller (1a): skip the weeks the forecast says vol will jump.

None of these needs a faster connection than a home one, and all of them can be
falsified in this repository with its existing leak checks.
