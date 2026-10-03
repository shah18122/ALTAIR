#!/usr/bin/env bash
# app/tests/live_replay.sh -- a recorded session, replayed, decides byte for byte the same.
#
#   live_replay.sh <altair_price_service> <altair_live_engine> <source dir> <work dir>
#                  [<altair_exec_study> <altair_paper_report> <altair_readiness>]
#
# A simulated morning (09:15 to 09:25, the 09:20 strangles included) runs
# through the engine with --record, as two sessions: the engine stops at 09:21
# and restarts from its journal. Each tape is replayed into a fresh directory,
# twice; together the replays must make the journal, fills, decisions, margin
# samples and open positions the two sessions made, byte for byte. A tape whose
# bundle no longer matches the inputs must be refused (exit 5) unless
# --force-replay. Each replay also checks itself against the recorded ledger
# (data/live/replay_checks/). Given the three tools, the execution study, the
# paper report and the readiness gates then run on what was recorded.
# Needs dataset/ (the SIM day's minute bars and the history).
set -euo pipefail

ps_bin=$1
engine=$2
src=$3
work=$4
exec_study=${5:-}
paper_report=${6:-}
readiness=${7:-}
day=2026-09-24
port=$((20000 + $$ % 10000))

rm -rf "$work"
mkdir -p "$work/root/data/live" "$work/feed"
ln -s "$src/dataset" "$work/root/dataset"
ln -s "$src/config" "$work/root/config"

ps_pid=
engine_pid=
cleanup() {
    [ -n "$ps_pid" ] && kill "$ps_pid" 2>/dev/null || true
    [ -n "$engine_pid" ] && kill "$engine_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    # The tape is ~100 MB; keep the logs for a failure, not the bytes.
    rm -f "$work"/root/data/live/tapes/*.tape "$work"/bad.tape
}
trap cleanup EXIT

feed=(--sim --date "$day" --from 09:15 --strikes 10 --depth-strikes 1 --no-stocks --live-dir "$work/feed" --port "$port")

# The universe first (the engine reads it at start), from the same options.
"$ps_bin" "${feed[@]}" --seconds 1 > "$work/universe.log" 2>&1 || true
[ -s "$work/feed/universe.csv" ] || { echo "the price service wrote no universe"; cat "$work/universe.log"; exit 1; }
cp "$work/feed/universe.csv" "$work/root/data/live/universe.csv"

# Session A listens before the feed starts and stops at 09:21; session B is a
# restart mid-morning -- it resumes A's positions from the journal -- and runs
# on to 09:25 while the feed carries on.
record() {   # record <log> <until>
    "$engine" --root "$work/root" --port "$port" --date "$day" --unverified-costs --record --until "$2" --seconds 240 > "$1" 2>&1
}
record "$work/recordA.log" 09:21 &
engine_pid=$!
for _ in $(seq 1 600); do
    grep -q "^engine on" "$work/recordA.log" && break
    kill -0 "$engine_pid" 2>/dev/null || { echo "the engine stopped before the session"; cat "$work/recordA.log"; exit 1; }
    sleep 0.1
done
"$ps_bin" "${feed[@]}" --speed 10 --seconds 200 > "$work/feed.log" 2>&1 &
ps_pid=$!
wait "$engine_pid" || { echo "session A failed"; cat "$work/recordA.log"; exit 1; }
engine_pid=
record "$work/recordB.log" 09:25 || { echo "session B (the restart) failed"; cat "$work/recordB.log"; exit 1; }
kill "$ps_pid" 2>/dev/null || true
wait "$ps_pid" 2>/dev/null || true
ps_pid=
tail -3 "$work/recordA.log"
grep "^resuming" "$work/recordB.log"
tail -3 "$work/recordB.log"

tapes=("$work"/root/data/live/tapes/*.tape)
[ "${#tapes[@]}" -eq 2 ] || { echo "expected two tapes, found ${#tapes[@]}"; exit 1; }
paper="$work/root/data/live/paper"
grep -q '"Strangle 80% NIFTY"' "$paper/decisions.csv" || { echo "no 09:20 decision was recorded"; cat "$paper/decisions.csv"; exit 1; }
fills=0
[ -f "$paper/journal.csv" ] && fills=$(($(wc -l < "$paper/journal.csv") - 1))
echo "recorded: $(($(wc -l < "$paper/decisions.csv") - 1)) decision(s), $fills fill(s)"
# With every trade frame seen (a build fast enough for a 10x morning), the
# 09:20 strangle sells -- so the replay is checked on fills, and the restart
# on resuming them. A slow build (sanitizers) can fall behind and pause on a
# gap: still two sessions to replay.
if grep -q "(0 trade frames missed)" "$work/recordA.log" && [ "$fills" -lt 2 ]; then
    echo "every frame seen, yet no strangle filled"; cat "$paper/decisions.csv"; exit 1
fi
if [ "$fills" -ge 2 ] && ! grep -q "^resuming from paper/journal.csv: [1-9]" "$work/recordB.log"; then
    echo "the restart did not resume the morning's positions"; cat "$work/recordB.log"; exit 1
fi

# The two replays together must make exactly the files the two sessions made:
# B's start holds A's journal, so the journal and the positions are B's
# replay's; every other view is A's rows followed by B's.
same() {   # same <replay A dir> <replay B dir>
    local f
    for f in journal.csv open_positions.csv; do
        cmp "$paper/$f" "$2/paper/$f" || { echo "REPLAY DIFFERS: $f"; diff "$paper/$f" "$2/paper/$f" | head -20; exit 1; }
    done
    for f in fills.csv trades.csv decisions.csv margin.csv marks.csv; do
        [ -e "$paper/$f" ] || [ -e "$1/paper/$f" ] || [ -e "$2/paper/$f" ] || continue
        {
            if [ -e "$1/paper/$f" ]; then cat "$1/paper/$f"; fi
            if [ -e "$2/paper/$f" ]; then
                if [ -e "$1/paper/$f" ]; then tail -n +2 "$2/paper/$f"; else cat "$2/paper/$f"; fi
            fi
        } > "$work/combined.csv"
        cmp "$paper/$f" "$work/combined.csv" || { echo "REPLAY DIFFERS: $f"; diff "$paper/$f" "$work/combined.csv" | head -20; exit 1; }
    done
}

# Replayed twice into the same directories: the second clears the first.
for round in 1 2; do
    for k in 0 1; do
        "$engine" --root "$work/root" --replay "${tapes[$k]}" --replay-out "$work/replay$k" > "$work/replay$k-$round.log" 2>&1 \
            || { echo "replay $k ($round) failed"; cat "$work/replay$k-$round.log"; exit 1; }
        grep -q "the inputs are the recorded session's" "$work/replay$k-$round.log" \
            || { echo "replay $k ($round): bundle"; cat "$work/replay$k-$round.log"; exit 1; }
    done
    same "$work/replay0" "$work/replay1"
done
echo "both sessions replayed twice, restart included: journal, fills, decisions, margin and positions identical"
for k in 0 1; do
    grep -q "IDENTICAL to the recorded session" "$work/replay$k-2.log" || { echo "replay $k did not verify itself"; cat "$work/replay$k-2.log"; exit 1; }
done
checks=("$work"/root/data/live/replay_checks/*.json)
[ "${#checks[@]}" -eq 2 ] && grep -q '"identical": true' "${checks[0]}" && grep -q '"identical": true' "${checks[1]}" \
    || { echo "the replay verdicts are missing or not identical"; exit 1; }
tape=${tapes[0]}

# A directory that is not a replay's is never cleared.
mkdir -p "$work/keep" && echo keep > "$work/keep/file.txt"
if "$engine" --root "$work/root" --replay "$tape" --replay-out "$work/keep" > "$work/keep.log" 2>&1; then
    echo "a replay cleared a directory it did not make"; exit 1
fi
[ -f "$work/keep/file.txt" ] || { echo "a replay removed a file it did not write"; exit 1; }

# A tape whose bundle differs from the inputs: refused, then run with --force-replay.
if command -v python3 > /dev/null; then
    python3 - "$tape" "$work/bad.tape" <<'PY'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    magic = f.read(14)
    kind, wall, n = struct.unpack("<BqI", f.read(13))
    body = f.read(n)
assert magic == b"ALTAIR-TAPE 1\n" and kind == 8
sections, at = [], 0
while at < len(body):
    a = body.index(b"\n", at); b = body.index(b"\n", a + 1); k = int(body[a + 1:b])
    sections.append([body[at:a], body[b + 1:b + 1 + k]]); at = b + 1 + k
for s in sections:
    if s[0] == b"bundle_id": s[1] = b"0" * 16
packed = b"".join(name + b"\n" + str(len(v)).encode() + b"\n" + v for name, v in sections)
with open(dst, "wb") as f:
    f.write(magic + struct.pack("<BqI", 8, wall, len(packed)) + packed)
PY
    set +e
    "$engine" --root "$work/root" --replay "$work/bad.tape" --replay-out "$work/bad" > "$work/bad.log" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 5 ] || { echo "a mismatched bundle was not refused (exit $rc)"; cat "$work/bad.log"; exit 1; }
    "$engine" --root "$work/root" --replay "$work/bad.tape" --replay-out "$work/bad" --force-replay > "$work/bad.log" 2>&1 \
        || { echo "--force-replay failed"; cat "$work/bad.log"; exit 1; }
    echo "a mismatched bundle is refused, and runs with --force-replay"
fi
if [ -n "$exec_study" ]; then
    "$exec_study" --root "$work/root" --tape "${tapes[0]}" --tape "${tapes[1]}" --out "$work/study" > "$work/study.log" 2>&1 \
        || { echo "the execution study failed"; cat "$work/study.log"; exit 1; }
    labelled=$(($(wc -l < "$work/study/exec_fills.csv") - 1))
    [ "$labelled" -eq "$fills" ] || { echo "the study labelled $labelled of $fills fills"; cat "$work/study.log"; exit 1; }
    echo "execution study: $labelled fill(s) labelled"
    "$paper_report" --root "$work/root" --out "$work/report" > "$work/report.log" 2>&1 \
        || { echo "the paper report failed"; cat "$work/report.log"; exit 1; }
    [ -s "$work/report/summary.json" ] && [ -s "$work/report/daily.csv" ] || { echo "the report wrote nothing"; exit 1; }
    echo "paper report: $(head -1 "$work/report.log")"
    set +e
    "$readiness" --root "$work/root" > "$work/readiness.log" 2>&1
    rc=$?
    set -e
    # SIM sessions are not evidence: the verdict must be NOT READY, with no LIVE session counted.
    [ "$rc" -eq 1 ] && grep -q "^NOT READY -- 0 LIVE session" "$work/readiness.log" \
        || { echo "readiness counted SIM sessions or did not run"; cat "$work/readiness.log"; exit 1; }
    echo "readiness: NOT READY on SIM sessions, as it must be"
fi
echo "PASS"
