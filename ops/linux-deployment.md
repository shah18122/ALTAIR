# Linux deployment — CPU isolation, hugepages, io_uring, NIC tuning

**P12-01.** Status: **PLAN, NOT PROCEDURE.** Steps 1 and 2 of the order below
are done: since 2026-09-29 the engine and helpers build and pass `ctest` on
Linux in CI (GCC 13, Clang 18 + libc++, ASan+UBSan, TSan). The tuning itself
(isolation, hugepages, io_uring, NIC) has still not been measured on this
project. Read it as a design with named numbers, and expect the first
afternoon of real contact to change several of them.

---

## 0. Read this before the rest of the document

**Against Zerodha Kite today, none of this buys a single microsecond you can
trade on.**

CLAUDE.md says it plainly: retail broker APIs cap you at 10–50 ms round trips.
That is four orders of magnitude above everything tuned below. Isolating a core
to shave 8 µs of scheduler jitter off a path that ends in a 30 ms HTTPS round
trip is not an optimisation, it is a decoration.

So the honest case for doing any of this is the other one, and it is real:

1. **Evaluating every model on every tick.** The engine is event-driven because
   order-book imbalance decays in 10–200 ms and the predecessor's 500 ms poll
   could not, even in principle, see it (ROADMAP §3). Breadth — every strategy,
   every instrument, every tick — is a throughput problem, and throughput is
   what isolation and hugepages actually deliver.
2. **Headroom for a DMA/FIX line that does not exist yet.** If that line is ever
   leased, the tuning has to already be understood, because that is not the week
   to be learning what `nohz_full` does.
3. **Jitter, not latency.** A 3 ms GC-style stall from a page fault or an IRQ
   storm during the 09:15 open is visible even at broker latencies, because it
   lands on the one tick that mattered.

Do it for 1 and 3. Do not report 2 as a current benefit.

---

## 1. What must be true before any of this matters

**The build does not exist yet.** Phase 0's gate — a clean build under
`-Wall -Wextra -Wpedantic -Wconversion` on clang — is BLOCKED on this box: the
shell is not elevated, no clang or clang-cl is installed, and `wsl` is present
with no distribution. Every warning MSVC does not emit is unmeasured.

Order of operations, and it is not negotiable:

1. Get a Linux toolchain and build with the four warning flags. Fix what it
   finds. **Expect real findings** — MSVC and GCC disagree most about narrowing
   conversions, and this codebase is full of `std::int64_t` paise flowing into
   `double` analytics.
2. Get `ctest` green there. The same suite as MSVC — see the live baseline in
   SMIT.txt section 5. The "93 tests" this line used to name is from P12-01's
   original writing and is long superseded.
3. Confirm the conservation invariant still holds bit-for-bit. It is integer
   arithmetic, so it must; if it does not, something is undefined behaviour and
   the tuning below is irrelevant.
4. *Then* tune.

---

## 2. CPU isolation

Kernel command line:

```
isolcpus=2-5 nohz_full=2-5 rcu_nocbs=2-5 intel_idle.max_cstate=0 \
  processor.max_cstate=1 idle=poll mce=ignore_ce
```

- `isolcpus` keeps the scheduler off 2–5. The engine pins itself there.
- `nohz_full` stops the 1000 Hz tick on those cores. Without it the isolation
  is half-done and you still eat a timer interrupt every millisecond.
- `rcu_nocbs` moves RCU callbacks to the housekeeping cores. Missing this is
  the classic "isolated core still stalls for 200 µs" bug.
- **C-states off.** A core in C6 takes tens of microseconds to wake. `idle=poll`
  burns power to keep it awake, which is the correct trade for a machine that
  exists to answer ticks.

Leave cores 0–1 for the kernel, IRQs, logging and the Qt UI.

**`desktop/` runs in the same process** (CLAUDE.md, "The in-process decision").
That is a live constraint here, not a footnote: the UI thread must never be
pinned to an isolated core, and a Qt repaint must never run on one. Pin the
engine threads explicitly and let Qt keep the default affinity mask over 0–1.

### The hazard specific to this codebase: the TSC

`core/time/tsc_clock.hpp` **measures** the TSC frequency. Three things break it
on a machine that has not been prepared:

- **Non-invariant TSC.** Check `constant_tsc` and `nonstop_tsc` in
  `/proc/cpuinfo`. Without both, the counter changes rate with the P-state and
  every duration the engine reports is wrong by a varying factor.
- **Cross-socket drift.** The TSC is only guaranteed synchronised within a
  socket on most hardware. Pin to one socket.
- **The measurement window itself.** A frequency calibrated during a boot storm
  is calibrated against a busy machine.

ROADMAP §3 says measurements carry error and the error must propagate. The TSC
frequency is a measurement. **Record its confidence interval at startup and
refuse to start if it is wider than the latency budget being asserted** —
otherwise the p99 in P12-03 is a number with an unknown scale factor on it.

---

## 3. Hugepages

```
default_hugepagesz=2M hugepagesz=2M hugepages=2048     # 4 GiB
```

The engine's arenas (`core/mem/arena.hpp`) are the target. CLAUDE.md rule 4
forbids heap allocation inside `ALTAIR_HOT`; hugepages address the *other* half
of the same problem, which is the TLB miss on memory that was already
allocated.

Reserve at boot, not at runtime — after a few hours of uptime the physical
memory is too fragmented to assemble 2 MiB pages, and the request silently
falls back to 4 KiB. **Verify the fallback did not happen** (`AnonHugePages` in
`/proc/meminfo`, or `/sys/kernel/mm/transparent_hugepage`). A hugepage request
that quietly degraded is indistinguishable from success at the API.

Do **not** enable transparent hugepages (`THP=always`). `khugepaged` compacting
in the background is exactly the multi-millisecond stall this whole section
exists to eliminate. `madvise` only.

---

## 4. io_uring

For the binary log writer (`core/log/`) and the replayer's file reads, not for
the broker socket — the broker socket is TLS over a WAN and its cost is the
WAN.

- `IORING_SETUP_SQPOLL` so the submitting thread never enters the kernel. Pin
  the poller to a **housekeeping** core, not an isolated one; it spins, and
  putting a spinner on the core running the strategy defeats the isolation.
- Registered buffers and registered files. Both remove per-op reference
  counting.
- The log is append-only and fire-and-forget, so completions can be reaped
  lazily. **But the kill-switch file is not**: rule 5 says the kill switch is
  hardware-simple, one file, one flag, checked every loop. Read it with a plain
  synchronous `read()`. A kill switch whose check is queued behind 400 log
  writes is not a kill switch.

Kernel 5.19+ for the useful features; 6.1 LTS is the sane target.

---

## 5. NIC tuning

Only meaningful once there is a direct line. For a broker WebSocket over the
public internet, stop after the first two items.

- **Interrupt affinity.** Move every NIC IRQ to the housekeeping cores. This one
  matters even on a WAN feed, because an IRQ landing on an isolated core during
  the open is a real stall on the real hot path.
- **Ring buffers up** (`ethtool -G rx 4096`). Absorbs a burst at 09:15 that
  would otherwise drop. Feed drops are gauge `feed.drops` in P12-03, and this is
  the knob that moves it.
- Interrupt coalescing **off** (`ethtool -C rx-usecs 0 rx-frames 1`) — trades
  CPU for latency, which is the trade this machine exists to make.
- `SO_BUSY_POLL` on the feed socket.
- Kernel bypass (DPDK, Solarflare Onload, AF_XDP) is **out of scope** and stays
  out until a DMA line exists. It is weeks of work for microseconds that do not
  reach the broker.

---

## 6. Verification — the part that is usually skipped

Tuning is not done when the flags are set; it is done when the jitter is
measured. Do all four:

1. **`ctest` green on Linux**, all 93, before any tuning. Then again after.
2. **A histogram, not a mean.** `LatencyHistogram` (P12-03) already exists and
   reports percentiles at the bucket's upper edge. Run the replayer over a full
   session and read p50 / p99 / max. P12-03's own test shows the shape of the
   failure being looked for: a mean of 84 µs and a p99 of 3.1 ms on the same
   data.
3. **Compare against the untuned box.** A tuning document without a before is a
   list of superstitions. Keep both numbers.
4. **`cyclictest` on the isolated cores for an hour.** If the max latency there
   is worse than a millisecond, one of `isolcpus` / `nohz_full` / `rcu_nocbs` /
   C-states did not take effect, and it is always one of those four.

---

## 7. What is deliberately not here

- **Containers.** An isolated, hugepage-backed, IRQ-pinned process in a
  container is possible and is a second set of failure modes on top of the
  first. Run it on the metal.
- **A second box for failover.** Warm restart (P12-02) recovers a process on the
  same machine. Cross-machine failover needs a shared, ordered view of fills
  that does not exist and is not in the 159 cards.
- **Anything about the broker's own latency**, which is not tunable and is the
  binding constraint. See §0.
