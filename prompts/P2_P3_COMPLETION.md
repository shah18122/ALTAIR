# Phase 2/3 implementation and operational gates

Updated 2026-09-25. This record contains no credentials and authorises no
order submission.

## Implemented and verified locally

- Secure setup, OAuth verification and typed FYERS/Kite account snapshots.
- Broker-service state machine: epochs, one poll in flight, bounded backoff,
  immutable publication, revocation/helper-death/staleness handling and
  bounded redacted audit events.
- Desktop pills and Brokers overview consume only short-lived verified helper
  evidence. Session/account files alone cannot create a connected state.
- FYERS identity, JSON decoder, fixed-capacity SDK callback adapter,
  subscribe/unsubscribe state, reconnect/gap/backpressure accounting.
- Epoch-aware shared source router with normalisation, duplicate refusal and
  depth reset generation. It is fully headless.
- FYERS 1m/5m history request/parser/fetch path and the existing exact-paise,
  regular-session OHLCV consistency audit.

Focused verification:

```text
ctest --test-dir build/net -C RelWithDebInfo \
  -R "desktop_broker_status|fyers_account_dry_run|kite_account_dry_run|fyers_history_dry_run|fyers_auth|account_snapshot|snapshot_lifecycle|snapshot_publisher|broker_service_runtime|fyers_decoder|fyers_adapter|failover|fyers_historical|desktop_bar_consistency"
```

Result: 15/15 passed. The full network suite result was 173/174; only the
unrelated `desktop_pages` parity-report assertion failed.

## Safe FYERS history audit

After linking FYERS in the Brokers page, choose new output paths:

```text
build/net/app/altair_fyers_history.exe --symbol NSE:SBIN-EQ \
  --from 2026-09-01 --to 2026-09-24 \
  --out-1m audit/fyers-sbin-1m.csv --out-5m audit/fyers-sbin-5m.csv --go

build/net/desktop/altair_bar_consistency.exe \
  --base audit/fyers-sbin-1m.csv --target audit/fyers-sbin-5m.csv \
  --base-minutes 1 --target-minutes 5
```

The fetcher is dry-run unless `--go` is present and refuses to overwrite either
CSV. The report never modifies historical data. Establish provider, instrument,
contract/roll, adjustment, timezone and closed-bar equivalence before treating
a mismatch as a market-data defect.

## Still operationally gated

- Install/link the official FYERS C data WebSocket SDK (and its curl, cJSON,
  OpenSSL and libwebsockets dependencies) to run the transport callback against
  a real entitled account. The engine-side adapter deliberately does not
  reimplement FYERS' proprietary binary HSM protocol.
- Run the 1m/5m fetch with a linked account and preserve the generated JSON
  discrepancy report as P3-05 evidence.
- A continuously running app-level supervisor and the dashboard's historical
  filter/export log view remain P2-09/P2-10 work; the state machine, bounded
  log and verified snapshot projection are complete foundations.

No live order path was added or enabled.
