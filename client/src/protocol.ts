// client/src/protocol.ts -- the client half of the wire protocol.
//
// P11-02a. A SECOND IMPLEMENTATION, ON PURPOSE.
//
// server/ encodes these bytes in C++ and this file decodes them in
// TypeScript. The two share no code -- CLAUDE.md forbids a header crossing
// that boundary, because a client that can #include an engine header is a
// client that can be made to trade. What keeps them in agreement is
// server/tests/vectors/frames.txt, a checked-in file of conformance vectors
// that both sides test against. Reading a data file is not linking a library.
//
// EVERY 64-BIT FIELD IS A BIGINT. NONE OF THEM IS EVER A NUMBER.
//
// A JavaScript number is an IEEE-754 double, exact only to 2^53. The engine
// stamps time in nanoseconds since the Unix epoch, currently about 1.79e18 --
// between 2^60 and 2^61, where consecutive doubles are 256 ns apart. Decode a
// timestamp into a number and ticks less than 256 ns apart become the same
// instant: not mis-ordered, since rounding to nearest is monotone, but
// EQUAL -- and then whatever the grid sorts by next decides their order.
//
// So `seq`, `engineTimeNs` and `serverTimeNs` are `bigint` in the type, and
// there is no accessor that hands back a number. The 32-bit fields stay
// `number` because every uint32 is exactly representable; the hazard there is
// not magnitude but the shift idiom, and `DataView` avoids it -- see the note
// on `decodeHeader`.
//
// MEASURED. The test runs the decoder anyone would write by hand -- `a | b <<
// 8 | ...` for the 32-bit fields, two halves recombined into a number for the
// 64-bit ones -- against the same vectors, and it is wrong on 5 of 5:
//
//   js_number_loss    engine_time_ns 1788393600123456800 for ...789
//   negative_i64      engine_time_ns 18446744073709552000 for -1
//   high_bit_u32      topic -2147483647, payload_len -2
//   seq_u64_max       18446744073709552000 for ...551615
//   seq_above_2_53    9007199254740992 for ...993
//
// Read the second and third lines as what a trader would see: a LOSS rendered
// as an enormous gain, and a NEGATIVE payload length. Neither throws; both
// render. That is why test 1 asserting the two implementations agree is a
// result rather than a tautology.
//
// AND NOTHING HERE CAN ASK THE SERVER TO TRADE.
//
// `ClientMsg` has four members and none of them places an order. That is not
// a policy this file enforces at runtime; it is a vocabulary that has no word
// for the thing, which is a stronger guarantee than a check somebody can
// forget to write.

/** Wire format version this client speaks. A server on another version is
 *  refused rather than reinterpreted against these field offsets. */
export const WIRE_VERSION = 1;

/** 'ALTR'. Present so a mis-framed stream fails at byte 0 rather than 40
 *  bytes later with a plausible-looking payload. */
export const FRAME_MAGIC = 0x414c5452;

export const FRAME_HEADER_BYTES = 48;

// Plain frozen objects rather than `enum`: TypeScript enums are not erasable
// syntax, and Node runs these files by stripping types rather than compiling
// them. An enum here would not run at all.

export const FrameKind = {
  Unspecified: 0,
  /** A complete baseline. Every delta is meaningless without one. */
  Snapshot: 1,
  /** A change against the last snapshot plus every delta since. */
  Delta: 2,
  /** Proves the link is alive. Deliberately does not advance `seq`. */
  Heartbeat: 3,
  /** The server saying the client fell behind, and by how much. */
  Gap: 4,
  /** Orderly close, with a reason. */
  Bye: 5,
} as const;
export type FrameKind = (typeof FrameKind)[keyof typeof FrameKind];

export const Channel = {
  Unspecified: 0,
  /** Coalescible: quotes, greeks, book levels, marks. Only the latest value
   *  matters, because the next frame restates the field. */
  State: 1,
  /** Never coalesced: fills, flags, kill-switch trips, P&L increments, audit
   *  rows. Each is a separate fact and nothing later restates it. */
  Event: 2,
} as const;
export type Channel = (typeof Channel)[keyof typeof Channel];

/** Everything the client is allowed to say. There is no order-placing member,
 *  and `KillSwitch` is one-way: it can only flatten, never open. */
export const ClientMsg = {
  Unspecified: 0,
  Subscribe: 1,
  Unsubscribe: 2,
  SnapshotRequest: 3,
  KillSwitch: 4,
} as const;
export type ClientMsg = (typeof ClientMsg)[keyof typeof ClientMsg];

export const ProtocolError = {
  ShortBuffer: 'ShortBuffer',
  BadMagic: 'BadMagic',
  VersionMismatch: 'VersionMismatch',
  Unspecified: 'Unspecified',
  PayloadTooLarge: 'PayloadTooLarge',
} as const;
export type ProtocolError = (typeof ProtocolError)[keyof typeof ProtocolError];

export interface FrameHeader {
  readonly version: number;
  readonly kind: FrameKind;
  readonly channel: Channel;
  /** Per channel per topic. bigint: sequence numbers outlive 2^53. */
  readonly seq: bigint;
  readonly topic: number;
  readonly payloadLen: number;
  /** The ENGINE'S tick time, nanoseconds since the Unix epoch. Read off the
   *  tick, never a wall clock -- a replay renders at the timestamps the ticks
   *  actually carried. */
  readonly engineTimeNs: bigint;
  /** When the server sent it. Differs from `engineTimeNs` by hours during a
   *  replay, which is what makes a replay visibly a replay. */
  readonly serverTimeNs: bigint;
}

/** Mirrors std::expected on the C++ side: a failure is a value, not a throw.
 *  A decode failure is an ordinary event on a socket and should not unwind
 *  the render loop. */
export type DecodeResult =
  | { readonly ok: true; readonly header: FrameHeader }
  | { readonly ok: false; readonly error: ProtocolError };

/**
 * Decode a 48-byte frame header.
 *
 * Uses `DataView`, not hand-rolled shifts, and that is a correctness choice
 * rather than a stylistic one. JavaScript's bitwise operators coerce to
 * 32-bit SIGNED integers, so the idiomatic
 *
 *     b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)
 *
 * returns a NEGATIVE number whenever the top byte is >= 0x80 -- which for
 * `payloadLen` means a negative length, and for `topic` means a subscription
 * that matches nothing. The `high_bit_u32` conformance vector exists to catch
 * exactly this, and the test measures that the naive decoder fails it.
 */
export function decodeHeader(bytes: Uint8Array): DecodeResult {
  if (bytes.length < FRAME_HEADER_BYTES) {
    return { ok: false, error: ProtocolError.ShortBuffer };
  }
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  if (dv.getUint32(0, true) !== FRAME_MAGIC) {
    return { ok: false, error: ProtocolError.BadMagic };
  }
  const version = dv.getUint16(4, true);
  if (version !== WIRE_VERSION) {
    return { ok: false, error: ProtocolError.VersionMismatch };
  }
  const kind = dv.getUint8(6) as FrameKind;
  const channel = dv.getUint8(7) as Channel;
  if (kind === FrameKind.Unspecified || channel === Channel.Unspecified) {
    return { ok: false, error: ProtocolError.Unspecified };
  }
  return {
    ok: true,
    header: {
      version,
      kind,
      channel,
      seq: dv.getBigUint64(8, true),
      topic: dv.getUint32(16, true),
      payloadLen: dv.getUint32(20, true),
      engineTimeNs: dv.getBigInt64(24, true),
      serverTimeNs: dv.getBigInt64(32, true),
    },
  };
}

/** Snapshot and Delta advance the sequence. Nothing else does -- so a
 *  heartbeat can never be mistaken for a lost delta. */
export function advancesSeq(kind: FrameKind): boolean {
  return kind === FrameKind.Snapshot || kind === FrameKind.Delta;
}

export const SeqVerdict = {
  Unspecified: 'Unspecified',
  InOrder: 'InOrder',
  /** Already seen. Harmless: discard it. */
  Duplicate: 'Duplicate',
  /** Frames were lost. Not harmless: every delta after this applies to a
   *  baseline this client does not have. */
  Gap: 'Gap',
} as const;
export type SeqVerdict = (typeof SeqVerdict)[keyof typeof SeqVerdict];

/**
 * One channel of one topic. The same state machine as `altair::SeqTracker`
 * in server/protocol.hpp, reimplemented rather than shared.
 *
 * The load-bearing part is `canApply()`. A delta applied to a baseline the
 * client does not have is not a smaller error than a missing frame -- it is a
 * wrong number rendered confidently, and a dashboard that renders nothing is
 * strictly better than one that renders a position you do not hold.
 */
export class SeqTracker {
  #last = 0n;
  #primed = false;
  #needsSnapshot = true;
  #framesMissed = 0n;

  get framesMissed(): bigint {
    return this.#framesMissed;
  }
  get last(): bigint {
    return this.#last;
  }

  accept(header: FrameHeader): SeqVerdict {
    if (!advancesSeq(header.kind)) return SeqVerdict.InOrder;

    if (header.kind === FrameKind.Snapshot) {
      this.#last = header.seq;
      this.#primed = true;
      this.#needsSnapshot = false;
      return SeqVerdict.InOrder;
    }
    if (!this.#primed) {
      this.#framesMissed += 1n;
      this.#needsSnapshot = true;
      return SeqVerdict.Gap;
    }
    if (header.seq <= this.#last) return SeqVerdict.Duplicate;
    if (header.seq === this.#last + 1n) {
      this.#last = header.seq;
      return SeqVerdict.InOrder;
    }
    this.#framesMissed += header.seq - this.#last - 1n;
    this.#last = header.seq;
    this.#needsSnapshot = true;
    return SeqVerdict.Gap;
  }

  canApply(): boolean {
    return this.#primed && !this.#needsSnapshot;
  }
}

// ---------------------------------------------------------------------------
// Money
// ---------------------------------------------------------------------------

/** Integer paise. CLAUDE.md rule 3: all money is integer paise, and doubles
 *  appear only in analytics, never in the ledger. The rule does not stop at
 *  the socket. */
export type Paise = bigint;

/**
 * Format paise as rupees, exactly, with Indian digit grouping.
 *
 * Takes a bigint and never converts to a number, so the question of when
 * float formatting would lose a paisa never comes up. This is a
 * do-not-have-the-problem design rather than a claim that any particular
 * magnitude breaks: P12-04 reconciles against a broker contract note to the
 * paisa, and a display that disagrees with the ledger by a paisa costs an
 * afternoon to chase down.
 */
export function formatPaise(paise: Paise): string {
  const negative = paise < 0n;
  const abs = negative ? -paise : paise;
  const rupees = abs / 100n;
  const fraction = abs % 100n;

  const digits = rupees.toString();
  // Indian grouping: the last three digits, then pairs. 12345678 reads as
  // 1,23,45,678 -- one crore twenty-three lakh.
  let grouped: string;
  if (digits.length <= 3) {
    grouped = digits;
  } else {
    const head = digits.slice(0, -3);
    const tail = digits.slice(-3);
    const parts: string[] = [];
    let rest = head;
    while (rest.length > 2) {
      parts.unshift(rest.slice(-2));
      rest = rest.slice(0, -2);
    }
    if (rest.length > 0) parts.unshift(rest);
    grouped = `${parts.join(',')},${tail}`;
  }
  const paisa = fraction.toString().padStart(2, '0');
  return `${negative ? '-' : ''}${grouped}.${paisa}`;
}
