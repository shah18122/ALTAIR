# Altair

[![CI](https://github.com/shah18122/ALTAIR/actions/workflows/ci.yml/badge.svg)](https://github.com/shah18122/ALTAIR/actions/workflows/ci.yml)
[![macOS arm64](https://github.com/shah18122/ALTAIR/actions/workflows/macos-arm64.yml/badge.svg)](https://github.com/shah18122/ALTAIR/actions/workflows/macos-arm64.yml)

A C++23 trading engine and Qt 6 desktop for NSE and BSE equities, futures,
options and indices. It covers market data, instrument masters, microstructure,
models, strategies, risk and cost, paper execution and a broker portal, all in
one process.

> **Live order submission is disabled by design.** Every broker order path sits
> behind one dispatch permit that refuses while live trading is off. Paper mode
> runs on real data with an isolated simulated ledger.

The single planning and status document is **[SMIT.txt](SMIT.txt)**: the plan,
the status verified against the code, next steps and the change log.

---

## Brokers

| Broker | Role | What works |
|---|---|---|
| **FYERS** | Primary market data, read-only account | OAuth login, profile/funds/positions/holdings/orders, historical candles, **live data socket** (quotes, 5-level depth, indices) |
| **Zerodha Kite** | Fallback market data, read-only account | Login, account, quotes, historical candles, live ticker |

Credentials never live in source. Helpers read them from the OS vault or
`data/*_session.json`, and every network helper is a dry run unless you pass `--go`.

## Layout

One component, one directory:

```
core/         strong units (integer paise, lots, time), clocks, memory, lock-free queues
instruments/  point-in-time contract spec store; NSE/BSE/FYERS masters
feed/         Kite + FYERS decoders, normaliser, failover, tick store, source router
book/         L2 book, microstructure, order flow
features/ analytics/ models/ strategies/ risk/ backtest/ flagging/
oms/          order state, exit ladder, broker adapters (behind the permit)
broker/       OAuth, verified-TLS HTTPS, account snapshots, WebSockets
app/          helper binaries: the only code that opens sockets or reads credentials
server/       price bus / price service
desktop/      Qt 6 Widgets UI (may not link oms/ or broker/: enforced at link time)
ops/          runbooks
```

## Build

Requirements: CMake 3.28+, Ninja and a C++23 compiler (MSVC 2022, GCC 13+,
Clang 18+ or Apple Clang 16). Networking helpers need Boost (Asio/Beast) and
OpenSSL. The desktop needs Qt 6.8+ (Core, Gui, Network, Svg, Widgets).

### Windows (primary)

```bat
build.bat            :: default preset: engine + tests (+ desktop when Qt is found)
build.bat net        :: + broker helpers via vcpkg (Boost, OpenSSL)
```

On Windows, CMake defaults Qt to `D:/Qt/6.8.3/msvc2022_64`; pass
`-DALTAIR_QT_ROOT=<path>` to use another install. The desktop is at
`build\<preset>\desktop\altair_desktop.exe`.

### Linux

```bash
sudo apt-get install ninja-build libboost-dev libssl-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DALTAIR_BUILD_BENCH=OFF -DALTAIR_NATIVE_ARCH=OFF
cmake --build build
```

### macOS (Apple Silicon)

```bash
brew install ninja boost openssl@3
export ALTAIR_QT_ROOT=$HOME/Qt/6.8.3/macos        # optional, for the desktop
cmake --preset macos-arm64 -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build --preset macos-arm64
cmake --install build/macos-arm64 --prefix ./stage   # Altair.app + helpers
```

## Test

```bash
ctest --test-dir build                  # everything
ctest --test-dir build -LE dataset      # what CI runs
ctest --test-dir build -L dataset       # the real-data tier only
```

Tests labelled `dataset` read `dataset/` (market data, tracked in git). They
fail loudly when it is missing; CI runs them only when `dataset/` is present.
Tests labelled `concurrency` are the threaded ones that CI runs under TSan.

## FYERS quick start

```bash
altair_fyers_login                     # browser OAuth -> data/fyers_session.json
altair_fyers_account --go              # read-only account snapshot
altair_fyers_history --symbol NSE:NIFTY50-INDEX --from 2026-09-01 --to 2026-09-26 --go
altair_fyers_quotes --go --symbols-file config/market_watch.txt   # -> data/fyers_quotes.json
altair_fyers_ticker --go --depth --seconds 60 \
    --symbols NSE:NIFTY50-INDEX,NSE:SBIN-EQ    # live stream -> data/fyers_ticks.json
```

In the desktop, open **Terminal → Positions & Greeks** and press **Refresh FYERS**
for the GETS-style workspace: positions and funds, Greek market watch (IV and
Greeks per contract, user IV override), portfolio Greek summary, what-if
simulation, expense and margin, trade history, RMS, top movers and index
information. It runs `altair_fyers_account` and `altair_fyers_quotes` (both
read-only). Put the Kite instrument master at `data/instruments.csv` so monthly
expiries and lot sizes resolve.

**Headless hosts** (a cloud session, a scheduled job) have no browser for the
login. `altair_fyers_history` and `altair_fyers_ticker` also accept the day's
session from two environment variables. The file wins when both exist, and
neither value is ever printed:
- `ALTAIR_FYERS_CLIENT_ID`
- `ALTAIR_FYERS_ACCESS_TOKEN`

A FYERS token lasts one trading day, so the variable has to be refreshed with
it. The host's network must also allow `api-t1.fyers.in`, `public.fyers.in`
and `socket.fyers.in`. These CLIs contain no order endpoint.

For ticks, open **Live Feed**, choose **FYERS (primary)** and press **Listen**.
The live socket implements FYERS' HSM protocol natively in C++.
`feed/tests/test_fyers_hsm.cpp` checks it byte for byte against vectors
produced by FYERS' official SDK.

## Data audit

`altair_data_audit` checks `dataset/` across timeframes (every coarser bar
rebuilt from every finer one), against its other sources, and against FYERS
and Kite candles, and writes `data/verified/data_audit.xlsx` (one sheet per
instrument) plus one merged CSV per instrument per timeframe. On Windows:

```powershell
powershell -ExecutionPolicy Bypass -File ops\broker_audit.ps1
```

See [ops/data-audit.md](ops/data-audit.md) for the checks and the findings.

## Forecast curriculum

`altair_forecast_curriculum` trains every Model Atlas forecaster that can
make a next-bar call: 33 direction models, from the baselines through ridge,
trees, SVM and kNN, the neural nets (MLP, LSTM, GRU, Transformer and CNN, all
trained by backpropagation), DQN, PPO and actor-critic,
AR/ARMA/SARIMA/VAR, Kalman and HMM, plus 12 volatility-band models (GARCH,
GJR, EGARCH, EWMA, Heston, seasonal, jump diffusion …), with ensembles and
confidence filters, on a doubling schedule: learn 3 days, forecast the
next 3, record every call right or wrong, refit on 6, 12, 24 … days. Tracks:
NIFTY, BANKNIFTY and INDIA VIX at 1m, 5m, 15m, 60m and 1d, plus NIFTY futures
daily, with INDIA VIX as a feature, and NIFTY / BANKNIFTY from 09:20 and
10:15 to the close. A second pass feeds the first pass's out-of-sample
forecasts back in as inputs: band widths, regimes, other timeframes, the
other index, INDIA VIX and every model. Two targets per track: next-bar
direction, and an 80 % range band (scored on how narrow it can be). A
Tradability sheet gives the accuracy each track needs to pay its costs.

```powershell
build\net\app\altair_forecast_curriculum.exe --dataset dataset --out data\verified
```

Writes `data/verified/forecast_curriculum.xlsx` (direction summary, range
bands, accuracy-vs-coverage frontier, learning curve per track, data
cleaning) and a per-forecast log for the hourly and daily tracks. See
[ops/forecast-curriculum.md](ops/forecast-curriculum.md) for the method and
the results.

## Demo trading: band-fade short options and futures pairs

Paper trades only, never sent to a broker; the desktop's **Strategies →
Demo Trading** page runs both and shows trades, per-model totals and equity.

- `altair_band_option_demo`: each band model's 09:20 forecast sells options
  under six rules, one lot, bought back by 15:20. The rules: wait for a touch;
  a strangle at the band edges; with a 2× stop; delta hedged; expiry day.
  Premiums are synthetic (Black-76 at INDIA VIX).
  - Touch-fading loses under every assumption: a touch is a breakout more
    often than a reversal.
  - The 09:20 strangle earns only on the variance clock (+₹98 NIFTY, +₹283
    BANKNIFTY a trade with the stop; t = 3.4 and 7.1). It does so only since
    2021, and mostly from the volatility premium rather than the forecast.
  - It needs real intraday option prices before it means anything.
- `altair_vol_premium`: the options-desk trade. Sell the at-the-money monthly
  straddle when implied vol beats a HAR realised-vol forecast, delta hedged
  with futures.
  - On VIX-priced (synthetic) options it earns in every year to 2025 (t ≈ 5),
    but the forecast adds nothing over selling every month.
  - Real prices come from NSE bhavcopy (`ops/fetch_bhavcopy.ps1`,
    `--source bhavcopy`).
- `altair_resid_reversion`: the quant-fund version of pairs. Each NIFTY 50
  stock's residual against the market and its sector peers is traded on an
  OU s-score (Avellaneda-Lee) with many small hedged positions.
  - The stock data comes from FYERS (`ops/fetch_universe.ps1`).
  - The universe is today's members, so results carry survivorship bias.
- `altair_depth_study`: the HFT question asked honestly. It tests whether
  order flow imbalance, imbalance and microprice in recorded FYERS depth
  (`ops/record_depth.ps1`, `altair_fyers_ticker --stamp`) predict the next
  mid move out of sample, and by more than half the spread.
- `altair_pairs_futures`: walk-forward Engle-Granger pairs, long one future
  and short the other in whole lots, over `config/pairs.csv` (NIFTY-BANKNIFTY,
  CIPLA-SUNPHARMA, HDFCBANK-ICICIBANK and more). Stock legs come from FYERS via
  `ops/fetch_pairs.ps1`. The BANKNIFTY/NIFTY ratio passed 2.6 on 91 days in
  2019-20 (max 2.661).

Expenses are refused while `config/charges.toml` is unverified. Pass
`--unverified-costs` to price them with an UNVERIFIED stamp. See
[ops/demo-trading.md](ops/demo-trading.md). The papers behind both are
feature cards in [research/papers/index.md](research/papers/index.md).

## CI

| Workflow | Runs | What it proves |
|---|---|---|
| [`ci.yml`](.github/workflows/ci.yml) | Push to `main`, every PR | Linux GCC 13 and Clang 18/libc++ build and test; ASan+UBSan over the suite and TSan over the `concurrency` tests; Windows MSVC builds the engine and the Qt 6.8.3 desktop and runs the tests |
| [`macos-arm64.yml`](.github/workflows/macos-arm64.yml) | Manual for now (Actions → Run workflow), parked until the macOS compile errors are fixed | Phase 1 evidence on real Apple Silicon: native build and tests, Darwin memory and clock paths, `Altair.app` bundle and launch |

## Engineering rules (short form)

Money is integer paise, and doubles never carry money across a module
boundary. Lot and tick sizes come from the point-in-time spec store, never
literals. Backtest and live share one code path. There is no look-ahead. A
fixed bound refuses, proves it is unreachable, or truncates visibly. The full
list, and the configure-time audits that enforce it, are in SMIT.txt
section 3.
