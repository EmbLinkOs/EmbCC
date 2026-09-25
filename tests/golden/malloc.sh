#!/bin/sh
# The allocator: that it hands out memory nobody else is using, that the
# memory keeps what was written to it, and that it does so in time
# proportional to the work rather than to the square of it.
#
# The last one is why this exists. The first version searched ONE list --
# every block, in address order -- from the head on every malloc. A
# program that allocates and does not free puts every live block ahead of
# every free one, so each allocation walked all of them. Measured on the
# Linux target: 2000 allocations 0.055s, 4000 0.199s, 8000 0.791s, 16000
# 3.281s -- four times the work for twice the blocks -- and 60000 did not
# finish inside the harness's twenty-second timeout. A compiler parsing a
# file is exactly that program, which is why `embcc` built against this
# libc was unusable on a large source.
#
# Correctness is checked first and separately, because an allocator that
# is fast and wrong is worse than the one it replaced. The check that
# matters most is OVERLAP: two live allocations must never share a byte,
# and a first-fit allocator with coalescing gets that wrong by splitting
# or joining one block too many.
set -eu
echo "TEST-MARKER malloc"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/malloc
rm -rf "$out"; mkdir -p "$out"

LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
[ "$ARCH" = x86_64 ] || { echo "skipped: the libc harness is x86-64"; exit 0; }
[ -f "$LIBDIR/libc.a" ] || {
    echo "skipped: no $LIBDIR/libc.a (make libc-linux-x86_64)"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped: running it needs a kernel for tests/harness/linux"
    exit 0; }

build() {                          # build SRC OUT
    "$EMBCC" --target=x86_64-linux-gnu -c "$1" -o "$out/o.o" 2> "$out/cc.log" \
        || { echo "FAIL: did not compile:"; cat "$out/cc.log"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$2" "$LIBDIR/crt1.o" "$out/o.o" "$LIBDIR/libc.a" \
        2> "$out/ld.log" || {
        echo "FAIL: did not link:"; cat "$out/ld.log"; exit 1; }
}

# ---- 1. correctness ------------------------------------------------------
cat > "$out/c.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 4000
static unsigned char *p[N];
static size_t sz[N];

/* A cheap deterministic pattern, so a byte can say which allocation it
 * belongs to and at what offset. Anything that overlaps, or that moves
 * without copying, shows up as a mismatch. */
static unsigned char pat(int i, size_t k) { return (unsigned char)(i * 31 + k); }
static void fill(int i) { for (size_t k = 0; k < sz[i]; k++) p[i][k] = pat(i, k); }
static int  ok(int i)
{
    for (size_t k = 0; k < sz[i]; k++)
        if (p[i][k] != pat(i, k)) {
            printf("corrupt: alloc %d byte %lu is %u, wanted %u\n",
                   i, (unsigned long)k, p[i][k], pat(i, k));
            return 0;
        }
    return 1;
}

int main(void)
{
    int i, round;

    /* Sizes across the interesting boundaries: below the 16-byte
     * minimum, exactly at it, either side of the 32-byte header, and
     * large enough to force the allocator to ask the OS for more. */
    for (i = 0; i < N; i++) {
        sz[i] = (size_t)(i % 7 == 0 ? 1 + i % 5
                       : i % 11 == 0 ? 4096 + i
                       : 16 + i % 200);
        p[i] = malloc(sz[i]);
        if (!p[i]) { printf("malloc failed at %d\n", i); return 1; }
        fill(i);
    }
    for (i = 0; i < N; i++) if (!ok(i)) return 2;

    /* Every live allocation must be disjoint from every other. O(n^2) in
     * the number of live blocks, so it runs on a subset. */
    for (i = 0; i < 400; i++)
        for (int j = i + 1; j < 400; j++) {
            unsigned char *a = p[i], *b = p[j];
            if (a < b + sz[j] && b < a + sz[i]) {
                printf("overlap: %d and %d share bytes\n", i, j);
                return 3;
            }
        }

    /* Free in a scattered order, reallocate, and check the survivors.
     * Coalescing runs here: freeing 3 out of every 4 leaves runs of
     * adjacent free blocks that must join without swallowing a live
     * neighbour. */
    for (round = 0; round < 3; round++) {
        for (i = 0; i < N; i++)
            if ((i + round) % 4 != 0) { free(p[i]); p[i] = 0; }
        for (i = 0; i < N; i++) if (p[i] && !ok(i)) return 4;
        for (i = 0; i < N; i++)
            if (!p[i]) {
                sz[i] = (size_t)(16 + (i * 7 + round) % 300);
                p[i] = malloc(sz[i]);
                if (!p[i]) { printf("malloc failed in round %d\n", round); return 5; }
                fill(i);
            }
        for (i = 0; i < N; i++) if (!ok(i)) return 6;
    }
    for (i = 0; i < N; i++) free(p[i]);

    /* realloc: grows, shrinks, keeps contents, and behaves as malloc/free
     * at its two edges. */
    unsigned char *r = malloc(32);
    for (size_t k = 0; k < 32; k++) r[k] = (unsigned char)k;
    for (size_t want = 64; want <= 65536; want *= 2) {
        r = realloc(r, want);
        if (!r) { printf("realloc to %lu failed\n", (unsigned long)want); return 7; }
        for (size_t k = 0; k < 32; k++)
            if (r[k] != (unsigned char)k) {
                printf("realloc lost byte %lu growing to %lu\n",
                       (unsigned long)k, (unsigned long)want);
                return 8;
            }
    }
    r = realloc(r, 16);
    for (size_t k = 0; k < 16; k++)
        if (r[k] != (unsigned char)k) { printf("realloc lost a byte shrinking\n");
                                        return 9; }
    if (realloc(r, 0) != 0) { printf("realloc(p,0) did not return NULL\n"); return 10; }
    r = realloc(0, 24);
    if (!r) { printf("realloc(0,n) did not allocate\n"); return 11; }
    free(r);

    /* calloc zeroes, and refuses a product that would wrap. */
    unsigned char *z = calloc(1000, 3);
    if (!z) { printf("calloc failed\n"); return 12; }
    for (size_t k = 0; k < 3000; k++)
        if (z[k]) { printf("calloc did not zero byte %lu\n", (unsigned long)k);
                    return 13; }
    free(z);
    if (calloc((size_t)-1 / 2, 4) != 0) {
        printf("calloc accepted a product that overflows\n"); return 14; }

    /* aligned_alloc: the alignment is real, the memory is usable, and --
     * the part that is easy to get wrong -- plain free() takes it back,
     * because C requires that and there is only one kind of block. */
    for (size_t al = 32; al <= 4096; al *= 2) {
        void *q[16];
        for (i = 0; i < 16; i++) {
            q[i] = aligned_alloc(al, al * 2 + 17);
            if (!q[i]) { printf("aligned_alloc(%lu) failed\n",
                                (unsigned long)al); return 15; }
            if ((size_t)q[i] % al) {
                printf("aligned_alloc(%lu) returned a misaligned pointer\n",
                       (unsigned long)al);
                return 16;
            }
            memset(q[i], 0xAB, al * 2 + 17);
        }
        for (i = 0; i < 16; i++) {
            unsigned char *u = q[i];
            for (size_t k = 0; k < al * 2 + 17; k++)
                if (u[k] != 0xAB) { printf("aligned_alloc block damaged\n");
                                    return 17; }
            free(q[i]);                /* plain free, as C requires */
        }
    }

    /* malloc(0) is a unique address that free accepts. */
    void *a0 = malloc(0), *b0 = malloc(0);
    if (!a0 || !b0 || a0 == b0) { printf("malloc(0) is not a unique address\n");
                                  return 18; }
    free(a0); free(b0);
    free(0);                           /* defined, does nothing */

    printf("allocator: every check passed\n");
    return 42;
}
EOF
build "$out/c.c" "$out/c.bin"
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/c.bin" > "$out/c.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || {
    echo "FAIL: the correctness program exited $rc:"; cat "$out/c.txt"; exit 1; }
grep -q "every check passed" "$out/c.txt" || { cat "$out/c.txt"; exit 1; }
echo "correct: no two live allocations overlap, contents survive
coalescing, and realloc, calloc, aligned_alloc + plain free and
malloc(0) all behave"

# ---- 2. the cost is linear ----------------------------------------------
#
# Not "fast enough", which is a number that rots, but the SHAPE: doubling
# the number of allocations must not quadruple the time. The old
# allocator failed this by construction, and no correctness test could
# have noticed.
cat > "$out/t.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define BIG 40000
static void *p[BIG];
static double run(int n)
{
    double a = (double)clock() / CLOCKS_PER_SEC;
    for (int i = 0; i < n; i++) { p[i] = malloc(16 + (i % 48)); if (!p[i]) exit(1); }
    double b = (double)clock() / CLOCKS_PER_SEC;
    for (int i = 0; i < n; i++) free(p[i]);
    return b - a;
}
int main(void)
{
    double small = run(BIG / 4), big = run(BIG);
    /* Four times the allocations. Linear says about four times the time;
     * quadratic says about sixteen. The clock has a floor, so a tiny
     * `small` is reported rather than divided by. */
    printf("%d allocs %.4fs, %d allocs %.4fs\n", BIG / 4, small, BIG, big);
    if (small < 0.0005) { printf("ratio: below the clock's resolution\n");
                          return 42; }
    double ratio = big / small;
    printf("ratio %.1f (linear ~4, quadratic ~16)\n", ratio);
    return ratio < 8.0 ? 42 : 1;
}
EOF
build "$out/t.c" "$out/t.bin"
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/t.bin" > "$out/t.txt" 2>&1
rc=$?
set -e
cat "$out/t.txt"
[ "$rc" = 42 ] || {
    echo "FAIL: four times the allocations cost far more than four times"
    echo "      the time, which is the quadratic search coming back"
    exit 1; }
echo "the cost of an allocation does not grow with the number of LIVE blocks"
