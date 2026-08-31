# P1-08b — `instruments/master_fetch`: downloading the exchange masters

> Phase 1 · Card 8b of 9 · Status: **BLOCKED — vcpkg (blocker 7)**
> Depends on: P1-08a (the policy) · P1-02a/b/c · P1-03a
> Feeds: P2-04
>
> **Architect's note.** Split from P1-08a because the two halves are blocked on
> different things and have opposite risk profiles. The policy is pure logic and
> was implementable immediately; the fetch needs an HTTP client Altair does not
> have, and is the most fragile code in Phase 1 — NSE's endpoints want specific
> headers and a cookie handshake, payloads are gzipped, and URLs move without
> notice.
>
> **The order matters.** The policy exists *first* so that when the fetch fails —
> and it will, because scraping a hostile endpoint at 08:15 always eventually
> fails — there is already a decided answer about what happens next. A fetcher
> written before its failure policy invents one under pressure.

---

## 1. WHAT MAKES THIS FRAGILE

- **NSE rejects non-browser clients.** A bare `GET` returns 403. It wants a
  browser `User-Agent`, an `Accept` header, and a session cookie obtained by
  first requesting the site root. That handshake is undocumented and changes.
- **Payloads are gzipped**, sometimes with a `.zip` wrapper rather than
  `Content-Encoding`.
- **URLs move.** `www1.nseindia.com` became `nsearchives.nseindia.com`. A
  hardcoded URL is a scheduled outage, so every endpoint is **config**, never a
  literal — `config/altair.toml`, alongside `snapshot_dir`.
- **Nothing about this is authenticated**, which is the one mercy: these are
  public files, so this card needs no credentials and must not accept any.

---

## 2. THE RULE THAT GOVERNS IT

**A fetch never overwrites the last good snapshot until it has fully
succeeded.** Download to a temporary path, verify the payload parses with the
matching P1-02/P1-03 parser, and only then rename into `snapshot_dir`.

A half-written master is worse than no master: `NoSnapshot` halts the session
loudly, while a truncated file parses to a *plausible subset* of the universe,
and the missing symbols look like symbols that simply do not trade today. The
rename must be atomic on Windows and POSIX both.

---

## 3. FILE MANIFEST

```
CREATE   instruments/master_fetch.hpp
CREATE   instruments/master_fetch.cpp
CREATE   instruments/tests/test_master_fetch.cpp
MODIFY   instruments/CMakeLists.txt
```

Not header-only: this one links an HTTP client and must not drag it into every
translation unit that includes an instruments header.

---

## 4. REQUIREMENTS (contract to be finalised when vcpkg unblocks)

1. Every endpoint URL, timeout, and retry count comes from config. No literals.
2. Download to a temp path; parse-verify; then atomically rename. §2.
3. Snapshots are retained per `keep_snapshots_days` and named by trading date,
   so P1-08a's `SnapshotState::taken_at` is recoverable from the filename
   alone — not from a sidecar that can go missing.
4. A failed fetch is **not an error return**. It produces a
   `SnapshotState{present=<whatever is on disk>, fetch_ok=false}` and hands it
   to `judge_snapshot`. The fetcher does not decide; P1-08a does.
5. Retries are bounded and total elapsed time is capped **well inside the
   08:15→09:15 window**, with the cap in config. A fetcher that retries until
   09:20 has made the halt decision by accident.
6. No credentials are read, accepted, or stored. These files are public.
7. The HTTP client is confined to the `.cpp`. No header exposes it.

---

## 5. ACCEPTANCE TESTS

Network tests are not run in CI. Everything below is against a local fixture or
a temp directory:

1. **`atomic_replace`** — a fetch that fails midway leaves the previous
   snapshot byte-identical. Simulate by writing a truncated temp file and
   asserting the rename never happened.
2. **`parse_verify_before_rename`** — a payload that downloads cleanly but
   fails its parser does **not** replace the good snapshot.
3. **`failed_fetch_produces_state_not_error`** — a fetch failure with a good
   snapshot on disk yields `fetch_ok == false, present == true`, and
   `judge_snapshot` decides. The fetcher returns no error.
4. **`taken_at_from_filename`** — a snapshot's trading date round-trips through
   the filename with no sidecar.
5. **`retention`** — snapshots older than `keep_snapshots_days` are pruned;
   the most recent is **never** pruned regardless of age, because pruning the
   last one converts `TooOld` into `NoSnapshot` and turns a degraded session
   into a halted one.
6. **`time_cap`** — the retry loop honours the configured cap. Assert with an
   injected clock, not by sleeping.
7. **`no_credentials`** — grep-level: the target links no credential source and
   reads no `ALTAIR_*` secret env var.
