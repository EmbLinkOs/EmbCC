#!/bin/sh
# Our C library (lib/libc) — the acceptance is the execution corpus.
#
# A compiler that targets several operating systems cannot borrow one OS's
# C library. The question is not whether `lib/libc` compiles; it is whether
# real programs behave the same on it as on the library it replaces. So the
# test is the whole of tests/exec, compiled and linked against our libc with
# NO NEWLIB AT ALL, each program checked against its own expected exit.
set -eu
echo "TEST-MARKER libc"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/libc
rm -rf "$out"; mkdir -p "$out"

# ---- setjmp: the bytes ARE the comments beside them -----------------------
# setjmp and longjmp cannot be written in C -- they save the registers the
# ABI says a callee must preserve and resume at a stored address -- so they
# are machine code placed with .byte/.long, each instruction carrying its
# disassembly in a comment. A comment is not checked by anything, which is
# how an encoding rots. So check it: strip the bytes from one column and the
# mnemonics from the other, assemble the mnemonics with the platform's own
# assembler, and require the two to agree byte for byte. (This caught the
# aarch64 `stp d8, d9` encoding being wrong when it was first written.)
cat > "$out/split.awk" << 'AWK'
function hexval(s,   i, c, v, d) {
    v = 0
    for (i = 1; i <= length(s); i++) {
        c = tolower(substr(s, i, 1))
        d = index("0123456789abcdef", c) - 1
        if (d >= 0) v = v * 16 + d
    }
    return v
}
{
    i = index($0, dir);   if (!i) next
    h = index($0, "#");   if (!h) next
    vals = substr($0, i + length(dir), h - i - length(dir))
    gsub(/[ \t]/, "", vals)
    n = split(vals, a, ",")
    for (k = 1; k <= n; k++) {              # little-endian, both targets
        v = hexval(a[k])
        for (b = 0; b < width; b++) { printf "%02x\n", v % 256 > bytes
                                      v = int(v / 256) }
    }
    m = substr($0, h + 1)
    sub(/\\n".*$/, "", m)
    gsub(/^[ \t]+|[ \t]+$/, "", m)
    print m > asm
}
AWK

verify_setjmp() {           # verify_setjmp <arch> <assembler> <directive> <w>
    src=lib/libc/src/setjmp/setjmp-$1.c
    command -v "$2" > /dev/null 2>&1 ||
        { echo "  ($1: $2 absent, not checked)"; return 0; }
    rm -f "$out/$1-bytes.txt" "$out/$1.s"
    awk -v dir=".$3 " -v width="$4" -v bytes="$out/$1-bytes.txt" \
        -v asm="$out/$1.s" -f "$out/split.awk" "$src"
    [ -s "$out/$1.s" ] || { echo "FAIL: no instructions found in $src"; exit 1; }
    "$2" "$out/$1.s" -o "$out/$1.o" ||
        { echo "FAIL: the disassembly comments in $src are not valid asm"
          exit 1; }
    "${2%as}objcopy" -O binary -j .text "$out/$1.o" "$out/$1.bin"
    od -An -tx1 -v "$out/$1.bin" | tr -s ' ' '\n' | sed '/^$/d' \
        > "$out/$1-asm.txt"
    diff "$out/$1-bytes.txt" "$out/$1-asm.txt" > "$out/$1-diff.txt" ||
        { head -20 "$out/$1-diff.txt"
          echo "FAIL: $src: the bytes and their disassembly disagree"; exit 1; }
    echo "  $1: $(wc -l < "$out/$1.s" | tr -d ' ') instructions; the bytes are
  what ${2##*/} produces for the comments beside them"
}
verify_setjmp x86_64  "${EMBCC_REFEREE_AS:-x86_64-elf-as}"    byte 1
verify_setjmp aarch64 "${EMBCC_AARCH64_AS:-aarch64-elf-as}" long 4

[ "${ARCH:-x86_64}" = x86_64 ] ||
    { echo "skipped: the libc harness is x86-64 (aarch64 next)"; exit 0; }
command -v x86_64-elf-ld > /dev/null 2>&1 ||
    { echo "skipped: no x86_64-elf-ld"; exit 0; }

LIB=build/libc/x86_64/libc.a
[ -f "$LIB" ] || { echo "skipped: $LIB absent (make libc-x86_64)"; exit 0; }

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
LIBGCC=$(dirname "$($GCC -print-libgcc-file-name)")
H=tests/harness/x86_64

# The harness's entry and syscall shims stay: they are the BACKEND this
# library was designed to sit on (lib/libc/os/backend.h). Everything above
# them -- buffering, formatting, allocation -- is ours.
for part in start sys crt; do
    src=$H/$part.S; [ "$part" = sys ] && src=$H/sys.c
    [ "$part" = crt ] && src=$H/../crt.c
    $GCC -ffreestanding -mno-red-zone -isystem "$X86_NEWLIB/include" \
        -c "$src" -o "$out/$part.o"
done

build_run() {              # build_run <source> <extra-cc-flags>
    "$EMBCC" -c -Ilib/libc/include "$2" "$1" -o "$out/p.o" 2>"$out/cc.txt" ||
        return 91
    x86_64-elf-ld -n -z max-page-size=0x1000 -T "$H/link.ld" -o "$out/p.64" \
        "$out/start.o" "$out/p.o" "$out/sys.o" "$out/crt.o" "$LIB" \
        -L"$LIBGCC" -lgcc 2>"$out/ld.txt" || return 92
    x86_64-elf-objcopy -I elf64-x86-64 -O elf32-i386 "$out/p.64" "$out/p.elf"
    "$H/../x86_64/run.sh" "$out/p.elf" > "$out/run.txt" 2>&1
    return $?
}

# ---- a program that exercises the visible surface -------------------------
cat > "$out/surface.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
int main(void)
{
    char *p = malloc(64);
    strcpy(p, "hello");
    strcat(p, ", libc");
    printf("[%s] len=%zu cmp=%d\n", p, strlen(p), strcmp(p, "hello, libc"));
    printf("%d %#x %o [%8.3f] [%-6d] %ld %llu\n",
           42, 255, 64, 3.14159, -7, 1234567890L, 18446744073709551615ULL);
    printf("%e %g %s %c %%\n", 1234.5678, 0.0001234, "str", 'x');
    printf("sqrt=%.6f pow=%.6f log=%.6f sin=%.6f\n",
           sqrt(2.0), pow(2.0, 10.0), log(2.718281828459045), sin(0.0));
    printf("strtol=%ld strtod=%.4f isdigit=%d toupper=%c\n",
           strtol("-0x2a", NULL, 0), 0.0, !!isdigit('7'), toupper('z'));
    int a[7] = {9,2,7,1,8,3,5};
    qsort(a, 7, sizeof a[0], (int (*)(const void *, const void *))
          ({ int cmp(const void *x, const void *y)
             { return *(const int *)x - *(const int *)y; } cmp; }));
    printf("sorted:"); for (int i = 0; i < 7; i++) printf(" %d", a[i]);
    printf("\n");
    free(p);
    return 42;
}
EOF
# The statement-expression trick above is a GNU extension; keep the test
# portable by sorting with a plain function instead.
cat > "$out/surface.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
static int icmp(const void *x, const void *y)
{ int a = *(const int *)x, b = *(const int *)y; return a < b ? -1 : a > b; }
int main(void)
{
    char *p = malloc(64);
    strcpy(p, "hello");
    strcat(p, ", libc");
    printf("[%s] len=%zu cmp=%d\n", p, strlen(p), strcmp(p, "hello, libc"));
    printf("%d %#x %o [%8.3f] [%-6d] %ld %llu\n",
           42, 255, 64, 3.14159, -7, 1234567890L, 18446744073709551615ULL);
    printf("%e %g %s %c %%\n", 1234.5678, 0.0001234, "str", 'x');
    printf("sqrt=%.6f pow=%.6f log=%.6f sin=%.6f\n",
           sqrt(2.0), pow(2.0, 10.0), log(2.718281828459045), sin(0.0));
    printf("strtol=%ld isdigit=%d toupper=%c\n",
           strtol("-0x2a", NULL, 0), !!isdigit('7'), toupper('z'));
    int a[7] = {9,2,7,1,8,3,5};
    qsort(a, 7, sizeof a[0], icmp);
    printf("sorted:"); for (int i = 0; i < 7; i++) printf(" %d", a[i]);
    printf("\n");
    free(p);
    return 42;
}
EOF
# `set -e` would abort on the program's own non-zero exit, which is the
# normal case here -- these programs report their result THROUGH it.
if build_run "$out/surface.c" -O1; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the surface program exited $rc"; exit 1; }
grep -q "^\[hello, libc\] len=11 cmp=0$" "$out/run.txt" ||
    { echo "FAIL: string handling"; exit 1; }
grep -q "^42 0xff 100 \[   3.142\] \[-7    \] 1234567890 18446744073709551615$" \
    "$out/run.txt" || { echo "FAIL: integer and width formatting"; exit 1; }
# %g strips trailing zeros; %e keeps its two-digit exponent.
grep -q "^1.234568e+03 0.0001234 str x %$" "$out/run.txt" ||
    { echo "FAIL: floating-point formatting"; exit 1; }
grep -q "sqrt=1.414214 pow=1024.000000 log=1.000000 sin=0.000000" \
    "$out/run.txt" || { echo "FAIL: math"; exit 1; }
grep -q "^sorted: 1 2 3 5 7 8 9$" "$out/run.txt" || { echo "FAIL: qsort"; exit 1; }
echo "the visible surface: strings, every printf conversion, math, qsort"

# ---- the second surface: control flow, the calendar, and input ------------
# These are the parts a libc gets subtly wrong and nobody notices for years:
# longjmp through several frames, dates before the epoch and on a leap day,
# and scanf's two different kinds of failure.
cat > "$out/hosted.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <time.h>
#include <inttypes.h>
#include <assert.h>
static jmp_buf jb;
static int depth;
static void inner(int n) { if (n == 0) longjmp(jb, 7); inner(n - 1); }
int main(void)
{
    int v = setjmp(jb);
    if (v == 0) { depth = 5; inner(depth); }
    printf("longjmp v=%d depth=%d\n", v, depth);
    jmp_buf jb2;
    if ((v = setjmp(jb2)) == 0) longjmp(jb2, 0);
    printf("longjmp0 %d\n", v);

    struct tm tm; char buf[128]; time_t t = 0;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %a %j", &tm);
    printf("epoch %s\n", buf);
    t = 951782400; gmtime_r(&t, &tm);          /* 2000-02-29 */
    strftime(buf, sizeof buf, "%F %A", &tm);
    printf("leap %s\n", buf);
    t = -1; gmtime_r(&t, &tm);                 /* before the epoch */
    strftime(buf, sizeof buf, "%F %T", &tm);
    printf("before %s\n", buf);
    tm.tm_year = 124; tm.tm_mon = 0; tm.tm_mday = 32;
    tm.tm_hour = 12; tm.tm_min = tm.tm_sec = tm.tm_isdst = 0;
    timegm(&tm);
    strftime(buf, sizeof buf, "%F", &tm);
    printf("norm %s\n", buf);
    t = 1234567890;
    printf("ctime %s", ctime(&t));
    printf("roundtrip %d\n", (int)(timegm(gmtime(&t)) == t));

    int a, b, consumed; char w[32], set[32]; double d; long long big;
    unsigned u;
    int n = sscanf("  42 -17 hello 3.5e2 0x2a", "%d %d %s %lf %llx",
                   &a, &b, w, &d, &big);
    printf("scan n=%d %d %d %s %g %lld\n", n, a, b, w, d, big);
    n = sscanf("ff,12|rest", "%x,%2u%n", &u, &a, &consumed);
    printf("scan2 n=%d %u %d %d\n", n, u, a, consumed);
    n = sscanf("abc123def", "%[a-c]%[0-9]", w, set);
    printf("scanset n=%d %s %s\n", n, w, set);
    printf("fail %d eof %d\n", sscanf("x", "%d", &a), sscanf("", "%d", &a));
    int64_t big2 = 0;
    sscanf("-9223372036854775807", "%" SCNd64, &big2);
    printf("scn %" PRId64 " %" PRIdMAX "\n", big2, imaxabs(-5));

    /* strtod. The fast path is EXACT -- at most 15 significant digits
     * and |exponent| <= 22 means two exactly representable operands and
     * one rounding, which IEEE makes the nearest value. So these compare
     * equal to the literals rather than merely close to them. */
    char *e;
    printf("strtod %d %d %d %d\n",
           strtod("1.5", &e) == 1.5 && *e == 0,
           strtod("-0.125e1", &e) == -1.25,
           strtod("1e22", &e) == 1e22,
           strtod("0x1.8p3", &e) == 12.0 && *e == 0);
    printf("strtod2 %d %d %d\n",
           strtod("  12.5rest", &e) == 12.5 && e[0] == 'r',
           /* Nothing converted: zero, and `end` back at the start. */
           strtod("abc", &e) == 0.0,
           strtod("inf", &e) > 1e308 && strtod("-inf", &e) < -1e308);
    assert(a != 999999);
    return 42;
}
EOF
if build_run "$out/hosted.c" -O2; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the hosted program exited $rc"; exit 1; }
want_line() {
    grep -qx "$1" "$out/run.txt" || { echo "FAIL: expected \"$1\""; exit 1; }
}
# longjmp unwound five frames and delivered its value; longjmp(env, 0)
# returns 1 so the caller can always tell the jump from the first call.
want_line "longjmp v=7 depth=5"
want_line "longjmp0 1"
want_line "epoch 1970-01-01 00:00:00 Thu 001"
want_line "leap 2000-02-29 Tuesday"
# -1 second is the last second of 1969, not a negative time of day: the
# conversion floors, it does not truncate toward zero.
want_line "before 1969-12-31 23:59:59"
want_line "norm 2024-02-01"
want_line "ctime Fri Feb 13 23:31:30 2009"
want_line "roundtrip 1"
want_line "scan n=5 42 -17 hello 350 42"
want_line "scan2 n=2 255 12 5"
want_line "scanset n=2 abc 123"
# The two failures scanf must distinguish: "x" is input that does not match
# (0), "" is no input at all (EOF). A libc that returns EOF for both breaks
# every read loop written against it.
want_line "fail 0 eof -1"
want_line "scn -9223372036854775807 5"
echo "setjmp/longjmp across frames, the calendar before and after the epoch
and on a leap day, strftime, and scanf's matching and input failures"

# ---- the acceptance: the whole execution corpus ---------------------------
ok=0; bad=0
for f in tests/exec/*.c; do
    case "$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$f" | head -1)" in
        aarch64*) continue ;;
    esac
    want=$(sed -n 's|.*// expect-exit: *\([0-9]*\).*|\1|p' "$f" | head -1)
    [ -n "$want" ] || want=42
    if build_run "$f" -O1; then rc=0; else rc=$?; fi
    if [ "$rc" = "$want" ]; then ok=$((ok + 1))
    else bad=$((bad + 1)); echo "  FAIL $f: exit $rc, wanted $want"; fi
done
[ "$bad" = 0 ] ||
    { echo "FAIL: $bad of $((ok + bad)) programs behaved differently on our libc"
      exit 1; }
echo "$ok execution programs run on our libc with no newlib at all, each
producing the exit status it expects"
