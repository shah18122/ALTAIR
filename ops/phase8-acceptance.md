# Phase 8 acceptance record

Recorded 2026-09-28. This separates the completed Windows-local release tier
from evidence that can only come from entitled broker accounts or licensed
market data. An empty account is safe for read-only checks, but it is not proof
of reconnect, revocation, fill, route or live-feed behaviour.

## Windows-local tier — complete

| Gate | Result | Evidence |
|---|---|---|
| Broker disconnected/absent state | PASS | broker status, snapshot lifecycle, service runtime and Brokers page tests |
| Broker attribution and stale expiry | PASS | typed broker state, account snapshot parser/lifecycle and activity projection tests |
| UI structure and laptop scaling | PASS | native shell checks at 100%, 125% and 200%; Brokers page has six workflows and attributed broker cards |
| Brokers visual render | PASS | `build/net/broker-ui-qa.png.brokers.png` from the rebuilt native Windows renderer |
| Model/source publication concurrency | PASS | snapshot slots, MPSC/SPSC, model job, failover and price-client tests |
| Paper execution recovery | PASS | paper venue and paper-session journal/recovery tests |
| Target scale/inverse scale | PASS | train-fold-only `TargetScaler` round trip in `models_dataset` |
| Full regression | PASS | complete CTest suite; exact latest count/time in `remaining_work.md` |
| Desktop stage | PASS | `build/net/desktop/altair_desktop.exe` plus Qt runtime staged by `windeployqt` |
| Windows package | PASS | `build/net/Altair-0.1.0-Windows-AMD64.zip`, 53,789,281 bytes |
| Package SHA-256 | PASS | `2B70CC57889FB2092010FAF9FE4C25ECA20FDC79B0EFE9D320151C20C61B3F67` |
| Live-order safety | PASS | dispatch remains disabled; no live order was submitted |

## External acceptance tier — not yet provable locally

| Scenario | Required evidence |
|---|---|
| FYERS-only, Kite-only and both-linked live routes | fresh authorised sessions and timestamped route/account logs |
| Revoked/expired credentials and reconnect | controlled live-session expiry/revocation window |
| Helper crash during an entitled request | authorised account plus captured supervisor transition |
| Live FYERS 1m/5m discrepancy report | entitled historical API response for the chosen instruments/dates |
| Sustained live-feed latency percentiles | entitled live session; engine and broker/network timing reported separately |
| Licensed MTBT queue/fill/Hawkes validation | licensed sequenced order-level feed and labelled orders/fills |

The Windows build is releasable for research, replay, read-only account views
and isolated paper execution. It is not certified for live order submission.
