#!/bin/sh
# ColdFire interrupt handlers: __attribute__((interrupt)) and its m68k
# spelling interrupt_handler, as GCC's m68k port makes them -- every
# register the handler may change saved, d0, d1, a0 and a1 too, and rte,
# which takes back the format word, SR and PC the exception stacked.
#
#   THE BAD FORMS are refused by name: parameters, a result, an argument,
#   and interrupt_thread (the Fido's register banks), which an unknown
#   attribute's warning would have let through as an ordinary function.
#
#   THE SHAPE: every handler ends with rte (its one epilogue), and its
#   movem saves d0, d1, a0 and a1 with the callee-saved registers.
#
#   THE RUN, on QEMU's mcf5208evb (tests/golden/coldfire-isr/main.c): PIT0
#   interrupts a loop that keeps a pattern in d0-d7 and a0-a5, and a C
#   computation, with a handler that counts and with one that calls -- at
#   -O0 to -Os.
set -u
echo "TEST-MARKER coldfire-isr"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=m68k-none-elf
out=tests/golden/out/coldfire-isr
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
    'int __attribute__((interrupt_handler)) h(void) { return 1; }'
refc "interrupt(3)" "interrupt takes no argument on ColdFire" \
    'void __attribute__((interrupt(3))) h(void) { }'
refc "interrupt_thread" "Fido interrupt thread" \
    'void __attribute__((interrupt_thread)) h(void) { }'
# and the m68k spelling stays refused where it means nothing
printf 'void __attribute__((interrupt_handler)) h(void) { }\n' > "$out/rv.c"
if "$EMBCC" --target=riscv32-unknown-elf -c "$out/rv.c" -o /dev/null 2> "$out/rv.err"; then
    fail "interrupt_handler was accepted on RISC-V"
fi
grep -q "implemented on the ColdFire target only" "$out/rv.err" ||
    { cat "$out/rv.err"; fail "interrupt_handler on RISC-V was refused, but not by name"; }
echo "coldfire-isr: parameters, a result, an argument and interrupt_thread are refused by name"

# ---- the shape ---------------------------------------------------------------
cat > "$out/shape.c" <<'EOF'
volatile int count;
extern void work(int);
void __attribute__((interrupt)) leaf(void) { count++; }
void __attribute__((interrupt_handler)) calls(void) { work(count); }
EOF
"$EMBCC" --target=$T -O2 -c "$out/shape.c" -o "$out/shape.o" ||
    fail "the handlers do not compile"
# each function's bytes, from the symbol table (llvm has no m68k)
fbytes() {          # fbytes OBJ NAME -> the symbol's bytes, in hex
    llvm-readelf -s -W "$1" | awk -v n="$2" '$8 == n { print $2, $3 }' | {
        read -r v s
        llvm-objcopy -O binary --only-section=.text "$1" "$out/t.bin"
        od -An -tx1 -v -j $((0x$v)) -N "$s" "$out/t.bin" | tr -s ' \n' ' '
    }
}
for h in leaf calls; do
    b=$(fbytes "$out/shape.o" $h)
    case "$b" in
    *" 4e 73 ") ;;
    *) fail "$h does not end with rte (4e 73): $b" ;;
    esac
    # link.w %a6,#-n, then movem.l LIST,(d16,%a6): 48 ee, the mask, d16 --
    # whose mask has d0, d1, a0 and a1 (bits 0, 1, 8 and 9)
    set -- $b
    [ "$1 $2" = "4e 56" ] && [ "$5 $6" = "48 ee" ] ||
        fail "$h does not begin link.w %a6 / movem.l to its frame: $b"
    m=$((0x$7$8))
    [ $((m & 0x0303)) = $((0x0303)) ] ||
        fail "$h's movem mask $7$8 leaves out d0, d1, a0 or a1"
done
echo "coldfire-isr: every handler saves d0, d1, a0 and a1 in its movem and ends with rte"

# ---- the run -----------------------------------------------------------------
QEMU=${EMBCC_QEMU_M68K:-qemu-system-m68k}
command -v "$QEMU" >/dev/null 2>&1 || { echo "coldfire-isr: run skipped: $QEMU not found"; exit 0; }
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" > "$out/rt.log" 2>&1 ||
    fail "lib/rt does not build for $T"
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" > "$out/libc.log" 2>&1 ||
    fail "lib/libc does not build for $T"
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/coldfire/boot.c \
    -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/coldfire/io.c -o "$out/io.o" ||
    fail "the harness does not compile"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c tests/golden/coldfire-isr/main.c -o "$out/m$O.o" ||
        fail "main.c does not compile at $O"
    EMBLD="$EMBLD" EMBCC_CF_HARNESS="$out" sh tests/harness/coldfire/link.sh \
        "$out/m$O.elf" "$out/m$O.o" "$out/lib/libc.a" "$out/lib/librt.a" ||
        fail "main.c does not link at $O"
    sh tests/harness/coldfire/run.sh "$out/m$O.elf" > "$out/m$O.out" 2>&1
    s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$out/m$O.out" | tail -1)
    [ "$s" = 42 ] || { cat "$out/m$O.out"; fail "the run at $O exited ${s:-without the sentinel}"; }
    echo "  $O: $(grep '^leaf' "$out/m$O.out" | sed 's/, torture.*//'); $(grep '^calls' "$out/m$O.out" | sed 's/, torture.*//')"
done
echo "coldfire-isr: PIT0 interrupts every register and a computation unharmed, at -O0 to -Os"
