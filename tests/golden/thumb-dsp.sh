#!/bin/sh
# The DSP extension (ARMv7E-M: Cortex-M4/M7; ARMv8-M Mainline with it:
# Cortex-M33), from inline asm, .s files, <arm_acle.h> and CMSIS's
# cmsis_gcc.h, in the ways it can be wrong:
#
# 1. THE ENCODINGS. tools/tasmcheck hands the DSP vocabulary -- generated
#    from asm.c's own tables, every entry with each register in every field
#    and the immediates at their ends -- to EmbCC's assembler, and the same
#    lines to llvm-mc. The bytes must be identical; and EmbCC's bytes,
#    disassembled by llvm-objdump, must read back as the very line that was
#    written, mnemonic and operands, so a swapped opcode or a register in
#    the wrong field is named, not just counted.
# 2. THE REFUSALS. What EmbCC refuses at each level -- ARMv6-M, v7-M, v7E-M,
#    v8-M Mainline without and with the extension, v8-M Baseline -- is what
#    llvm-mc refuses for that core, line for line, over the vocabulary and
#    bad.s (sp/pc, bounds, shifts, rotations); and the driver refuses by
#    name.
# 3. THE RESULTS, on a Cortex-M4 under QEMU: dsp.c runs every instruction
#    through inline asm at four optimisation levels against a C model of
#    each built on the host; acle.c (<arm_acle.h>) and cmsis.c (cmsis_gcc.h)
#    are built by EmbCC and by clang, linked into the same harness, and must
#    print the same.
#
# 4. THE MACROS are tests/golden/predef.sh's: every Cortex-M -mcpu/-march
#    against clang's __ARM_ARCH*/__ARM_FEATURE_*.
set -u
echo "TEST-MARKER thumb-dsp"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
d=tests/golden/thumb-dsp
out=tests/golden/out/thumb-dsp
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- 1. the encodings, against llvm-mc ---------------------------------------
if command -v "$MC" >/dev/null 2>&1 && command -v "$OD" >/dev/null 2>&1 &&
   command -v "$OBJCOPY" >/dev/null 2>&1; then
    cc -std=c99 -Wall -Wextra -o "$out/tasmcheck" \
       tools/tasmcheck/tasmcheck.c src/arch/thumb/asm.c \
       src/arch/thumb/emit.c src/arch/thumb/a32.c src/arch/code.c src/driver/util.c \
       src/driver/diag.c src/platform/platform_common.c src/platform/platform_posix.c ||
        fail "tasmcheck did not build"
    "$out/tasmcheck" --dsp --list > "$out/v.s" || fail "could not list the vocabulary"
    n=$(wc -l < "$out/v.s" | tr -d ' ')
    [ "$n" -ge 1300 ] || fail "the DSP vocabulary is only $n lines"
    "$out/tasmcheck" --dsp > "$out/v.bin" 2> "$out/v.err" ||
        { head -3 "$out/v.err"; fail "the assembler refused its own DSP vocabulary"; }
    { echo '	.syntax unified'; cat "$out/v.s"; } > "$out/v-mc.s"
    "$MC" -triple=thumbv7em-none-eabi -mattr=+dsp -filetype=obj "$out/v-mc.s" \
        -o "$out/v.o" 2> "$out/v.mc" ||
        { head -4 "$out/v.mc"; fail "llvm-mc rejected the vocabulary: an entry claims an instruction that does not exist"; }
    "$OBJCOPY" -O binary --only-section=.text "$out/v.o" "$out/v.ref"
    if ! cmp -s "$out/v.bin" "$out/v.ref"; then
        od -An -tx1 "$out/v.ref" > "$out/theirs"
        od -An -tx1 "$out/v.bin" > "$out/ours"
        diff "$out/theirs" "$out/ours" | head -6
        fail "a DSP encoding differs from llvm-mc's"
    fi
    # EmbCC's bytes read back: the line written, less what llvm-objdump
    # prints differently and means the same (`lsl #0` and `ror #0` are no
    # shift; `pkhtb rd, rn, rm` with none is `pkhbt rd, rm, rn`).
    { echo '	.syntax unified'; echo '	.thumb'; echo '	.text'
      od -An -v -tx1 "$out/v.bin" | tr -s ' ' '\n' | sed '/^$/d' | sed 's/^/	.byte 0x/'
    } > "$out/blob.s"
    "$MC" -triple=thumbv7em-none-eabi -filetype=obj "$out/blob.s" -o "$out/blob.o" ||
        fail "llvm-mc could not wrap the emitted bytes"
    "$OD" -d --triple=thumbv7em --mattr=+dsp "$out/blob.o" |
        sed -n 's/^[ 	]*[0-9a-f]\{1,\}:[ 	]*//p' |
        sed 's/^\(\([0-9a-f][0-9a-f][0-9a-f][0-9a-f] \)\{1,2\}\)[ 	]*//' |
        sed 's/[ 	]*@.*$//; s/\.w\([ 	]\)/\1/; s/[ 	][ 	]*/ /g; s/ *$//' |
        awk '{ while (match($0, /#0x[0-9a-fA-F]+/)) {
                   h = substr($0, RSTART + 3, RLENGTH - 3); v = 0
                   for (i = 1; i <= length(h); i++)
                       v = v * 16 + index("0123456789abcdef", tolower(substr(h, i, 1))) - 1
                   $0 = substr($0, 1, RSTART - 1) "#" v substr($0, RSTART + RLENGTH)
               }
               print }' > "$out/got.txt"
    sed 's/^[ 	]*//; s/[ 	][ 	]*/ /g; s/, lsl #0$//; s/, ror #0$//' "$out/v.s" |
        sed 's/^pkhtb \([a-z0-9]*\), \([a-z0-9]*\), \([a-z0-9]*\)$/pkhbt \1, \3, \2/' \
        > "$out/want.txt"
    if ! diff "$out/want.txt" "$out/got.txt" > "$out/rb.diff"; then
        echo "(< written, > what EmbCC's bytes are)"; head -8 "$out/rb.diff"
        fail "an instruction assembled to a different one"
    fi
    echo "$n DSP, saturate and extend lines: llvm-mc's bytes, and they disassemble to the line written"

    # ---- 2. the refusals, core by core ----
    cat "$out/v.s" "$d/bad.s" > "$out/all.s"
    for c in "6 thumbv6m-none-eabi" "7 thumbv7m-none-eabi" \
             "7e thumbv7em-none-eabi -mattr=+dsp" "8 thumbv8m.main-none-eabi" \
             "8e thumbv8m.main-none-eabi -mattr=+dsp" "9 thumbv8m.base-none-eabi"; do
        set -- $c
        lv=$1 tr=$2; shift 2
        "$out/tasmcheck" --each "$lv" "$out/all.s" |
            awk '$2 == "no" { print $1 }' | sort -un > "$out/our-$lv.txt"
        { echo '	.syntax unified'; cat "$out/all.s"; } |
            "$MC" -triple="$tr" "$@" -filetype=obj -o /dev/null 2>&1 |
            grep -o '^<stdin>:[0-9]*:[0-9]*: error' |
            awk -F: '{ print $2 - 1 }' | sort -un > "$out/mc-$lv.txt"
        nmc=$(wc -l < "$out/mc-$lv.txt" | tr -d ' ')
        [ "$nmc" -ge 20 ] || fail "llvm-mc refused only $nmc lines for $tr: the comparison is not comparing"
        if ! cmp -s "$out/mc-$lv.txt" "$out/our-$lv.txt"; then
            echo "(< refused by llvm-mc only, > by EmbCC only)"
            diff "$out/mc-$lv.txt" "$out/our-$lv.txt" | grep '^[<>]' | head -6 |
            while read -r side k; do
                echo "  $side $(sed -n "${k}p" "$out/all.s" | tr -d '\t')"
            done
            fail "EmbCC and llvm-mc disagree about what $tr $* has"
        fi
        printf '%s ' "$tr $*: $nmc;"
    done
    echo
    echo "of $(wc -l < "$out/all.s" | tr -d ' ') lines, EmbCC refuses at each of six levels what llvm-mc refuses for that core"

    # .s files reach the same assembler, told the core by the target
    { echo '	.syntax unified'; echo '	.thumb'; echo '	.text'
      grep -E '^	(sadd16|uqsub8|smlald|pkhtb|ssat16|sxtab16|smmlsr|usada8|qdsub|smlawt) ' \
          "$out/v.s" | head -40; } > "$out/f.s"
    "$EMBCC" --target=thumbv7em-none-eabi -c "$out/f.s" -o "$out/f.o" 2> "$out/f.err" ||
        { head -3 "$out/f.err"; fail "a .s file of DSP instructions does not assemble for thumbv7em"; }
    "$MC" -triple=thumbv7em-none-eabi -mattr=+dsp -filetype=obj "$out/f.s" -o "$out/f.ref.o"
    "$OBJCOPY" -O binary --only-section=.text "$out/f.o" "$out/f.bin"
    "$OBJCOPY" -O binary --only-section=.text "$out/f.ref.o" "$out/f.ref"
    cmp -s "$out/f.bin" "$out/f.ref" || fail "the .s file's bytes are not llvm-mc's"
    "$EMBCC" --target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -c "$out/f.s" \
        -o "$out/f33.o" || fail "the .s file does not assemble for a Cortex-M33"
    if "$EMBCC" --target=thumbv7m-none-eabi -c "$out/f.s" -o "$out/f7.o" 2> "$out/f7.err"; then
        fail "a .s file of DSP instructions assembled for ARMv7-M"
    fi
    grep -q "DSP extension" "$out/f7.err" ||
        { cat "$out/f7.err"; fail "the .s refusal on ARMv7-M does not say why"; }
    echo "a .s file assembles them for v7E-M and v8-M+DSP, as llvm-mc does, and not for v7-M"
else
    echo "SKIP the encodings and refusals: llvm-mc/llvm-objdump/llvm-objcopy not found"
fi

# Inline asm refuses by name where the core lacks the extension -- and
# that includes thumbv8m.main without -mcpu=cortex-m33 or +dsp, as clang's
printf 'unsigned f(unsigned a, unsigned b) { unsigned r; __asm__("sadd16 %%0, %%1, %%2" : "=r"(r) : "r"(a), "r"(b)); return r; }\n' > "$out/ia.c"
for t in "thumbv7m-none-eabi" "thumbv6m-none-eabi" "thumbv8m.main-none-eabi" \
         "thumbv8m.base-none-eabi" "thumbv7em-none-eabi -mcpu=cortex-m3" \
         "thumbv8m.main-none-eabi -march=armv8-m.main+nodsp"; do
    # shellcheck disable=SC2086
    if "$EMBCC" --target=$t -c "$out/ia.c" -o "$out/ia.o" 2> "$out/ia.err"; then
        fail "inline sadd16 compiled for $t"
    fi
    grep -q '"sadd16" is an instruction of the DSP extension' "$out/ia.err" ||
        { cat "$out/ia.err"; fail "inline sadd16 on $t refused, but not by name"; }
done
for t in "thumbv7em-none-eabi" "thumbv7em-none-eabihf" "thumbv7m-none-eabi -mcpu=cortex-m4" \
         "thumbv8m.main-none-eabi -mcpu=cortex-m33" "thumbv7em-none-eabi -mcpu=cortex-m7" \
         "thumbv8m.main-none-eabi -march=armv8-m.main+dsp"; do
    # shellcheck disable=SC2086
    "$EMBCC" --target=$t -c "$out/ia.c" -o "$out/ia.o" 2> "$out/ia.err" ||
        { cat "$out/ia.err"; fail "inline sadd16 does not compile for $t"; }
done
if "$EMBCC" --target=armv7a-none-eabi -c "$out/ia.c" -o "$out/ia.o" 2> "$out/ia.err"; then
    fail "inline sadd16 compiled for ARM state, which EmbCC cannot assemble it in"
fi
grep -q "ARM-state" "$out/ia.err" || { cat "$out/ia.err"; fail "the ARM-state refusal does not say why"; }
echo "inline asm takes them on v7E-M and v8-M+DSP, and refuses them by name elsewhere"

# ---- 3. the results, on a Cortex-M4 ------------------------------------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the runs: $QEMU absent"; exit 0; }
T=thumbv7em-none-eabi
export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "tests/harness/thumb/$f.c" -o "$out/$f.o" ||
        fail "the harness does not compile"
done
# lm3s6965evb with a Cortex-M4 in it: the harness's UART, and a core with
# the extension (its own Cortex-M3 takes these as UNDEFINED)
run() {
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-20}" --until "==END==" "$QEMU" \
        -M lm3s6965evb -cpu cortex-m4 -nographic -kernel "$1" 2>/dev/null |
        tr -d '\r' | sed '/==END==/d'
}
cc -std=gnu99 -DHOST -O1 -o "$out/model" "$d/dsp.c" || fail "the host model does not build"
"$out/model" | sed '/==END==/d' > "$out/model.txt"
[ "$(wc -l < "$out/model.txt" | tr -d ' ')" -ge 80 ] || fail "the model printed too little"
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c "$d/dsp.c" -o "$out/dsp$opt.o" ||
        fail "$opt: dsp.c does not compile"
    sh tests/harness/thumb/link.sh "$out/dsp$opt.elf" "$out/dsp$opt.o" ||
        fail "$opt: dsp.c does not link"
    run "$out/dsp$opt.elf" > "$out/dsp$opt.txt"
    if ! cmp -s "$out/model.txt" "$out/dsp$opt.txt"; then
        echo "(< the model, > the Cortex-M4)"
        diff "$out/model.txt" "$out/dsp$opt.txt" | head -6
        fail "$opt: an instruction computed what its model does not"
    fi
done
echo "every instruction computes, at -O0, -O1, -O2 and -Os, what its C model does"

if command -v "$CLANG" >/dev/null 2>&1; then
    CF="--target=$T -mcpu=cortex-m4 -mfloat-abi=soft -ffreestanding -O2"
    pair() {   # name source flags...: built by both, run, compared
        nm=$1 src=$2; shift 2
        # shellcheck disable=SC2086
        "$CLANG" $CF "$@" -c "$src" -o "$out/$nm-clang.o" 2> "$out/$nm.cerr" ||
            { head -3 "$out/$nm.cerr"; fail "clang does not build $src"; }
        for opt in -O0 -O2; do
            "$EMBCC" --target=$T $opt "$@" -c "$src" -o "$out/$nm$opt.o" ||
                fail "$opt: EmbCC does not build $src"
        done
        for v in -clang -O0 -O2; do
            sh tests/harness/thumb/link.sh "$out/$nm$v.elf" "$out/$nm$v.o" ||
                fail "$nm$v does not link"
            run "$out/$nm$v.elf" > "$out/$nm$v.txt"
        done
        [ "$(wc -l < "$out/$nm-clang.txt" | tr -d ' ')" -ge 50 ] ||
            fail "clang's $nm printed too little"
        for v in -O0 -O2; do
            cmp -s "$out/$nm-clang.txt" "$out/$nm$v.txt" ||
                { diff "$out/$nm-clang.txt" "$out/$nm$v.txt" | head -6
                  fail "$nm $v: EmbCC's build disagrees with clang's"; }
        done
    }
    pair acle "$d/acle.c"
    echo "<arm_acle.h>: every DSP and SIMD32 intrinsic agrees with clang's builtins"
    CM=${EMBREF:-$HOME/EmbRef}/CMSIS_5/CMSIS/Core/Include
    if [ -f "$CM/cmsis_gcc.h" ]; then
        pair cmsis "$d/cmsis.c" -I"$CM" -fgnuc-version=4.2.1
        echo "CMSIS's cmsis_gcc.h: every SIMD intrinsic agrees with clang's build of it"
    else
        echo "SKIP CMSIS: no $CM (git clone ARM-software/CMSIS_5 there)"
    fi
else
    echo "SKIP the clang comparisons: no $CLANG"
fi
echo "ok thumb-dsp"
