#!/bin/sh
# embsim --svd over every SVD file in $EMBREF/svd (default ~/EmbRef/svd:
# ST's STM32F405 and Nordic's nRF52840, nRF5340, nRF54L15 and nRF9160):
# each is read, and the register map EmbSim builds from it (--svd-map)
# must put every register where tests/golden/svd-clusters/svdref.py, the
# CMSIS-SVD specification's address arithmetic in Python sharing no code
# with EmbSim, puts it: the same registers, at the same addresses, of the
# same sizes. And the map must be whole: every register is on the bus,
# or says which register at its address is (an alternate view, or a
# peripheral sharing another's space), or is the core's own; and each
# address on the bus is one register's.
# Without the directory, SKIP.
set -u
echo "TEST-MARKER embsim-svd-all"
. "$(dirname "$0")/../lib.sh"

EMBSIM=${EMBSIM:-./embsim}
dir=${EMBREF:-$HOME/EmbRef}/svd
out=tests/golden/out/embsim-svd-all
fail() { echo "FAIL: $*"; exit 1; }
[ -d "$dir" ] || { echo "SKIP: no $dir"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"
rm -rf "$out"; mkdir -p "$out"

n=0
for f in "$dir"/*.svd tests/golden/embsim-svd/regs.svd; do
    [ -f "$f" ] || continue
    b=$(basename "$f" .svd)
    # mps2-an386: a board with no SVD of its own, so none is picked for it
    "$EMBSIM" --board mps2-an386 --svd "$f" --svd-map > "$out/$b.map" 2> "$out/$b.err" ||
        { cat "$out/$b.err"; fail "EmbSim did not read $b.svd"; }
    grep -v '^#' "$out/$b.map" | awk '{ print $1, $2, $3, $4 }' | sort > "$out/$b.sim"
    python3 tests/golden/svd-clusters/svdref.py "$f" | sort > "$out/$b.ref" ||
        fail "svdref.py could not read $b.svd"
    cmp -s "$out/$b.sim" "$out/$b.ref" ||
        { diff "$out/$b.ref" "$out/$b.sim" | head; fail "$b: the registers are not where the SVD puts them"; }
    # whole: the header's count is the registers mapped; each address is
    # one register's; an unmapped one names a mapped one
    awk -v name="$b" '
        /^#/ { want = $5; next }
        / -- the core.s$/ { core++; next }
        / -- is / { is[$NF] = 1; nis++; next }
        { mapped++; if (seen[$2]++) { print name ": two registers at " $2; bad = 1 }
          names[$5] = 1 }
        END {
            for (r in is) if (!(r in names)) { print name ": " r " is not mapped"; bad = 1 }
            if (mapped != want) { print name ": " mapped " mapped, the header says " want; bad = 1 }
            printf "%d %d %d\n", mapped, nis, core
            exit bad
        }' "$out/$b.map" > "$out/$b.count" ||
        { cat "$out/$b.count"; fail "$b: the map is not whole"; }
    read mapped alts core < "$out/$b.count"
    echo "  $b: $(grep -c . "$out/$b.ref") registers where svdref.py puts them; $mapped on the bus, $alts sharing an address, $core the core's"
    n=$((n + 1))
done
[ $n -ge 2 ] || fail "only $n SVD files"
echo "embsim-svd-all: $n SVD files read and mapped"
