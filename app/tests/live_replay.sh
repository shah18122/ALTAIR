#!/usr/bin/env bash
# app/tests/live_replay.sh -- a recorded session, replayed, decides byte for byte the same.
#
#   live_replay.sh <altair_price_service> <altair_live_engine> <source dir> <work dir>
#
# A simulated morning (09:15 to 09:25, the 09:20 strangles included) runs
# through the engine with --record. The tape is then replayed into a fresh
# directory, twice; the journal, the fills, the decisions and the open
# positions must come out identical to the recorded session's. A tape whose
# bundle no longer matches the inputs must be refused (exit 5) unless
# --force-replay. Needs dataset/ (the SIM day's minute bars and the history).
set -euo pipefail

ps_bin=$1
engine=$2
src=$3
work=$4
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

# The engine first, so it is listening for the whole morning; then the feed.
"$engine" --root "$work/root" --port "$port" --date "$day" --unverified-costs --record --until 09:25 --seconds 240 \
    > "$work/record.log" 2>&1 &
engine_pid=$!
for _ in $(seq 1 600); do
    grep -q "^engine on" "$work/record.log" && break
    kill -0 "$engine_pid" 2>/dev/null || { echo "the engine stopped before the session"; cat "$work/record.log"; exit 1; }
    sleep 0.1
done
"$ps_bin" "${feed[@]}" --speed 10 --seconds 200 > "$work/feed.log" 2>&1 &
ps_pid=$!
wait "$engine_pid" || { echo "the recorded session failed"; cat "$work/record.log"; exit 1; }
engine_pid=
kill "$ps_pid" 2>/dev/null || true
wait "$ps_pid" 2>/dev/null || true
ps_pid=
tail -5 "$work/record.log"

tape=$(ls "$work"/root/data/live/tapes/*.tape)
paper="$work/root/data/live/paper"
grep -q '"Strangle 80% NIFTY"' "$paper/decisions.csv" || { echo "no 09:20 decision was recorded"; cat "$paper/decisions.csv"; exit 1; }
fills=0
[ -f "$paper/journal.csv" ] && fills=$(($(wc -l < "$paper/journal.csv") - 1))
echo "recorded: $(($(wc -l < "$paper/decisions.csv") - 1)) decision(s), $fills fill(s)"
# With every trade frame seen (a build fast enough for a 10x morning), the
# 09:20 strangle sells -- so the replay is checked on fills too. A slow build
# (sanitizers) can fall behind and pause on a gap: still a session to replay.
if grep -q "(0 trade frames missed)" "$work/record.log" && [ "$fills" -lt 2 ]; then
    echo "every frame seen, yet no strangle filled"; cat "$paper/decisions.csv"; exit 1
fi

same() {
    local out=$1 f
    for f in journal.csv fills.csv trades.csv decisions.csv open_positions.csv; do
        if [ -e "$paper/$f" ] || [ -e "$out/paper/$f" ]; then
            cmp "$paper/$f" "$out/paper/$f" || { echo "REPLAY DIFFERS: $f"; diff "$paper/$f" "$out/paper/$f" | head -20; exit 1; }
        fi
    done
}

# Replayed twice into the same directory: the second clears the first.
for round in 1 2; do
    "$engine" --root "$work/root" --replay "$tape" --replay-out "$work/replay" > "$work/replay$round.log" 2>&1 \
        || { echo "replay $round failed"; cat "$work/replay$round.log"; exit 1; }
    grep -q "the inputs are the recorded session's" "$work/replay$round.log" || { echo "replay $round: bundle"; cat "$work/replay$round.log"; exit 1; }
    same "$work/replay"
done
echo "replayed twice: journal, fills, decisions and positions identical"

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
echo "PASS"
