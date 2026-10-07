#!/bin/sh
# embsvd: clusters, register arrays, derivedFrom, and --json -- each
# checked by three witnesses that share no code:
#
#   - the C compiler: layout.c and nrflayout.c are _Static_asserts of
#     offsetof and sizeof, worked out by hand from the SVDs, compiled
#     against the generated headers by EmbCC and by clang for a Cortex-M;
#   - svdref.py, the CMSIS-SVD specification's address arithmetic in
#     Python: the register addresses --json lists must be its addresses;
#   - the header again: every register --json lists is turned into an
#     assert that BASE + offsetof(TYPE, PATH) is its address, compiled by
#     both compilers.
#
#   1. features.svd: every kind of cluster and array, nested clusters,
#      derivedFrom on registers, clusters, fields, enumeratedValues and
#      peripherals, a peripheral array, alternate clusters.
#   2. nrfshape.svd: a device in the shape of Nordic's nRF54L files.
#   3. ARM's ARM_Example.svd against the TIMER0_Type that CMSIS's own
#      svdconv wrote for it (CMSIS_5's documentation), when $EMBREF has it;
#      and Nordic's own SVD files, when $EMBREF/svd has them.
#   4. what embsvd cannot lay out is refused, by name and line.
set -u
echo "TEST-MARKER svd-clusters"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBSVD=${EMBSVD:-./embsvd}
CLANG=${EMBCC_REF_CLANG_ARM:-clang}
ref=${EMBREF:-$HOME/EmbRef}
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
"$CLANG" --target=thumbv7em-none-eabi -fsyntax-only -x c /dev/null 2>/dev/null ||
    { echo "SKIP: no clang for thumbv7em"; exit 0; }
d=tests/golden/svd-clusters
out=tests/golden/out/svd-clusters
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# both compilers, one target: $1 target, the rest the compile arguments
both() {
    t=$1; shift
    "$EMBCC" --target=$t "$@" -o "$out/embcc.o" > "$out/embcc.txt" 2>&1 ||
        { head -20 "$out/embcc.txt"; return 1; }
    "$CLANG" --target=$t -std=c11 -Wall -Werror "$@" -o "$out/clang.o" \
        > "$out/clang.txt" 2>&1 || { head -20 "$out/clang.txt"; return 1; }
}

# The three witnesses over one SVD: $1 the SVD, $2 its name, $3 a target.
# --no-cmsis, so the core's own peripherals are laid out too.
witness() {
    "$EMBSVD" "$1" --no-cmsis --header "$out/$2.h" --json "$out/$2.json" \
        > "$out/$2.gen.txt" 2>&1 || { cat "$out/$2.gen.txt"; fail "embsvd $2"; }
    python3 "$d/jsoncheck.py" "$out/$2.json" "$2.h" "$out/$2.regs" \
        "$out/$2-asserts.c" > "$out/$2.check.txt" 2>&1 ||
        { cat "$out/$2.check.txt"; fail "$2.json does not have the schema"; }
    python3 "$d/svdref.py" "$1" > "$out/$2.ref" ||
        fail "svdref.py could not read $2"
    cmp -s "$out/$2.ref" "$out/$2.regs" ||
        { diff "$out/$2.ref" "$out/$2.regs" | head -10
          fail "$2: the registers --json lists are not where the SVD puts them"; }
    both $3 -I"$out" -c "$out/$2-asserts.c" ||
        fail "$2: the header does not put the registers where --json says"
    echo "  $2: $(cat "$out/$2.check.txt"), where the SVD and the header put them"
}

# 1. every feature
witness "$d/features.svd" features thumbv7em-none-eabi
both thumbv7em-none-eabi -I"$out" -c "$d/layout.c" ||
    fail "features.h is not laid out as features.svd says"
"$EMBSVD" "$d/features.svd" --json "$out/features-mem.json" \
    --flash 0x08000000:256K --ram 0x20000000:64K || fail "--json with memories"
python3 "$d/features.py" "$out/features-mem.json" || fail "features.json"
for s in '} DMA_CH_Type;  /\* 32 bytes \*/' '__IO DMA_CH_Type CH\[4\];' \
         '__IO NET_QUEUE_DESC_Type DESC\[2\];' '} NETIF_Type;' \
         '__IO NETIF_LANE_Type SPARE;' '__IO NETIF_LANE_Type LANEX;' \
         '#define NET2 ((NETIF_Type \*)NET2_BASE)' \
         '#define TIMER2 ((TIMER_Type \*)TIMER2_BASE)'; do
    grep -q "$s" "$out/features.h" || fail "features.h has no '$s'"
done
# a derivedFrom cluster shares its base's struct
grep -q 'SPARE_Type\|LANEX_Type' "$out/features.h" &&
    fail "a derivedFrom cluster got a struct of its own"
echo "features.svd: clusters, arrays and derivedFrom laid out as worked by hand"

# 2. Nordic's shape
witness "$d/nrfshape.svd" nrfshape thumbv8m.main-none-eabi
both thumbv8m.main-none-eabi -I"$out" -c "$d/nrflayout.c" ||
    fail "nrfshape.h is not laid out as nrfshape.svd says"
CM=$ref/CMSIS_5/CMSIS/Core/Include
if [ -f "$CM/core_cm33.h" ]; then
    mkdir -p "$out/cmsis"
    "$EMBSVD" "$d/nrfshape.svd" --header "$out/cmsis/nrfshape.h" ||
        fail "nrfshape.h with CMSIS"
    grep -q '#include "core_cm33.h"' "$out/cmsis/nrfshape.h" &&
    grep -q '#define __MPU_PRESENT 1' "$out/cmsis/nrfshape.h" ||
        fail "nrfshape.h does not configure CMSIS-Core as the SVD says"
    both thumbv8m.main-none-eabi -fgnuc-version=4.2.1 -I"$out/cmsis" -I"$CM" \
        -c "$d/nrflayout.c" || fail "nrfshape.h with CMSIS-Core"
fi
echo "nrfshape.svd: Nordic's shape of header, with and without CMSIS-Core"

# 3. the vendors' own files
EX=$ref/CMSIS_5/CMSIS/Utilities/ARM_Example.svd
T0=$ref/CMSIS_5/CMSIS/DoxyGen/SVD/src/ARM_ExampleT0.h
if [ -f "$EX" ] && [ -f "$T0" ]; then
    "$EMBSVD" "$EX" --no-cmsis --header "$out/ARM_Example.h" ||
        fail "ARM_Example.svd"
    { echo '#include <stddef.h>'
      echo '#include "ARM_Example.h"'
      echo '#define TIMER0_Type SVDCONV_TIMER0_Type'
      echo "#include \"$T0\""
      echo '#undef TIMER0_Type'
      for m in CR SR INT COUNT MATCH PRESCALE_WR PRESCALE_RD RELOAD 'RELOAD[3]'; do
          echo "_Static_assert(offsetof(TIMER0_Type, $m) == offsetof(SVDCONV_TIMER0_Type, $m), \"$m\");"
      done
      echo '_Static_assert(sizeof(TIMER0_Type) == sizeof(SVDCONV_TIMER0_Type), "size");'
    } > "$out/svdconv.c"
    both thumbv7m-none-eabi -I"$out" -c "$out/svdconv.c" ||
        fail "ARM_Example's TIMER0 is not laid out as svdconv laid it out"
    echo "ARM_Example.svd: TIMER0_Type as CMSIS's svdconv wrote it"
fi
for f in "$ref"/svd/nrf*.svd; do
    [ -f "$f" ] || continue
    witness "$f" "$(basename "$f" .svd)" thumbv8m.main-none-eabi
done

# 4. refused, by name
n=0
refuse() {   # $1 what is refused, $2 the registers, $3 expected message
    n=$((n + 1))
    cat > "$out/bad$n.svd" <<EOF
<device><name>BAD</name><peripherals>
<peripheral><name>P</name><baseAddress>0x40000000</baseAddress><registers>
$2
</registers></peripheral>
<peripheral><name>Q</name><baseAddress>0x40001000</baseAddress><registers>
<register><name>R</name><addressOffset>0</addressOffset></register>
<cluster><name>K</name><addressOffset>0x10</addressOffset>
<register><name>Z</name><addressOffset>0</addressOffset></register></cluster>
</registers></peripheral>
</peripherals></device>
EOF
    if "$EMBSVD" "$out/bad$n.svd" --json "$out/bad$n.json" > "$out/bad$n.txt" 2>&1; then
        fail "$1 should be refused"
    fi
    grep -q "$3" "$out/bad$n.txt" ||
        { cat "$out/bad$n.txt"; fail "$1: the refusal does not say '$3'"; }
}
R='<register><name>A</name><addressOffset>0</addressOffset></register>'
refuse "a register off its alignment" \
    '<register><name>W</name><addressOffset>2</addressOffset></register>' \
    'line 3: P.W at offset 0x2 is not aligned'
refuse "two registers at one offset" \
    "$R<register><name>B</name><addressOffset>0</addressOffset></register>" \
    'P.B at offset 0x0 overlaps A'
refuse "cluster array elements that overlap" \
    '<cluster><dim>2</dim><dimIncrement>4</dimIncrement><name>C[%s]</name><addressOffset>0</addressOffset>
     <register><name>X</name><addressOffset>0</addressOffset></register>
     <register><name>Y</name><addressOffset>4</addressOffset></register></cluster>' \
    'array C\[%s\]: its elements are 8 bytes but 4 apart'
refuse "[%s] inside a name" \
    '<register><dim>2</dim><dimIncrement>4</dimIncrement><name>R[%s]_X</name><addressOffset>0</addressOffset></register>' \
    'register R\[%s\]_X: \[%s\] is only understood at the end'
refuse "%s with no dim" \
    '<register><name>R%s</name><addressOffset>0</addressOffset></register>' \
    'register R%s has a %s in its name but no <dim>'
refuse "a dim with no %s" \
    '<register><dim>2</dim><dimIncrement>4</dimIncrement><name>R</name><addressOffset>0</addressOffset></register>' \
    'register array R has no %s'
refuse "a dimIndex that names too few" \
    '<register><dim>3</dim><dimIncrement>4</dimIncrement><dimIndex>A,B</dimIndex><name>R%s</name><addressOffset>0</addressOffset></register>' \
    "dimIndex 'A,B' names fewer than 3"
refuse "a derivedFrom that names nothing" \
    "$R<register derivedFrom=\"NOPE\"><name>B</name><addressOffset>4</addressOffset></register>" \
    'derivedFrom="NOPE": there is no register NOPE'
refuse "a derivedFrom path that names nothing" \
    '<cluster derivedFrom="Q.NOPE"><name>B</name><addressOffset>4</addressOffset></cluster>' \
    'derivedFrom="Q.NOPE": there is no cluster NOPE there'
refuse "derivedFrom in a circle" \
    '<register derivedFrom="B"><name>A</name><addressOffset>0</addressOffset></register>
     <register derivedFrom="A"><name>B</name><addressOffset>4</addressOffset></register>' \
    'derivedFrom goes round in a circle'
refuse "a derived cluster with registers of its own" \
    '<cluster derivedFrom="Q.K"><name>B</name><addressOffset>0</addressOffset>
     <register><name>X</name><addressOffset>4</addressOffset></register></cluster>' \
    'cluster B is derivedFrom Q.K and has registers of its own'
refuse "two different clusters with one headerStructName" \
    '<cluster><name>A</name><headerStructName>S</headerStructName><addressOffset>0</addressOffset>
     <register><name>X</name><addressOffset>0</addressOffset></register></cluster>
     <cluster><name>B</name><headerStructName>S</headerStructName><addressOffset>8</addressOffset>
     <register><name>Y</name><addressOffset>0</addressOffset></register></cluster>' \
    'would both be struct S_Type'
refuse "a field outside its register" \
    '<register><name>R</name><addressOffset>0</addressOffset><size>16</size><fields>
     <field><name>F</name><bitOffset>12</bitOffset><bitWidth>8</bitWidth></field></fields></register>' \
    'field R.F, bits 12..19, is outside the 16-bit register'
sed 's|<peripheral><name>P</name>|<peripheral><dim>2</dim><dimIncrement>0x100</dimIncrement><name>P[%s]</name>|' \
    "$out/bad1.svd" > "$out/badp.svd"
"$EMBSVD" "$out/badp.svd" --json "$out/badp.json" > "$out/badp.txt" 2>&1 &&
    fail "an array of peripherals written [%s] should be refused"
grep -q 'peripheral P\[%s\]: an array of peripherals written \[%s\] is not supported' \
    "$out/badp.txt" || { cat "$out/badp.txt"; fail "the [%s] peripheral refusal"; }
sed 's|<device><name>BAD</name>|<device><name>BAD</name><addressUnitBits>16</addressUnitBits>|' \
    "$out/bad1.svd" > "$out/badu.svd"
"$EMBSVD" "$out/badu.svd" --json "$out/badu.json" > "$out/badu.txt" 2>&1 &&
    fail "addressUnitBits 16 should be refused"
grep -q 'addressUnitBits is 16' "$out/badu.txt" || { cat "$out/badu.txt"; fail "addressUnitBits"; }
echo "$((n + 2)) kinds of SVD embsvd cannot lay out are refused by name"
