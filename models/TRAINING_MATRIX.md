# Model training and accelerator matrix

Updated 2026-09-28. “Implemented” means the numerical engine and deterministic
tests exist. It does not mean the model has been fitted on a production dataset,
selected out of sample, or approved for live use.

## Hardware decision

No Altair model requires a graphics card for correctness. Every current model
has a deterministic CPU path. A GPU becomes economically useful—not logically
required—when the neural training dataset or hyperparameter search is large.
Moving to a Mac is therefore not a prerequisite for the current Windows build.
Choose Apple Silicon only after a representative CPU-versus-MPS benchmark shows
that training wall time or power use justifies the migration.

| Family | Current fit state | CPU | Graphics card | Production gate |
|---|---|---|---|---|
| Linear/ridge/logistic, SVM, KNN, random forest, GBDT | implemented; fixture/available-data fits | preferred | not useful | labelled causal dataset and purged OOS score |
| PCA/linear autoencoder, factor risk, named factors | implemented; market fit gated | preferred | not required | survivorship-safe point-in-time fundamentals |
| ARMA/ARIMA/SARIMA, OU, Markov, GARCH/GJR/EGARCH, HMM | implemented; CPU fit | preferred | not required | interval coverage, residual diagnostics and walk-forward score |
| Heston/SABR/local vol/PDE/lattices | implemented numerical calibration/pricing | preferred | optional for large surfaces | dated executable option surface |
| MLP | full backprop implemented and fixture-trained | suitable for small jobs | recommended for wide/deep searches | labelled causal dataset and purged OOS score |
| LSTM and GRU | analytic BPTT implemented and fixture-trained | suitable for small sequences | recommended for long sequences/searches | causal sequence dataset and purged OOS score |
| Transformer | trainable causal multi-head stack implemented; numerical-gradient reference | suitable only for tiny references | recommended for practical training/autograd | causal sequence dataset and purged OOS score |
| Causal CNN | implemented numerical engine | suitable for small jobs | recommended at scale | labelled sequential dataset |
| DQN, PPO, actor-critic | implemented and fixture-trained | suitable for deterministic test environments | optional/recommended for large environments | frozen paper environment plus heuristic baseline |
| Queue/fill/Hawkes | implemented numerical engines; not market-trained | preferred | not useful | licensed order-level stream and labelled fills |
| Event study and sentiment | event engine implemented; corpus fit gated | preferred for event study | optional for large NLP encoders | licensed point-in-time news corpus |
| Agent market simulation | implemented and seeded | preferred | optional for massive sweeps | validated agent assumptions |

## Scaling contract

- Fit feature and target scalers on the training fold only.
- Train time-series regressors on returns, basis points, volatility-scaled
  targets, or `TargetScaler` standard units rather than raw large price levels.
- Store the fitted mean/scale with the checkpoint.
- Inverse-transform every prediction before displaying price, P&L or risk.
- Never fit a scaler across validation/test rows and never compare a scaled
  prediction directly with an unscaled market value.
