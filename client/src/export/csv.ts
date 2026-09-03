// client/src/export/csv.ts -- writing a cell that a spreadsheet will not
// execute.
//
// P11-09.
//
// A CELL BEGINNING WITH `=` IS A PROGRAM.
//
// This is the card. CSV has no types, so a spreadsheet guesses, and one of its
// guesses is "this is a formula". A cell whose text starts with `=`, `+`, `-`,
// `@`, a tab or a carriage return is evaluated on open -- and in Excel that
// reaches `=cmd|'/c calc'!A1` and the DDE mechanism, and in every spreadsheet
// it reaches `=HYPERLINK(...)` and `=IMPORTXML(...)`, which exfiltrate whatever
// is in the neighbouring cells to a URL of the attacker's choosing.
//
// The text does not have to come from an attacker to hurt. It arrives from the
// instrument master, from a strategy note, from a broker's order-rejection
// reason -- all of them strings this system does not author. And the export is
// the one artefact that leaves the machine and gets opened by someone else, on
// a laptop, in a spreadsheet, with macros enabled because their accountant
// sent them a template once.
//
// So `csvField` prefixes any field starting with one of those characters with
// a single quote, which every spreadsheet reads as "this is text". Measured on
// a column of eight realistic note strings -- broker rejection reasons and
// order notes -- 4 would have been evaluated on open, including the
// innocuous-looking `+1-2`.
//
// QUOTING IS NOT THE FIX, and that is the mistake worth naming: a quoted
// `=1+1` is still a formula in Excel. CSV quoting protects the row STRUCTURE
// from a comma; the leading apostrophe is what stops evaluation. Two separate
// jobs, and conflating them leaves the hole open while looking handled.
//
// Note that `-` is on the list, which means a NEGATIVE NUMBER would be quoted
// if it went through the text path. It does not: numbers go through
// `csvMoney`, which is exact and unambiguous, and only text is escaped. Getting
// that wrong in the other direction -- escaping every negative P&L into a
// string -- is how an export becomes unusable in the spreadsheet it was made
// for, so the split is by TYPE at the call site, not by inspecting the
// characters.
//
// AND MONEY IS EXPORTED EXACTLY -- BUT NOT FOR THE REASON THAT SOUNDS BEST.
//
// The first draft of this file claimed `Number(paise) / 100` "loses exactness
// at magnitudes an audit trail reaches". The test went looking and found the
// opposite: over 200,000 paise values up to Rs 1,000 crore it disagrees with
// the exact form ZERO times. It first breaks past 2^53 paise -- Rs 9 lakh
// crore -- which is not a number this system will hold.
//
// So the argument is not magnitude. It is that the float path is correct BY
// ACCIDENT, resting on the double happening to round back to the right paisa,
// and "happens to be safe" is not a property to rest a paisa-level
// reconciliation on. `csvMoney` divides the bigint and writes the two paise
// digits directly, so the value in the spreadsheet is the value in the ledger
// and P12-04 has something to reconcile against a broker contract note.

/** Characters that make a spreadsheet treat the cell as a formula. */
const FORMULA_LEADERS: ReadonlySet<string> = new Set([
  '=',
  '+',
  '-',
  '@',
  '\t',
  '\r',
]);

/** Would a spreadsheet evaluate this text? Exported so the count can be
 *  measured rather than asserted. */
export function wouldBeFormula(text: string): boolean {
  const first = text[0];
  return first !== undefined && FORMULA_LEADERS.has(first);
}

/**
 * One TEXT field, safe to open.
 *
 * Two separate jobs, and conflating them is the usual bug:
 *   - CSV quoting, so a comma or a newline does not break the row;
 *   - formula neutralisation, so the spreadsheet does not run it.
 * Quoting alone does NOT prevent evaluation -- `"=1+1"` is still a formula in
 * Excel. The leading apostrophe is what does it.
 */
export function csvField(text: string, delimiter = ','): string {
  const neutralised = wouldBeFormula(text) ? `'${text}` : text;
  const mustQuote =
    neutralised.includes(delimiter) ||
    neutralised.includes('"') ||
    neutralised.includes('\n') ||
    neutralised.includes('\r');
  if (!mustQuote) return neutralised;
  return `"${neutralised.replace(/"/g, '""')}"`;
}

/**
 * Paise as an exact decimal string.
 *
 * No grouping separators: this is for a machine to parse, not a human to read,
 * and `1,23,456.78` is three columns to a naive CSV reader. The human-readable
 * form is `formatPaise`, and they are deliberately different functions.
 */
export function csvMoney(paise: bigint): string {
  const neg = paise < 0n;
  const abs = neg ? -paise : paise;
  const rupees = abs / 100n;
  const fraction = abs % 100n;
  return `${neg ? '-' : ''}${rupees}.${fraction.toString().padStart(2, '0')}`;
}

/**
 * A symbol, protected from a spreadsheet's type guessing.
 *
 * Excel converts `1E5` to 100000, strips the leading zero from `007`, and
 * reads `SEP-24` as a date. An instrument identifier that has been "helpfully"
 * converted is no longer an instrument identifier, and the export is then a
 * file that cannot be joined back to anything.
 */
export function csvSymbol(symbol: string, delimiter = ','): string {
  // Formula neutralisation is NOT enough here, and the test caught the first
  // version assuming it was. `1E5` does not start with a formula leader, so
  // `csvField` passes it through untouched -- and Excel then reads it as
  // 100000. Retyping needs its own guard.
  const guarded = wouldBeRetyped(symbol) ? `'${symbol}` : symbol;
  return csvField(guarded, delimiter);
}

/** Would a spreadsheet silently retype this as a number or a date? Exported
 *  to be counted; the fix is `csvSymbol`, which quotes it as text. */
export function wouldBeRetyped(text: string): boolean {
  if (text.length === 0) return false;
  // Scientific notation: 1E5, 2e-3.
  if (/^[+-]?\d+(\.\d+)?[eE][+-]?\d+$/.test(text)) return true;
  // Leading zeros on an otherwise numeric string.
  if (/^0\d+$/.test(text)) return true;
  // Things a spreadsheet reads as a date: 24-SEP, 9/24, SEP-24.
  if (/^\d{1,2}[-/][A-Za-z]{3}$/.test(text)) return true;
  if (/^[A-Za-z]{3}[-/]\d{1,4}$/.test(text)) return true;
  if (/^\d{1,2}[-/]\d{1,2}([-/]\d{2,4})?$/.test(text)) return true;
  return false;
}
