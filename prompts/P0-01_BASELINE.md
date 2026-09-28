# P0-01 Baseline Verification

Date: 2026-09-23  
Scope: read-only repository/build verification before the next implementation phase.

## Repository state

- HEAD: `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`
- Worktree: dirty with 131 entries. Existing user/Claude/Codex edits were preserved; no reset, checkout, commit, or push was performed.
- The root execution plan is [plan.md](../plan.md).

## Build configurations

| Build | Configuration | Qt | Torch | Network helpers |
|---|---|---|---|---|
| `build/codex-cx06` | RelWithDebInfo, tests ON | Qt 6.8.3 MSVC 2022 x64 | `ALTAIR_ENABLE_TORCH=OFF` | not enabled |
| `build/net` | RelWithDebInfo, tests ON | Qt 6.8.3 MSVC 2022 x64 | `ALTAIR_ENABLE_TORCH=OFF` | `ALTAIR_HAVE_NET=ON` |

The default executable is `build/codex-cx06/desktop/altair_desktop.exe`; the network executable is `build/net/desktop/altair_desktop.exe`. No Altair desktop process was running during this verification, so the prior linker-lock condition was absent.

## Focused checks

- `build/codex-cx06/desktop/altair_navigation_test.exe`: passed, `Navigation: 0 failure(s)`.
- `build/codex-cx06/desktop/altair_broker_status_test.exe`: passed, `all checks passed`.
- `cmake --build build/net --target altair_desktop --parallel 4`: passed; the network-enabled desktop executable relinked successfully after the prior process lock was gone.
- Full configured CTest run: **144/144 passed**, 0 failed, 241.99 seconds real time (`build/codex-cx06`, `RelWithDebInfo`).

The suite is the current Windows baseline only; it does not establish macOS portability or live broker connectivity.

## Known gaps carried into later phases

- No macOS/Apple-silicon build has been performed; page allocation and x86 TSC paths still require a portability decision.
- GPU/model acceleration is not active in either Windows build (`ALTAIR_ENABLE_TORCH=OFF`); the model interface is currently plain C++ unless a backend is explicitly enabled.
- A saved broker session/metadata probe is not proof of a live FYERS or Kite account connection. No account snapshot, quote stream, order, or broker login was made in this baseline.
- The existing terminal/navigation surface still contains legacy chart/grid/account routes; cleanup is scheduled in the plan rather than inferred as complete.

## Safety boundary

No broker credentials were read or written, no live broker/network trading call was made, no order was submitted, and no external repository state was changed.
