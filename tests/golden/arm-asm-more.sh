#!/bin/sh
# The ARM assemblers' multiplies (mla, mls, smull, umull, smlal, umlal,
# umaal), reversals (rev16, revsh), rrx, bit fields (bfi, bfc, sbfx, ubfx),
# ldrd/strd in each addressing form and ARM state's doubleword exclusives
# -- in Thumb state (Cortex-M) and in ARM state (armv7a-none-eabi), where
# the DSP extension's instructions (thumb-dsp.sh) are now assembled too --
# in the ways they can be wrong:
#
# 1. THE ENCODINGS. tools/tasmcheck generates the vocabulary from asm.c's
#    own tables (more_tab; and in ARM state dsp_tab and the parallel ops as
#    well), every entry with each register in every field and the
#    immediates at their ends, and hands it to EmbCC's assembler and to
#    llvm-mc: thumbv7em with +dsp, and armv7a. The bytes must be identical,
#    and EmbCC's bytes, disassembled by llvm-objdump, must read back as the
#    line that was written, so a swapped opcode or a register in the wrong
#    field is named.
# 2. THE REFUSALS. What EmbCC refuses at each Cortex-M level -- ARMv6-M,
#    v7-M, v7E-M, v8-M Mainline without and with the DSP extension, v8-M
#    Baseline -- and in ARM state is what llvm-mc refuses for that core,
#    line for line, over the vocabulary and bad.s / bad-a32.s. The few
#    UNPREDICTABLE forms llvm-mc takes and EmbCC refuses are in stricter.s
#    and stricter-a32.s, which EmbCC must refuse everywhere. The driver
#    refuses by name.
# 3. THE RESULTS, under QEMU: more.c runs each instruction through inline
#    asm on a Cortex-M4 and on a Cortex-A15 (ARM state), at -O0, -O1, -O2
#    and -Os, against a C model of each built on the host; thumb-dsp's
#    dsp.c (every DSP instruction) runs on the Cortex-A15 the same way; and
#    <arm_acle.h> in ARM state is built by EmbCC and by clang, linked into
#    the same harness, and must print the same.
set -u
echo "TEST-MARKER arm-asm-more"
. "$(dirname "$0")/../lib.sh"

MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
export EMBLD
d=tests/golden/arm-asm-more
out=tests/golden/out/arm-asm-more
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# readback BIN TRIPLE VOCAB NAME: EmbCC's bytes disassembled must be the
# lines written, less what llvm-objdump prints differently and means the
# same (`lsl #0` and `ror #0` are no shift; `pkhtb rd, rn, rm` with none
# is `pkhbt rd, rm, rn`; `cs` is `hs`).
readback() {
    st=.thumb; [ "$2" = armv7a ] && st=.arm
    { echo '	.syntax unified'; echo "	$st"; echo '	.text'
      od -An -v -tx1 "$1" | tr -s ' ' '\n' | sed '/^$/d' | sed 's/^/	.byte 0x/'
    } > "$out/$4-blob.s"
    "$MC" -triple="$2-none-eabi" -filetype=obj "$out/$4-blob.s" -o "$out/$4-blob.o" ||
        fail "llvm-mc could not wrap the emitted bytes"
    "$OD" -d --triple="$2" --mattr=+dsp "$out/$4-blob.o" |
        sed -n 's/^[ 	]*[0-9a-f]\{1,\}:[ 	]*//p' |
        sed 's/^[0-9a-f]\{4,8\}\( [0-9a-f]\{4\}\)\{0,1\}[ 	]*//' |
        sed 's/[ 	]*@.*$//; s/\.w\([ 	]\)/\1/; s/[ 	][ 	]*/ /g; s/ *$//' |
        awk '{ while (match($0, /#0x[0-9a-fA-F]+/)) {
                   h = substr($0, RSTART + 3, RLENGTH - 3); v = 0
                   for (i = 1; i <= length(h); i++)
                       v = v * 16 + index("0123456789abcdef", tolower(substr(h, i, 1))) - 1
                   $0 = substr($0, 1, RSTART - 1) "#" v substr($0, RSTART + RLENGTH)
               }
               print }' > "$out/$4-got.txt"
    sed 's/^[ 	]*//; s/[ 	][ 	]*/ /g; s/, lsl #0$//; s/, ror #0$//' "$3" |
        sed 's/^pkhtb\([a-z]*\) \([a-z0-9]*\), \([a-z0-9]*\), \([a-z0-9]*\)$/pkhbt\1 \2, \4, \3/' \
        > "$out/$4-want.txt"
    if ! diff "$out/$4-want.txt" "$out/$4-got.txt" > "$out/$4-rb.diff"; then
        echo "(< written, > what EmbCC's bytes are)"; head -8 "$out/$4-rb.diff"
        fail "$4: an instruction assembled to a different one"
    fi
}

# encodings NAME TASMCHECK-FLAG TRIPLE MC-FLAGS...: the vocabulary's bytes
# against llvm-mc's, then read back
encodings() {
    nm=$1 fl=$2 tr=$3; shift 3
    "$out/tasmcheck" "$fl" --list > "$out/$nm.s" || fail "could not list the $nm vocabulary"
    "$out/tasmcheck" "$fl" > "$out/$nm.bin" 2> "$out/$nm.err" ||
        { head -3 "$out/$nm.err"; fail "the assembler refused its own $nm vocabulary"; }
    { echo '	.syntax unified'; cat "$out/$nm.s"; } > "$out/$nm-mc.s"
    "$MC" -triple="$tr-none-eabi" "$@" -filetype=obj "$out/$nm-mc.s" \
        -o "$out/$nm.o" 2> "$out/$nm.mc" ||
        { head -4 "$out/$nm.mc"; fail "llvm-mc rejected the $nm vocabulary: an entry claims an instruction that does not exist"; }
    "$OBJCOPY" -O binary --only-section=.text "$out/$nm.o" "$out/$nm.ref"
    if ! cmp -s "$out/$nm.bin" "$out/$nm.ref"; then
        od -An -tx1 "$out/$nm.ref" > "$out/theirs"
        od -An -tx1 "$out/$nm.bin" > "$out/ours"
        diff "$out/theirs" "$out/ours" | head -6
        fail "a $nm encoding differs from llvm-mc's"
    fi
    readback "$out/$nm.bin" "$tr" "$out/$nm.s" "$nm"
}

# refusals LEVEL FILE TRIPLE MC-FLAGS...: EmbCC refuses, line for line,
# what llvm-mc refuses
refusals() {
    lv=$1 f=$2 tr=$3; shift 3
    "$out/tasmcheck" --each "$lv" "$f" |
        awk '$2 == "no" { print $1 }' | sort -un > "$out/our-$lv.txt"
    { echo '	.syntax unified'; cat "$f"; } |
        "$MC" -triple="$tr" "$@" -filetype=obj -o /dev/null 2>&1 |
        grep -o '^<stdin>:[0-9]*:[0-9]*: error' |
        awk -F: '{ print $2 - 1 }' | sort -un > "$out/mc-$lv.txt"
    nmc=$(wc -l < "$out/mc-$lv.txt" | tr -d ' ')
    [ "$nmc" -ge 20 ] || fail "llvm-mc refused only $nmc lines for $tr: the comparison is not comparing"
    if ! cmp -s "$out/mc-$lv.txt" "$out/our-$lv.txt"; then
        echo "(< refused by llvm-mc only, > by EmbCC only)"
        diff "$out/mc-$lv.txt" "$out/our-$lv.txt" | grep '^[<>]' | head -6 |
        while read -r side k; do
            echo "  $side $(sed -n "${k}p" "$f" | tr -d '\t')"
        done
        fail "EmbCC and llvm-mc disagree about what $tr $* has"
    fi
    printf '%s ' "$tr $*: $nmc;"
}

# stricter LEVEL FILE: every instruction line refused
stricter() {
    "$out/tasmcheck" --each "$1" "$2" > "$out/strict-$1.txt"
    grep -n '^	' "$2" | cut -d: -f1 | while read -r k; do
        grep -q "^$k no$" "$out/strict-$1.txt" ||
            echo "  $(sed -n "${k}p" "$2" | tr -d '\t')"
    done > "$out/strict-$1.bad"
    [ -s "$out/strict-$1.bad" ] && { cat "$out/strict-$1.bad"
        fail "EmbCC assembled an UNPREDICTABLE form at level $1"; }
    return 0
}

# ---- 1 and 2: the encodings and the refusals, against llvm-mc ------------
if command -v "$MC" >/dev/null 2>&1 && command -v "$OD" >/dev/null 2>&1 &&
   command -v "$OBJCOPY" >/dev/null 2>&1; then
    cc -std=c99 -Wall -Wextra -o "$out/tasmcheck" \
       tools/tasmcheck/tasmcheck.c src/arch/thumb/asm.c \
       src/arch/thumb/emit.c src/arch/thumb/a32.c src/arch/code.c src/driver/util.c \
       src/driver/diag.c src/platform/platform_common.c src/platform/platform_posix.c ||
        fail "tasmcheck did not build"
    encodings more --more thumbv7em -mattr=+dsp
    n=$(wc -l < "$out/more.s" | tr -d ' ')
    [ "$n" -ge 240 ] || fail "the Thumb vocabulary is only $n lines"
    echo "Thumb: $n multiply, reversal, bit-field and pair lines: llvm-mc's bytes, and they disassemble to the line written"
    encodings a32 --a32 armv7a
    na=$(wc -l < "$out/a32.s" | tr -d ' ')
    [ "$na" -ge 1600 ] || fail "the ARM-state vocabulary is only $na lines"
    echo "ARM state: $na DSP, multiply, reversal, bit-field, pair and exclusive lines: llvm-mc's bytes, and they disassemble to the line written"

    cat "$out/more.s" "$d/bad.s" > "$out/all.s"
    for c in "6 thumbv6m-none-eabi" "7 thumbv7m-none-eabi" \
             "7e thumbv7em-none-eabi -mattr=+dsp" "8 thumbv8m.main-none-eabi" \
             "8e thumbv8m.main-none-eabi -mattr=+dsp" "9 thumbv8m.base-none-eabi"; do
        # shellcheck disable=SC2086
        set -- $c
        lv=$1; shift
        refusals "$lv" "$out/all.s" "$@"
    done
    cat "$out/a32.s" "$d/bad-a32.s" > "$out/all-a32.s"
    refusals a "$out/all-a32.s" armv7a-none-eabi
    echo
    echo "EmbCC refuses at each of six Cortex-M levels, and in ARM state, what llvm-mc refuses for that core"
    for lv in 6 7 7e 8 8e 9; do stricter "$lv" "$d/stricter.s"; done
    stricter a "$d/stricter-a32.s"
    echo "and the UNPREDICTABLE forms llvm-mc takes, everywhere"

    # .s files reach the same assemblers, told the core by the target
    { echo '	.syntax unified'; echo '	.thumb'; echo '	.text'
      grep -E '^	(mla|umlal|umaal|rev16|revsh|rrxs|bfi|bfc|sbfx|ldrd|strd) ' \
          "$out/more.s" | awk 'NR % 3 == 0'; } > "$out/f.s"
    "$EMBCC" --target=thumbv7em-none-eabi -c "$out/f.s" -o "$out/f.o" 2> "$out/f.err" ||
        { head -3 "$out/f.err"; fail "a .s file of them does not assemble for thumbv7em"; }
    "$MC" -triple=thumbv7em-none-eabi -mattr=+dsp -filetype=obj "$out/f.s" -o "$out/f.ref.o"
    "$OBJCOPY" -O binary --only-section=.text "$out/f.o" "$out/f.bin"
    "$OBJCOPY" -O binary --only-section=.text "$out/f.ref.o" "$out/f.ref"
    cmp -s "$out/f.bin" "$out/f.ref" || fail "the Thumb .s file's bytes are not llvm-mc's"
    { echo '	.syntax unified'; echo '	.text'
      awk 'NR % 7 == 0' "$out/a32.s"; } > "$out/fa.s"
    "$EMBCC" --target=armv7a-none-eabi -c "$out/fa.s" -o "$out/fa.o" 2> "$out/fa.err" ||
        { head -3 "$out/fa.err"; fail "a .s file of them does not assemble for armv7a"; }
    "$MC" -triple=armv7a-none-eabi -filetype=obj "$out/fa.s" -o "$out/fa.ref.o"
    "$OBJCOPY" -O binary --only-section=.text "$out/fa.o" "$out/fa.bin"
    "$OBJCOPY" -O binary --only-section=.text "$out/fa.ref.o" "$out/fa.ref"
    cmp -s "$out/fa.bin" "$out/fa.ref" || fail "the ARM-state .s file's bytes are not llvm-mc's"
    echo "a .s file of them assembles as llvm-mc assembles it, in Thumb and in ARM state"
else
    echo "SKIP the encodings and refusals: llvm-mc/llvm-objdump/llvm-objcopy not found"
fi

# The driver refuses by name: a Thumb-2 instruction on ARMv6-M and v8-M
# Baseline, umaal without the DSP extension, a doubleword exclusive on any
# Cortex-M, an odd ldrd pair in ARM state.
refuse() {   # target, the message, then the asm
    t=$1 msg=$2; shift 2
    printf 'unsigned f(unsigned a, unsigned b) { unsigned r = a; __asm__("%s" : "+r"(r) : "r"(b) : "memory"); return r; }\n' "$1" > "$out/ia.c"
    # shellcheck disable=SC2086
    if "$EMBCC" --target=$t -c "$out/ia.c" -o "$out/ia.o" 2> "$out/ia.err"; then
        fail "inline \"$1\" compiled for $t"
    fi
    grep -F "$msg" "$out/ia.err" | grep -qw "${1%% *}" ||
        { cat "$out/ia.err"; fail "inline \"$1\" on $t refused, but not by name"; }
}
for t in thumbv6m-none-eabi thumbv8m.base-none-eabi; do
    refuse $t 'is not an ARMv' 'mla %0, %0, %1, %1'
    refuse $t 'is not an ARMv' 'bfi %0, %1, #3, #4'
    refuse $t 'is not an ARMv' 'ldrd %0, %1, [sp]'
done
refuse thumbv7m-none-eabi 'is an instruction of the DSP extension' 'umaal %0, %1, %1, %1'
refuse thumbv7em-none-eabi 'ldrexd is not an M-profile instruction' 'ldrexd r2, r3, [%1]'
refuse armv7a-none-eabi 'ldrd: in ARM state the pair is an even register' 'ldrd r1, r2, [%1]'
for t in thumbv7m-none-eabi thumbv8m.main-none-eabi thumbv7em-none-eabi armv7a-none-eabi; do
    printf 'unsigned f(unsigned a, unsigned b) { unsigned r; __asm__("mls %%0, %%1, %%2, %%1\\n\\tbfc %%0, #1, #2\\n\\trevsh %%0, %%0" : "=&r"(r) : "r"(a), "r"(b)); return r; }\n' > "$out/ok.c"
    "$EMBCC" --target=$t -c "$out/ok.c" -o "$out/ok.o" 2> "$out/ok.err" ||
        { cat "$out/ok.err"; fail "inline mls/bfc/revsh does not compile for $t"; }
done
echo "inline asm takes them where the core has them, and refuses them by name elsewhere"

# ---- 3. the results, on a Cortex-M4 and a Cortex-A15 ----------------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP the runs: $QEMU absent"; exit 0; }
export EMBCC_THUMB_HARNESS="$PWD/$out/th" EMBCC_A32_HARNESS="$PWD/$out/ah"
mkdir -p "$out/th" "$out/ah"
for f in boot io; do
    "$EMBCC" --target=thumbv7em-none-eabi -c "tests/harness/thumb/$f.c" -o "$out/th/$f.o" &&
    "$EMBCC" --target=armv7a-none-eabi -c "tests/harness/arm-a32/$f.c" -o "$out/ah/$f.o" ||
        fail "the harness does not compile"
done
EMBCC="$EMBCC" sh tools/build-rt.sh armv7a-none-eabi "$out/alib" > "$out/alib.log" 2>&1 ||
    { tail -3 "$out/alib.log"; fail "lib/rt does not build for armv7a"; }
# the Thumb harness's UART on lm3s6965evb with a Cortex-M4 in it; ARM
# state on virt's Cortex-A15. Either prints up to ==END==.
run_m4() {
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-20}" --until "==END==" "$QEMU" \
        -M lm3s6965evb -cpu cortex-m4 -nographic -kernel "$1" 2>/dev/null |
        tr -d '\r' | sed '/==END==/,$d'
}
run_a15() { sh tests/harness/arm-a32/run.sh "$1" | tr -d '\r' | sed '/==END==/,$d'; }
link_m4() { sh tests/harness/thumb/link.sh "$@"; }
link_a15() { sh tests/harness/arm-a32/link.sh "$@" "$out/alib/librt.a"; }

# check NAME SOURCE MODEL: SOURCE on both boards at four levels against
# MODEL (the M4 skips its ARM-state-only lines)
check() {
    nm=$1 src=$2 model=$3
    grep -v '^ldrexd' "$model" > "$out/$nm-model-m4.txt"
    for b in m4 a15; do
        [ $b = m4 ] && T=thumbv7em-none-eabi || T=armv7a-none-eabi
        [ $b = m4 ] && want="$out/$nm-model-m4.txt" || want=$model
        [ "$nm" = dsp ] && [ $b = m4 ] && continue    # thumb-dsp.sh's
        for opt in -O0 -O1 -O2 -Os; do
            o="$out/$nm-$b$opt"
            "$EMBCC" --target=$T $opt -c "$src" -o "$o.o" || fail "$b $opt: $src does not compile"
            link_$b "$o.elf" "$o.o" || fail "$b $opt: $src does not link"
            run_$b "$o.elf" > "$o.txt"
            if ! cmp -s "$want" "$o.txt"; then
                echo "(< the model, > the $b)"
                diff "$want" "$o.txt" | head -6
                fail "$b $opt: an instruction of $src computed what its model does not"
            fi
        done
    done
}
cc -std=gnu99 -DHOST -O1 -o "$out/model" "$d/more.c" || fail "the host model does not build"
"$out/model" | sed '/==END==/,$d' > "$out/model.txt"
[ "$(wc -l < "$out/model.txt" | tr -d ' ')" -ge 24 ] || fail "the model printed too little"
check more "$d/more.c" "$out/model.txt"
echo "each instruction computes, on a Cortex-M4 and a Cortex-A15 at -O0, -O1, -O2 and -Os, what its C model does"
cc -std=gnu99 -DHOST -O1 -o "$out/dmodel" tests/golden/thumb-dsp/dsp.c || fail "the DSP model does not build"
"$out/dmodel" | sed '/==END==/,$d' > "$out/dmodel.txt"
[ "$(wc -l < "$out/dmodel.txt" | tr -d ' ')" -ge 80 ] || fail "the DSP model printed too little"
check dsp tests/golden/thumb-dsp/dsp.c "$out/dmodel.txt"
echo "every DSP instruction computes, in ARM state on a Cortex-A15 at four levels, what its C model does"

if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" --target=armv7a-none-eabi -mfloat-abi=soft -fsyntax-only -x c /dev/null 2>/dev/null; then
    T=armv7a-none-eabi
    "$CLANG" --target=$T -mfloat-abi=soft -ffreestanding -O2 -c tests/golden/thumb-dsp/acle.c \
        -o "$out/acle-clang.o" 2> "$out/acle.cerr" ||
        { head -3 "$out/acle.cerr"; fail "clang does not build acle.c for $T"; }
    link_a15 "$out/acle-clang.elf" "$out/acle-clang.o" || fail "clang's acle.c does not link"
    run_a15 "$out/acle-clang.elf" > "$out/acle-clang.txt"
    [ "$(wc -l < "$out/acle-clang.txt" | tr -d ' ')" -ge 50 ] || fail "clang's acle.c printed too little"
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c tests/golden/thumb-dsp/acle.c -o "$out/acle$opt.o" ||
            fail "$opt: <arm_acle.h> does not compile in ARM state"
        link_a15 "$out/acle$opt.elf" "$out/acle$opt.o" || fail "$opt: acle.c does not link"
        run_a15 "$out/acle$opt.elf" > "$out/acle$opt.txt"
        cmp -s "$out/acle-clang.txt" "$out/acle$opt.txt" ||
            { diff "$out/acle-clang.txt" "$out/acle$opt.txt" | head -6
              fail "$opt: <arm_acle.h> in ARM state disagrees with clang's"; }
    done
    echo "<arm_acle.h> in ARM state: every DSP and SIMD32 intrinsic agrees with clang's builtins"
else
    echo "SKIP the clang comparison: no $CLANG for armv7a"
fi
echo "ok arm-asm-more"
