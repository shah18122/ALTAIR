# `ops/` — runbooks

Documents, not code. Nothing here is built, linked, or tested, and no card
manifest may list a source file in this directory.

These are the procedures a person follows when the engine is running or about
to be. They exist separately from `ROADMAP.md` (what we are building) and
`prompts/` (how we build it) because they are read under different conditions:
at 09:14 with a position open, or at 15:25 with a feed down.

| File | Card | Read it when |
|---|---|---|
| [`linux-deployment.md`](linux-deployment.md) | P12-01 | Moving off the Windows dev box |
| [`disaster-recovery.md`](disaster-recovery.md) | P12-05 | Something is wrong right now |
| [`go-live.md`](go-live.md) | P12-06 | Before the first rupee of real capital |

## The rule these three share

**Every claim states whether it has been verified, and on what.**

Altair has been built and tested on exactly one machine: Windows 10 Pro, MSVC
2022, `--preset default`. Every Linux number below is a *plan*, and every
runbook step that has never been executed against a live broker says so. An
operations document that reads as though it has been rehearsed, when it has
not, is worse than no document — it is followed under pressure by someone who
believes it was.
