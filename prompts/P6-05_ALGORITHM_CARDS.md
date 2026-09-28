# P6-05 — Independent algorithm implementation cards

This began as the issued child-card queue. It is now also the authoritative
per-card implementation ledger. A checked parent card means the queue was
split and specified; the status table below separately records what landed.
Each child must land in its owning non-UI module before its separate UI wiring
card. Synthetic fixtures may prove numerics, never market validity.

Every implementation card has the same acceptance contract:

1. State equations, input units, parameter bounds and failure modes in code.
2. Match at least one independent numerical reference and test degeneracies.
3. Fit preprocessing on the training fold only; purge overlapping labels and
   embargo where horizons overlap.
4. Report sample window, uncertainty and pre/post-cost out-of-sample metrics.
5. For trainable models, test deterministic checkpoint round-trip and a finite,
   directionally correct gradient on a tiny fixture.
6. Add a `ModelJobPayload` UI card only after the numerical card is green.

| Child | Owner | Required implementation and numerical reference | Data/backend gate | UI card |
|---|---|---|---|---|
| M01 SVM | `models/` | Linear/RBF binary SVM; primal/dual convention, C and gamma bounds; compare a separable fixture with a published LIBSVM result | labelled causal features | decision margin, calibration and fold table |
| M02 KNN | `models/` | k-NN classification/regression; distance, scaling, tie rule; hand-computed neighbour fixture | labelled causal features | neighbour distances and fold metrics |
| M03 Autoencoder | `models/` | trainable encoder/decoder, reconstruction loss and bottleneck; identity/low-rank fixtures | neural backend; P1-05 only for optional MPS | reconstruction distribution and anomaly threshold |
| M04 ARMA | `models/` | ARMA(p,q), stationarity/invertibility and likelihood; recover seeded coefficients | evenly sampled stationary series | coefficients, roots, residual diagnostics |
| M05 ARIMA | `models/` | ARIMA(p,d,q), differencing/integration contract; seeded integrated process | evenly sampled series | forecast table and inverse-difference provenance |
| M06 SARIMA | `models/` | SARIMA(p,d,q)(P,D,Q)s; seasonal roots; seeded seasonal process | adequate seasonal history | seasonal diagnostics and horizons |
| M07 EGARCH | `analytics/` | log-variance EGARCH with leverage; compare a fixed recursion by hand | return series | variance path, leverage and residual tests |
| M08 OU | `models/` | exact-discretisation OU estimation and simulation; recover theta/mu/sigma with confidence intervals | stationary spread definition | half-life, parameters and simulation envelope |
| M09 Heston | `analytics/` | characteristic-function pricer with branch convention; published option-price vector | option inputs and numerical integration | price/Greeks plus integration error |
| M10 Trinomial | `analytics/` | recombining trinomial lattice; convergence to Black–Scholes and American bounds | option contract | convergence table and exercise boundary |
| M11 PDE | `analytics/` | finite-difference option PDE; stability/grid contract; converge to analytic European prices | option contract | grid error and boundary conditions |
| M12 MLP | `models/` | trainable causal MLP; deterministic optimiser/checkpoint and gradient fixture | labelled dataset; CPU baseline first | learning curves and purged OOS table |
| M13 LSTM | `models/` | gated recurrence with BPTT; sequence-memory fixture and checkpoint test | sequential labelled dataset | horizon metrics and hidden-state policy |
| M14 GRU | `models/` | reset/update recurrence; sequence-memory fixture and checkpoint test | sequential labelled dataset | horizon metrics and hidden-state policy |
| M15 Transformer | `models/` | causal multi-head attention with mask proof; tiny attention reference and gradient | sequential labelled dataset | attention diagnostics, horizon metrics |
| M16 Causal CNN | `models/` | defined dilated causal convolution and receptive field; impulse-response proof | sequential labelled dataset | receptive field and horizon metrics |
| M17 Stat-arb portfolio | `strategies/` | multi-leg hedge/cointegration, costs and constrained sizing; conservation fixture | point-in-time universe and tradable mapping | legs, hedge ratios, stability and net edge |
| M18 Factor risk | `risk/` | exposure/covariance decomposition; reconcile component and total variance | point-in-time holdings and factor returns | exposure and risk-contribution table |
| M19 Named factors | `models/` | explicit value/quality/momentum definitions and rebalance lag; published toy portfolio | survivorship-safe fundamentals/prices | factor definition, coverage and IC decay |
| M20 Queue position | `book/` | order-level queue state and cancellation rules; deterministic message replay | licensed order-by-order depth | queue state, uncertainty and sequence gaps |
| M21 Fill probability | `models/` | calibrated fill hazard conditional on queue/depth; reliability fixture | M20 data plus labelled fills | reliability curve and horizon probability |
| M22 Hawkes | `models/` | stable univariate/multivariate Hawkes likelihood; recover seeded parameters | timestamped trades/orders | intensity, branching ratio and residual test |
| M23 Event/news | `models/` | timestamp/availability contract, entity join and embargoed event study | licensed point-in-time corpus | event timeline, coverage and abnormal return |
| M24 Agent simulation | `backtest/` | specified agents, clearing and conservation; deterministic seeded scenario | validated simulator assumptions | state trajectory and invariant report |
| M25 DQN | `models/` | replay/target-network DQN against a stated heuristic; deterministic checkpoint | specified environment from M24 or paper execution | reward, risk and baseline comparison |
| M26 PPO | `models/` | clipped objective/GAE with action constraints; gradient and checkpoint tests | specified environment and baseline | learning stability and OOS policy report |
| M27 Actor-critic | `models/` | defined actor/critic losses and target policy; gradient/checkpoint tests | specified environment and baseline | reward, drawdown and baseline comparison |

## Implementation status — 2026-09-28

`NUMERICAL GREEN` means the equations and deterministic fixtures are in the
tree. It does not mean profitable, market-trained or live-validated. `DATA
GATED` has a numerical implementation but cannot honestly clear its market
acceptance contract without the named P6-06 evidence. `PARTIAL` identifies an
actual remaining implementation gap rather than an external evidence gap.

| Child | Status | Evidence / remaining boundary |
|---|---|---|
| M01 SVM | NUMERICAL GREEN | deterministic linear/RBF SMO and fixtures in `models/classical.hpp` |
| M02 KNN | NUMERICAL GREEN | classification/regression, fold-only scale and tie fixture |
| M03 Autoencoder | NUMERICAL GREEN | fitted linear/PCA encoder-decoder and low-rank reconstruction fixture; MPS remains optional |
| M04 ARMA | NUMERICAL GREEN | bounded conditional least squares and seeded recovery |
| M05 ARIMA | NUMERICAL GREEN | differencing/inverse forecast and seeded integrated process |
| M06 SARIMA | DATA GATED | seasonal recursion is tested; G-DATA-06 blocks coverage claims |
| M07 EGARCH | NUMERICAL GREEN | bounded deterministic Gaussian QMLE recovers persistence/leverage on a seeded process |
| M08 OU | NUMERICAL GREEN | exact discretisation, estimates, standard errors and half-life |
| M09 Heston | DATA GATED | Little-Heston-trap pricer passes limits/parity; G-DATA-07 blocks market validation |
| M10 Trinomial | DATA GATED | recombining European/American convergence; G-DATA-07 remains |
| M11 PDE | DATA GATED | stable explicit log-space grid, convergence and parity; G-DATA-07 remains |
| M12 MLP | NUMERICAL GREEN | full hidden/readout backpropagation, finite-difference gradient proof and complete checkpoint |
| M13 LSTM | NUMERICAL GREEN | analytic BPTT over every gate, finite-difference gradient proof and complete checkpoint |
| M14 GRU | NUMERICAL GREEN | analytic BPTT over reset/update/candidate gates, gradient proof and complete checkpoint |
| M15 Transformer | NUMERICAL GREEN | causal multi-head stacked reference with positions, layer norms, full-parameter training and checkpoint |
| M16 Causal CNN | NUMERICAL GREEN | explicit left padding, dilation and receptive-field impulse proof |
| M17 Stat-arb portfolio | NUMERICAL GREEN | capacity-bounded, costed dollar/factor-neutral basket and conservation fixture |
| M18 Factor risk | DATA GATED | covariance/idiosyncratic reconciliation is green; G-DATA-03 remains |
| M19 Named factors | DATA GATED | explicit PIT value/quality/momentum portfolios; G-DATA-03 remains |
| M20 Queue position | DATA GATED | strict FIFO replay and gap invalidation; G-DATA-01 remains |
| M21 Fill probability | DATA GATED | fold-scaled hazard/reliability fixture; G-DATA-01/02 remain |
| M22 Hawkes | DATA GATED | univariate exponential MLE/residual fixture; licensed timestamps and multivariate extension remain |
| M23 Event/news | DATA GATED | PIT event study is green; corpus/text sentiment needs G-DATA-04 |
| M24 Agent simulation | DATA GATED | deterministic clearing and conservation; assumption validation needs G-DATA-05 |
| M25 DQN | DATA GATED | neural replay/target/checkpoint fixture; paper-environment baseline needs G-DATA-05 |
| M26 PPO | DATA GATED | clipped objective, GAE, masks and checkpoint fixture; G-DATA-05 remains |
| M27 Actor-critic | DATA GATED | defined losses and gradient direction fixture; G-DATA-05 remains |

Current numerical-engine tally: **27/27 implemented, 0/27 partial, 0/27
absent**. Data-gated cards remain explicitly non-market-validated; synthetic
fixtures are never relabelled as market performance or a production fit.
