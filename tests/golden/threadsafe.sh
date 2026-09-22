#!/bin/sh
# The library's own state, under threads.
#
# Threads landed before the library was ready for them. malloc's block
# lists, a FILE's buffer and position, and the C++ one-shot latch behind
# a function-local static are all shared mutable state that nothing
# guarded, and each fails differently:
#
#   - Two threads in malloc splice the same list node twice. It does not
#     fail there; it fails later, in an unrelated allocation. "malloc
#     segfaults with four threads" was the report, and two threads was
#     enough. One was fine, which is what made it look like a heap bug
#     rather than a missing lock.
#   - Two threads in printf interleave inside a line. Nothing crashes and
#     the program looks like it printed something it never printed.
#   - Two threads reaching `static T x = f();` both run f(). [stmt.dcl]/4
#     is explicit that the second must WAIT, not skip and not repeat.
#
# All three are checked by running them, on a real kernel, because that
# is the only place the concurrency is real. A test that builds and does
# not run proves the lock compiles.
set -eu
echo "TEST-MARKER threadsafe"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/threadsafe
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || { echo "skipped: the thread harness here is x86-64"
                          exit 0; }
LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
CXXDIR=$EMBCC_ROOT/build/libcxx/linux-x86_64
[ -f "$LIBDIR/libc.a" ] || {
    echo "skipped: no $LIBDIR/libc.a (make libc-linux-x86_64)"; exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "skipped: running threads needs a kernel for tests/harness/linux"
    exit 0; }

run() {                            # run NAME [extra archives...]
    name=$1; shift
    "$EMBCC" --target=x86_64-linux-gnu ${XF:-} -c "$out/$name.$EXT" \
        -I"$EMBCC_ROOT/lib/libc/include" \
        -I"$EMBCC_ROOT/lib/libc/os" \
        -I"$EMBCC_ROOT/lib/libcxx/include" \
        -o "$out/$name.o" 2> "$out/$name-cc.log" || {
        echo "FAIL: did not compile $name:"; cat "$out/$name-cc.log"; exit 1; }
    "$EMBCC_ROOT/embld" -o "$out/$name" "$LIBDIR/crt1.o" "$out/$name.o" \
        "$@" "$LIBDIR/libc.a" 2> "$out/$name-ld.log" || {
        echo "FAIL: did not link $name:"; cat "$out/$name-ld.log"; exit 1; }
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/$name" \
        > "$out/$name.txt" 2>&1
    rc=$?
    set -e
}

# ---- 1. the heap ---------------------------------------------------------
#
# Four threads allocating and freeing at once, each writing its own id
# into every block and reading it back before freeing. A block handed to
# two threads shows up as the wrong id -- which is the failure a crash
# would otherwise hide, since a crash says only that something went
# wrong somewhere.
EXT=c
cat > "$out/heap.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define T 4
#define ROUNDS 3000
#define HELD 64

static int work(void *arg)
{
    int id = *(int *)arg;
    void *p[HELD];
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < HELD; i++) {
            size_t n = (size_t)(16 + ((r + i + id) % 300));
            p[i] = malloc(n);
            if (!p[i]) return 1;
            memset(p[i], id + 1, n);        /* claim it */
        }
        for (int i = 0; i < HELD; i++) {
            size_t n = (size_t)(16 + ((r + i + id) % 300));
            unsigned char *q = p[i];
            for (size_t k = 0; k < n; k++)
                if (q[k] != (unsigned char)(id + 1))
                    return 2;               /* somebody else was here */
            free(p[i]);
        }
    }
    return 0;
}

int main(void)
{
    thrd_t t[T];
    int id[T], bad = 0, rc;
    for (int i = 0; i < T; i++) {
        id[i] = i;
        if (thrd_create(&t[i], work, &id[i]) != thrd_success) {
            printf("thrd_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < T; i++) { thrd_join(t[i], &rc); if (rc) bad = rc; }
    if (bad == 2) { printf("a block was handed to two threads at once\n");
                    return 1; }
    if (bad)      { printf("an allocation failed\n"); return 1; }
    printf("heap: %d threads x %d rounds x %d blocks, none shared\n",
           T, ROUNDS, HELD);
    return 42;
}
EOF
run heap
[ "$rc" = 42 ] || {
    echo "FAIL: the heap did not survive four threads (exit $rc):"
    cat "$out/heap.txt"
    echo "      an empty output here means it died rather than reported"
    exit 1; }
head -1 "$out/heap.txt"

# ---- 2. stdio ------------------------------------------------------------
#
# Four threads printing whole lines. Each line carries its writer's id in
# every field, so an interleaved line is one whose fields disagree -- a
# stronger check than counting lines, because a torn line usually still
# ends in a newline.
cat > "$out/io.c" << 'EOF'
#include <stdio.h>
#include <threads.h>

#define T 4
#define LINES 400

static int work(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < LINES; i++)
        printf("line %d %d %d %d %d\n", id, id, id, id, id);
    return 0;
}

int main(void)
{
    thrd_t t[T];
    int id[T], rc;
    for (int i = 0; i < T; i++) {
        id[i] = i;
        if (thrd_create(&t[i], work, &id[i]) != thrd_success) return 1;
    }
    for (int i = 0; i < T; i++) thrd_join(t[i], &rc);
    fflush(stdout);
    return 42;
}
EOF
run io
[ "$rc" = 42 ] || { echo "FAIL: the printf program exited $rc"
                    tail -5 "$out/io.txt"; exit 1; }
# Every line must be one writer's, whole. The harness's own boot output
# is filtered out by matching the shape.
torn=$(grep -c '^line ' "$out/io.txt" || true)
good=$(awk '/^line /{ if (NF==6 && $2==$3 && $3==$4 && $4==$5 && $5==$6) n++ }
            END { print n+0 }' "$out/io.txt")
[ "$torn" = "$good" ] || {
    echo "FAIL: $((torn - good)) of $torn lines were interleaved -- two"
    echo "      threads were inside printf at the same time. For example:"
    awk '/^line /{ if (!(NF==6 && $2==$3 && $3==$4 && $4==$5 && $5==$6)) {
                       print "        " $0; if (++n == 3) exit } }' \
        "$out/io.txt"
    exit 1; }
[ "$good" -ge 1600 ] || {
    echo "FAIL: only $good complete lines; $((4 * 400)) were printed, so"
    echo "      output was lost rather than interleaved"; exit 1; }
echo "stdio: $good lines from 4 threads, not one of them interleaved"

# ---- 3. the seam's own contract: wake ALL -------------------------------
#
# `__os_futex_wake(addr, -1)` means "every waiter", and the Linux backend
# handed the -1 straight to the kernel, whose FUTEX_WAKE stops once it
# has woken `count` -- so a negative count woke NOBODY. It did not fail:
# it returned 0 and the waiters slept forever.
#
# It is checked here rather than trusted because every broadcast in the
# C++ library is this call: condition_variable::notify_all,
# atomic::notify_all, latch, barrier, and the guard below. Nothing
# exercised it with waiters actually asleep, and the EmbLinkOS backend
# beside it had translated the value all along -- so the contract was
# being honoured by one implementation and not the other, which is the
# shape a seam is supposed to prevent.
EXT=c
cat > "$out/wakeall.c" << 'EOF'
#include <stdio.h>
#include <threads.h>
#include "backend.h"

#define T 4
static volatile int word;
static volatile int awake;

static int waiter(void *arg)
{
    (void)arg;
    /* Sleep until `word` changes. A spurious return is allowed, so this
     * loops on the value rather than on the call. */
    while (__atomic_load_n(&word, __ATOMIC_ACQUIRE) == 0)
        __os_futex_wait(&word, 0, -1);
    __atomic_fetch_add(&awake, 1, __ATOMIC_SEQ_CST);
    return 0;
}

int main(void)
{
    thrd_t t[T];
    int rc;
    for (int i = 0; i < T; i++)
        if (thrd_create(&t[i], waiter, 0) != thrd_success) return 1;
    /* Give them time to be asleep rather than still starting up: a wake
     * that arrives before the wait is not the case under test. */
    struct timespec ts = { 0, 200000000L };
    thrd_sleep(&ts, 0);

    __atomic_store_n(&word, 1, __ATOMIC_RELEASE);
    int woken = __os_futex_wake(&word, -1);      /* ALL of them */

    for (int i = 0; i < T; i++) thrd_join(t[i], &rc);
    printf("futex_wake(-1) reported %d, %d of %d threads came back\n",
           woken, awake, T);
    return awake == T ? 42 : 1;
}
EOF
run wakeall
[ "$rc" = 42 ] || {
    echo "FAIL: futex_wake(addr, -1) did not wake every waiter (exit $rc)."
    cat "$out/wakeall.txt"
    echo "      A timeout here is the symptom: the sleepers were never"
    echo "      woken, and every broadcast in the C++ library is this call"
    exit 1; }
head -1 "$out/wakeall.txt"

# ---- 4. the C++ one-shot latch -------------------------------------------
[ -f "$CXXDIR/libcxx.a" ] || {
    echo "skipped the C++ half: no $CXXDIR/libcxx.a (make libcxx-linux-x86_64)"
    exit 0; }
EXT=cc
# -fno-exceptions because the Linux target has no unwinder (D-014), and
# this program does not throw: without it the compiler still emits the
# cleanup landing pads, and `_Unwind_Resume` does not link here. That is
# EmbLD saying so, by name, rather than an undefined symbol nobody has
# heard of.
XF="-x c++ -fno-exceptions"
cat > "$out/guard.cc" << 'EOF'
/* A function-local static whose initialiser is SLOW, so the window the
 * guard is about is wide enough to lose. Four threads reach it at once:
 * exactly one must run the initialiser, and the other three must not
 * proceed until it has finished -- [stmt.dcl]/4, which says "waits",
 * not "may skip". */
#include <stdio.h>
#include <threads.h>

static volatile int runs;
static volatile int inside;
static int overlap;

struct Slow {
    int value;
    Slow()
    {
        __atomic_fetch_add(&runs, 1, __ATOMIC_SEQ_CST);
        if (__atomic_fetch_add(&inside, 1, __ATOMIC_SEQ_CST) != 0)
            overlap = 1;              /* two initialisers at once */
        /* Long enough that another thread certainly arrives. */
        for (volatile long i = 0; i < 4000000; i++) { }
        value = 1234;
        __atomic_fetch_sub(&inside, 1, __ATOMIC_SEQ_CST);
    }
};

static int seen[8];

static Slow &get() { static Slow s; return s; }

static int work(void *arg)
{
    int id = *(int *)arg;
    seen[id] = get().value;           /* must be the FINISHED object */
    return 0;
}

int main(void)
{
    thrd_t t[4];
    int id[4], rc;
    for (int i = 0; i < 4; i++) {
        id[i] = i;
        if (thrd_create(&t[i], work, &id[i]) != thrd_success) return 1;
    }
    for (int i = 0; i < 4; i++) thrd_join(t[i], &rc);

    if (runs != 1) { printf("the initialiser ran %d times, not once\n", runs);
                     return 1; }
    if (overlap)   { printf("two threads ran the initialiser at once\n");
                     return 2; }
    for (int i = 0; i < 4; i++)
        if (seen[i] != 1234) {
            printf("thread %d saw %d: it did not wait for the "
                   "initialisation\n", i, seen[i]);
            return 3;
        }
    printf("static init: one initialiser, four threads, all saw it finished\n");
    return 42;
}
EOF
run guard "$CXXDIR/libcxx.a"
[ "$rc" = 42 ] || {
    echo "FAIL: the guard program exited $rc:"; cat "$out/guard.txt"; exit 1; }
head -1 "$out/guard.txt"
