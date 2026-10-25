#!/bin/sh
# RX interrupt handlers: __attribute__((interrupt)), as GCC's rx-elf
# compiles it -- every register the handler may change pushed, the
# caller-saved r1-r5, r14 and r15 too, and rte, which takes back the PC
# and PSW the CPU pushed.
#
#   THE BAD FORMS are refused by name: parameters, a result, GCC's
#   interrupt(N) vector-table argument (whose $tableentry$ symbols EmbLD
#   gathers into nothing), and fast_interrupt (rtfi), which an unknown
#   attribute's warning would have let through as an ordinary function.
#
#   THE SHAPE: every handler ends with rte (its one epilogue); one that
#   calls pushes what GCC's does -- pushm r14-r15, pushm r1-r5 -- read
#   against rx-elf-gcc's own object when there is one.
#
#   THE RUN, on QEMU's gdbsim-r5f562n8 (tests/golden/rx-isr/main.c): the
#   CMT0 compare match interrupts a loop that keeps a pattern in every
#   register but the stack pointer, and a C computation, thousands of
#   times with a handler that counts and dozens with one that calls --
#   at -O0 to -Os.
set -u
echo "TEST-MARKER rx-isr"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=rx-none-elf
out=tests/golden/out/rx-isr
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
refc "interrupt(5)" "interrupt takes no argument on RX" \
    'void __attribute__((interrupt(5))) h(void) { }'
refc "fast_interrupt" "fast interrupt returns with rtfi" \
    'void __attribute__((fast_interrupt)) h(void) { }'
echo "rx-isr: parameters, a result, interrupt(N) and fast_interrupt are refused by name"

# ---- the shape ---------------------------------------------------------------
cat > "$out/shape.c" <<'EOF'
volatile int count;
extern void work(int);
void __attribute__((interrupt)) leaf(void) { count++; }
void __attribute__((interrupt)) calls(void) { work(count); }
EOF
"$EMBCC" --target=$T -O2 -c "$out/shape.c" -o "$out/shape.o" ||
    fail "the handlers do not compile"
# each function's bytes, from the symbol table (llvm has no RX)
fbytes() {          # fbytes OBJ NAME [SECTION] -> the symbol's bytes, in hex
    llvm-readelf -s -W "$1" | awk -v n="$2" '$8 == n { print $2, $3 }' | {
        read -r v s
        llvm-objcopy -O binary --only-section="${3:-.text}" "$1" "$out/t.bin"
        od -An -tx1 -v -j $((0x$v)) -N "$s" "$out/t.bin" | tr -s ' \n' ' '
    }
}
for h in leaf calls; do
    b=$(fbytes "$out/shape.o" _$h)
    case "$b" in
    *" 7f 95 ") ;;
    *) fail "$h does not end with rte (7f 95): $b" ;;
    esac
done
calls=$(fbytes "$out/shape.o" _calls)
case "$calls" in
" 6e ef 6e 15 "*" 6f 15 6f ef 7f 95 ") ;;
*) fail "calls does not push r14-r15 and r1-r5 and pop them before rte: $calls" ;;
esac
RXBIN=${EMBCC_RX_BIN:-$HOME/EmbRef/rx-elf/bin}
GCC=
[ -x "$RXBIN/rx-elf-gcc" ] && GCC=$RXBIN/rx-elf-gcc
[ -z "$GCC" ] && command -v rx-elf-gcc >/dev/null 2>&1 && GCC=rx-elf-gcc
if [ -n "$GCC" ] && "$GCC" -O2 -c "$out/shape.c" -o "$out/gcc.o" 2>/dev/null; then
    g=$(fbytes "$out/gcc.o" _calls P)      # (GCC's code section is P)
    gp=$(echo "$g" | cut -d' ' -f2-5); ep=$(echo "$calls" | cut -d' ' -f2-5)
    [ "$gp" = "$ep" ] || fail "calls pushes '$ep' where rx-elf-gcc pushes '$gp'"
    echo "rx-isr: every handler ends with rte; one that calls pushes what rx-elf-gcc's does ($ep)"
else
    echo "rx-isr: every handler ends with rte; one that calls pushes r14-r15 and r1-r5 (no rx-elf-gcc to compare)"
fi

# ---- the run -----------------------------------------------------------------
QEMU=${EMBCC_QEMU_RX:-qemu-system-rx}
command -v "$QEMU" >/dev/null 2>&1 || { echo "rx-isr: run skipped: $QEMU not found"; exit 0; }
OBJCOPY=${EMBCC_OBJCOPY:-llvm-objcopy}
export EMBCC_OBJCOPY="$OBJCOPY"
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" > "$out/rt.log" 2>&1 ||
    fail "lib/rt does not build for $T"
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" > "$out/libc.log" 2>&1 ||
    fail "lib/libc does not build for $T"
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/rx/boot.c -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/rx/io.c -o "$out/io.o" ||
    fail "the harness does not compile"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c tests/golden/rx-isr/main.c -o "$out/m$O.o" ||
        fail "main.c does not compile at $O"
    "$EMBLD" -e _start -Ttext 0x01800000 -Tstack 0x02000000 "$out/boot.o" \
        "$out/io.o" "$out/m$O.o" "$out/lib/libc.a" "$out/lib/librt.a" \
        -o "$out/m$O.elf" && "$OBJCOPY" -O binary "$out/m$O.elf" "$out/m$O.bin" ||
        fail "main.c does not link at $O"
    sh tests/harness/rx/run.sh "$out/m$O.bin" > "$out/m$O.out" 2>&1
    s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$out/m$O.out" | tail -1)
    [ "$s" = 42 ] || { cat "$out/m$O.out"; fail "the run at $O exited ${s:-without the sentinel}"; }
    echo "  $O: $(grep '^leaf' "$out/m$O.out" | sed 's/, torture.*//'); $(grep '^calls' "$out/m$O.out" | sed 's/, torture.*//')"
done
echo "rx-isr: the timer interrupts every register and a computation unharmed, at -O0 to -Os"
