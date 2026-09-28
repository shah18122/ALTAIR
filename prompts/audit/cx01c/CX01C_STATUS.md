# CX-01 (Claude continuation) — status and resume point

**Phase 1 (CX-01) audit is COMPLETE — 2026-09-15 ~12:45 IST.**
- **Coverage:** 481/481 in-scope files and 141,880/141,880 lines fully read, every file by one of 22 independent reviewer groups or the lead. Recorded ranges and before/after SHA-256 match the inventory (0 changed).
- **Findings:** 321 indexed (P1 16 · P2 107 · P3 198); 16 lead-verified against source (V-01…V-16).
- **Unverified validation gates (external blockers, not audit gaps):** ThreadSanitizer, UBSan, Linux GCC/Clang build.
- **Coverage method:** coverage is reviewer-attested through per-file ledgers. The lead personally re-read only the lines cited in `LEAD_VERIFICATION.md` and the LEAD files.

The rows below are the historical progress record.
Last update: 2026-09-15 ~02:00 IST · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`

## How this continuation is organised

- Inventory and assignment: `CX01C_PARTITION.json` — 481 in-scope files / 141,880 lines, split into 22 reviewer groups plus LEAD. Codex's `CX01_MANIFEST.json` (476 files) is unchanged. The differences are 2 config CSVs and 3 `.gitkeep` files; Codex's own audit artefacts are excluded from the denominator.
- Each reviewer writes `<GROUP>_coverage.json` (exact ranges, hashes) and `<GROUP>_findings.md`.
- Merge: `python <scratchpad>/cx01_merge_coverage.py` → `CX01C_COVERAGE_LEDGER.{json,md}`. It counts a file FULL only when the ranges cover every line and the hash still matches. Codex inherited coverage is listed, never counted.
- Lead spot-checks: `LEAD_VERIFICATION.md`.
- **Usage limits interrupted reviewers twice** (2026-09-14 ~16:15 and ~21:12). Run at most **2** reviewers concurrently, and require findings to be written incrementally.

## Coverage at this checkpoint (merge run 2026-09-15 01:53)

**228 / 481 files, 63,649 / 141,880 lines FULL.**

| Group | State |
|---|---|
| LEAD, G02, G03, G05, G08, G13, G15 | Coverage complete; findings files complete |
| G14 broker/kite tools | All files read per reviewer; findings file complete; coverage JSON being finalised |
| G17 desktop shell/terminal | **Complete** (13/13; 22 findings: 1 P1, 10 P2, 11 P3) — 2026-09-15 ~02:25 |
| G16 desktop quant pages | **Complete** (13/13, 8,089 lines; 17 findings, 1 P1; training-status-in-UI table) — 2026-09-15 ~02:55 |
| G09 model families | resumed 2026-09-15 ~02:55 (was 7/17) |
| G12 risk/charges | **Complete** (26/26; 21 findings, 3 P1) — 2026-09-15 ~02:35 |
| G01 core-a/build | **Complete** (34/34, 5,557 lines; E001/E002/E006 confirmed) — 2026-09-15 ~02:45 |
| G04 instruments/research | **Complete** (26/26, 7,492 lines; 18 findings, 5 P2) — 2026-09-15 ~02:20 |
| G09 model families | **Complete** (17/17, 6,338 lines; 20 findings, no P1) — 2026-09-15 ~07:00 |
| G11 strategies-b | **Complete** (13/13, 4,794 lines; 3 P1: overnight look-ahead, spot-parity formula, exercise STT) — 2026-09-15 ~07:15 |
| G07 analytics-b | **Complete** (15/15, 5,657 lines; 12 findings, all P3) — 2026-09-15 ~07:35 |
| G22 docs cards/config/ops | **Complete** (46/46, 7,283 lines; 6 findings P2/P3; `config/altair.toml` has zero consumers) — 2026-09-15 ~12:05 |
| G19 retired client A | **Complete** (25/25, 5,786 lines; 3 findings, all P3; 5 desktop survivals verified) — ~12:25. **Process deviation, disclosed by the reviewer:** one `node -e` call to syntax-check its own output JSON, against the "no node" rule. No project file was touched and `client/` is unchanged per `git status`. |
| G20 retired client B | **started** ~12:25 as a fresh reviewer (smaller model; explicitly no JS tooling) |
| G18 desktop widgets/data | **Complete** (28/28, 7,811 lines; UI-005 confirmed; Atlas 26 rows spot-checked, no status mismatch) — 2026-09-15 ~12:00 |
| G21 P0 task cards | **Complete** (14/14, 6,876 lines; 4 P2 — four core defects originate in the card specs; all named acceptance tests present) — ~12:30 |
| Not yet started | G19, G20 (retired client) |
| G10 strategies-a | **Complete** (13/13, 4,668 lines; P2s only) — 2026-09-15 ~07:25 |
| G18 desktop widgets/data | resumed 07:25 (no checkpoint before; reads in agent context) |
| G06 analytics-a | **Complete** (18/18, 5,553 lines; 13 findings, 3 P2, no P1) — 2026-09-15 ~07:05 |
| G10 strategies-a | resumed 07:05 (no checkpoint before; reads in agent context) |
| Merge at 06:52 | 292 / 481 files, 88,245 / 141,880 lines FULL |
| Merge at 07:20 | 331 / 481 files, 101,751 / 141,880 lines FULL; index 255 rows |
| Merge at 12:15 | **424 / 481 files, 124,225 / 141,880 lines FULL** — every code group complete; remaining G19 (25 files), G20 (18), G21 (14) = 57 files / 17,655 lines of docs and retired client; index 312 rows (P1 16 · P2 103 · P3 193); lead verifications V-01…V-16 |
| Merge at 02:45 | **271 / 481 files, 81,996 / 141,880 lines FULL**; index 190 rows (P1 13 · P2 74 · P3 103) |
| G01 core-a/build | 26/34 recorded; no findings file |
| G04 instruments/research | 10/26 recorded; no findings file |
| G06 analytics-a | 6/18 recorded; no findings file |
| G09 model families | 7/17 recorded; no findings file |
| G16 desktop quant pages | 5/13 recorded; no findings file |
| G07, G10, G11, G18, G19 | 0 recorded (interrupted before any checkpoint) |
| G20, G21, G22 | never started |

## Deliverables state

| File | State |
|---|---|
| `CX01C_BASELINE_REPORT.md` | **Complete** (default 130/130, net 135/135, asan 129/129 with 0 warnings; tsan unavailable; latency table) |
| `CX01C_FYERS_PREREQUISITES.md` | **Complete** |
| `CX01C_IDEA_ATLAS_READINESS.md` | **Complete** — idea.txt sections; §4 Atlas status × integration × disposition table (written ~12:10, all code groups in); §5 training status reconciled with G09 |
| `LEAD_VERIFICATION.md` | V-01..V-10 recorded |
| `CX01C_COVERAGE_LEDGER.{json,md}` | Interim; re-run the merge |
| `CX01C_FINDINGS_INDEX.md` | Generated by `cx01_findings_index.py`; re-run as groups finish |
| `CX01C_FINDINGS.md` (prioritised narrative) | **Draft written** 2026-09-15 ~02:15 (leads table, 10 priority P1s, 10 themes, first 12 regression tests); revise when remaining groups land |
| `CX01C_ARCHITECTURE.md` | **Draft written** 2026-09-15 ~02:05; sections marked PROVISIONAL need G01/G04/G06/G07/G09/G10/G11/G12/G16/G18 |
| `CX01C_PHASE2_TASKS.md` | **Draft written** 2026-09-15 ~02:30 (tiers 0, A–I; Models GUI request = CX06-G4 with Atlas-unchanged constraint and scope to confirm) |
| `prompts/CODEX_PROJECT_MEMORY.md`, `change_by_codex.txt` | Not yet appended |

## Resume order

1. Finish G14 and G17.
2. Resume G12 and G16, then G01 and G04, then G06 and G09. Each keeps its reads in its agent context; instruct it to write findings first.
3. Fresh reviewers for G07, G10, G11, G18, then G19, G20, G21, G22 (docs and retired client suit a smaller model).
4. Re-run the merge and index. Write FINDINGS, ARCHITECTURE and PHASE2_TASKS. Fill Atlas section 4 and training section 5. Append memory and change log.

## User requests received with the Phase 1 brief (2026-09-14)

- "fetch data till yesterday": **not done.** Kite session file was last written 2026-09-13 00:01; daily tokens expire, so a fetch needs Smit to log in today. Daily NIFTY/BANKNIFTY/VIX already end 2026-09-11 (Fri), the last trading day before Sat 12 / Sun 13.
- "i want gui in models", "dont change model atlas": **deferred to Phase 2** (the brief forbids implementation until Phase 1 is complete); recorded as a Phase 2 card with the Atlas-unchanged constraint.
- "is training of all models perfectly done?": answered in `CX01C_IDEA_ATLAS_READINESS.md` section 5 from reviewer evidence. Early answer from G08: **no**. Nothing is persisted; models are refit in memory per run; evaluated forecasts are at or below a random walk.
