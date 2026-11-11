#!/usr/bin/env bash
# tools/check_data_policy.sh -- fail if the banned exchange source comes back.
#
# docs/DATA_POLICY.md: ALTAIR never uses the NSE bhavcopy (old or UDiFF format).
# This scans every tracked file -- names and contents -- and exits 1 on any
# hit outside the short allowlist of files that define and test the ban.
# Runs in CI (first job) and as ctest `data_policy_scan`.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || dirname "$0"/..)"

# The only files allowed to name the source: the rule, its lock, and this scan.
allow='^(docs/DATA_POLICY\.md|app/data_policy\.hpp|tools/check_data_policy\.sh)$'
# Company names that merely contain the letters (NSE equity master).
benign='SADBHAV|VAIBHAV'
pattern='bhav|udiff|_(fo|cm)_0_0_0_'

if git rev-parse --git-dir >/dev/null 2>&1; then
    files() { git ls-files -z --cached --others --exclude-standard; }   # new files too, before they are committed
else
    files() { find . -type f -not -path './.git/*' -not -path './build*' -printf '%P\0'; }
fi

bad=0
while IFS= read -r -d '' f; do
    [[ "$f" =~ $allow ]] && continue
    if echo "$f" | grep -Eiq "$pattern"; then
        echo "BANNED FILE: $f"; bad=1; continue
    fi
    [ -f "$f" ] || continue
    # Lines over 4000 characters are embedded blobs (base64 PDFs in saved sheets),
    # where four random letters can spell anything.
    hits=$(grep -EinI "$pattern" -- "$f" 2>/dev/null | grep -Eiv "$benign" | awk 'length($0) < 4000' || true)
    if [ -n "$hits" ]; then
        echo "BANNED REFERENCE in $f:"; echo "$hits" | cut -c1-200 | sed 's/^/    /'; bad=1
    fi
done < <(files)

if [ "$bad" -ne 0 ]; then
    echo
    echo "FAIL: the NSE bhavcopy is banned in ALTAIR (docs/DATA_POLICY.md). Use FYERS data;"
    echo "if FYERS lacks it, say so and stop -- never fall back."
    exit 1
fi
echo "data policy: clean (no bhavcopy anywhere)"
