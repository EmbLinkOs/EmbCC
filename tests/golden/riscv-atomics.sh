#!/bin/sh
# RISC-V atomics: the A extension, at both widths.
#
# Hazard3 -- the RTOS requirements' fourth target, and the RP2350's RISC-V
# core -- is RV32IMAC, and a kernel cannot be written without a
# compare-and-swap. These were refused by name until now.
#
# Two things about the lowering are worth asserting rather than assuming.
#
# EVERY OPERATION IS AQRL: acquire AND release ordering, not relaxed. A C11
# atomic defaults to seq_cst, and a lock that is merely relaxed is a lock
# that does not work on a core that reorders. Hazard3 is in-order, so getting
# this wrong costs nothing there and everything on the first core that is not
# -- which is exactly the kind of bug that must be caught by reading the
# instruction rather than by running it.
#
# NARROWER THAN A WORD IS REFUSED. The A extension provides .w and, at RV64,
# .d, and nothing smaller. gcc answers a one-byte atomic by calling
# libatomic; EmbCC has no such library, so a read-modify-write of the
# containing word would be the only option and it is not atomic against a
# neighbouring byte. The predefined macros say so too: only the
# __GCC_HAVE_SYNC_COMPARE_AND_SWAP_ widths that exist are claimed.
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
        echo "$m" | grep -q "SYNC_COMPARE_AND_SWAP_$w" && {
            echo "$t: claims a $w-byte compare-and-swap. The A extension has
            no such instruction and EmbCC has no libatomic to call, so a
            program using one would compile and fail to link"; exit 1; }
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
echo "both widths claim exactly the compare-and-swap sizes the A extension has"

# ---- a sub-word atomic is refused by name -----------------------------
printf 'char c;\nchar f(void){return __sync_fetch_and_add(&c,1);}\n' \
    > "$out/nb.c"
"$EMBCC" --target=riscv32-unknown-elf -O1 -c "$out/nb.c" -o "$out/nb.o" \
    2> "$out/nb.err" && {
    echo "a one-byte atomic compiled. The A extension has no such
    instruction, so whatever was emitted is not atomic"; exit 1; }
grep -q "narrower than four bytes" "$out/nb.err" || {
    echo "the one-byte atomic was refused, but not by name:"
    head -3 "$out/nb.err"; exit 1; }
echo "a one-byte atomic is refused by name"

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
    __sync_synchronize();
    puts_("DONE\n");
    return 0;
}
EOF
want="0 1 0 10 15 15 12 12 4 4 7 7 6 100 200 200 200 1 400 0 400 DONE"
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
