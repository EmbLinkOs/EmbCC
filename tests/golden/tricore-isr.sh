#!/bin/sh
# TriCore interrupt and trap handlers: __attribute__((interrupt)), as
# HighTec's GCC makes one. The hardware saves the upper context on entry,
# as a CALL does; the handler saves the lower one with svlcx, and leaves
# with rslcx and rfe, which takes the upper context back and returns to
# the interrupted instruction.
#
#   THE BAD FORMS are refused by name: parameters, a result, an argument.
#
#   THE SHAPE: svlcx first, rslcx and rfe last, no ret and no tail call.
#
#   THE RUN, on QEMU's tricore_testboard (tests/golden/tricore-isr/
#   main.c): it has no interrupt router, but a syscall trap is entered and
#   left the same way, so a loop that keeps a pattern in D0-D7 and A2-A7
#   traps two thousand times into a counting handler and into one that
#   calls, at -O0 to -Os.
set -u
echo "TEST-MARKER tricore-isr"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=tricore-none-elf
out=tests/golden/out/tricore-isr
rm -rf "${out:?}"; mkdir -p "$out"
export EMBCC_VERIFY=1
fail() { echo "FAIL: $*"; exit 1; }

# ---- the bad forms -----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        fail "$1 was accepted"
    fi
    grep -q -- "$2" "$out/bad.err" || {
        cat "$out/bad.err"; fail "$1 was refused, but not by name"; }
}
refc "a handler with parameters" "takes parameters" \
    'void __attribute__((interrupt)) h(int x) { (void)x; }'
refc "a handler with a result" "returns a value" \
    'int __attribute__((interrupt)) h(void) { return 1; }'
refc "interrupt(3)" "interrupt takes no argument on TriCore" \
    'void __attribute__((interrupt(3))) h(void) { }'
echo "tricore-isr: parameters, a result and an argument are refused by name"

# ---- the shape ---------------------------------------------------------------
cat > "$out/shape.c" <<'EOF'
volatile int count;
extern void work(int);
void __attribute__((interrupt)) leaf(void) { count++; }
void __attribute__((interrupt)) calls(void) { work(count); }
EOF
# each function's bytes, from the symbol table (llvm has no TriCore)
fbytes() {          # fbytes OBJ NAME -> the symbol's bytes, in hex
    llvm-readelf -s -W "$1" | awk -v n="$2" '$8 == n { print $2, $3 }' | {
        read -r v s
        llvm-objcopy -O binary --only-section=.text "$1" "$out/t.bin"
        od -An -tx1 -v -j $((0x$v)) -N "$s" "$out/t.bin" | tr -s ' \n' ' '
    }
}
for O in -O0 -O2 -Os; do
    "$EMBCC" --target=$T $O -c "$out/shape.c" -o "$out/shape$O.o" ||
        fail "$O: the handlers do not compile"
    for h in leaf calls; do
        b=$(fbytes "$out/shape$O.o" $h)
        case "$b" in
        " 0d 00 00 02 "*" 0d 00 40 02 0d 00 c0 01 ") ;;
        *) fail "$O: $h is not svlcx ... rslcx, rfe: $b" ;;
        esac
    done
done
echo "tricore-isr: every handler begins with svlcx and ends with rslcx and rfe, -O0 -O2 -Os"

# ---- the run -----------------------------------------------------------------
QEMU=${EMBCC_QEMU_TRICORE:-qemu-system-tricore}
command -v "$QEMU" >/dev/null 2>&1 || { echo "tricore-isr: run skipped: $QEMU not found"; exit 0; }
why=$(qemu_plugin tests/harness/tricore/putc.c "$out/putc.so") || {
    echo "tricore-isr: run skipped: the TriCore harness prints through a QEMU plugin, and $why"
    exit 0; }
EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
export EMBCC_TRICORE_PLUGIN
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" > "$out/rt.log" 2>&1 ||
    fail "lib/rt does not build for $T"
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" > "$out/libc.log" 2>&1 ||
    fail "lib/libc does not build for $T"
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/tricore/boot.c -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/tricore/io.c -o "$out/io.o" ||
    fail "the harness does not compile"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c tests/golden/tricore-isr/main.c -o "$out/m$O.o" ||
        fail "main.c does not compile at $O"
    EMBLD="$EMBLD" EMBCC_TRICORE_HARNESS="$out" sh tests/harness/tricore/link.sh \
        "$out/m$O.elf" "$out/m$O.o" "$out/lib/libc.a" "$out/lib/librt.a" ||
        fail "main.c does not link at $O"
    sh tests/harness/tricore/run.sh "$out/m$O.elf" > "$out/m$O.out" 2>&1
    s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$out/m$O.out" | tail -1)
    [ "$s" = 42 ] || { cat "$out/m$O.out"; fail "the run at $O exited ${s:-without the sentinel}"; }
    echo "  $O: $(grep '^leaf' "$out/m$O.out"); $(grep '^calls' "$out/m$O.out")"
done
echo "tricore-isr: D0-D7 and A2-A7 survive every trap into either handler, at -O0 to -Os"
