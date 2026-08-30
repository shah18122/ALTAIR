# Build Protocol — Claude ⇄ DeepSeek V4

The operating system of this project. 115 task cards will pass through it.

---

## 1. Roles

| Actor | Owns | Never does |
|---|---|---|
| **Smit** | Decisions, capital, broker accounts, phase go/no-go | — |
| **Claude** | Architecture, task cards, interface contracts, review of every output, phase gates, financial and physics correctness | Write bulk implementation |
| **DeepSeek V4** | Implementation of exactly one card per prompt, plus its tests | Invent interfaces, add dependencies, touch files outside its manifest |

**Why this split:** 115 prompts across ~6 months will drift unless something
external carries continuity. That something is the **interface contract** in each
card — not DeepSeek's memory, not a growing context window. Every card is
self-contained and every header signature is given verbatim, so card 87 still
compiles against card 3.

---

## 2. The loop

```
┌─ Claude writes card P<phase>-<nn> ──────────────────────────────────┐
│                                                                     │
│   Smit pastes it into DeepSeek V4                                   │
│              ↓                                                      │
│   DeepSeek returns complete files + tests                           │
│              ↓                                                      │
│   Smit pastes the output back to Claude                             │
│              ↓                                                      │
│   Claude runs the 8 review gates                                    │
│         ├── any gate fails → Claude writes a CORRECTION card ───────┤
│         └── all gates pass → commit, mark DONE in LEDGER.md         │
│                                                                     │
└─ next card ─────────────────────────────────────────────────────────┘

At the end of each phase: Claude runs the PHASE GATE
  — full replay regression, latency suite, exit criteria checked off,
    written go/no-go before the next phase starts.
```

---

## 3. Card anatomy

Fixed order. Every card has all eight sections.

| § | Section | Purpose |
|---|---|---|
| 1 | **Context** | 3–6 lines. Where this sits. No more — DeepSeek drifts when given the whole roadmap. |
| 2 | **File manifest** | Exact files to create or modify. Anything not listed is off-limits. |
| 3 | **Interface contract** | Header signatures, verbatim. DeepSeek fills in bodies; it does not design the API. |
| 4 | **Behavioural spec** | Numbered, individually testable requirements. |
| 5 | **Constraints** | Allocation, latency, threading, exception, dependency rules. |
| 6 | **Acceptance tests** | Named tests with the assertions spelled out, including expected numeric values. |
| 7 | **Forbidden** | Explicit list of what fails review. |
| 8 | **Rules block** | The standard block from §5 below, verbatim. |

---

## 4. The eight review gates

Nothing is committed until all eight pass.

| # | Gate | Check |
|---|---|---|
| 1 | **Compiles clean** | `cmake --build --preset default`, zero warnings at `/W4` or `-Wall -Wextra -Wpedantic -Wconversion` |
| 2 | **Contract honoured** | Produced headers diffed against the card's interface contract — byte-level on signatures |
| 3 | **Manifest respected** | `git status` shows only files listed in the card |
| 4 | **Tests exist and pass** | Every named acceptance test present, `ctest --preset default` green |
| 5 | **No hot-path allocation** | Grep for `new`, `malloc`, `std::vector` growth, `std::string`, `shared_ptr`, `std::function` inside any `ALTAIR_HOT` function |
| 6 | **Latency budget** | Google Benchmark result vs ROADMAP §11; a regression fails the gate |
| 7 | **Numerical / financial sanity** | Units correct · no catastrophic cancellation · boundary cases (T→0, IV→0, zero depth, negative rates, expiry day) · sign conventions · day-count basis · **STT/charge side correct** · premium-vs-notional turnover for options |
| 8 | **Physics sanity** | Dimensional consistency · conservation invariants hold · no look-ahead · no sampling above Nyquist · error propagation preserved |

**Gate 7 is the one that matters most and is easiest to skip when in a hurry.**
DeepSeek produces confident, plausible, subtly wrong finance: the sign of Θ, which
side STT applies to, ACT/365 vs ACT/252, and premium vs notional turnover for
options are all things it will get wrong while looking correct. Never wave it through.

---

## 5. Standard rules block

Paste verbatim at the bottom of every card.

```
RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No `using namespace` at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If a requirement is ambiguous, implement the most conservative reading and
    list the ambiguity under "ASSUMPTIONS" at the end of your response.
```

---

## 6. Countermeasures for DeepSeek's known failure modes

| Failure mode | Countermeasure |
|---|---|
| Silently redesigning the API | Interface contract verbatim in the card; gate 2 diffs it |
| Truncating long files | Cards sized so no file exceeds ~400 lines; split otherwise |
| `#include <iostream>` and printf debugging | Forbidden list; gate 1 warnings |
| Hidden heap allocation in "clean-looking" code | `ALTAIR_HOT` marker + gate 5 grep |
| Plausible-but-wrong finance | Gate 7; formulas given in the card with a reference |
| Losing context across prompts | Cards are self-contained; the contract carries continuity |
| Tests that assert whatever the code happens to do | Assertions specified *in the card*, with expected values |
| Inventing a dependency to avoid work | vcpkg.json is frozen; gate 1 fails on a missing package |
| "Improving" adjacent code | Manifest + gate 3 |
| Swallowing errors to make tests pass | Gate 4 reviews the test bodies, not just the exit code |

---

## 7. Correction cards

A failed gate produces a **correction card**, never a from-scratch rewrite.
Rewrites lose the parts that were right and burn a prompt.

Format:

```
CORRECTION to P2-04.

WHAT FAILED
  Gate 7 — numerical/financial.

WHAT IS WRONG
  Line 88: turnover for options uses strike × qty. Options turnover for both
  STT and exchange charges is PREMIUM × qty.

WHY IT MATTERS
  Overstates NIFTY options cost by ~200x, which makes every options signal
  look unprofitable and silently disables the strategy.

FIX
  Change `turnover = strike * qty` to `turnover = premium * qty` when
  spec.segment == Segment::OPT. Add the assertion below to
  test_cost_options_uses_premium_turnover.

  REQUIRE(cost.turnover == Notional{premium.value() * qty.value()});

Change ONLY the lines needed for this fix. Return the complete file.
Everything else in P2-04 passed review — do not alter it.
```

---

## 8. Card sizing

| Signal | Action |
|---|---|
| Any file would exceed ~400 lines | Split the card |
| More than 4 files in the manifest | Split the card |
| More than 12 numbered requirements | Split the card |
| More than 8 acceptance tests | Split the card |
| The card needs a design decision DeepSeek must make | **Stop.** Claude makes the decision and puts it in the contract. |

---

## 9. Commit convention

```
P<phase>-<nn>: <deliverable>

Card:    prompts/P0-01_core_types_time.md
Gates:   1✓ 2✓ 3✓ 4✓ 5✓ 6✓ 7✓ 8✓
Bench:   Clock::now p99 = 18ns (budget 25ns)
Notes:   <assumptions DeepSeek listed, and how they were resolved>
```

One card = one commit. Never squash cards; the ledger and the git history must
line up, so any regression can be traced to the prompt that produced it.

---

## 10. Phase gate checklist

Run by Claude at the end of every phase, before the next begins.

- [ ] Every card in the phase is DONE in `LEDGER.md`
- [ ] Full build clean on both Windows and Linux
- [ ] `ctest` green, no skipped tests
- [ ] Latency suite within ROADMAP §11 budgets
- [ ] Replay regression: prior phases' golden outputs still byte-identical
- [ ] Invariants armed and never tripped over a full replayed session
- [ ] The phase's stated **Exit** criterion in ROADMAP §12 is met, demonstrably
- [ ] Written go/no-go, with anything deferred recorded in the ledger
