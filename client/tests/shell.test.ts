// P11-02b acceptance tests.
//
// Test 1: locking discards the data rather than covering it.
// Test 2: the PIN's protection is MEASURED, in both directions, so the claim
//         made for it is the one the numbers support.
// Test 3: attempts are bounded, and barring is not recoverable by guessing.
// Test 4: a hidden tab stops watching prices and does not stop watching fills.
// Test 5: a saved layout cannot carry data -- fed a blob full of positions,
//         none of it survives.
// Test 6: storage is untrusted input, and a bad layout falls back LOUDLY.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';

import { Channel } from '../src/protocol.ts';
import {
  DEFAULT_PIN_POLICY,
  LockState,
  SessionLock,
  makeVerifier,
  subscriptionsWhenHidden,
  topicsNeedingSnapshotOnShow,
} from '../src/session.ts';
import type { PinPolicy, TabSubscription } from '../src/session.ts';
import {
  DEFAULT_LAYOUT,
  LayoutProblem,
  LAYOUT_KEY,
  PanelKind,
  Theme,
  loadLayout,
  parseLayout,
  saveLayout,
  serialiseLayout,
} from '../src/layout.ts';
import type { KeyValueStore, SavedLayout } from '../src/layout.ts';

/** Cheap iterations for the state-machine tests. The real cost is measured
 *  separately in test 2 -- running 600k iterations six times per assertion
 *  would make this suite slow without testing anything extra. */
const FAST: PinPolicy = { iterations: 1000, maxAttempts: 5, minLength: 6 };

class MemoryStore implements KeyValueStore {
  readonly map = new Map<string, string>();
  getItem(key: string): string | null {
    return this.map.get(key) ?? null;
  }
  setItem(key: string, value: string): void {
    this.map.set(key, value);
  }
}

test('[1] locking discards the book rather than covering it', async () => {
  const lock = new SessionLock(await makeVerifier('314159', FAST), FAST);

  assert.equal(lock.state, LockState.Locked, 'a session starts locked');
  assert.equal(
    lock.mayHoldData(),
    false,
    'and holds no data until it is unlocked',
  );

  assert.equal(await lock.unlock('314159'), true);
  assert.equal(lock.mayHoldData(), true);

  const teardown = lock.lock();
  assert.deepEqual(
    teardown,
    {
      unsubscribeAll: true,
      discardDecodedState: true,
      closeSocket: true,
    },
    'locking unsubscribes, discards the decoded book AND closes the socket -- an overlay over a live grid protects nothing, because the positions are still in memory, still in the DOM and still arriving',
  );
  assert.equal(
    lock.mayHoldData(),
    false,
    'so after locking the client is not entitled to apply a delta -- the same state a detected gap leaves it in, which is why there is one recovery path here and not two that can drift apart',
  );
});

test('[2] the PIN buys hours, not safety -- measured both ways', async () => {
  const KEYSPACE = 1_000_000; // six digits

  // The attacker's path if the PIN were stored under a fast hash.
  const N = 200_000;
  const t0 = performance.now();
  for (let i = 0; i < N; i++) {
    createHash('sha256').update(String(i).padStart(6, '0')).digest();
  }
  const fastPerAttemptMs = (performance.now() - t0) / N;
  const fastKeyspaceSec = (fastPerAttemptMs * KEYSPACE) / 1000;

  // What this module actually does.
  const real = DEFAULT_PIN_POLICY;
  const verifier = await makeVerifier('271828', real);
  const lock = new SessionLock(verifier, real);
  const t1 = performance.now();
  await lock.unlock('000000');
  const kdfPerAttemptMs = performance.now() - t1;
  const kdfKeyspaceHours = (kdfPerAttemptMs * KEYSPACE) / 1000 / 3600;

  console.log(
    `    plain SHA-256              ${fastPerAttemptMs.toFixed(4)} ms/attempt -> whole 6-digit keyspace in ${fastKeyspaceSec.toFixed(1)} s`,
  );
  console.log(
    `    PBKDF2-SHA256, ${real.iterations} iters  ${kdfPerAttemptMs.toFixed(1)} ms/attempt -> whole keyspace in ${kdfKeyspaceHours.toFixed(1)} hours`,
  );

  assert.ok(
    fastKeyspaceSec < 600,
    'a six-digit PIN under a fast hash falls in minutes or less, so storing one that way would make the lock decorative',
  );
  assert.ok(
    kdfPerAttemptMs > fastPerAttemptMs * 1000,
    'the KDF raises the cost per attempt by orders of magnitude, which is why it is used',
  );
  assert.ok(
    kdfKeyspaceHours < 24 * 365,
    'and yet the honest reading is that it buys HOURS against someone who already has the machine -- it is not what makes locking safe. What makes locking safe is test 1: there is nothing behind the lock to take',
  );
});

test('[3] attempts are bounded, and barring is not guessable through', async () => {
  const lock = new SessionLock(await makeVerifier('123456', FAST), FAST);

  for (let i = 0; i < FAST.maxAttempts; i++) {
    assert.equal(await lock.unlock('000000'), false);
  }
  assert.equal(
    lock.state,
    LockState.Barred,
    'after the attempt budget the session is barred, not merely still locked -- otherwise an automated guesser sits in a loop against a warm client',
  );
  assert.equal(
    await lock.unlock('123456'),
    false,
    'and the CORRECT pin does not clear it: recovering requires tearing the session down and rebuilding it',
  );
  assert.equal(lock.mayHoldData(), false);

  await assert.rejects(
    () => makeVerifier('12345', FAST),
    'a PIN shorter than the policy minimum is refused at creation, not at first use',
  );
});

test('[4] a hidden tab stops watching prices, not fills', () => {
  const subs: TabSubscription[] = [
    { topic: 1, channel: Channel.State },
    { topic: 1, channel: Channel.Event },
    { topic: 7, channel: Channel.State },
    { topic: 9, channel: Channel.Event },
  ];

  const kept = subscriptionsWhenHidden(subs);
  assert.deepEqual(
    kept.map((s) => `${s.topic}/${s.channel}`),
    [`1/${Channel.Event}`, `9/${Channel.Event}`],
    'State goes -- quotes are coalescible and a snapshot restores them. Event stays: nothing later restates a fill, and "I was looking at another tab" is not a reason to have missed one',
  );
  assert.equal(
    kept.every((s) => s.channel === Channel.Event),
    true,
    'no State subscription survives being hidden',
  );

  assert.deepEqual(
    topicsNeedingSnapshotOnShow(subs),
    [1, 7],
    'and coming back needs a snapshot for exactly the topics whose baseline was dropped -- the client has no right to apply a delta to them',
  );
});

test('[5] a saved layout cannot carry data', () => {
  // The tempting blob: a real layout with the grid's contents stapled on.
  const poisoned = JSON.stringify({
    version: 1,
    theme: Theme.Dark,
    activeTab: 0,
    positions: [{ symbol: 'NIFTY26SEP24000CE', qty: 150, avgPaise: 1875500 }],
    realisedPnlPaise: 41231900,
    tabs: [
      {
        title: 'Positions',
        panel: PanelKind.Grid,
        columnWidths: [140, 90],
        sortColumn: 'symbol',
        sortAscending: true,
        cachedRows: [['NIFTY26SEP24000CE', 150, 18755]],
        lastMarkPaise: 1902000,
      },
    ],
  });

  const r = parseLayout(poisoned);
  assert.equal(r.ok, true, 'the layout part is perfectly valid and loads');
  if (!r.ok) return;

  const text = JSON.stringify(r.layout);
  for (const leak of [
    'positions',
    'realisedPnlPaise',
    'cachedRows',
    'lastMarkPaise',
    'NIFTY26SEP24000CE',
    '41231900',
  ]) {
    assert.equal(
      text.includes(leak),
      false,
      `${leak} did not survive parsing -- the schema has nowhere to put it, so this is not a rule someone has to remember`,
    );
  }

  // And the same on the way out.
  const sneaky = {
    ...DEFAULT_LAYOUT,
    positions: [{ symbol: 'RELIANCE', qty: 500 }],
  } as unknown as SavedLayout;
  assert.equal(
    serialiseLayout(sneaky).includes('RELIANCE'),
    false,
    'serialising rebuilds field by field rather than stringifying whatever it was handed, so a spread of component state cannot write rows to disk',
  );

  console.log(
    '    a layout that painted yesterday\'s positions in yesterday\'s exact colours and sort order would look live; an empty grid is at least unambiguous',
  );
});

test('[6] storage is untrusted input, and a bad layout falls back loudly', () => {
  const cases: ReadonlyArray<readonly [string, string | null, LayoutProblem]> =
    [
      ['nothing stored at all', null, LayoutProblem.Absent],
      ['truncated write', '{"version":1,"the', LayoutProblem.NotJson],
      ['an array, not an object', '[]', LayoutProblem.NotAnObject],
      [
        'written by another version',
        '{"version":2,"theme":3,"tabs":[],"activeTab":0}',
        LayoutProblem.WrongVersion,
      ],
      [
        'theme ordinal zero',
        '{"version":1,"theme":0,"tabs":[],"activeTab":0}',
        LayoutProblem.BadTheme,
      ],
      [
        'no tabs',
        '{"version":1,"theme":3,"tabs":[],"activeTab":0}',
        LayoutProblem.BadTabs,
      ],
      [
        'a column dragged to zero width',
        '{"version":1,"theme":3,"tabs":[{"title":"t","panel":1,"columnWidths":[0],"sortColumn":null,"sortAscending":true}],"activeTab":0}',
        LayoutProblem.BadColumnWidth,
      ],
      [
        'a panel kind this build does not have',
        '{"version":1,"theme":3,"tabs":[{"title":"t","panel":99,"columnWidths":[100],"sortColumn":null,"sortAscending":true}],"activeTab":0}',
        LayoutProblem.BadPanel,
      ],
      [
        'activeTab past the end',
        '{"version":1,"theme":3,"tabs":[{"title":"t","panel":1,"columnWidths":[100],"sortColumn":null,"sortAscending":true}],"activeTab":4}',
        LayoutProblem.BadActiveTab,
      ],
    ];

  for (const [what, raw, expected] of cases) {
    const r = parseLayout(raw);
    assert.equal(r.ok, false, `${what}: refused`);
    if (r.ok) continue;
    assert.equal(r.problem, expected, `${what}: named as ${expected}`);
    assert.deepEqual(
      r.fallback,
      DEFAULT_LAYOUT,
      `${what}: a usable default comes back, HANDED to the caller rather than substituted silently -- silently repairing it is how a user loses their layout every session and never finds out why`,
    );
  }
  console.log(`    ${cases.length} malformed layouts, each named rather than repaired`);

  // Round trip.
  const store = new MemoryStore();
  saveLayout(store, DEFAULT_LAYOUT);
  assert.ok(store.getItem(LAYOUT_KEY) !== null);
  const back = loadLayout(store);
  assert.equal(back.ok, true);
  if (back.ok) assert.deepEqual(back.layout, DEFAULT_LAYOUT);
});
