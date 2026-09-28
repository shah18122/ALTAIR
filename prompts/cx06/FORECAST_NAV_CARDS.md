# Forecast and Atlas navigation correction — 2026-09-17

User authorises implementation and Atlas changes, superseding the earlier
Atlas-preservation constraint. Remaining phases retain their verification gates.

## N9 — Atlas catalogue navigation

Manifest: desktop/workspace_navigation.hpp, desktop/atlas.hpp,
desktop/main_window.hpp, desktop/tests/test_navigation.cpp.
Contract: navigation exposes `std::function<void(QString)> open_atlas_model`;
AtlasPanel exposes `void focus_model(const QString&)`.
Show every Atlas entry grouped under Model Atlas, including absent entries.
Selecting an entry opens its exact Atlas description; existing routes remain.
Group text clicks toggle expansion, arrows and keyboard retain native behavior.
Tests verify all catalogue entries and absent entries are reachable.
No model is marked built solely because navigation exposes it.

## F1 — cached forward inference

Manifest: models/stream_forecast.hpp, models/tests/test_stream_forecast.cpp,
models/CMakeLists.txt.
Train a fixed GBDT on completed bars in a worker, using lagged log returns and
causal volatility-scaled targets. Use a chronological calibration partition,
never training error, for the reported uncertainty and random-walk comparison.
Inference is allocation-free with fixed 8-element feature/bin arrays. Targets
are strictly after issue time by the chosen duration. Reject invalid/stale or
out-of-order history and invalid numeric outputs. Live intrabar inputs carry an
explicit calibration limitation: bar-boundary evaluation is not tick validation.
Tests cover timing, causality, scaling, invalid input and no training per tick.

## F2 — responsive forecast page

Manifest: desktop/stream_forecast_page.hpp, desktop/main_window.hpp,
desktop/quant_pages.hpp, desktop/CMakeLists.txt.
Provide 1m, 5m, 15m, 60m and daily forecasts; show current price separately from
future prediction and target time. Background training/reporting must not block
the Qt event loop. Subscribe to the existing read-only price client; reject replay
against a model trained on later data, stale ticks and missing timestamps.
Compute on each admitted trade update, throttle only painting. Gaps/disconnects
invalidate live readiness. Changing symbol/timeframe invalidates old results.

## Verification and remaining phases

F3 manifest: desktop/price_client.hpp. Add a trade-only notification so book
updates do not masquerade as new prices on the forecast input.
F4 manifest: desktop/live_forecast.hpp. Each fetch owns a temporary scratch
directory; simultaneous jobs cannot erase or read another job's instrument.

Build isolated desktop/model targets; test model and navigation plus shell
regressions. Record results in change_by_codex.txt. No predictive accuracy or
complete-phase claim without measured evidence. Remaining CX02–CX08 tasks stay
open until their own acceptance requirements pass.
