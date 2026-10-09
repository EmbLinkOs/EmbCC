#!/bin/sh
# TrustZone-M: ACLE's CMSE, the Secure side of ARMv8-M (-mcmse), at both
# profiles -- Mainline (thumbv8m.main-none-eabi) and Baseline
# (thumbv8m.base-none-eabi).
#
#  1. THE SHAPE, against clang. For every cmse_nonsecure_entry function in
#     thumbv8m-cmse/clear.c, the registers its return overwrites with lr and
#     whether it writes APSR from lr before BXNS; for every call through a
#     cmse_nonsecure_call pointer, the registers holding the target at the
#     BLXNS and whether APSR is written from it -- the same sets clang
#     -mcmse -mfloat-abi=soft gives, at -O0, -O1, -O2 and -Os
#     (thumbv8m-cmse/shape.awk reads both listings).
#  2. TWO IMAGES ON THE BOARD. A Secure image (secure.c, linked by embld
#     with thumbv8m-cmse/secure.ld, --cmse-implib --out-implib) boots the
#     mps2-an505's Cortex-M33 in the Secure state, gives the Non-secure state
#     the upper half of SSRAM1 in the memory protection controller and the
#     SAU, marks the SG veneers Non-secure Callable in the SAU and the IDAU
#     (NSCCFG), and enters the Non-secure image's reset handler through a
#     cmse_nonsecure_call pointer. The Non-secure image (ns.c), linked
#     against the import library alone, calls the entry functions through
#     their veneers -- out of a BL's reach, so through embld's long-branch
#     veneers too -- and reads back what each crossing left: r1-r3, r12 and
#     APSR after an entry function that put a secret in all of them, and,
#     in a Non-secure callee called back through a cmse_nonsecure_call
#     pointer, every register that is not an argument and APSR, where the
#     Secure caller had the secret. It also hands a Non-secure and a Secure
#     buffer to an entry function that checks them with <arm_cmse.h>'s
#     cmse_check_address_range, and asks TTA about both; and a Non-secure
#     function pointer is called with its Thumb bit still set, which the
#     call must clear. Both profiles, at
#     four optimisation levels. QEMU has no Cortex-M23, so the Baseline
#     images run on the M33 too (tests/golden/thumbv8mbase-exec.sh scans
#     what Baseline code contains).
#  3. THE LINK. The veneers are `sg; b.w __acle_se_<f>` in .gnu.sgstubs and
#     <f> names them; the import library is a relocatable holding absolute
#     Thumb function symbols at the veneers; and a Non-secure link without
#     it fails.
#  4. THE REFUSALS, by name: what CMSE forbids (arguments on the stack, a
#     result through memory, a static or variadic entry function), what
#     EmbCC does not do (an FPU with -mcmse, --in-implib,
#     cmse_nonsecure_caller), and -mcmse below ARMv8-M.
set -u
echo "TEST-MARKER thumbv8m-cmse"
. "$(dirname "$0")/../lib.sh"

D=tests/golden/thumbv8m-cmse
out=tests/golden/out/thumbv8m-cmse
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
EMBLD=${EMBLD:-./embld}
fail=0

# ---- 1. the shape, against clang -------------------------------------------
if command -v "$CLANG" >/dev/null 2>&1 && command -v "$OD" >/dev/null 2>&1; then
    for p in main base; do
        T=thumbv8m.$p-none-eabi
        "$CLANG" --target=$T -mfloat-abi=soft -mcmse -O2 -c $D/clear.c \
            -o "$out/cl-$p.o" 2>/dev/null || { echo "clang: $T"; fail=1; continue; }
        "$OD" -d --triple=thumbv8m.$p --mattr=+8msecext "$out/cl-$p.o" |
            awk -f $D/shape.awk | sort > "$out/cl-$p.shape"
        [ "$(wc -l < "$out/cl-$p.shape" | tr -d ' ')" -ge 60 ] || {
            echo "$p: clang's listing has too few CMSE shapes to compare"; fail=1; }
        for O in -O0 -O1 -O2 -Os; do
            "$EMBCC" --target=$T -mcmse $O -c $D/clear.c -o "$out/em-$p$O.o" \
                2> "$out/em.err" || { echo "$p $O: clear.c:"; head -3 "$out/em.err"
                                      fail=1; continue; }
            "$OD" -d --triple=thumbv8m.$p --mattr=+8msecext "$out/em-$p$O.o" |
                awk -f $D/shape.awk | grep -v '^__acle_se_' |
                sort > "$out/em-$p$O.shape"
            diff "$out/cl-$p.shape" "$out/em-$p$O.shape" > "$out/$p$O.diff" || {
                echo "$p $O: what the CMSE crossings clear differs from clang's"
                echo "  (< clang, > EmbCC):"; head -8 "$out/$p$O.diff"; fail=1; }
        done
    done
    [ "$fail" = 0 ] && echo "entry returns and Non-secure calls clear what clang's clear, at both profiles and four levels"
else
    echo "SKIP the clang comparison: no $CLANG or $OD"
fi

# ---- 2 and 3. two images on the board, and the link --------------------------
build() {          # build PROFILE OPT -> $out/PROFILE$OPT/{secure,ns}.elf
    T=thumbv8m.$1-none-eabi; b=$out/$1$2; mkdir -p "$b"
    "$EMBCC" --target=$T -mcmse $2 -c $D/secure.c -o "$b/secure.o" &&
    "$EMBCC" --target=$T -mcmse $2 -c $D/s_boot.c -o "$b/s_boot.o" &&
    "$EMBLD" -T $D/secure.ld --cmse-implib --out-implib="$b/implib.o" \
        "$b/s_boot.o" "$b/secure.o" -o "$b/secure.elf" &&
    "$EMBCC" --target=$T $2 -c $D/ns.c -o "$b/ns.o" &&
    "$EMBCC" --target=$T $2 -c $D/ns_boot.c -o "$b/ns_boot.o" &&
    "$EMBLD" -e ns_reset -Ttext 0x00200000 -Tdata 0x00300000 \
        "$b/ns_boot.o" "$b/ns.o" "$b/implib.o" -o "$b/ns.elf"
}
run() {            # run PROFILE OPT
    b=$out/$1$2
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-10}" --until '==EXIT ' \
        "$QEMU" -M mps2-an505 -cpu cortex-m33 -nographic \
        -kernel "$b/secure.elf" -device loader,file="$b/ns.elf" \
        > "$b/run.txt" 2>&1
    grep -q '^==EXIT 0==' "$b/run.txt" &&
    [ "$(grep -c '^ok ' "$b/run.txt")" = 12 ]
}
for p in main base; do
    for O in -O0 -O1 -O2 -Os; do
        build $p $O 2> "$out/build.err" || {
            echo "$p $O: the two images do not build:"; head -4 "$out/build.err"
            fail=1; continue; }
        if ! command -v "$QEMU" >/dev/null 2>&1 ||
           ! "$QEMU" -M help 2>/dev/null | grep -q '^mps2-an505'; then
            continue
        fi
        run $p $O || {
            echo "$p $O: the Secure and Non-secure images disagree:"
            grep -v '^ok ' "$out/$p$O/run.txt" | head -6; fail=1; }
    done
done
if command -v "$QEMU" >/dev/null 2>&1; then
    [ "$fail" = 0 ] && echo "both profiles, four levels: the Non-secure image enters through the veneers, finds no secret in any register or flag either way, and the Secure side checks its pointers"
else
    echo "SKIP the board: no $QEMU"
fi

b=$out/main-O1
if [ -f "$b/secure.elf" ] && command -v "$OD" >/dev/null 2>&1; then
    # the veneers: each `sg; b.w` to its __acle_se_ twin, named by the entry
    "$OD" -d --triple=thumbv8m.main --mattr=+8msecext -j .gnu.sgstubs \
        "$b/secure.elf" > "$b/sg.txt"
    nv=$(grep -c '	sg$' "$b/sg.txt")
    nb=$(grep -c 'b\.w	.*<__acle_se_' "$b/sg.txt")
    [ "$nv" = 8 ] && [ "$nb" = 8 ] || {
        echo "the secure gateway veneers are not 8 x sg; b.w __acle_se_ ($nv, $nb):"
        head -8 "$b/sg.txt"; fail=1; }
    for f in s_add s_call_back s_call_raw s_done s_report s_secret s_sum s_tta; do
        grep -q "<$f>:" "$b/sg.txt" || { echo "$f does not name its veneer"; fail=1; }
    done
    # the import library: a relocatable, each symbol absolute, a Thumb
    # function, at its veneer
    llvm-readelf -h "$b/implib.o" | grep -q 'REL (Relocatable' || {
        echo "the import library is not a relocatable object"; fail=1; }
    llvm-readelf -s "$b/implib.o" | awk '$7 == "ABS" && $4 == "FUNC" &&
        $5 == "GLOBAL" { print $8, $2 }' | sort > "$b/implib.txt"
    [ "$(wc -l < "$b/implib.txt" | tr -d ' ')" = 8 ] || {
        echo "the import library does not hold the 8 entry functions:"
        cat "$b/implib.txt"; fail=1; }
    while read -r name val; do
        grep -q "<$name>:" "$b/sg.txt" || continue
        at=$(sed -n "s/^\([0-9a-f]*\) <$name>:/\1/p" "$b/sg.txt")
        [ "$(printf '%08x' $((0x$at + 1)))" = "$val" ] || {
            echo "the import library's $name is $val, and its veneer is at $at"
            fail=1; }
    done < "$b/implib.txt"
    if "$EMBLD" -e ns_reset -Ttext 0x00200000 -Tdata 0x00300000 \
         "$b/ns_boot.o" "$b/ns.o" -o "$b/nolib.elf" 2> "$b/nolib.err"; then
        echo "the Non-secure image linked without the import library"; fail=1
    fi
    [ "$fail" = 0 ] && echo "the veneers are sg; b.w __acle_se_*, and the import library names them, as absolute Thumb functions"
fi

# ---- 4. the refusals ------------------------------------------------------------
refuse() {         # refuse TAG PATTERN COMMAND... : must fail, naming it
    tag=$1; pat=$2; shift 2
    if "$@" > "$out/r.out" 2> "$out/r.err"; then
        echo "$tag: accepted"; fail=1
    elif ! grep -q "$pat" "$out/r.err"; then
        echo "$tag: refused, but not by name:"; head -2 "$out/r.err"; fail=1
    fi
}
M=thumbv8m.main-none-eabi
printf 'int __attribute__((cmse_nonsecure_entry)) f(int a, int b, int c, int d, int e) { return e; }\n' > "$out/stk.c"
refuse "stack arguments" "requires arguments on the stack" \
    "$EMBCC" --target=$M -mcmse -c "$out/stk.c" -o "$out/x.o"
refuse "stack arguments, Baseline" "requires arguments on the stack" \
    "$EMBCC" --target=thumbv8m.base-none-eabi -mcmse -c "$out/stk.c" -o "$out/x.o"
printf 'struct s { int a[5]; };\nstruct s __attribute__((cmse_nonsecure_entry)) f(int a) { struct s r = {{a}}; return r; }\n' > "$out/sret.c"
refuse "result through memory" "return its value through memory" \
    "$EMBCC" --target=$M -mcmse -c "$out/sret.c" -o "$out/x.o"
printf 'static int __attribute__((cmse_nonsecure_entry)) f(int a) { return a; }\nint g(void) { return f(1); }\n' > "$out/static.c"
refuse "static entry" "internal linkage" \
    "$EMBCC" --target=$M -mcmse -c "$out/static.c" -o "$out/x.o"
printf 'int __attribute__((cmse_nonsecure_entry)) f(int a, ...) { return a; }\n' > "$out/va.c"
refuse "variadic entry" "is variadic" \
    "$EMBCC" --target=$M -mcmse -c "$out/va.c" -o "$out/x.o"
printf 'typedef int __attribute__((cmse_nonsecure_call)) t(int, int, int, int, int);\nint g(t *f) { return f(1, 2, 3, 4, 5); }\n' > "$out/nsstk.c"
refuse "Non-secure call with stack arguments" "passes arguments on the stack" \
    "$EMBCC" --target=$M -mcmse -c "$out/nsstk.c" -o "$out/x.o"
printf 'int __attribute__((cmse_nonsecure_call)) x;\n' > "$out/misplaced.c"
refuse "cmse_nonsecure_call on an int" "applies to a function type" \
    "$EMBCC" --target=$M -mcmse -c "$out/misplaced.c" -o "$out/x.o"
refuse "-mcmse with an FPU" "with an FPU" \
    "$EMBCC" --target=thumbv8m.main-none-eabihf -mcmse -c "$out/va.c" -o "$out/x.o"
refuse "-mcmse on ARMv7-M" "is not ARMv8-M" \
    "$EMBCC" --target=thumbv7m-none-eabi -mcmse -c "$out/va.c" -o "$out/x.o"
printf '#include <arm_cmse.h>\nint g(void) { return cmse_nonsecure_caller(); }\n' > "$out/caller.c"
refuse "cmse_nonsecure_caller" "cmse_nonsecure_caller_is_not_supported_by_EmbCC" \
    "$EMBCC" --target=$M -mcmse -c "$out/caller.c" -o "$out/x.o"
refuse "--in-implib" "in-implib is not supported" \
    "$EMBLD" --in-implib=x.o "$out/x.o" -o "$out/x.elf"
if [ -f "$b/secure.o" ]; then
    refuse "--out-implib alone" "needs --cmse-implib" \
        "$EMBLD" -T $D/secure.ld --out-implib="$out/i.o" "$b/s_boot.o" \
        "$b/secure.o" -o "$out/x.elf"
fi
# Without -mcmse the attributes are ignored, with a warning, as clang does
printf 'int __attribute__((cmse_nonsecure_entry)) f(int a) { return a; }\n' > "$out/plain.c"
"$EMBCC" --target=$M -c "$out/plain.c" -o "$out/plain.o" 2> "$out/plain.err" &&
grep -q "ignored without -mcmse" "$out/plain.err" || {
    echo "without -mcmse, cmse_nonsecure_entry is not ignored with a warning"
    fail=1; }
[ "$fail" = 0 ] && echo "what CMSE forbids and what EmbCC does not do are refused by name"
pkill -f "qemu-system-arm.*$out" 2>/dev/null
exit $fail
