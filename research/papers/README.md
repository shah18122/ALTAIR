# Altair — Research Paper Intake

Drop papers here. Nothing else to do.

## How to add a paper

1. Copy the PDF into `research/papers/inbox/`.
2. Filename convention (helps the extractor, not enforced):
   `YYYY_firstauthor_short-topic.pdf`
   e.g. `2019_cartea_optimal-execution-order-imbalance.pdf`
3. Tell Claude: *"ingest new papers"*.

## What happens on ingest

Each paper is read and reduced to a **feature card** written into
`research/papers/index.md` and a machine-readable row in
`research/papers/registry.json`:

| Field | Meaning |
|---|---|
| `id` | stable slug, referenced by C++ feature code |
| `claim` | the one sentence the paper actually proves |
| `inputs` | what market data it needs (L1/L2 book, trades, OI, IV surface…) |
| `horizon` | prediction horizon the paper validates (ticks / seconds / minutes) |
| `formula` | the implementable core, transcribed |
| `assumptions` | what breaks it (market regime, venue, tick size, liquidity) |
| `impl` | `none` \| `prototyped` \| `live` — path to the C++ file once written |
| `status` | `unverified` \| `replicated` \| `rejected` on OUR data |
| `edge_bps` | measured, after cost, on NSE/BSE data — filled in only after replication |

**Rule: nothing goes live from a paper until `status: replicated` and
`edge_bps` survives the cost model.** A paper is a hypothesis, not a signal.

## Directory layout

```
research/
├── papers/
│   ├── inbox/          ← you drop PDFs here
│   ├── processed/      ← moved here after a feature card is written
│   ├── rejected/       ← replication failed; keep the card explaining why
│   ├── index.md        ← human-readable feature cards
│   └── registry.json   ← machine-readable, consumed by the feature builder
├── notebooks/          ← C++ / xeus-cling or plain cpp scratch studies
└── replication/        ← one folder per paper: data slice, test, result
```

## Reading list to seed (drop these first if you have them)

Microstructure & order flow
- Kyle (1985) — *Continuous Auctions and Insider Trading* (lambda / price impact)
- Glosten & Milgrom (1985) — bid-ask spread from adverse selection
- Almgren & Chriss (2000) — *Optimal Execution of Portfolio Transactions*
- Cont, Kukanov & Stoikov (2014) — *The Price Impact of Order Book Events* (OFI)
- Cartea, Jaimungal & Penalva — *Algorithmic and High-Frequency Trading*
- Easley, López de Prado & O'Hara (2012) — *Flow Toxicity and Liquidity* (VPIN)
- Bouchaud et al. — *Trades, Quotes and Prices*

Volatility & options
- Black–Scholes (1973), Merton (1973)
- Heston (1993) — stochastic volatility closed form
- Dupire (1994) — local volatility
- Gatheral & Jacquier (2014) — *Arbitrage-free SVI volatility surfaces*
- Bergomi — *Stochastic Volatility Modeling*
- Carr & Wu (2006) — *A Tale of Two Indices* (VIX construction / variance swaps)
- CBOE VIX White Paper (India VIX uses the same methodology on NIFTY options)

Statistical arbitrage & pairs
- Gatev, Goetzmann & Rouwenhorst (2006) — *Pairs Trading*
- Avellaneda & Lee (2010) — *Statistical Arbitrage in the US Equities Market*
- Engle & Granger (1987) — cointegration
- Johansen (1991) — multivariate cointegration test

Portfolio & sizing
- Kelly (1956), Thorp on fractional Kelly
- Ledoit & Wolf (2004) — shrinkage covariance estimation
- López de Prado — *Advances in Financial Machine Learning* (triple-barrier
  labelling, purged K-fold CV, meta-labelling, fractional differentiation)

Deep learning for time series
- Hochreiter & Schmidhuber (1997) — LSTM
- Cho et al. (2014) — GRU
- Lim et al. (2019) — *Temporal Fusion Transformers*
- Zhang, Zohren & Roberts (2019) — *DeepLOB* (CNN/LSTM on limit order books)
- Sirignano & Cont (2019) — *Universal features of price formation*
- Oreshkin et al. (2020) — N-BEATS

Valuation (for the sector hedge book)
- Fama & French (1993, 2015) — factor models
- Damodaran — *Investment Valuation* (DCF discipline)

> Nothing above is copied into the repo. Source your own PDFs legally —
> arXiv and SSRN preprints cover a large share of this list.
