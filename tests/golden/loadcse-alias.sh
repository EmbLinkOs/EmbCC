#!/bin/sh
# Loads and stores told apart by base and offset (mem_access, acc_overlap).
#
# A store used to make value numbering forget every load it had seen,
# and load CSE and dead-store elimination asked only "same object?" -- so
# in a function handed `struct q *p`, where everything is one object,
# `p->wr = p->head` made `p->len` and `p->isz` load again. A store to
# [p + 8] cannot change [p + 60], and that is now how it is read: two
# accesses a constant away from the SAME base value overlap only when
# their byte ranges do. Nothing about types is assumed, and offsets wrap
# at the address width.
#
# Three parts:
#
#  1. The shape. A small struct example on Thumb and RV32 at -O1, -O2
#     and -Os: the reloads across disjoint stores are gone, in one block
#     (value numbering) and across a branch (load CSE); a store that a
#     later store overwrites with a read of another field between is
#     gone, and so is a byte store a later word store covers (DSE). And
#     the ones that must stay, stay: a load after a store to a byte
#     INSIDE it.
#
#  2. The answers. tests/exec/loadcse-alias.c is the program whose every
#     case is a store that DOES reach a remembered load -- a byte inside
#     a word, another pointer into the same object, a different base
#     value that lands on the same bytes -- run on QEMU's Cortex-M3 and
#     RV32 boards at the same three levels. (On x86-64 it runs with every
#     tests/exec program, at -O0 under make test and at -O1 in
#     optimizer.sh.) The program was checked against compilers that keep
#     too much: one whose stores never kill, one that compares offsets
#     from different bases, one that takes every access as one byte, one
#     whose dead-store test is the wrong way round, and one that adds a
#     subtracted offset each fail it.
#
#  3. A vector store kills loads too: load CSE once let a load from
#     before a vectorized loop be reused after the loop.
set -u
echo "TEST-MARKER loadcse-alias"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/loadcse-alias
rm -rf "$out"; mkdir -p "$out"

cat > "$out/q.c" <<'EOF2'
struct q { char *head, *tail, *wr, *rd; unsigned len, isz; };

/* one block: head, len and isz are read once */
void reset(struct q *p)
{
    p->tail = p->head + p->len * p->isz;
    p->wr = p->head;
    p->rd = p->head + (p->len - 1u) * p->isz;
}

/* across a branch: len is read once */
unsigned across(struct q *p, int c)
{
    unsigned n = p->len;
    if (c)
        p->wr = p->head;
    else
        p->rd = 0;
    return n + p->len;
}

/* len = 1 is overwritten, and the read of isz between cannot see it */
void twice(struct q *p)
{
    p->len = 1;
    p->isz = p->isz + 1;
    p->len = 2;
}

/* the byte store is covered by the word store */
void cover(unsigned *w)
{
    ((unsigned char *)w)[1] = 5;
    *w = 7;
}

/* and here the byte IS inside the word: both loads stay */
unsigned inside(unsigned *w)
{
    unsigned x = *w;
    ((unsigned char *)w)[1] = 5;
    return x + *w;
}
EOF2

count() {                       # count FILE FUNC REGEX -> matches in FUNC
    awk -v f="func @$2 " -v re="$3" 'index($0, f) == 1 { on = 1; next }
        /^}/ { on = 0 } on && $0 ~ re { n++ } END { print n + 0 }' "$1"
}
expect() {                      # expect FILE FUNC REGEX N WHAT
    got=$(count "$1" "$2" "$3")
    [ "$got" = "$4" ] || {
        echo "FAIL: $tag: $2 should have $4 $5, has $got:"
        awk -v f="func @$2 " 'index($0, f) == 1 { on = 1 } on { print }
            /^}/ { on = 0 }' "$1"
        exit 1; }
}

LD='= load\.'
ST='^[ 	]*store'
for T in thumbv7m-none-eabi riscv32-unknown-elf; do
    for opt in -O1 -O2 -Os; do
        tag="$T $opt"
        f="$out/q-$T$opt.ir"
        "$EMBCC" inspect ir --target=$T $opt -c "$out/q.c" -o /dev/null \
            > "$f" 2>&1 || {
            echo "FAIL: $tag: could not compile:"; cat "$f"; exit 1; }
        expect "$f" reset  "$LD" 3 "loads (head, len, isz)"
        expect "$f" across "$LD" 2 "loads (len once, head)"
        expect "$f" twice  "$ST" 2 "stores (isz, then len = 2)"
        expect "$f" cover  "$ST" 1 "store (the word)"
        expect "$f" inside "$LD" 2 "loads (the byte store is inside the word)"
    done
done
echo "Thumb and RV32 at -O1/-O2/-Os: no reload across a store to other bytes"
echo "of the same base, no store a later one covers, and the overlapping"
echo "reload kept"

# ---- 2. the program, on the boards ---------------------------------------
cat > "$out/drv.c" <<'EOF2'
void puts_(const char *s);
void putn(long v);
int lca_main(void);
int main(void)
{
    puts_("exit ");
    putn(lca_main());
    puts_("\n==END==\n");
    return 0;
}
EOF2

prog=$EMBCC_ROOT/tests/exec/loadcse-alias.c
board() {                       # board ARCH TRIPLE QEMU RUNARG
    H=$EMBCC_ROOT/tests/harness/$1
    command -v "$3" >/dev/null 2>&1 || {
        echo "SKIP $2: $3 not found"; return 0; }
    for f in boot io; do
        "$EMBCC" --target=$2 -c "$H/$f.c" -o "$out/$1-$f.o" || {
            echo "FAIL: $2: the harness does not compile"; exit 1; }
        cp "$out/$1-$f.o" "$out/$f.o"
    done
    "$EMBCC" --target=$2 -O1 -c "$out/drv.c" -o "$out/$1-drv.o" || {
        echo "FAIL: $2: the driver does not compile"; exit 1; }
    for opt in -O1 -O2 -Os; do
        o=$out/$1$opt
        "$EMBCC" --target=$2 $opt -Dmain=lca_main -c "$prog" -o "$o.o" || {
            echo "FAIL: $2 $opt: the program does not compile"; exit 1; }
        EMBLD=$EMBCC_ROOT/embld sh "$H/link.sh" "$o.elf" "$out/$1-drv.o" \
            "$o.o" || { echo "FAIL: $2 $opt: could not link"; exit 1; }
        sh "$H/run.sh" "$o.elf" $4 > "$o.txt" 2>&1
        grep -q '==END==' "$o.txt" || {
            echo "FAIL: $2 $opt: the image did not finish:"
            sed -n '1,10p' "$o.txt"; exit 1; }
        grep -q '^exit 42 ' "$o.txt" || {
            echo "FAIL: $2 $opt: tests/exec/loadcse-alias.c returned" \
                 "$(sed -n 's/^exit \([0-9-]*\).*/\1/p' "$o.txt"), not 42"
            exit 1; }
    done
    echo "$2: tests/exec/loadcse-alias.c returns 42 at -O1, -O2 and -Os"
}
export EMBCC_THUMB_HARNESS=$out EMBCC_RISCV_HARNESS=$out
board thumb thumbv7m-none-eabi "${EMBCC_QEMU_ARM:-qemu-system-arm}" ""
board riscv riscv32-unknown-elf "${EMBCC_QEMU_RISCV:-qemu-system-riscv32}" 32

# ---- 3. a vector store is a store ----------------------------------------
# Load CSE's list of writes had every scalar one and not IR_VSTORE, so a
# load from before a vectorized loop was reused after it: this returned
# 2 at -O2 on x86-64, where the loop vectorizes, instead of 1 + 41.
cat > "$out/vec.c" <<'EOF2'
int a[64], b[64];
__attribute__((noinline)) int f(void)
{
    int x = a[0];
    for (int i = 0; i < 64; i++)
        a[i] = b[i] + 1;
    return x + a[0];
}
int main(void)
{
    for (int i = 0; i < 64; i++)
        b[i] = i + 40;
    a[0] = 1;
    return f();
}
EOF2
"$EMBCC" --target="$TARGET" -O2 -c "$out/vec.c" -o "$out/vec.o" || {
    echo "FAIL: the vector program does not compile for $TARGET"; exit 1; }
t_link "$out/vec" "$out/vec.o" || { echo "FAIL: could not link vec"; exit 1; }
t_run "$out/vec" > /dev/null 2>&1; got=$?
[ "$got" = 42 ] || {
    echo "FAIL: $TARGET -O2: a load before a vectorized loop was reused after"
    echo "      it (got $got, want 42)"; exit 1; }
echo "$TARGET -O2: a load is not reused across a vectorized loop's stores"
