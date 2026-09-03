// client/src/export/rows.ts -- exporting what is on screen, in chunks.
//
// P11-09.
//
// AN EXPORT IS OF THE VIEW, NOT OF THE STORE.
//
// The same argument as P11-08's select-all, and it lands harder here because
// the output leaves the machine. A user filters to four positions, presses
// Export, and mails the file to their accountant. If the exporter walked the
// store rather than the view, that file has five thousand rows in it and the
// filter chips on screen said four.
//
// So `exportRows` takes `view` and `columns` -- the filtered, sorted ids and
// the visible columns, in display order -- and has no access to anything else.
// The signature is the guarantee.
//
// A MILLION ROWS IS NOT A STRING.
//
// The second thing. The obvious implementation builds one big string and hands
// it to a Blob. At a million rows and ~80 bytes a row that is an 80 MB
// contiguous string, built by repeated concatenation, on the main thread,
// while the grid is meant to be rendering at 60 fps.
//
// `exportRows` is a generator yielding chunks, so the caller streams into a
// Blob, a File System Access writer, or a fetch body without ever holding the
// whole thing. `CHUNK_ROWS` is deliberately modest: the point is bounded
// memory and a yield point, not throughput.
//
// THE HEADER SAYS WHAT THE NUMBERS MEAN.
//
// The third, and it is cheap insurance. A column of paise exported as
// `123435.00` is ambiguous between rupees and paise to anyone who did not
// write this program. The header carries the unit -- `P&L (INR)` -- and a
// preamble row carries the filter that produced the file, so a spreadsheet
// sitting in someone's downloads folder still says what it is a view OF.

import type { RowId, RowStore } from '../grid/store.ts';
import { ColumnType } from '../grid/filter.ts';
import type { ColumnSpec, Filter } from '../grid/filter.ts';
import { csvField, csvMoney } from './csv.ts';

export const Format = {
  Unspecified: 0,
  Csv: 1,
  /** Tab-separated. What a spreadsheet expects on the clipboard. */
  Tsv: 2,
  /** One JSON object per line. Streams, unlike a single JSON array, and every
   *  64-bit value is a STRING for the reason P11-01 gives at length. */
  Jsonl: 3,
} as const;
export type Format = (typeof Format)[keyof typeof Format];

export const CHUNK_ROWS = 2048;

export interface ExportOptions {
  readonly format: Format;
  /** Include a comment line naming the filters this file is a view under.
   *  Default on: a file in a downloads folder should say what it is. */
  readonly includeProvenance?: boolean;
  readonly filters?: readonly Filter[];
  readonly generatedAtNs?: bigint;
}

function delimiterFor(f: Format): string {
  return f === Format.Tsv ? '\t' : ',';
}

/** Unit suffix for a header cell, so `123435.00` is not ambiguous between
 *  rupees and paise to someone who did not write this program. */
export function headerLabel(spec: ColumnSpec): string {
  switch (spec.type) {
    case ColumnType.Money:
      return `${spec.label} (INR)`;
    case ColumnType.Integer:
    case ColumnType.Real:
    case ColumnType.Text:
    case ColumnType.Unspecified:
    default:
      return spec.label;
  }
}

function cellText(
  store: RowStore,
  id: RowId,
  spec: ColumnSpec,
  delimiter: string,
): string {
  const c = store.read(id, spec.key);
  // An absent cell is EMPTY, not 0 and not "null". A zero in an exported P&L
  // column is a claim that the position was flat, and it survives into
  // whatever the recipient does with the file.
  if (!c.present) return '';
  if (spec.type === ColumnType.Money && typeof c.value === 'bigint') {
    return csvMoney(c.value);
  }
  if (typeof c.value === 'bigint' || typeof c.value === 'number') {
    return String(c.value);
  }
  return csvField(c.value, delimiter);
}

/**
 * Yield the export in chunks.
 *
 * A generator rather than a string: at a million rows the string form is tens
 * of megabytes built by concatenation on the main thread, while the grid is
 * meant to be drawing.
 */
export function* exportRows(
  store: RowStore,
  view: readonly RowId[],
  columns: readonly ColumnSpec[],
  options: ExportOptions,
): Generator<string, void, undefined> {
  const d = delimiterFor(options.format);

  if (options.format === Format.Jsonl) {
    for (let i = 0; i < view.length; i += CHUNK_ROWS) {
      let chunk = '';
      for (const id of view.slice(i, i + CHUNK_ROWS)) {
        const obj: Record<string, string | number | null> = {};
        for (const spec of columns) {
          const c = store.read(id, spec.key);
          if (!c.present) {
            obj[spec.key] = null;
            continue;
          }
          // Every 64-bit value is a STRING. JSON numbers are doubles, and
          // P11-01 measured what that costs.
          obj[spec.key] =
            typeof c.value === 'bigint' ? c.value.toString() : c.value;
        }
        chunk += `${JSON.stringify(obj)}\n`;
      }
      yield chunk;
    }
    return;
  }

  if (options.includeProvenance !== false) {
    const when =
      options.generatedAtNs === undefined
        ? ''
        : ` at ${options.generatedAtNs.toString()} ns`;
    const filterText =
      options.filters === undefined || options.filters.length === 0
        ? 'no filters'
        : `${options.filters.length} filter(s) applied`;
    yield `# Altair export -- ${view.length} of ${store.size} rows, ${filterText}${when}\n`;
  }

  yield `${columns.map((c) => csvField(headerLabel(c), d)).join(d)}\n`;

  for (let i = 0; i < view.length; i += CHUNK_ROWS) {
    let chunk = '';
    for (const id of view.slice(i, i + CHUNK_ROWS)) {
      chunk += `${columns.map((spec) => cellText(store, id, spec, d)).join(d)}\n`;
    }
    yield chunk;
  }
}

/** Collect the whole export. For tests and small selections only -- the
 *  generator exists precisely so the render path does not do this. */
export function exportToString(
  store: RowStore,
  view: readonly RowId[],
  columns: readonly ColumnSpec[],
  options: ExportOptions,
): string {
  let out = '';
  for (const chunk of exportRows(store, view, columns, options)) out += chunk;
  return out;
}

/**
 * What goes on the clipboard.
 *
 * TSV, because that is what a spreadsheet reads from `text/plain` when you
 * paste -- and it goes through exactly the same escaping, because a paste into
 * Excel evaluates formulas just as an opened file does. A clipboard path that
 * skipped the neutralisation would be the same hole with a shorter route.
 */
export function clipboardText(
  store: RowStore,
  view: readonly RowId[],
  columns: readonly ColumnSpec[],
): string {
  return exportToString(store, view, columns, {
    format: Format.Tsv,
    includeProvenance: false,
  });
}
