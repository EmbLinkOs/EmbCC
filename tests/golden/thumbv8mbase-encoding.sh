#!/bin/sh
# ARMv8-M Baseline (Cortex-M23): every 32-bit encoding the core has, as
# EmbCC's assembler writes it, against llvm-mc's thumbv8m.base encoder --
# and that nothing else gets through.
#
#  1. THE BASELINE SET. tools/tasmcheck --v8m-base lists, from asm.c's own
#     tables, every 32-bit instruction Baseline has -- the divides, MOVW and
#     MOVT, the exclusives at one, two and four bytes and with an offset,
#     CLREX, the load-acquire/store-release family, TT/TTT/TTA/TTAT, SG,
#     MRS and MSR on each special register the core has, the barriers --
#     and BXNS/BLXNS, across the registers each field takes. llvm-mc must
#     take every line for thumbv8m.base (a line it refuses is an
#     instruction the core lacks) and write the same bytes.
#  2. THE BRANCHES, which need labels: BL, B.W and CBZ/CBNZ in a .s file,
#     through the file assembler (src/as/gas.c), against llvm-mc.
#  3. ARMv8-M MAINLINE's additions (the security extension, the
#     acquire/release forms, VLSTM/VLLDM, the v8-M special registers) the
#     same way, for thumbv8m.main.
#  4. NOTHING ELSE. Every line of the ARMv7E-M and the v8-M vocabularies
#     assembled one at a time at the Baseline level: EmbCC must refuse
#     exactly the lines llvm-mc refuses for thumbv8m.base. That is the check
#     on t_thumb1_ok32 (emit.c), which the assembler and the backend's scan
#     (v6m.c) both ask -- so a Thumb-2 form cannot reach a Cortex-M23 from
#     an inline asm or a .s file either. The backend's own output is
#     scanned by tests/golden/thumbv8mbase-exec.sh.
set -u
echo "TEST-MARKER thumbv8mbase-encoding"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
out=tests/golden/out/thumbv8mbase-encoding
rm -rf "$out"; mkdir -p "$out"
command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1 || {
    echo "SKIP: llvm-mc/llvm-objcopy not found (set EMBCC_LLVM_MC)"; exit 0; }

cc -std=c99 -Wall -Wextra -o "$out/tasmcheck" \
   tools/tasmcheck/tasmcheck.c src/arch/thumb/asm.c \
   src/arch/thumb/emit.c src/arch/thumb/a32.c src/arch/code.c src/driver/util.c \
   src/driver/diag.c src/platform/platform_common.c src/platform/platform_posix.c || {
    echo "tasmcheck did not build"; exit 1; }

# bytes LEVEL TRIPLE NAME: the vocabulary at LEVEL, ours and llvm-mc's
bytes() {
    "$out/tasmcheck" --list "$1" > "$out/$3.s" &&
    "$out/tasmcheck" "$1" > "$out/$3.bin" 2> "$out/$3.err" || {
        echo "$3: the assembler refused its own vocabulary:"
        head -3 "$out/$3.err"; return 1; }
    "$MC" -triple="$2" -mattr=+8msecext -filetype=obj "$out/$3.s" \
        -o "$out/$3.o" 2> "$out/$3.mc" || {
        echo "$3: llvm-mc refused the vocabulary -- an entry is an"
        echo "        instruction $2 does not have:"
        head -4 "$out/$3.mc"; return 1; }
    "$OBJCOPY" -O binary --only-section=.text "$out/$3.o" "$out/$3.ref"
    cmp -s "$out/$3.bin" "$out/$3.ref" || {
        echo "$3: an encoding differs from llvm-mc's (llvm-mc first):"
        od -An -tx1 -v "$out/$3.ref" | tr -s ' ' '\n' > "$out/theirs"
        od -An -tx1 -v "$out/$3.bin" | tr -s ' ' '\n' > "$out/ours"
        diff "$out/theirs" "$out/ours" | head -8
        return 1; }
    echo "$3: $(wc -l < "$out/$3.s" | tr -d ' ') instructions encode as llvm-mc does"
}

fail=0
# ---- 1. every 32-bit instruction Baseline has ------------------------------
bytes --v8m-base thumbv8m.base-none-eabi base || fail=1
# ...and every one of them is in the list, so a form dropped from the sweep
# is unchecked by name rather than silently
missing=
for m in sdiv udiv movw movt ldrex strex ldrexb ldrexh strexb strexh clrex \
         lda ldab ldah ldaex ldaexb ldaexh stl stlb stlh stlex stlexb stlexh \
         tt ttt tta ttat sg bxns blxns mrs msr dsb dmb isb; do
    grep -q "^	$m " "$out/base.s" || grep -q "^	$m\$" "$out/base.s" ||
        missing="$missing $m"
done
[ -z "$missing" ] || { echo "not in the Baseline sweep:$missing"; fail=1; }
[ "$(wc -l < "$out/base.s")" -ge 4000 ] || {
    echo "the Baseline sweep shrank below 4000 lines"; fail=1; }

# ---- 2. the branches, through the file assembler ---------------------------
# (to labels in the file: llvm-mc leaves a branch to a global symbol for the
# linker, which is a relocation and not an encoding)
cat > "$out/br.s" <<'EOF'
	.syntax unified
	.thumb
	.text
	.globl	f
	.thumb_func
f:
top:	cbz	r0, 1f
	cbnz	r7, 1f
	bl	g
	b.w	far
1:	nop
	.space	4096
far:	bl	top
	b.w	top
	.thumb_func
g:	bx	lr
EOF
"$EMBCC" --target=thumbv8m.base-none-eabi -c "$out/br.s" -o "$out/br.o" \
    2> "$out/br.err" || { echo "the branches do not assemble:"; head -3 "$out/br.err"; fail=1; }
"$MC" -triple=thumbv8m.base-none-eabi -filetype=obj "$out/br.s" -o "$out/br.ref.o"
if [ -f "$out/br.o" ]; then
    "$OBJCOPY" -O binary --only-section=.text "$out/br.o" "$out/br.bin"
    "$OBJCOPY" -O binary --only-section=.text "$out/br.ref.o" "$out/br.ref"
    if cmp -s "$out/br.bin" "$out/br.ref"; then
        echo "bl, b.w, cbz and cbnz assemble as llvm-mc assembles them"
    else
        echo "a branch differs from llvm-mc's:"
        cmp -l "$out/br.ref" "$out/br.bin" | head -4; fail=1
    fi
fi

# ---- 3. Mainline's v8-M additions ------------------------------------------
bytes --v8m-main thumbv8m.main-none-eabi main || fail=1

# ---- 4. and nothing else -----------------------------------------------------
"$out/tasmcheck" --list > "$out/v7.s"
cat "$out/v7.s" "$out/main.s" > "$out/all.s"
"$out/tasmcheck" --each 9 "$out/all.s" > "$out/each.txt"
"$MC" -triple=thumbv8m.base-none-eabi -mattr=+8msecext -filetype=obj \
    "$out/all.s" -o /dev/null 2> "$out/all.mc"
grep -o '^[^:]*:[0-9]*:[0-9]*: error' "$out/all.mc" |
    awk -F: '{print $2}' | sort -un > "$out/mc-no.txt"
awk '$2 == "no" { print $1 }' "$out/each.txt" | sort -un > "$out/our-no.txt"
nmc=$(wc -l < "$out/mc-no.txt" | tr -d ' ')
if [ "$nmc" -lt 50 ]; then
    echo "llvm-mc refused only $nmc lines -- the comparison is not comparing"
    fail=1
elif ! cmp -s "$out/mc-no.txt" "$out/our-no.txt"; then
    echo "EmbCC's Baseline assembler and llvm-mc disagree about what the core"
    echo "has (< refused by llvm-mc only, > by EmbCC only):"
    diff "$out/mc-no.txt" "$out/our-no.txt" | grep '^[<>]' | head -8 |
    while read -r side n; do
        echo "  $side $(sed -n "${n}p" "$out/all.s" | tr -d '\t')"
    done
    fail=1
else
    echo "of $(wc -l < "$out/all.s" | tr -d ' ') ARMv7E-M and v8-M lines, EmbCC refuses at Baseline the same $nmc llvm-mc does"
fi

# The same refusal from inline asm, by name: a Thumb-2 form the core lacks,
# and a Main Extension special register.
printf 'int f(int a) { int r; __asm__("add.w %%0, %%1, #4096" : "=r"(r) : "r"(a)); return r; }\n' > "$out/ia.c"
if "$EMBCC" --target=thumbv8m.base-none-eabi -c "$out/ia.c" -o "$out/ia.o" \
       2> "$out/ia.err"; then
    echo "inline asm with add.w compiled for ARMv8-M Baseline"; fail=1
elif ! grep -q "not an ARMv8-M Baseline instruction" "$out/ia.err"; then
    echo "inline asm with add.w refused, but not by name:"; head -2 "$out/ia.err"; fail=1
fi
printf 'unsigned f(void) { unsigned r; __asm__ volatile("mrs %%0, basepri" : "=r"(r)); return r; }\n' > "$out/bp.c"
if "$EMBCC" --target=thumbv8m.base-none-eabi -c "$out/bp.c" -o "$out/bp.o" \
       2> "$out/bp.err"; then
    echo "mrs basepri compiled for ARMv8-M Baseline"; fail=1
elif ! grep -q "Main Extension" "$out/bp.err"; then
    echo "mrs basepri refused, but not by name:"; head -2 "$out/bp.err"; fail=1
fi
[ "$fail" = 0 ] && echo "inline asm outside the Baseline set is refused by name"
exit $fail
