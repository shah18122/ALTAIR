// P11-02a acceptance tests.
//
// Test 1 is the contract: this decoder agrees with the C++ encoder on every
// conformance vector, byte for byte and field for field.
//
// Test 2 asserts THE HAZARD EXISTS before claiming it is handled -- the naive
// decoder anyone would write by hand is run against the same vectors and the
// specific ones it gets wrong are named. Without this, test 1 only proves
// that two things agree, not that agreeing was hard.
//
// Test 3: the sequence tracker behaves as server/protocol.hpp's does, since
// the two are separate implementations of one state machine.
//
// Test 4: money formats exactly, including past 2^53 where a number cannot.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import {
  Channel,
  FrameKind,
  FRAME_HEADER_BYTES,
  ProtocolError,
  SeqTracker,
  SeqVerdict,
  decodeHeader,
  formatPaise,
} from '../src/protocol.ts';
import type { FrameHeader } from '../src/protocol.ts';

// The ONLY thing this directory reads out of server/. A data file, not a
// header: nothing here links C++, and this client still cannot place an
// order.
const VECTOR_FILE = join(
  import.meta.dirname,
  '..',
  '..',
  'server',
  'tests',
  'vectors',
  'frames.txt',
);

interface Vector {
  readonly name: string;
  readonly bytes: Uint8Array;
  readonly accept: boolean;
  readonly fields: ReadonlyMap<string, string>;
}

function loadVectors(): Vector[] {
  const text = readFileSync(VECTOR_FILE, 'utf8');
  const out: Vector[] = [];
  for (const rawLine of text.split('\n')) {
    const line = rawLine.trim();
    if (line.length === 0 || line.startsWith('#')) continue;
    const parts = line.split('|');
    if (parts.length < 3) continue;
    const name = parts[0] ?? '';
    const hex = parts[1] ?? '';
    const expect = parts[2] ?? '';

    const bytes = new Uint8Array(hex.length / 2);
    for (let i = 0; i < bytes.length; i++) {
      bytes[i] = Number.parseInt(hex.slice(2 * i, 2 * i + 2), 16);
    }
    const fields = new Map<string, string>();
    for (const kv of expect.split(';')) {
      const eq = kv.indexOf('=');
      if (eq > 0) fields.set(kv.slice(0, eq), kv.slice(eq + 1));
    }
    out.push({
      name,
      bytes,
      accept: fields.get('accept') === '1',
      fields,
    });
  }
  return out;
}

const vectors = loadVectors();

// ---------------------------------------------------------------------------
// The decoder nobody should write, written out so its failures can be counted.
// ---------------------------------------------------------------------------

/** `a | b << 8 | c << 16 | d << 24` -- the idiom. JavaScript's bitwise
 *  operators are 32-bit SIGNED, so this goes negative the moment the top byte
 *  reaches 0x80. */
function naiveU32(b: Uint8Array, o: number): number {
  return (
    (b[o] ?? 0) |
    ((b[o + 1] ?? 0) << 8) |
    ((b[o + 2] ?? 0) << 16) |
    ((b[o + 3] ?? 0) << 24)
  );
}

/** Two 32-bit halves recombined as a `number`. Exact only below 2^53, and it
 *  has no idea about two's complement. */
function naiveI64AsNumber(b: Uint8Array, o: number): number {
  const lo = naiveU32(b, o) >>> 0;
  const hi = naiveU32(b, o + 4) >>> 0;
  return hi * 4294967296 + lo;
}

test('[1] every conformance vector decodes exactly as the C++ encoder wrote it', () => {
  assert.ok(vectors.length >= 8, 'the vector file was found and parsed');

  let accepted = 0;
  let refused = 0;
  for (const v of vectors) {
    assert.equal(
      v.bytes.length,
      FRAME_HEADER_BYTES,
      `${v.name}: a frame header is 48 bytes`,
    );
    const r = decodeHeader(v.bytes);

    if (!v.accept) {
      assert.equal(r.ok, false, `${v.name}: must be refused`);
      if (!r.ok) {
        assert.equal(
          r.error,
          v.fields.get('reason'),
          `${v.name}: refused for the reason the encoder recorded, not merely refused`,
        );
      }
      refused++;
      continue;
    }

    assert.equal(r.ok, true, `${v.name}: must be accepted`);
    if (!r.ok) continue;
    const h: FrameHeader = r.header;

    // Compared as decimal STRINGS, so the comparison itself never routes a
    // 64-bit value through a number.
    assert.equal(h.seq.toString(), v.fields.get('seq'), `${v.name}: seq`);
    assert.equal(
      h.engineTimeNs.toString(),
      v.fields.get('engine_time_ns'),
      `${v.name}: engine time, to the nanosecond`,
    );
    assert.equal(
      h.serverTimeNs.toString(),
      v.fields.get('server_time_ns'),
      `${v.name}: server time, to the nanosecond`,
    );
    assert.equal(String(h.topic), v.fields.get('topic'), `${v.name}: topic`);
    assert.equal(
      String(h.payloadLen),
      v.fields.get('payload_len'),
      `${v.name}: payload length`,
    );
    assert.equal(String(h.kind), v.fields.get('kind'), `${v.name}: kind`);
    assert.equal(
      String(h.channel),
      v.fields.get('channel'),
      `${v.name}: channel`,
    );
    accepted++;
  }
  console.log(
    `    ${accepted} vectors decoded field-exact, ${refused} refused for the recorded reason`,
  );
});

test('[2] the naive decoder gets specific vectors wrong -- the hazard is real', () => {
  const wrong: string[] = [];

  for (const v of vectors) {
    if (!v.accept) continue;
    const naiveTopic = naiveU32(v.bytes, 16);
    const naiveLen = naiveU32(v.bytes, 20);
    const naiveEngine = naiveI64AsNumber(v.bytes, 24);
    const naiveSeq = naiveI64AsNumber(v.bytes, 8);

    const okTopic = String(naiveTopic) === v.fields.get('topic');
    const okLen = String(naiveLen) === v.fields.get('payload_len');
    const okEngine = String(naiveEngine) === v.fields.get('engine_time_ns');
    const okSeq = String(naiveSeq) === v.fields.get('seq');

    if (!okTopic || !okLen || !okEngine || !okSeq) {
      wrong.push(v.name);
      const detail: string[] = [];
      if (!okTopic) detail.push(`topic ${naiveTopic} != ${v.fields.get('topic')}`);
      if (!okLen) detail.push(`payload_len ${naiveLen} != ${v.fields.get('payload_len')}`);
      if (!okEngine) detail.push(`engine_time_ns ${naiveEngine} != ${v.fields.get('engine_time_ns')}`);
      if (!okSeq) detail.push(`seq ${naiveSeq} != ${v.fields.get('seq')}`);
      console.log(`    naive decoder, ${v.name}: ${detail.join('; ')}`);
    }
  }

  console.log(`    the naive decoder is wrong on ${wrong.length} of ${vectors.filter((v) => v.accept).length} accepted vectors`);

  assert.ok(
    wrong.includes('high_bit_u32'),
    'the 32-bit signed shift idiom returns a negative topic and a negative payload length once the top byte reaches 0x80 -- a negative length is the classic hand-written-parser bug',
  );
  assert.ok(
    wrong.includes('js_number_loss'),
    'and a nanosecond timestamp recombined into a number comes back changed, because 1.79e18 is past 2^53 where doubles are 256 ns apart',
  );
  assert.ok(
    wrong.includes('negative_i64'),
    'and a negative int64 -- a losing session -- decodes as a very large positive number, which is a loss displayed as a gain',
  );
  assert.ok(
    wrong.length >= 3,
    'so passing test 1 is a real result rather than two implementations agreeing by construction',
  );
});

test('[3] the sequence tracker matches the C++ state machine', () => {
  const t = new SeqTracker();
  const frame = (kind: FrameKind, seq: bigint): FrameHeader => ({
    version: 1,
    kind,
    channel: Channel.State,
    seq,
    topic: 1,
    payloadLen: 0,
    engineTimeNs: 0n,
    serverTimeNs: 0n,
  });

  assert.equal(t.canApply(), false, 'no baseline yet, so no delta may be applied');
  assert.equal(
    t.accept(frame(FrameKind.Delta, 1n)),
    SeqVerdict.Gap,
    'a delta before any snapshot is a gap, not something to apply to nothing',
  );

  assert.equal(t.accept(frame(FrameKind.Snapshot, 100n)), SeqVerdict.InOrder);
  assert.equal(t.canApply(), true, 'a snapshot establishes the baseline');

  assert.equal(t.accept(frame(FrameKind.Delta, 101n)), SeqVerdict.InOrder);
  assert.equal(
    t.accept(frame(FrameKind.Delta, 101n)),
    SeqVerdict.Duplicate,
    'a repeat is discarded, which is harmless',
  );

  const before = t.framesMissed;
  assert.equal(t.accept(frame(FrameKind.Delta, 105n)), SeqVerdict.Gap);
  assert.equal(
    t.framesMissed - before,
    3n,
    'losing 102-104 is counted, not merely flagged',
  );
  assert.equal(
    t.canApply(),
    false,
    'and no delta may be applied until a fresh snapshot: a wrong number rendered confidently is worse than rendering nothing',
  );

  const last = t.last;
  t.accept(frame(FrameKind.Heartbeat, 0n));
  assert.equal(t.last, last, 'a heartbeat does not advance the sequence');

  t.accept(frame(FrameKind.Snapshot, 106n));
  assert.equal(t.canApply(), true, 'only a snapshot clears the block');

  // The sequence numbers the C++ vectors carry are past 2^53. A tracker
  // holding them as numbers would stop distinguishing adjacent frames here.
  const big = new SeqTracker();
  big.accept(frame(FrameKind.Snapshot, 9007199254740992n));
  assert.equal(
    big.accept(frame(FrameKind.Delta, 9007199254740993n)),
    SeqVerdict.InOrder,
    'and past 2^53 the tracker still tells consecutive frames apart -- as numbers those two are the same value, so gap detection would quietly stop working on a long-lived stream',
  );
});

test('[4] money formats exactly, in paise, past where a number gives up', () => {
  assert.equal(formatPaise(0n), '0.00');
  assert.equal(formatPaise(5n), '0.05');
  assert.equal(formatPaise(100n), '1.00');
  assert.equal(formatPaise(-1234567n), '-12,345.67');
  assert.equal(
    formatPaise(1234567890n),
    '1,23,45,678.90',
    'Indian grouping: last three digits, then pairs',
  );

  // Past Number.MAX_SAFE_INTEGER. A formatter that converted to a number
  // first would round here; this one never converts.
  const beyond = 9007199254740993n; // 2^53 + 1 paise
  assert.equal(
    formatPaise(beyond),
    '9,00,71,99,25,47,409.93',
    'a value one paisa past 2^53 formats to its exact paisa',
  );
  assert.notEqual(
    formatPaise(beyond),
    formatPaise(beyond - 1n),
    'and it is still distinguishable from its neighbour, which as a number it would not be',
  );
});
