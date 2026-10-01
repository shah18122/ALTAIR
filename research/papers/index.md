# Feature Card Index

> One card per paper. `research/papers/registry.json` holds the same rows; both are written by
> research/tools/feature_cards.py so they cannot disagree (edit the cards there). No PDFs are copied into the repo: each card links the published
> version (DOI) or a public preprint. A paper's result is a fact about ITS market and period;
> `status` is about ours. `rejected` means it failed on our data and STAYS here (the registry counts
> attempts, not successes: research/feature_card.hpp).

Expense figures quoted from the demos are UNVERIFIED until config/charges.toml is verified.

---

## Pairs and statistical arbitrage

### `gatev-2006-pairs`
- **Paper:** Gatev, E., Goetzmann, W. N. & Rouwenhorst, K. G. (2006). Pairs Trading: Performance of a Relative-Value Arbitrage Rule. Review of Financial Studies 19(3), 797-827.
- **Link:** https://doi.org/10.1093/rfs/hhj020 (working paper: https://www.nber.org/papers/w7032)
- **Claim:** Pairs matched on a 12-month formation window and traded for the next 6 months when the normalised spread opens by 2 sd earn excess returns of up to 11 % a year.
- **Claimed on:** US equities (CRSP), daily, 1962-2002; distance method; returns fall in later years.
- **Inputs:** daily closes of both legs
- **Horizon:** days to months
- **Formula:** `formation window -> pair and spread sd; open at |spread| >= 2 sd, close when the spread crosses zero`
- **Assumptions:** liquid shorting; costs low relative to the spread; the relationship persists out of sample. Profits shrank after 1990.
- **Impl:** `prototyped: strategies/pairs_futures.hpp (walk-forward formation/trading windows, nothing estimated on a traded day)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** NIFTY-BANKNIFTY futures, 2000-2026: 8 of 107 windows cointegrated, 15 trades, 60 % won, gross Rs 54,789, net Rs 46,193 with UNVERIFIED expenses. Too few trades to call. The stock pairs wait for the FYERS fetch.

### `engle-granger-1987-cointegration`
- **Paper:** Engle, R. F. & Granger, C. W. J. (1987). Co-integration and Error Correction: Representation, Estimation, and Testing. Econometrica 55(2), 251-276.
- **Link:** https://doi.org/10.2307/1913236
- **Claim:** Two I(1) series are cointegrated when a linear combination is stationary; regress one on the other and test the residual for a unit root.
- **Claimed on:** theory, with macro examples
- **Inputs:** two price series (logs)
- **Horizon:** any
- **Formula:** `log A = alpha + beta log B + e; ADF on e against MacKinnon's residual-based critical values, not the ordinary ADF table`
- **Assumptions:** one cointegrating vector; the regression's direction matters in small samples; breaks in the relationship invalidate it.
- **Impl:** `prototyped: strategies/cointegration.hpp (engle_granger, eg_critical)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** A test, not a signal. Used to refuse a window: the pairs walk trades only windows that pass at 5 %.

### `mackinnon-2010-critical-values`
- **Paper:** MacKinnon, J. G. (2010). Critical Values for Cointegration Tests. Queen's Economics Department Working Paper 1227.
- **Link:** Queen's Economics Department Working Paper No. 1227 (updates MacKinnon 1991)
- **Claim:** Response-surface critical values for residual-based cointegration tests at any sample size.
- **Claimed on:** simulation
- **Inputs:** sample size, number of variables, trend terms
- **Horizon:** n/a
- **Formula:** `c(p, T) = b_inf + b1/T + b2/T^2 + b3/T^3`
- **Assumptions:** the residual-based (Engle-Granger) test with the stated deterministic terms.
- **Impl:** `prototyped: strategies/cointegration.hpp (eg_critical)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Method. Two series, no trend; the test checks it is stricter than the plain ADF value at the same size, as it must be.

### `johansen-1991-cointegration`
- **Paper:** Johansen, S. (1991). Estimation and Hypothesis Testing of Cointegration Vectors in Gaussian Vector Autoregressive Models. Econometrica 59(6), 1551-1580.
- **Link:** https://doi.org/10.2307/2938278
- **Claim:** Maximum-likelihood test for the number of cointegrating vectors among n series, with no normalisation choice.
- **Claimed on:** theory
- **Inputs:** n price series
- **Horizon:** any
- **Formula:** `reduced-rank VECM; trace and max-eigenvalue statistics`
- **Assumptions:** Gaussian VAR; enough data per parameter.
- **Impl:** `none (two-leg pairs use Engle-Granger; Johansen is needed for baskets of 3+)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Not implemented.

### `vidyamurthy-2004-pairs`
- **Paper:** Vidyamurthy, G. (2004). Pairs Trading: Quantitative Methods and Analysis. Wiley.
- **Link:** ISBN 978-0-471-46067-1
- **Claim:** Trade the cointegration residual, not the price ratio or a correlation; a high correlation is not cointegration.
- **Claimed on:** practitioner text, US equities
- **Inputs:** two price series
- **Horizon:** days to weeks
- **Formula:** `spread = log A - beta log B; enter and exit on the spread's z-score`
- **Assumptions:** beta is stable over the trading window.
- **Impl:** `prototyped: strategies/pairs_futures.hpp`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Our data shows why a fixed ratio cap fails: BANKNIFTY/NIFTY ran from 0.61 (2000) to 2.66 (2019-07-05) and stayed above 2.6 for 91 days (2019-03-26 to 2020-02-28). The '< 2.6 forever' hypothesis is rejected.

### `elliott-2005-ou-spread`
- **Paper:** Elliott, R. J., van der Hoek, J. & Malcolm, W. P. (2005). Pairs Trading. Quantitative Finance 5(3), 271-276.
- **Link:** https://doi.org/10.1080/14697680500149370
- **Claim:** Model the spread as a mean-reverting (Ornstein-Uhlenbeck) state observed with noise; the reversion speed sets how long a trade should take.
- **Claimed on:** theory with simulated data
- **Inputs:** spread series
- **Horizon:** days
- **Formula:** `dX = kappa (theta - X) dt + sigma dW; half-life = ln 2 / kappa`
- **Assumptions:** constant kappa; Gaussian noise.
- **Impl:** `prototyped: strategies/cointegration.hpp (half_life); strategies/pairs_futures.hpp (time stop at 2 half-lives, max half-life 30 days)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Used as a filter and a time stop.

### `avellaneda-lee-2010-statarb`
- **Paper:** Avellaneda, M. & Lee, J.-H. (2010). Statistical Arbitrage in the US Equities Market. Quantitative Finance 10(7), 761-782.
- **Link:** https://doi.org/10.1080/14697680903124632
- **Claim:** Residuals of stocks against sector ETFs or PCA factors, modelled as OU, traded on an s-score, earned Sharpe ratios of about 1.4 (1997-2007), lower afterwards.
- **Claimed on:** US equities, 1997-2007
- **Inputs:** stock and factor returns
- **Horizon:** days
- **Formula:** `s = (X - m) / sigma_eq; open at |s| > 1.25, close at |s| < 0.5-0.75; keep only fast reversion (kappa > 252/30)`
- **Assumptions:** factor model is stable; ETF hedges available; costs of about 10 bp round trip.
- **Impl:** `partial: strategies/pairs_futures.hpp (entry, exit and stop z, fast-reversion filter)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Thresholds here are 2.0 / 0.5 / 4.0 (Gatev's 2 sd); the 1.25 s-score is not tested.

### `bertram-2010-optimal-thresholds`
- **Paper:** Bertram, W. K. (2010). Analytic Solutions for Optimal Statistical Arbitrage Trading. Physica A 389(11), 2234-2243.
- **Link:** https://doi.org/10.1016/j.physa.2010.01.045
- **Claim:** For an OU spread with a fixed cost per trade, entry and exit levels that maximise expected return per unit time have a closed form.
- **Claimed on:** theory
- **Inputs:** OU parameters, cost per trade
- **Horizon:** days
- **Formula:** `maximise (exit - entry - cost) / E[cycle time]; first-passage times of the OU process`
- **Assumptions:** known OU parameters; symmetric entry and exit.
- **Impl:** `none`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Not implemented; the thresholds are fixed.

### `krauss-2017-review`
- **Paper:** Krauss, C. (2017). Statistical Arbitrage Pairs Trading Strategies: Review and Outlook. Journal of Economic Surveys 31(2), 513-545.
- **Link:** https://doi.org/10.1111/joes.12153
- **Claim:** Survey of the five families of pairs methods (distance, cointegration, time series, stochastic control, other); profits fall over time and after costs.
- **Claimed on:** literature survey
- **Inputs:** -
- **Horizon:** -
- **Formula:** -
- **Assumptions:** -
- **Impl:** `none (reference)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Used to pick the cointegration family over distance.

### `rad-low-faff-2016-pairs-methods`
- **Paper:** Rad, H., Low, R. K. Y. & Faff, R. (2016). The Profitability of Pairs Trading Strategies: Distance, Cointegration and Copula Methods. Quantitative Finance 16(10), 1541-1558.
- **Link:** https://doi.org/10.1080/14697688.2016.1164337 (SSRN 2614233)
- **Claim:** Distance and cointegration pairs both earn positive excess returns after time-varying costs; cointegration holds up better in later years; copulas trade more steadily but earn less.
- **Claimed on:** US equities, 1962-2014
- **Inputs:** daily closes
- **Horizon:** days to months
- **Formula:** `distance, Engle-Granger and copula pair selection on the same formation/trading design`
- **Assumptions:** US cost levels; liquid shorts.
- **Impl:** `prototyped: strategies/pairs_futures.hpp (cointegration method only)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** See gatev-2006-pairs.

### `do-faff-2010-pairs-decay`
- **Paper:** Do, B. & Faff, R. (2010). Does Simple Pairs Trading Still Work? Financial Analysts Journal 66(4), 83-95.
- **Link:** https://doi.org/10.2469/faj.v66.n4.1
- **Claim:** Profits of simple pairs trading keep falling, but the strategy does well in long turbulent periods, including 2007-2009.
- **Claimed on:** US equities, 1962-2009
- **Inputs:** daily closes
- **Horizon:** days to months
- **Formula:** `Gatev distance method`
- **Assumptions:** -
- **Impl:** `none (reference)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Expect few cointegrated windows in calm years. On NIFTY-BANKNIFTY only 8 of 107 windows qualified.

### `sen-2022-india-pairs`
- **Paper:** Sen, J. (2022). Designing Efficient Pair-Trading Strategies Using Cointegration for the Indian Stock Market. arXiv:2211.07080.
- **Link:** https://arxiv.org/abs/2211.07080
- **Claim:** Cointegrated pairs inside NSE sectors (formed 2018-2020) made money in 2021, auto and realty most; some IT pairs lost.
- **Claimed on:** NSE stocks, 5 sectors, formed 2018-2020, traded 2021 (one year out of sample)
- **Inputs:** daily closes
- **Horizon:** days to months
- **Formula:** `Engle-Granger within a sector; z-score bands`
- **Assumptions:** one year out of sample; cash equities, not futures; costs are not the point of the paper.
- **Impl:** `prototyped: strategies/pairs_futures.hpp; config/pairs.csv pairs stocks within a sector`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Stock pairs wait for ops/fetch_pairs.ps1.

---

## Volatility models behind the bands

### `bollerslev-1986-garch`
- **Paper:** Bollerslev, T. (1986). Generalized Autoregressive Conditional Heteroskedasticity. Journal of Econometrics 31(3), 307-327.
- **Link:** https://doi.org/10.1016/0304-4076(86)90063-1
- **Claim:** Today's variance is a weighted sum of a constant, yesterday's squared shock and yesterday's variance.
- **Claimed on:** theory, with US inflation data
- **Inputs:** returns
- **Horizon:** one step ahead
- **Formula:** `sigma2_t = omega + alpha r2_{t-1} + beta sigma2_{t-1}; alpha + beta < 1`
- **Assumptions:** symmetric response to shocks; stationary parameters.
- **Impl:** `prototyped: analytics/garch.hpp; models/band_curriculum.hpp ('GARCH(1,1)')`
- **Status:** `rejected`
- **Edge (bps, post-cost):** —
- **On our data:** As a band for fading touches: NIFTY 957 trades, 40.3 % won, -Rs 693 a trade net; BANKNIFTY 954, 45.0 %, -Rs 656 (band-fade demo, 27,095 trades, 2015-2026, 1 lot, synthetic Black-76 premiums at INDIA VIX). The band is not at fault: the fade rule loses with every band.

### `glosten-1993-gjr`
- **Paper:** Glosten, L. R., Jagannathan, R. & Runkle, D. E. (1993). On the Relation between the Expected Value and the Volatility of the Nominal Excess Return on Stocks. Journal of Finance 48(5), 1779-1801.
- **Link:** https://doi.org/10.1111/j.1540-6261.1993.tb05128.x
- **Claim:** Negative shocks raise next-period variance more than positive ones (leverage effect).
- **Claimed on:** US equity index, monthly
- **Inputs:** returns
- **Horizon:** one step ahead
- **Formula:** `sigma2_t = omega + (alpha + gamma 1[r_{t-1} < 0]) r2_{t-1} + beta sigma2_{t-1}`
- **Assumptions:** threshold at zero.
- **Impl:** `prototyped: models/band_curriculum.hpp ('GJR-GARCH')`
- **Status:** `rejected`
- **Edge (bps, post-cost):** —
- **On our data:** Fade on GJR bands: NIFTY -Rs 674 a trade net, BANKNIFTY -Rs 685 (band-fade demo, 27,095 trades, 2015-2026, 1 lot, synthetic Black-76 premiums at INDIA VIX).

### `nelson-1991-egarch`
- **Paper:** Nelson, D. B. (1991). Conditional Heteroskedasticity in Asset Returns: A New Approach. Econometrica 59(2), 347-370.
- **Link:** https://doi.org/10.2307/2938260
- **Claim:** Model log variance, so no positivity constraints are needed, with an asymmetric response to the sign of the shock.
- **Claimed on:** US equity index, daily
- **Inputs:** returns
- **Horizon:** one step ahead
- **Formula:** `ln sigma2_t = omega + beta ln sigma2_{t-1} + alpha (|z| - E|z|) + gamma z`
- **Assumptions:** stationary log variance.
- **Impl:** `prototyped: models/band_curriculum.hpp ('EGARCH')`
- **Status:** `rejected`
- **Edge (bps, post-cost):** —
- **On our data:** Fade on EGARCH bands: NIFTY -Rs 657 a trade net, BANKNIFTY -Rs 690 (band-fade demo, 27,095 trades, 2015-2026, 1 lot, synthetic Black-76 premiums at INDIA VIX).

### `hansen-lunde-2005-garch11`
- **Paper:** Hansen, P. R. & Lunde, A. (2005). A Forecast Comparison of Volatility Models: Does Anything Beat a GARCH(1,1)? Journal of Applied Econometrics 20(7), 873-889.
- **Link:** https://doi.org/10.1002/jae.800
- **Claim:** Out of 330 volatility models, none forecasts exchange-rate variance significantly better than GARCH(1,1); for IBM stock, models with a leverage effect do better.
- **Claimed on:** DM/USD and IBM, daily, about 250 days out of sample
- **Inputs:** returns, realised variance
- **Horizon:** one day
- **Formula:** `superior predictive ability test across a model universe`
- **Assumptions:** loss function choice; realised variance as the target.
- **Impl:** `prototyped: models/band_curriculum.hpp (the band curriculum ranks 14 band models on coverage)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** In the fade demo the spread between the band models is small next to the loss every one of them makes.

### `corsi-2009-har-rv`
- **Paper:** Corsi, F. (2009). A Simple Approximate Long-Memory Model of Realized Volatility. Journal of Financial Econometrics 7(2), 174-196.
- **Link:** https://doi.org/10.1093/jjfinec/nbp001 (SSRN 1365738)
- **Claim:** Realised volatility is forecast well by a regression on its own last day, last week and last month (HAR): three horizons of traders, long memory without a long-memory model.
- **Claimed on:** USD/CHF, S&P 500 and T-bond futures, intraday, 1990s-2000s
- **Inputs:** intraday returns -> daily realised variance
- **Horizon:** 1 day to 1 month
- **Formula:** `RV(t+1..t+h) = b0 + bd RV_d + bw RV_w + bm RV_m (fitted here in logs, lognormal mean)`
- **Assumptions:** variance components are stable; jumps not separated.
- **Impl:** `prototyped: analytics/har_rv.hpp (walk-forward, no look-ahead; test_har_rv)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** 21-session NIFTY realised vol: HAR RMSE 7.2 vol points against 8.1 for the trailing 22-day value; BANKNIFTY 9.0 against 9.8. As an entry filter for the short straddle it adds nothing over selling every month.

### `andersen-bollerslev-1997-intraday`
- **Paper:** Andersen, T. G. & Bollerslev, T. (1997). Intraday Periodicity and Volatility Persistence in Financial Markets. Journal of Empirical Finance 4(2-3), 115-158.
- **Link:** https://doi.org/10.1016/S0927-5398(97)00004-2
- **Claim:** Intraday volatility has a strong U-shaped time-of-day pattern; filter it out before modelling persistence.
- **Claimed on:** S&P 500 futures and DM/USD, 5-minute
- **Inputs:** intraday returns
- **Horizon:** intraday
- **Formula:** `r_{t,n} = sigma_t s_n Z_{t,n}, with s_n the time-of-day factor`
- **Assumptions:** the pattern is stable across days.
- **Impl:** `prototyped: models/band_curriculum.hpp ('Seasonal vol (time of day)')`
- **Status:** `rejected`
- **Edge (bps, post-cost):** —
- **On our data:** The least-bad band for fading on NIFTY: -Rs 562 a trade net; BANKNIFTY -Rs 607. Still a loss.

### `heston-1993-sv`
- **Paper:** Heston, S. L. (1993). A Closed-Form Solution for Options with Stochastic Volatility. Review of Financial Studies 6(2), 327-343.
- **Link:** https://doi.org/10.1093/rfs/6.2.327
- **Claim:** Variance that mean-reverts as a square-root process gives closed-form option prices.
- **Claimed on:** theory
- **Inputs:** returns or an option surface
- **Horizon:** any
- **Formula:** `dv = kappa (theta - v) dt + xi sqrt(v) dW`
- **Assumptions:** continuous paths; correlation between price and variance is constant.
- **Impl:** `prototyped: models/band_curriculum.hpp ('Heston variance drift': the variance path only)`
- **Status:** `rejected`
- **Edge (bps, post-cost):** —
- **On our data:** Fade on Heston bands: NIFTY -Rs 684 a trade net, BANKNIFTY -Rs 691.

### `merton-1976-jumps`
- **Paper:** Merton, R. C. (1976). Option Pricing When Underlying Stock Returns Are Discontinuous. Journal of Financial Economics 3(1-2), 125-144.
- **Link:** https://doi.org/10.1016/0304-405X(76)90022-2
- **Claim:** Adding Poisson jumps to diffusion fattens the tails that short-dated options price.
- **Claimed on:** theory
- **Inputs:** returns
- **Horizon:** any
- **Formula:** `dS/S = (mu - lambda k) dt + sigma dW + (J - 1) dN`
- **Assumptions:** jumps are independent of the diffusion.
- **Impl:** `prototyped: models/band_curriculum.hpp ('Jump diffusion (Merton)')`
- **Status:** `rejected`
- **Edge (bps, post-cost):** —
- **On our data:** Fade on jump-diffusion bands: NIFTY -Rs 594 a trade net, BANKNIFTY -Rs 740.

---

## Option selling

### `black-1976-futures-options`
- **Paper:** Black, F. (1976). The Pricing of Commodity Contracts. Journal of Financial Economics 3(1-2), 167-179.
- **Link:** https://doi.org/10.1016/0304-405X(76)90024-6
- **Claim:** Options on a forward are priced off the forward price, discounted at the risk-free rate.
- **Claimed on:** theory
- **Inputs:** forward, strike, time, vol, rate
- **Horizon:** to expiry
- **Formula:** `C = e^{-rT} [F N(d1) - K N(d2)], d1 = (ln F/K + sigma2 T / 2) / (sigma sqrt T)`
- **Assumptions:** lognormal forward; one volatility for every strike (no skew).
- **Impl:** `prototyped: analytics/greeks.hpp (black76); strategies/band_option_fade.hpp prices both legs with it`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Premiums in the demo are SYNTHETIC: Black-76 at INDIA VIX, flat across strikes. With no skew, out-of-the-money puts are priced too cheap and calls too dear relative to the market.

### `carr-wu-2009-vrp`
- **Paper:** Carr, P. & Wu, L. (2009). Variance Risk Premiums. Review of Financial Studies 22(3), 1311-1341.
- **Link:** https://doi.org/10.1093/rfs/hhn038
- **Claim:** Implied variance (synthetic variance swap rates) exceeds realised variance on average, strongly for indices; the premium is not explained by standard risk factors.
- **Claimed on:** 5 US/EU indices and 35 stocks, 1996-2003
- **Inputs:** option chain, realised variance
- **Horizon:** 30 days
- **Formula:** `VRP = RV - SW (negative on average): sellers of variance earn it`
- **Assumptions:** liquid strikes across the smile; the premium pays for crash risk.
- **Impl:** `prototyped: strategies/vol_premium.hpp, app/vol_premium_main.cpp (synthetic VIX prices or NSE bhavcopy via app/bhavcopy.hpp)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** INDIA VIX was above NIFTY's next-21-session realised vol on 85.7 % of days, by 2.4 vol points on average (2015-2026). Selling the delta-hedged ATM monthly straddle every month at VIX-priced (SYNTHETIC) premiums nets Rs +6,295 a trade on NIFTY (t = 5.6, 136 trades) and Rs +10,366 on BANKNIFTY, UNVERIFIED expenses. Real ATM IV sits below VIX (VIX carries the put skew), so this overstates the premium: unproven until bhavcopy prices agree.

### `bakshi-kapadia-2003-delta-hedged`
- **Paper:** Bakshi, G. & Kapadia, N. (2003). Delta-Hedged Gains and the Negative Market Volatility Risk Premium. Review of Financial Studies 16(2), 527-566.
- **Link:** https://doi.org/10.1093/rfs/16.2.0527
- **Claim:** Long delta-hedged index options lose money on average, more so at the money and when volatility is high: the volatility risk premium is negative.
- **Claimed on:** S&P 500 options, 1988-1995
- **Inputs:** option prices, underlying
- **Horizon:** to expiry
- **Formula:** `gain = C_T - C_0 - sum delta_t (S_{t+1} - S_t) - financing`
- **Assumptions:** frequent delta hedging.
- **Impl:** `prototyped: strategies/vol_premium.hpp (short straddle, futures delta hedge in whole lots, reset every close)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** The band fade is NOT delta-hedged and loses when the underlying keeps moving (see gao-2018). The hedged straddle in strategies/vol_premium.hpp roughly doubles the t-statistic of the same trade unhedged (NIFTY 4.5 vs 2.6, BANKNIFTY 5.2 vs 1.8, SYNTHETIC prices); in March 2020 the hedge earned back Rs 176,686 of a Rs 191,655 option loss.

### `coval-shumway-2001-option-returns`
- **Paper:** Coval, J. D. & Shumway, T. (2001). Expected Option Returns. Journal of Finance 56(3), 983-1009.
- **Link:** https://doi.org/10.1111/0022-1082.00352
- **Claim:** Index calls earn less and puts far less than their betas imply; selling out-of-the-money puts and straddles earns a premium for crash risk.
- **Claimed on:** S&P 500 index options, 1990-1995, weekly
- **Inputs:** option prices
- **Horizon:** a week to expiry
- **Formula:** `mean weekly returns by strike; zero-beta straddle returns`
- **Assumptions:** no crash in the sample window; margin is available.
- **Impl:** `none`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Needs option-chain history.

### `gao-2018-intraday-momentum`
- **Paper:** Gao, L., Han, Y., Li, S. Z. & Zhou, G. (2018). Market Intraday Momentum. Journal of Financial Economics 129(2), 394-414.
- **Link:** https://doi.org/10.1016/j.jfineco.2018.05.009
- **Claim:** The market's first half-hour return predicts its last half-hour return, more strongly on volatile days and high-volume days.
- **Claimed on:** SPY and other US ETFs, 1993-2013
- **Inputs:** intraday returns
- **Horizon:** intraday
- **Formula:** `r_last30 = a + b r_first30 + e, b > 0`
- **Assumptions:** US ETFs; driven partly by late-day hedging and informed trading.
- **Impl:** `none (the evidence is the band-fade demo)`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —
- **On our data:** Consistent with our data: only 42.7 % (NIFTY) and 44.6 % (BANKNIFTY) of band touches close back inside the edge by 15:20. A touch is a breakout more often than a reversal, so fading it loses with every band model (-Rs 588 gross a trade on average). The paper's own regression is not yet run on NSE.
