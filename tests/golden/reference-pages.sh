#!/bin/sh
# The reference pages the documentation includes are printed by the
# compiler from the tables they describe (`embcc --reference=PAGE`), so
# they cannot list what the compiler does not have or miss what it has.
# Each is checked against a list the compiler gives another way:
#
#   warnings  every name --help-warnings lists has a row, with what it
#             diagnoses and what turns it on;
#   targets   every target an unknown --target lists has a row, as a
#             target or as another spelling of one;
#   options   its tables are well formed (options-table.sh checks its
#             content);
#
# and every Markdown table on the three pages has as many cells in each
# row as in its header, so a description with a stray '|' cannot shift
# a column.
set -u
echo "TEST-MARKER reference-pages"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/reference-pages
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

for p in options warnings targets; do
    "$EMBCC" --reference=$p > "$out/$p.md" 2> "$out/$p.err" ||
        { cat "$out/$p.err"; fail "--reference=$p"; }
    [ -s "$out/$p.md" ] || fail "--reference=$p printed nothing"
done

# ---- warnings ---------------------------------------------------------------
"$EMBCC" --help-warnings | sed -n 's/^  --warn=\([a-z0-9-]*\) .*/\1/p' > "$out/w.names"
nw=$(wc -l < "$out/w.names" | tr -d ' ')
[ "$nw" -ge 30 ] || fail "--help-warnings listed only $nw warnings"
while read -r w; do
    grep -q "^| \`--warn=$w\` | [A-Z\`].* | .* |\$" "$out/warnings.md" || echo "$w"
done < "$out/w.names" > "$out/w.missing"
[ ! -s "$out/w.missing" ] || { cat "$out/w.missing"; fail "warnings with no row in --reference=warnings"; }
rows=$(grep -c '^| `--warn=' "$out/warnings.md")
[ "$rows" = "$nw" ] || fail "--reference=warnings has $rows rows for $nw warnings"
echo "reference-pages: all $nw warnings, each with what it diagnoses and what turns it on"

# ---- targets ----------------------------------------------------------------
printf 'int x;\n' > "$out/t.c"
"$EMBCC" --target=no-such-target -c "$out/t.c" -o "$out/t.o" 2>&1 |
    sed -n 's/^embcc:   \([^ ]*\)$/\1/p' > "$out/t.names"
nt=$(wc -l < "$out/t.names" | tr -d ' ')
[ "$nt" -ge 40 ] || fail "an unknown --target listed only $nt targets"
while read -r t; do
    grep -q "\`$t\`" "$out/targets.md" || echo "$t"
done < "$out/t.names" > "$out/t.missing"
[ ! -s "$out/t.missing" ] || { cat "$out/t.missing"; fail "targets missing from --reference=targets"; }
echo "reference-pages: all $nt target spellings, each canonical one with what it runs on"

# ---- every table well formed ------------------------------------------------
for p in options warnings targets; do
    # a row's cells: its '|' that are not escaped (\|), less one
    awk -v page=$p '
        /^\|/ {
            s = $0; gsub(/\\\|/, "", s); n = gsub(/\|/, "|", s) - 1
            if (!in_t) { want = n; in_t = 1 }
            else if (n != want) { print page ": line " NR " has " n " cells, the header " want; bad = 1 }
            next
        }
        { in_t = 0 }
        END { exit bad }' "$out/$p.md" || fail "a malformed table on the $p page"
done
echo "reference-pages: every table row has its header's number of cells"
