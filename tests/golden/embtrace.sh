#!/bin/sh
# -finstrument-functions and EmbTrace, end to end on two boards: a program
# compiled with the flag records every entry and exit in lib/rt's ring
# (lib/rt/embtrace.c), dumps it to the console, and tools/embtrace reads
# the dump back against the image.
#
# Cortex-M3 (lm3s6965evb), a ring big enough for the whole run:
#   - fib(10) is 177 calls, nested 10 deep under main (11 in all) -- an
#     exit hook placed before a return's value is computed would end each
#     fib before its callees begin, and flatten the tree;
#   - helper is called 5 times from an uninstrumented function, so its
#     caller on the stack is main;
#   - no_instrument_function, on the definition after a plain prototype
#     (the hook once recursed through such a clock forever), the
#     -finstrument-functions-exclude-function-list and the -file-list each
#     keep their functions out;
#   - main is still open when it dumps;
#   - every entry's call site (the hooks' return-address argument) is in
#     the function the stack says called it, or -- helper's five -- in the
#     uninstrumented function that did;
#   - the Chrome trace is JSON with a begin per entry and an end per exit.
# RV32 (virt), the default 128-event ring and mcycle for a clock:
#   - fib(12) records 931 events (465 calls and main's entry), of which
#     the ring keeps 128 and reports 803 lost; the exits whose entries
#     went with them are set aside, and the times are not all 0;
#   - every call site is in its caller.
set -u
EMBCC=${EMBCC:-./embcc}
EMBTRACE=${EMBTRACE:-./embtrace}
[ -x "$EMBTRACE" ] || { echo "FAIL: $EMBTRACE is not built (make embtrace)"; exit 1; }
echo "TEST-MARKER embtrace"
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embtrace
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
want() {    # want FILE TEXT: FILE has a line holding TEXT
    grep -q -- "$2" "$1" || fail "$3: '$2' not in: $(head -12 "$1")"
}

# ---- what the flag does to a function ------------------------------------
cat > "$out/shape.c" <<'EOF'
int g(void);
__attribute__((no_instrument_function)) int g(void) { return 1; }
int h(int a) { return a + g(); }
EOF
"$EMBCC" --target=thumbv7m-none-eabi -O2 -finstrument-functions -c "$out/shape.c" \
    -o "$out/shape.o" || fail "shape.c"
"${EMBCC_LLVM_NM:-llvm-nm}" "$out/shape.o" > "$out/shape.nm"
want "$out/shape.nm" "U __cyg_profile_func_enter" "the hooks"
want "$out/shape.nm" "U __cyg_profile_func_exit" "the hooks"
"$EMBCC" --target=thumbv7m-none-eabi -O2 -c "$out/shape.c" -o "$out/plain.o" &&
"${EMBCC_LLVM_NM:-llvm-nm}" "$out/plain.o" | grep -q __cyg &&
    fail "the hooks are called without -finstrument-functions"

# ---- the program ----------------------------------------------------------
cat > "$out/prog.c" <<'EOF'
#include <embtrace.h>
void writec(int c);
void puts_(const char *s);
void putn(long v);
int lib2_twice(int k);

EMBTRACE_NOI void embtrace_putc(int c) { writec(c); }

#ifdef BIG_RING
struct embtrace_ev { void *fn, *site; unsigned t, kind; };
struct embtrace_ev embtrace_events[600];
const unsigned embtrace_capacity = 600;
#endif
#ifdef MCYCLE
unsigned embtrace_clock(void);
EMBTRACE_NOI unsigned embtrace_clock(void) { return embtrace_mcycle(); }
#endif

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int helper(int k) { return k * 3; }
__attribute__((no_instrument_function)) static int quiet(int k)
{
    return k + helper(k);
}
int excluded(int k) { return k - 1; }
static volatile int sink;

int main(void)
{
    sink = fib(N);
    for (int i = 0; i < 5; i++)
        sink += quiet(i) + excluded(i) + lib2_twice(i);
    embtrace_dump();
    puts_("fib "); putn(fib(N)); puts_("\n");
    return 42;
}
EOF
cat > "$out/lib2.c" <<'EOF'
int lib2_twice(int k) { return 2 * k; }
EOF
FLAGS="-O2 -finstrument-functions -finstrument-functions-exclude-function-list=excluded,nothing_by_that_name -finstrument-functions-exclude-file-list=lib2"

# ---- Cortex-M3 -------------------------------------------------------------
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
T=thumbv7m-none-eabi
if command -v "$QARM" >/dev/null 2>&1 && [ -f build/libc/$T/librt.a ]; then
    d=$out/m3
    mkdir -p "$d"
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    for f in prog lib2; do
        "$EMBCC" --target=$T $FLAGS -DN=10 -DBIG_RING -Ilib/libc/include \
            -c "$out/$f.c" -o "$d/$f.o" || fail "$f.c for $T"
    done
    EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/prog.elf" \
        "$d/prog.o" "$d/lib2.o" build/libc/$T/librt.a > "$d/link.log" 2>&1 ||
        fail "the M3 image does not link: $(head -3 "$d/link.log")"
    sh tests/harness/qrun.sh 20 "$QARM" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$d/prog.elf" > "$d/console.log" 2>&1
    want "$d/console.log" "fib 55" "the M3 program"
    "$EMBTRACE" "$d/prog.elf" "$d/console.log" --chrome "$d/trace.json" \
        > "$d/summary.txt" || fail "embtrace: $(cat "$d/summary.txt")"
    "$EMBTRACE" "$d/prog.elf" "$d/console.log" --tree > "$d/tree.txt" ||
        fail "embtrace --tree"
    want "$d/summary.txt" "(365 recorded, 0 lost to the ring)" "M3"
    want "$d/summary.txt" "deepest 11$" "M3 nesting"
    want "$d/summary.txt" "1 calls still open" "M3 (main dumps)"
    want "$d/summary.txt" "177 of 177 call sites in their caller, 5 in a function not traced" "M3 call sites"
    # with no clock, times are counts of events: exact, from the calls alone
    grep -Eq "^fib +177 +2225 +353 +353$" "$d/summary.txt" ||
        fail "M3: fib is not 177 calls, 2225/353/353: $(cat "$d/summary.txt")"
    grep -Eq "^helper +5 " "$d/summary.txt" || fail "M3: helper is not 5 calls"
    for f in quiet excluded lib2_twice; do
        grep -q "^$f " "$d/summary.txt" && fail "M3: $f was instrumented"
    done
    want "$d/tree.txt" "^main  x0" "M3 tree"
    want "$d/tree.txt" "^  fib  x1" "M3 tree"
    want "$d/tree.txt" "^    fib  x2" "M3 tree"
    want "$d/tree.txt" "^  helper  x5" "M3 tree"
    python3 - "$d/trace.json" <<'PY' || fail "M3: the Chrome trace"
import json, sys
ev = json.load(open(sys.argv[1]))["traceEvents"]
b = sum(1 for e in ev if e["ph"] == "B")
e = sum(1 for e in ev if e["ph"] == "E")
assert (b, e) == (183, 182), (b, e)
assert ev[0]["name"] == "main" and ev[1]["name"] == "fib"
PY
else
    echo "SKIP: the Cortex-M3 half ($QARM or the Thumb librt.a is missing)"
fi

# ---- RV32 -------------------------------------------------------------------
QRV=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
T=riscv32-unknown-elf
if command -v "$QRV" >/dev/null 2>&1 && [ -f build/libc/$T/librt.a ]; then
    d=$out/rv32
    mkdir -p "$d"
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/riscv/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    for f in prog lib2; do
        "$EMBCC" --target=$T $FLAGS -DN=12 -DMCYCLE -Ilib/libc/include \
            -c "$out/$f.c" -o "$d/$f.o" || fail "$f.c for $T"
    done
    EMBCC_RISCV_HARNESS=$d sh tests/harness/riscv/link.sh "$d/prog.elf" \
        "$d/prog.o" "$d/lib2.o" build/libc/$T/librt.a > "$d/link.log" 2>&1 ||
        fail "the RV32 image does not link: $(head -3 "$d/link.log")"
    EMBCC_QEMU_RISCV=$QRV sh tests/harness/riscv/run.sh "$d/prog.elf" 32 \
        > "$d/console.log"
    want "$d/console.log" "fib 144" "the RV32 program"
    "$EMBTRACE" "$d/prog.elf" "$d/console.log" > "$d/summary.txt" ||
        fail "embtrace: $(cat "$d/summary.txt")"
    want "$d/summary.txt" "^128 events (941 recorded, 813 lost to the ring)" "RV32 ring"
    python3 - "$d/summary.txt" <<'PY' || fail "RV32 call sites: $(cat "$d/summary.txt")"
import re, sys
m = re.search(r"^(\d+) of (\d+) call sites in their caller", open(sys.argv[1]).read(), re.M)
assert m and m.group(1) == m.group(2) and int(m.group(2)) > 0
PY
    grep -q "no clock" "$d/summary.txt" && fail "RV32: mcycle read nothing"
else
    echo "SKIP: the RV32 half ($QRV or the RV32 librt.a is missing)"
fi
echo "ok embtrace"
