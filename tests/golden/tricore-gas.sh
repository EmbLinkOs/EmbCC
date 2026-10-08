#!/bin/sh
# TriCore assembly in files: the control transfers against QEMU's
# translator, a .S file and file-scope asm with C on the board, the
# relocations, and what is refused.
#
#  1. tools/tricorecheck's walk (tricore-encoding.sh's referee), built with
#     every jump, call, conditional branch and LOOP ASSEMBLED FROM ITS TEXT
#     by src/arch/tricore/asm.c -- `jeq d4, d5, .+8`, `loop a3, .-6` --
#     instead of encoded by a call; QEMU executes it one instruction per
#     translation block and each must do what its operands say.
#  2. tests/golden/tricore-gas/forms.S (calls both ways between C and
#     assembly, hi:/lo: and %hi/%lo address halves, [aB]lo:sym loads, a jl
#     to a local routine, numeric labels, the conditional branches, LOOP,
#     a tail jump into C, .word sym) with main.c (a file-scope function, a
#     naked function, templates with numeric labels and branches) runs on
#     the tricore_testboard at -O0, -O1, -O2 and -Os.
#  3. forms.o carries the relocation each form names: R_TRICORE_HIADJ,
#     LO, LO2, 24REL and 32ABS.
#  4. What cannot be relocated or encoded is refused by name.
set -u
echo "TEST-MARKER tricore-gas"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=tricore-none-elf
d=tests/golden/tricore-gas
out=tests/golden/out/tricore-gas
rm -rf "$out"; mkdir -p "$out"
export EMBCC_VERIFY=1

"$EMBCC" --target=$T -c "$d/forms.S" -o "$out/forms.o" || {
    echo "EmbCC could not assemble forms.S"; exit 1; }

# ---- 1. the transfers, assembled from their text, on QEMU's translator -----
if command -v "$QEMU" >/dev/null 2>&1; then
    cc -std=c99 -Wall -Wextra -o "$out/tricorecheck" \
       tools/tricorecheck/tricorecheck.c src/arch/tricore/emit.c \
       src/arch/tricore/asm.c src/arch/code.c src/driver/util.c \
       src/driver/diag.c src/arch/target.c src/sema/type.c src/sema/ldfloat.c \
       src/platform/platform_common.c src/platform/platform_posix.c || {
        echo "tricorecheck did not build"; exit 1; }
    "$out/tricorecheck" --image-asm "$out/walk.elf" 2> "$out/walk.err" || {
        echo "the walk could not be assembled:"; cat "$out/walk.err"; exit 1; }
    na=$(sed -n 's/.*: \([0-9]*\) transfers assembled.*/\1/p' "$out/walk.err")
    [ "${na:-0}" -ge 700 ] || {
        echo "only ${na:-0} transfers were assembled from their text"; exit 1; }
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-20}" \
        "$QEMU" -M tricore_testboard -cpu tc27x -display none -monitor none \
        -kernel "$out/walk.elf" -d op -accel tcg,one-insn-per-tb=on \
        -D "$out/walk.log" > "$out/qemu.txt" 2>&1
    st=$?
    "$out/tricorecheck" --check-asm "$out/walk.log" > "$out/check.txt" || {
        head -40 "$out/check.txt"; exit 1; }
    [ "$st" = 0 ] || {
        echo "the walk did not finish cleanly on the board: status $st"; exit 1; }
    echo "$na jumps, calls, branches and loops assembled from their text do"
    echo "what their operands say in QEMU's TriCore translator"
else
    echo "SKIP the translator half: $QEMU not found"
fi

# ---- 2. on the board ---------------------------------------------------------
if command -v "$QEMU" >/dev/null 2>&1; then
    inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
    cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
       -undefined dynamic_lookup -o "$out/putc.so" \
       tests/harness/tricore/putc.c 2>/dev/null ||
        { echo "the harness's output plugin does not build"; exit 1; }
    EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
    EMBCC_TRICORE_HARNESS=$PWD/$out
    export EMBCC_TRICORE_PLUGIN EMBCC_TRICORE_HARNESS EMBLD
    "$EMBCC" --target=$T -O1 -c tests/harness/tricore/boot.c -o "$out/boot.o" &&
    "$EMBCC" --target=$T -O1 -c tests/harness/tricore/io.c -o "$out/io.o" ||
        { echo "the harness does not compile"; exit 1; }
    want1="55 107 103"
    want2="82 43 110 46 22 1 106 52 1 1234 43 42 42"
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$d/main.c" -o "$out/main$opt.o" || {
            echo "main.c $opt does not compile"; exit 1; }
        sh tests/harness/tricore/link.sh "$out/t$opt.elf" "$out/main$opt.o" \
            "$out/forms.o" > "$out/t$opt.lerr" 2>&1 || {
            echo "$opt: embld could not link it:"; head -3 "$out/t$opt.lerr"
            exit 1; }
        sh tests/harness/tricore/run.sh "$out/t$opt.elf" > "$out/t$opt.txt"
        got1=$(sed -n '1p' "$out/t$opt.txt" | sed 's/ *$//')
        got2=$(sed -n '2p' "$out/t$opt.txt" | sed 's/ *$//')
        [ "$got1" = "$want1" ] && [ "$got2" = "$want2" ] || {
            echo "$opt: the program printed"
            head -3 "$out/t$opt.txt" | sed 's/^/    /'
            echo "  not"; echo "    $want1"; echo "    $want2"; exit 1; }
    done
    echo "forms.S, a file-scope block, a naked function and templates with"
    echo "labels run on the tricore_testboard at -O0, -O1, -O2 and -Os"
else
    echo "SKIP the board half: $QEMU not found"
fi

# ---- 3. the relocations ---------------------------------------------------------
if command -v "$RE" >/dev/null 2>&1; then
    # the type is the Info word's low byte: llvm-readelf has no TriCore names
    "$RE" -r "$out/forms.o" | awk 'NF >= 5 && $2 ~ /^[0-9a-f]+$/ {
            print substr($2, length($2) - 1), $5 }' | sort -u > "$out/rel.txt"
    for r in "06 counter" "08 counter" "03 c_twice" "06 table" "07 table" \
             "02 counter" "02 tc_add"; do
        grep -q "^$r\$" "$out/rel.txt" || {
            echo "forms.o lacks relocation $r (type, symbol):"
            cat "$out/rel.txt"; exit 1; }
    done
    echo "HIADJ, LO, LO2, 24REL and 32ABS where forms.S names a symbol"
fi

# ---- 4. the refusals ---------------------------------------------------------------
refs() {            # refs WHAT PATTERN SOURCE: a .s file
    printf '%s\n' "$3" > "$out/bad.s"
    if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "a conditional branch to a symbol" "R_TRICORE_15REL" '	jne d2, 3, elsewhere'
refs "a loop to a symbol" "R_TRICORE_15REL" '	loop a2, elsewhere'
refs "hi: on addi" "is movh's or movh.a's operand" '	addi d2, d2, hi:x'
refs "lo: on movh" "is addi's" '	movh d2, lo:x'
refs "lo: on swap.w" "belongs to lea or a load or store" '	swap.w [a2]lo:x, d2'
refs "an odd target" "not an even distance" '	j .+3'
refs "a branch out of reach" "not an even distance" '	jeq d2, d3, .+32768'
refs "a constant out of range" "does not fit its 4 bits" '	jge.u d2, 16, .+8'
refs "an unknown instruction" "is not an instruction" '	frob d2'
echo "nine statements that cannot be encoded or relocated are refused by name"
