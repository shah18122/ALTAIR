# Validating the volatility premium (the hedged short straddle)

The one edge ALTAIR has, checked the way Morini (*Understanding and Managing
Model Risk*, Wiley 2011, §1.5 and §2.8) says a desk should check a model before
it carries money: verification, validation, stress, then provisions.

DATA: FYERS only (the recorded chains in the private altair-data repo and
FYERS history pulled on the PC). Results below stay open until they are run
on FYERS data.

## 1. Verification

- [x] Payoff: ATM straddle of the first monthly expiry with at least 10
      sessions left, bought back one session before expiry; an unquoted
      strike at the exit is closed at intrinsic (test_vol_premium).
- [x] No look-ahead: the HAR forecast is refitted only on days before the
      signal.
- [ ] Expenses: config/charges.toml must stay verified; STT on exercise is
      avoided only because the trade exits before expiry. Re-check after any
      NSE charge change.

## 2. Validation: which delta? (open)

Tang & Li (§3.5) and Morini (§5.3) say Black-76 delta is the wrong hedge under
a skew: Δ = Δ_BS + Vega·∂σ/∂F. Whether that is worth hedging is an empirical
question. The test hedges the same straddles at every close with each rule and
compares the RMS of daily hedged P&L, trade-clustered:

- Black-76 (sticky strike, today's hedge)
- sticky moneyness, ∂σ/∂F = −slope/F
- local-vol-like, ∂σ/∂F = +slope/F
- minimum variance on the hedge error, fitted only on earlier days

where slope = d(implied vol)/d(ln K) of the OTM smile within ±4 % of the
future. Pass rule: a rule replaces Black-76 only if it cuts hedged-P&L
variance out of sample with t > 2 and does not cost P&L after hedge expenses.

## 3. Stress (open)

Portfolio P&L per lot on the named days, on FYERS prices: 2016-11-08
(demonetisation), 2020-03 (Covid crash), 2024-06-04 (election result), plus
any day in the recorded FYERS history with a gap over 3 %.

## 4. Provisions (strategies/vol_premium.hpp, `har-0-limits`)

- **Model line on vega** (`max_vega`, rupees per vol point): lots are cut to
  fit and no entry if one lot exceeds it. Off by default; set it before
  trading more than one lot.
- **Triggered revision** (`max_drawdown`, default Rs 1,00,000): once closed
  trades draw down that far from their peak, no new entries until the model
  is re-validated.
- **Stress ceiling** (`rv_ceiling`): no entry while trailing 20-session
  realised vol is above the ceiling. Off by default until tested on FYERS
  data: selling right after a vol spike is where the premium is richest.
- Review triggers outside the engine: the skew or vol-of-vol leaving its
  validated range, an NSE contract change (lot size, expiry day, weekly
  listings), or live fills more than 2 % off the recorded close.
