#!/bin/sh
# A Darwin program EmbCC compiles links against Apple's C library, and
# reads the SDK's headers for it (with EmbCC's fixes in include/darwin:
# NAN, which the SDK spells as an x86-only function for a compiler without
# __GNUC__). Until 2026-10 they were EmbCC's own, made to match; either
# way the headers have to describe THAT library, not EmbCC's own:
# its stream and errno names (`stderr` did not link), its errno numbers
# (EAGAIN is 35), and the size of every object it writes into -- a
# 12-byte mbstate_t, a 36-byte struct tm and an 8-byte fenv_t were
# overrun by mbrtowc, mktime and fegetenv, and a 32-byte L_tmpnam buffer
# by tmpnam, all silently.
#
# Two checks. The layout of every such type and the value of every such
# constant must equal what the system compiler sees through the SDK's
# headers, for arm64 and x86-64 (compared as data, so x86-64 needs no
# Rosetta). And a program that uses each of them, compiled by EmbCC at -O0
# and -O2 and run natively, must print what the same program compiled by
# the system compiler prints, with guard words around each object Apple's
# library writes intact.
set -u
echo "TEST-MARKER darwin-libsystem"
. "$(dirname "$0")/../lib.sh"

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || {
    echo "SKIP: needs macOS on arm64"; exit 0; }
command -v cc > /dev/null 2>&1 || { echo "SKIP: no system cc"; exit 0; }

EMBCC=${EMBCC:-./embcc}
out=$PWD/tests/golden/out/darwin-libsystem
rm -rf "${out:?}"; mkdir -p "$out"

# ---- 1. the layouts and constants, as data -----------------------------
cat > "$out/lay.c" <<'CEOF'
#include <setjmp.h>
#include <wchar.h>
#include <time.h>
#include <fenv.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
long lay[] = {
    sizeof(jmp_buf), sizeof(mbstate_t), sizeof(struct tm), sizeof(fenv_t),
    sizeof(fexcept_t), sizeof(struct lconv), sizeof(fpos_t), sizeof(clock_t),
    offsetof(struct tm, tm_isdst), RAND_MAX, CLOCKS_PER_SEC, BUFSIZ,
    FILENAME_MAX, L_tmpnam, TMP_MAX, FOPEN_MAX, LC_ALL, LC_NUMERIC,
    FE_INVALID, FE_DIVBYZERO, FE_OVERFLOW, FE_UNDERFLOW, FE_INEXACT,
    FE_ALL_EXCEPT, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO,
    FP_NAN, FP_INFINITE, FP_ZERO, FP_NORMAL, FP_SUBNORMAL,
    EPERM, ENOENT, EAGAIN, EDEADLK, ENAMETOOLONG, ENOLCK, ENOSYS, ENOTEMPTY,
    ELOOP, EOVERFLOW, EILSEQ, ECANCELED, ETIMEDOUT, ECONNREFUSED, ENOTSUP,
    EINPROGRESS, EWOULDBLOCK };
CEOF
data() { llvm-objdump -s --section=__data "$1" | sed -n '/Contents/,$p' |
         tail -n +2 | awk '{print $2 $3 $4 $5}' | tr -d '\n'; }
for a in aarch64 x86_64; do
    "$EMBCC" --target=$a-apple-darwin -c "$out/lay.c" -o "$out/lay-e-$a.o" || {
        echo "FAIL: lay.c does not compile for $a-apple-darwin"; exit 1; }
    cc -target $a-apple-darwin -c "$out/lay.c" -o "$out/lay-c-$a.o" 2>/dev/null || {
        echo "SKIP the $a layouts: the system compiler cannot target it"; continue; }
    [ "$(data "$out/lay-e-$a.o")" = "$(data "$out/lay-c-$a.o")" ] || {
        echo "FAIL: on $a-apple-darwin a type or constant differs from the SDK's"
        exit 1; }
done
echo "every libSystem type and constant is laid out as the SDK has it, arm64 and x86-64"

# ---- 2. a program that uses them, run natively -------------------------
cat > "$out/use.c" <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>
#include <time.h>
#include <fenv.h>
#include <math.h>
#include <setjmp.h>
#include <assert.h>
#include <ctype.h>
#include <locale.h>
#define GUARD 0x5a5a5a5a5a5a5a5aULL
struct { unsigned long long a; mbstate_t s; unsigned long long b; } m;
struct { unsigned long long a; struct tm t; unsigned long long b; } tmg;
struct { unsigned long long a; fenv_t e; unsigned long long b; } fe;
static jmp_buf jb;
static volatile double third = 3.0;
static void jump(void) { longjmp(jb, 7); }
int main(void)
{
    FILE *f = fopen("/nonexistent/dir/x", "r");
    printf("fopen: %s errno=%d enoent=%d eagain=%d\n", f ? "opened" : "failed",
           errno, errno == ENOENT, EAGAIN);
    fprintf(stderr, "to stderr\n");

    /* UTF-8, so a character spans calls and the state carries it */
    printf("locale: %s\n", setlocale(LC_CTYPE, "en_US.UTF-8") ? "utf-8" : "none");
    m.a = m.b = GUARD;
    memset(&m.s, 0, sizeof m.s);
    const char *u8 = "\xc3\xa9\xe2\x82\xac";          /* e-acute, euro */
    wchar_t wc = 0;
    size_t r1 = mbrtowc(&wc, u8, 1, &m.s);           /* a partial character */
    size_t r2 = mbrtowc(&wc, u8 + 1, 1, &m.s);       /* ...completed */
    printf("mbrtowc: %ld %ld U+%04X guards=%d\n", (long)r1, (long)r2,
           (unsigned)wc, m.a == GUARD && m.b == GUARD);

    tmg.a = tmg.b = GUARD;
    memset(&tmg.t, 0, sizeof tmg.t);
    tmg.t.tm_year = 100; tmg.t.tm_mon = 0; tmg.t.tm_mday = 31 + 29 + 1;
    tmg.t.tm_hour = 12; tmg.t.tm_isdst = -1;
    time_t when = mktime(&tmg.t);
    printf("mktime: %s mon=%d mday=%d guards=%d\n", when == (time_t)-1 ? "fail" : "ok",
           tmg.t.tm_mon, tmg.t.tm_mday, tmg.a == GUARD && tmg.b == GUARD);

    fe.a = fe.b = GUARD;
    fegetenv(&fe.e);
    fesetround(FE_UPWARD);
    double up = 1.0 / third;
    fesetround(FE_DOWNWARD);
    double down = 1.0 / third;
    fesetenv(&fe.e);
    printf("fenv: round=%d up>down=%d guards=%d\n", fegetround() == FE_TONEAREST,
           up > down, fe.a == GUARD && fe.b == GUARD);

    printf("class: nan=%d inf=%d zero=%d normal=%d sub=%d\n",
           isnan(NAN) != 0, isinf(INFINITY) != 0, fpclassify(0.0) == FP_ZERO,
           fpclassify(1.0) == FP_NORMAL, fpclassify(4.9e-324) == FP_SUBNORMAL);

    int j = setjmp(jb);
    if (j == 0) jump();
    printf("setjmp: %d\n", j);

    assert(j == 7);
    printf("ctype: %d %c\n", isalpha('q') != 0, toupper('q'));
    return 0;
}
CEOF
cc -w -o "$out/use-c" "$out/use.c" && "$out/use-c" > "$out/want.txt" 2> "$out/want.err" || {
    echo "FAIL: the reference program does not build or run"; exit 1; }
for O in -O0 -O2; do
    "$EMBCC" --target=aarch64-apple-darwin $O -c "$out/use.c" -o "$out/use$O.o" || {
        echo "FAIL: use.c does not compile at $O"; exit 1; }
    cc -o "$out/use$O" "$out/use$O.o" 2> "$out/link$O.txt" || {
        echo "FAIL: $O: the EmbCC object does not link against libSystem:"
        head -5 "$out/link$O.txt"; exit 1; }
    "$out/use$O" > "$out/got$O.txt" 2> "$out/got$O.err" || {
        echo "FAIL: $O: the program exited $?"; cat "$out/got$O.txt"; exit 1; }
    cmp -s "$out/want.txt" "$out/got$O.txt" && cmp -s "$out/want.err" "$out/got$O.err" || {
        echo "FAIL: $O: EmbCC's program and the system compiler's disagree:"
        diff "$out/want.txt" "$out/got$O.txt"; exit 1; }
done
echo "and a program using each of them agrees with the system compiler's, at -O0 and -O2"
