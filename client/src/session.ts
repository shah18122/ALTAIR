// client/src/session.ts -- lock state, and what a tab is allowed to stop
// listening to.
//
// P11-02b.
//
// LOCKING MUST DISCARD THE DATA, NOT COVER IT.
//
// This is the card. The obvious PIN lock draws an overlay over the dashboard
// and asks for four digits. That protects nothing: the positions are still in
// memory, still in the DOM, still arriving over an open socket, and anyone who
// can open devtools -- or scroll the overlay aside on a stale render -- reads
// the whole book. It is a screensaver wearing a padlock icon.
//
// So `lock()` returns a teardown that is not optional: unsubscribe every
// topic, discard the decoded state, close the socket. Unlocking reconnects and
// requests a fresh snapshot, which is exactly the machinery P11-01 already
// built for a detected gap -- a locked session is deliberately indistinguisha-
// ble from a session that fell behind, because in both cases the client holds
// no baseline it is entitled to trust.
//
// AND THE PIN IS A SHOULDER-SURFING GUARD, NOT A SECRET.
//
// Say the true thing rather than the flattering one. A six-digit PIN has a
// keyspace of one million. Measured on this machine:
//
//     plain SHA-256              0.0029 ms/attempt   whole keyspace in 2.9 s
//     PBKDF2-SHA256, 600k iters  292 ms/attempt      whole keyspace in 81 hours
//
// Both numbers are worth reading carefully. The first is one Node thread with
// per-call allocation overhead, so it is an UPPER bound on what the attempt
// costs an attacker -- dedicated hardware is orders of magnitude faster, and
// the honest summary of a fast-hashed six-digit PIN is that it is not a
// secret at all.
//
// The second is why the KDF is here. But read it honestly too: it buys HOURS,
// against someone who already has the machine. It is not what makes locking
// safe. What makes locking safe is that there is nothing behind the lock to
// take -- see above. The PIN's actual job is to stop the person standing
// behind you at a desk, and it is good at that.
//
// The verifier is therefore deliberately unforgiving about attempts: after
// `maxAttempts` the session does not merely stay locked, it requires a full
// reconnect, so an automated guesser cannot sit in a loop against a warm
// client.
//
// A HIDDEN TAB STOPS WATCHING PRICES. IT DOES NOT STOP WATCHING FILLS.
//
// The third thing, and it falls straight out of P11-01's two channels. A tab
// the user cannot see does not need quotes: `Channel.State` is coalescible and
// re-snapshottable, so dropping it costs a snapshot on return and saves the
// bandwidth and the render.
//
// `Channel.Event` is not droppable at any time. A fill, a flag or a kill-
// switch trip is a separate fact with nothing later to restate it, and "I was
// looking at another tab" is not a reason to have missed one. So hidden tabs
// unsubscribe from State and stay subscribed to Event, and that asymmetry is
// the whole reason the protocol has two channels rather than one.

import { Channel } from './protocol.ts';

export const LockState = {
  Unspecified: 0,
  /** No data held, no socket open. The only state that is safe to leave a
   *  machine in. */
  Locked: 1,
  Unlocked: 2,
  /** Too many failed attempts. Stays locked AND refuses further attempts
   *  until the session is torn down and rebuilt. */
  Barred: 3,
} as const;
export type LockState = (typeof LockState)[keyof typeof LockState];

/** What locking obliges the caller to do. Every field is true, and the type
 *  exists so the obligations are named in one place rather than remembered at
 *  each call site. */
export interface Teardown {
  readonly unsubscribeAll: true;
  readonly discardDecodedState: true;
  readonly closeSocket: true;
}

export interface PinPolicy {
  /** PBKDF2 iterations. Not a tuning knob to lower "if it feels slow": the
   *  measured cost per attempt IS the protection this offers. */
  readonly iterations: number;
  /** Attempts before the session is barred and must be rebuilt. */
  readonly maxAttempts: number;
  /** Minimum digits. Six is the floor, and even six is only a shoulder-
   *  surfing guard. */
  readonly minLength: number;
}

export const DEFAULT_PIN_POLICY: PinPolicy = {
  iterations: 600_000,
  maxAttempts: 5,
  minLength: 6,
};

/** A stored PIN verifier. The PIN itself is never stored, and neither is
 *  anything from which it can be recovered cheaply. */
export interface PinVerifier {
  readonly salt: Uint8Array;
  readonly hash: Uint8Array;
  readonly iterations: number;
}

const KEY_BITS = 256;

async function pbkdf2(
  pin: string,
  salt: Uint8Array,
  iterations: number,
): Promise<Uint8Array> {
  const enc = new TextEncoder();
  const key = await crypto.subtle.importKey(
    'raw',
    enc.encode(pin),
    'PBKDF2',
    false,
    ['deriveBits'],
  );
  const bits = await crypto.subtle.deriveBits(
    {
      name: 'PBKDF2',
      // A fresh copy: deriveBits wants a plain BufferSource and callers
      // should not be able to mutate the salt underneath it.
      salt: salt.slice(),
      iterations,
      hash: 'SHA-256',
    },
    key,
    KEY_BITS,
  );
  return new Uint8Array(bits);
}

/** Constant-time comparison. A length-varying or early-returning compare
 *  leaks the matching prefix, which turns a million-guess keyspace into six
 *  ten-guess ones. */
function equalConstantTime(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= (a[i] ?? 0) ^ (b[i] ?? 0);
  return diff === 0;
}

export async function makeVerifier(
  pin: string,
  policy: PinPolicy = DEFAULT_PIN_POLICY,
): Promise<PinVerifier> {
  if (pin.length < policy.minLength) {
    throw new Error(`PIN must be at least ${policy.minLength} digits`);
  }
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const hash = await pbkdf2(pin, salt, policy.iterations);
  return { salt, hash, iterations: policy.iterations };
}

/**
 * The session's lock. Holds no market data itself -- it hands back the
 * teardown and the caller performs it, because the thing holding the decoded
 * book is the caller.
 */
export class SessionLock {
  #state: LockState = LockState.Locked;
  #attempts = 0;
  readonly #verifier: PinVerifier;
  readonly #policy: PinPolicy;

  constructor(verifier: PinVerifier, policy: PinPolicy = DEFAULT_PIN_POLICY) {
    this.#verifier = verifier;
    this.#policy = policy;
  }

  get state(): LockState {
    return this.#state;
  }
  get attemptsRemaining(): number {
    return Math.max(0, this.#policy.maxAttempts - this.#attempts);
  }

  /**
   * Lock, and say what must be thrown away.
   *
   * The return type has no optional fields on purpose: there is no such thing
   * as locking that keeps the socket, or locking that keeps the decoded book.
   */
  lock(): Teardown {
    if (this.#state !== LockState.Barred) this.#state = LockState.Locked;
    return {
      unsubscribeAll: true,
      discardDecodedState: true,
      closeSocket: true,
    };
  }

  /**
   * Try to unlock.
   *
   * On success the caller must reconnect and request a SNAPSHOT -- there is no
   * retained state to resume from, by design. That is the same path a detected
   * gap takes, which is why there is only one recovery path in this client
   * rather than two that can drift apart.
   */
  async unlock(pin: string): Promise<boolean> {
    if (this.#state === LockState.Barred) return false;
    if (this.#state === LockState.Unlocked) return true;

    const candidate = await pbkdf2(
      pin,
      this.#verifier.salt,
      this.#verifier.iterations,
    );
    if (equalConstantTime(candidate, this.#verifier.hash)) {
      this.#attempts = 0;
      this.#state = LockState.Unlocked;
      return true;
    }
    this.#attempts++;
    if (this.#attempts >= this.#policy.maxAttempts) {
      this.#state = LockState.Barred;
    }
    return false;
  }

  /** True when the client is entitled to apply deltas. Locked and Barred both
   *  hold no baseline, so both answer false. */
  mayHoldData(): boolean {
    return this.#state === LockState.Unlocked;
  }
}

// ---------------------------------------------------------------------------
// Tab visibility and what it may unsubscribe from
// ---------------------------------------------------------------------------

export interface TabSubscription {
  readonly topic: number;
  readonly channel: Channel;
}

/**
 * Which subscriptions a tab keeps when it is no longer visible.
 *
 * State goes; Event stays. A hidden tab does not need quotes -- they are
 * coalescible and a snapshot restores them on return. It absolutely does need
 * fills: nothing later restates one, and "I was on another tab" is not a
 * reason to have missed it.
 *
 * Returns the subscriptions to KEEP, so the caller unsubscribes the
 * difference. Phrased that way round because the dangerous mistake is
 * forgetting to keep something, not forgetting to drop something.
 */
export function subscriptionsWhenHidden(
  subs: readonly TabSubscription[],
): TabSubscription[] {
  return subs.filter((s) => s.channel === Channel.Event);
}

/** Coming back into view needs a snapshot for every State topic that was
 *  dropped -- the client has no baseline for those any more. */
export function topicsNeedingSnapshotOnShow(
  subs: readonly TabSubscription[],
): number[] {
  const topics = new Set<number>();
  for (const s of subs) if (s.channel === Channel.State) topics.add(s.topic);
  return [...topics].sort((a, b) => a - b);
}
