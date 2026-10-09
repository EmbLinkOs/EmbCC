#!/bin/sh
# RISC-V atomics: the A extension, at both widths.
#
# Hazard3 -- the RTOS requirements' fourth target, and the RP2350's RISC-V
# core -- is RV32IMAC, and a kernel cannot be written without a
# compare-and-swap. These were refused by name until now.
#
# Two things about the lowering are worth asserting rather than assuming.
#
# THE ORDERING IS THE ONE ASKED FOR, AND SEQ_CST BY DEFAULT. A C11 atomic
# defaults to seq_cst -- an AMO .aqrl, an LR/SC loop lr.aqrl / sc.rl -- and
# a lock that is merely relaxed is a lock that does not work on a core that
# reorders. Hazard3 is in-order, so getting this wrong costs nothing there
# and everything on the first core that is not -- which is exactly the kind
# of bug that must be caught by reading the instruction rather than by
# running it. An explicit memory order maps to .aq and .rl as clang maps
# it; orders.c compares every function's atomic instructions with clang's.
#
# NARROWER THAN A WORD WORKS ON THE WORD AROUND IT. The A extension
# provides .w and, at RV64, .d, and nothing smaller, so a one- or two-byte
# atomic is an AMO with the other lanes neutral (AND, OR, XOR) or an LR/SC
# loop that rewrites only its lane -- atomic against the neighbouring bytes
# too, since a write to any of them breaks the reservation. That is GCC's
# and LLVM's lowering. tests/golden/riscv-atomics/subword.c runs every
# operation on every lane of one word on the boards and compares with the
# host; pressure.c does it with t3, the allocator's one temporary, in use,
# and race.c with a timer interrupt writing the neighbouring lanes. The
# predefined macros claim the compare-and-swap widths there are.
set -u
echo "TEST-MARKER riscv-atomics"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/riscv-atomics
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}

# ---- the macros claim exactly the widths that exist -------------------
for pair in "riscv32-unknown-elf 32" "riscv64-unknown-elf 64"; do
    t=${pair% *}; x=${pair#* }
    m=$("$EMBCC" --target=$t --dump-predef 2>/dev/null)
    echo "$m" | grep -q '^#define __riscv_a ' || {
        echo "$t: __riscv_a is not defined, so the A extension is not in the
        baseline -- Hazard3 is RV32IMAC"; exit 1; }
    echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_4' || {
        echo "$t: a four-byte compare-and-swap is not claimed"; exit 1; }
    for w in 1 2; do
        echo "$m" | grep -q "SYNC_COMPARE_AND_SWAP_$w" || {
            echo "$t: a $w-byte compare-and-swap is not claimed, and the
            backend has one (an LR/SC loop on the word)"; exit 1; }
    done
    if [ "$x" = 64 ]; then
        echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_8' || {
            echo "RV64: an eight-byte compare-and-swap is not claimed, and
            amo*.d exists there"; exit 1; }
    else
        echo "$m" | grep -q 'SYNC_COMPARE_AND_SWAP_8' && {
            echo "RV32: claims an eight-byte compare-and-swap; amo*.d is
            RV64-only"; exit 1; }
    fi
done
echo "both widths claim exactly the compare-and-swap sizes there are"

# ---- a sub-word atomic, on the board ----------------------------------
# every operation on every lane of one word, the whole word after each, at
# both widths and every level, against the same program on the host; and
# the same with every caller-saved register taken (pressure.c)
HOSTCC=${HOSTCC:-cc}
S=$out/s; mkdir -p "$S"
for prog in subword pressure; do
    "$HOSTCC" -std=c99 -w -O2 -o "$out/$prog-host" \
        tests/golden/riscv-atomics/$prog.c tests/harness/thumb/hostio.c ||
        { echo "the host does not build $prog.c"; exit 1; }
    "$out/$prog-host" > "$out/$prog-want.txt"
    for x in 32 64; do
        t=riscv$x-unknown-elf
        Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
        command -v "$Q" >/dev/null 2>&1 || { echo "(SKIP: no $Q for RV$x)"; continue; }
        for O in -O0 -O1 -O2 -Os; do
            for f in boot io; do
                "$EMBCC" --target=$t $O -c tests/harness/riscv/$f.c -o "$S/$f.o" ||
                    { echo "RV$x $O: the harness did not compile"; exit 1; }
            done
            "$EMBCC" --target=$t $O -c tests/golden/riscv-atomics/$prog.c \
                -o "$S/sub.o" 2> "$out/sub.err" || {
                echo "RV$x $O: $prog.c did not compile:"
                head -3 "$out/sub.err"; exit 1; }
            EMBCC_RISCV_HARNESS="$S" sh tests/harness/riscv/link.sh "$S/sub.elf" "$S/sub.o" ||
                { echo "RV$x $O: $prog.c did not link"; exit 1; }
            EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
                sh tests/harness/riscv/run.sh "$S/sub.elf" "$x" 2>/dev/null |
                tr -d '\r' | sed -n '1,/^DONE/p' > "$out/sub-got.txt"
            cmp -s "$out/$prog-want.txt" "$out/sub-got.txt" || {
                echo "RV$x $O: $prog.c's one- and two-byte atomics differ from the host:"
                diff "$out/$prog-want.txt" "$out/sub-got.txt" | head -8; exit 1; }
        done
    done
done
echo "one- and two-byte atomics on every lane of a word, at RV32 and RV64 and every level, as the host computes them, and with t3 in use"

# ---- against an interrupt ---------------------------------------------
# race.c: the machine timer's handler increments the other lanes of the
# word with plain stores while main runs twenty thousand atomics on its
# own; every lane must come out exact. -icount: the interrupts land in the
# same places every run.
for x in 32 64; do
    t=riscv$x-unknown-elf
    Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$Q" >/dev/null 2>&1 || { echo "(SKIP: no $Q for RV$x)"; continue; }
    for O in -O0 -O1 -O2 -Os; do
        for f in boot io; do
            "$EMBCC" --target=$t -O1 -c tests/harness/riscv/$f.c -o "$S/$f.o" ||
                { echo "RV$x: the harness did not compile"; exit 1; }
        done
        "$EMBCC" --target=$t $O -c tests/golden/riscv-atomics/race.c \
            -o "$S/race.o" 2> "$out/race.err" || {
            echo "RV$x $O: race.c did not compile:"; head -3 "$out/race.err"; exit 1; }
        EMBCC_RISCV_HARNESS="$S" sh tests/harness/riscv/link.sh "$S/race.elf" "$S/race.o" ||
            { echo "RV$x $O: race.c did not link"; exit 1; }
        tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-60}" "$Q" -M virt \
            -bios none -nographic -m 8 -icount shift=0 -kernel "$S/race.elf" \
            2>/dev/null | tr -d '\r' | sed -n '1,/^DONE/p' > "$out/race-got.txt"
        grep -q '^DONE' "$out/race-got.txt" &&
            [ "$(grep -c ' 1 1 1 *$' "$out/race-got.txt")" = 14 ] || {
            echo "RV$x $O: an interrupt broke a one- or two-byte atomic (operation,"
            echo "lane, its own lane exact, the neighbours exact, enough interrupts):"
            grep -v ' 1 1 1 *$' "$out/race-got.txt" | head -8; exit 1; }
    done
done
echo "under a timer interrupt writing the neighbouring lanes, fetch_add on every byte and halfword, compare-exchange and fetch_xor on every byte lose nothing and touch nothing else, at RV32 and RV64 and every level"

# ---- every order, as clang emits it -----------------------------------
# orders.c: every operation at every width and memory order; each
# function's atomic instructions, aq and rl included, must be clang's
if command -v clang >/dev/null 2>&1 && command -v llvm-objdump >/dev/null 2>&1; then
    shape() {
        llvm-objdump -d --no-show-raw-insn --mattr=+a,+c,+m "$1" | awk '
            /^[0-9a-f]+ <[^>]+>:$/ { if (f != "") print f ":" s
                f = $2; gsub(/[<>:]/, "", f); s = ""; next }
            $2 ~ /^(lr\.|sc\.|amo)/ { s = s " " $2 }
            END { if (f != "") print f ":" s }' | grep -v ': *$' | sort
    }
    for x in 32 64; do
        clang --target=riscv$x -march=rv${x}imac -O2 -w -c \
            tests/golden/riscv-atomics/orders.c -o "$out/orders-clang.o" ||
            { echo "clang did not compile orders.c"; exit 1; }
        shape "$out/orders-clang.o" > "$out/orders-clang.txt"
        [ "$(grep -c 'lr.w.aqrl' "$out/orders-clang.txt")" -gt 10 ] || {
            echo "clang's orders.c was not read"; exit 1; }
        for O in -O0 -O2 -Os; do
            "$EMBCC" --target=riscv$x-unknown-elf $O -c \
                tests/golden/riscv-atomics/orders.c -o "$out/orders.o" ||
                { echo "RV$x $O: orders.c did not compile"; exit 1; }
            shape "$out/orders.o" > "$out/orders.txt"
            cmp -s "$out/orders-clang.txt" "$out/orders.txt" || {
                echo "RV$x $O: atomic instructions differ from clang's (< clang, > embcc):"
                diff "$out/orders-clang.txt" "$out/orders.txt" | head -12; exit 1; }
        done
        echo "RV$x: $(wc -l < "$out/orders.txt" | tr -d ' ') functions' atomic instructions and their aq/rl bits are clang's, at -O0, -O2 and -Os"
    done
else
    echo "(SKIP: no clang or llvm-objdump for the orders)"
fi

# ---- the ordering is AQRL, read off the instruction -------------------
command -v llvm-objdump >/dev/null 2>&1 || {
    echo "(SKIP: no llvm-objdump for the ordering check)"; }
if command -v llvm-objdump >/dev/null 2>&1; then
    cat > "$out/ord.c" <<'EOF'
int v;
int a(void) { return __sync_lock_test_and_set(&v, 1); }
int b(void) { return __sync_fetch_and_add(&v, 1); }
int c(void) { return __sync_fetch_and_and(&v, 1); }
int d(void) { return __sync_fetch_and_or(&v, 1); }
int e(void) { return __sync_fetch_and_xor(&v, 1); }
int f(void) { return __sync_val_compare_and_swap(&v, 1, 2); }
EOF
    "$EMBCC" --target=riscv32-unknown-elf -O1 -c "$out/ord.c" \
        -o "$out/ord.o" || { echo "the ordering case did not compile"; exit 1; }
    llvm-objdump -d --triple=riscv32 --mattr=+a "$out/ord.o" 2>/dev/null \
        > "$out/ord.dis"
    for want in amoswap.w.aqrl amoadd.w.aqrl amoand.w.aqrl amoor.w.aqrl \
                amoxor.w.aqrl; do
        grep -q "$want" "$out/ord.dis" || {
            echo "expected a '$want' and did not find one. A relaxed atomic
            here is a lock that works on an in-order core and not on any
            other:"
            grep -E "amo|lr\.|sc\." "$out/ord.dis" | head -8; exit 1; }
    done
    # A compare-and-swap is a load-reserved / store-conditional loop: lr must
    # acquire and sc must release, or the critical section it guards can be
    # reordered out of.
    grep -q "lr.w.aq" "$out/ord.dis" || {
        echo "the compare-and-swap's lr does not acquire"; exit 1; }
    grep -q "sc.w.rl" "$out/ord.dis" || {
        echo "the compare-and-swap's sc does not release"; exit 1; }
    # and no relaxed form slipped through
    grep -E "^\s+[0-9a-f]+:.*\b(amo[a-z]+\.w|lr\.w|sc\.w)\s" "$out/ord.dis" |
        grep -vE "\.aqrl|\.aq\b|\.rl\b" > "$out/relaxed" || true
    [ -s "$out/relaxed" ] && {
        echo "a RELAXED atomic was emitted:"; head -4 "$out/relaxed"; exit 1; }
    echo "every atomic carries acquire-release ordering, and the
compare-and-swap's lr acquires and its sc releases"
fi

# ---- and they RUN, at both widths -------------------------------------
cat > "$out/run.c" <<'EOF'
void puts_(const char *s);
void putn(long v);

static volatile int lock;
static volatile int counter;

/* Through a POINTER ARGUMENT: the address arrives in a0 and the result
 * leaves in a0, so an allocator that keeps atomics' operands in registers
 * may give the result the address's own register. The NAND loop writes
 * its result before its store-conditional reads the address again. */
__attribute__((noinline)) int nand_at(volatile int *p, int v)
{ return __sync_fetch_and_nand(p, v); }
__attribute__((noinline)) int swap_at(volatile int *p, int v)
{ return __sync_lock_test_and_set(p, v); }
__attribute__((noinline)) int add_at(volatile int *p, int v)
{ return __sync_fetch_and_add(p, v); }
__attribute__((noinline)) int cas_at(volatile int *p, int o, int n)
{ return __sync_val_compare_and_swap(p, o, n); }

int main(void)
{
    int prev;
    lock = 0;
    prev = __sync_lock_test_and_set(&lock, 1);
    putn(prev);                                   /* 0: it was free */
    prev = __sync_lock_test_and_set(&lock, 1);
    putn(prev);                                   /* 1: already held */
    __sync_lock_release(&lock);
    putn(lock);                                   /* 0 */

    counter = 10;
    putn(__sync_fetch_and_add(&counter, 5));      /* 10 */
    putn(counter);                                /* 15 */
    putn(__sync_fetch_and_sub(&counter, 3));      /* 15 */
    putn(counter);                                /* 12 */
    putn(__sync_fetch_and_and(&counter, 6));      /* 12 */
    putn(counter);                                /* 4 */
    putn(__sync_fetch_and_or(&counter, 3));       /* 4 */
    putn(counter);                                /* 7 */
    putn(__sync_fetch_and_xor(&counter, 1));      /* 7 */
    putn(counter);                                /* 6 */

    /* compare-and-swap, both outcomes and both shapes */
    counter = 100;
    putn(__sync_val_compare_and_swap(&counter, 100, 200));  /* 100 */
    putn(counter);                                          /* 200 */
    putn(__sync_val_compare_and_swap(&counter, 100, 300));  /* 200 */
    putn(counter);                                          /* 200 */
    putn(__sync_bool_compare_and_swap(&counter, 200, 400)); /* 1 */
    putn(counter);                                          /* 400 */
    putn(__sync_bool_compare_and_swap(&counter, 200, 500)); /* 0 */
    putn(counter);                                          /* 400 */

    counter = 12;
    putn(nand_at(&counter, 10));                  /* 12 */
    putn(counter & 0xff);                         /* ~(12 & 10) = 0xf7 */
    putn(swap_at(&counter, 21) & 0xff);           /* 0xf7 again */
    putn(add_at(&counter, 4));                    /* 21 */
    putn(cas_at(&counter, 25, 30));               /* 25 */
    putn(counter);                                /* 30 */
    __sync_synchronize();
    puts_("DONE\n");
    return 0;
}
EOF
want="0 1 0 10 15 15 12 12 4 4 7 7 6 100 200 200 200 1 400 0 400 12 247 247 21 25 30 DONE"
H=$out/h; mkdir -p "$H"
for pair in "riscv32-unknown-elf 32" "riscv64-unknown-elf 64"; do
    t=${pair% *}; x=${pair#* }
    Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$Q" >/dev/null 2>&1 || {
        echo "(SKIP: no $Q for RV$x)"; continue; }
    for O in -O0 -O1 -O2 -Os; do
        for f in boot io; do
            "$EMBCC" --target=$t $O -c tests/harness/riscv/$f.c \
                -o "$H/$f.o" 2> "$out/h.err" || {
                echo "RV$x $O: the harness $f did not compile:"
                head -4 "$out/h.err"; exit 1; }
        done
        "$EMBCC" --target=$t $O -c "$out/run.c" -o "$H/run.o" \
            2> "$out/r.err" || {
            echo "RV$x $O: did not compile:"; head -5 "$out/r.err"; exit 1; }
        EMBCC_RISCV_HARNESS="$H" sh tests/harness/riscv/link.sh \
            "$H/run.elf" "$H/run.o" 2> "$out/l.err" || {
            echo "RV$x $O: link failed:"; head -4 "$out/l.err"; exit 1; }
        EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
            sh tests/harness/riscv/run.sh "$H/run.elf" "$x" \
            > "$out/got" 2>/dev/null
        got=$(sed -n '1,/DONE/p' "$out/got" | tr -d '\n' | sed 's/  *$//')
        [ "$got" = "$want" ] || {
            echo "RV$x $O: the atomics disagree."
            echo "  want: $want"
            echo "  got:  $got"; exit 1; }
    done
    echo "RV$x: every atomic is right at four optimisation levels"
done

echo "the A extension works at both widths: test-and-set, the five
fetch-and-modify forms, and compare-and-swap in both its value and its
boolean shape, with both outcomes"
