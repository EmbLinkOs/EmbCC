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
#include <string.h>
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
    /* printf's floating conversion is EXACT, which is a stronger claim
     * than "close" and is checked as one. Each of these is a tie or a
     * near-tie -- the cases where an implementation that scales in
     * floating point and rounds half away from zero gives a different
     * answer than C requires.
     *
     * 2.5 and 0.35 are the two shapes of the bug. 2.5 is an exact tie
     * and rounds to EVEN, so "2". 0.35 is not a tie at all: the nearest
     * double is slightly below 0.35, so it rounds DOWN to 0.3 -- but
     * multiplying its fraction by ten rounds up to exactly 3.5 and
     * invents a tie, which then rounds up to 0.4. */
    char fb[400];
    snprintf(fb, sizeof fb, "%.0f %.0f %.0f %.0f", 0.5, 1.5, 2.5, 3.5);
    printf("ties %s\n", fb);
    snprintf(fb, sizeof fb, "%.1f %.1f %.2f", 0.25, 0.35, 2.675);
    printf("neartie %s\n", fb);
    /* A carry out of the leading digit, in both styles: %f grows by a
     * character, %e keeps one digit and moves the exponent. */
    snprintf(fb, sizeof fb, "%.1f %.1e %.0e", 9.99, 9.99, 9.5);
    printf("carry %s\n", fb);
    /* %g's style is chosen from the exponent AFTER rounding, so 9.9999
     * at four significant digits is 10 and prints as %f, not 1e+01. */
    snprintf(fb, sizeof fb, "%.4g %g %g", 9.9999, 100.0, 0.0001);
    printf("gstyle %s\n", fb);
    /* The extremes: a subnormal, and the largest double, exactly. */
    snprintf(fb, sizeof fb, "%.2e %.0f", 5e-324, 1e15 + 0.5);
    printf("extreme %s\n", fb);
    /* The largest double printed in full: 309 digits, all of them exact
     * -- it is an integer, and every one of its digits is determined. */
    snprintf(fb, sizeof fb, "%.0f", 1.7976931348623157e308);
    printf("dblmax %d %c%c%c\n", (int)strlen(fb), fb[0], fb[1], fb[2]);

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
# Ties round to EVEN, and a value that is not a tie is not turned into one.
want_line "ties 0 2 2 4"
want_line "neartie 0.2 0.3 2.67"
want_line "carry 10.0 1.0e+01 1e+01"
want_line "gstyle 10 100 0.0001"
want_line "extreme 4.94e-324 1000000000000000"
want_line "dblmax 309 179"
echo "setjmp/longjmp across frames, the calendar before and after the epoch
and on a leap day, strftime, scanf's matching and input failures, and
printf's floating conversion exact at every tie, carry and extreme"

# ---- the wide half, and the three headers that reach the hardware ---------
#
# UTF-8 is where the bugs are, and they are always the same three: an
# overlong form that smuggles a NUL past a NUL check, a surrogate that
# gives one string two encodings, and a sequence split across two calls
# that a stateless decoder mangles. Each is checked here because each is
# a real attack rather than a corner case.
cat > "$out/wide.c" << 'EOF'
#include <wchar.h>
#include <wctype.h>
#include <uchar.h>
#include <locale.h>
#include <fenv.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static int fails;
#define CHK(e) do { if (!(e)) { printf("FAIL %d: %s\n", __LINE__, #e); fails++; } } while (0)

static volatile sig_atomic_t caught;
static void handler(int s) { (void)s; caught = 1; }

int main(void)
{
    /* ---- wide strings ------------------------------------------------ */
    CHK(wcslen(L"hello") == 5);
    CHK(wcslen(L"") == 0);
    wchar_t buf[16];
    wcscpy(buf, L"abc");
    CHK(wcscmp(buf, L"abc") == 0);
    wcscat(buf, L"def");
    CHK(wcscmp(buf, L"abcdef") == 0);
    CHK(wcsncmp(L"abcx", L"abcy", 3) == 0);
    CHK(wcscmp(L"a", L"b") < 0 && wcscmp(L"b", L"a") > 0);
    CHK(wcschr(L"abc", L'b') != NULL);
    CHK(wcschr(L"abc", L'z') == NULL);
    CHK(*wcsrchr(L"abcabc", L'b') == L'b');
    CHK(wcsstr(L"hello world", L"world") != NULL);
    CHK(wcsstr(L"hello", L"xyz") == NULL);
    CHK(wcsspn(L"aabbc", L"ab") == 4);
    CHK(wcscspn(L"aabbc", L"c") == 4);
    wmemset(buf, L'x', 3);
    CHK(buf[0] == L'x' && buf[2] == L'x');
    CHK(wmemchr(L"abc", L'c', 3) != NULL);

    /* wcstok is the reentrant one -- the only form C11 gives for wide
     * strings, because strtok's hidden state was a mistake. */
    wchar_t tok[] = L"a,b,,c";
    wchar_t *save = NULL;
    CHK(wcscmp(wcstok(tok, L",", &save), L"a") == 0);
    CHK(wcscmp(wcstok(NULL, L",", &save), L"b") == 0);
    CHK(wcscmp(wcstok(NULL, L",", &save), L"c") == 0);
    CHK(wcstok(NULL, L",", &save) == NULL);

    /* ---- UTF-8, and what must be refused ------------------------------ */
    wchar_t wc;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    CHK(mbsinit(&st));

    CHK(mbrtowc(&wc, "A", 1, &st) == 1 && wc == 0x41);
    CHK(mbrtowc(&wc, "\xc3\xa9", 2, &st) == 2 && wc == 0xE9);
    CHK(mbrtowc(&wc, "\xe2\x82\xac", 3, &st) == 3 && wc == 0x20AC);
    CHK(mbrtowc(&wc, "\xf0\x9f\x98\x80", 4, &st) == 4 && wc == 0x1F600);
    /* A NUL converts and returns 0, not 1: the standard says so, and a
     * loop that adds the return value must handle it. */
    CHK(mbrtowc(&wc, "\0", 1, &st) == 0 && wc == 0);

    /* OVERLONG: 0xC0 0x80 decodes arithmetically to U+0000, and a
     * decoder that accepts it lets a NUL through a string that was
     * checked for NULs. */
    memset(&st, 0, sizeof st);
    CHK(mbrtowc(&wc, "\xc0\x80", 2, &st) == (size_t)-1);
    memset(&st, 0, sizeof st);
    CHK(mbrtowc(&wc, "\xe0\x80\x80", 3, &st) == (size_t)-1);
    /* A SURROGATE: not a character, and accepting it gives one string
     * two encodings. */
    memset(&st, 0, sizeof st);
    CHK(mbrtowc(&wc, "\xed\xa0\x80", 3, &st) == (size_t)-1);
    /* Above U+10FFFF, and a lone continuation byte. */
    memset(&st, 0, sizeof st);
    CHK(mbrtowc(&wc, "\xf5\x80\x80\x80", 4, &st) == (size_t)-1);
    memset(&st, 0, sizeof st);
    CHK(mbrtowc(&wc, "\x80", 1, &st) == (size_t)-1);

    /* SPLIT across calls: -2 means incomplete, and the state remembers
     * where we were. A stateless decoder mangles exactly this. */
    memset(&st, 0, sizeof st);
    CHK(mbrtowc(&wc, "\xe2", 1, &st) == (size_t)-2);
    CHK(!mbsinit(&st));
    CHK(mbrtowc(&wc, "\x82", 1, &st) == (size_t)-2);
    CHK(mbrtowc(&wc, "\xac", 1, &st) == (size_t)-2 || wc == 0x20AC);
    CHK(wc == 0x20AC);
    CHK(mbsinit(&st));

    /* ---- the other direction ------------------------------------------ */
    char out[8];
    memset(&st, 0, sizeof st);
    CHK(wcrtomb(out, 0x41, &st) == 1 && out[0] == 'A');
    CHK(wcrtomb(out, 0xE9, &st) == 2);
    CHK((unsigned char)out[0] == 0xc3 && (unsigned char)out[1] == 0xa9);
    CHK(wcrtomb(out, 0x20AC, &st) == 3);
    CHK(wcrtomb(out, 0x1F600, &st) == 4);
    /* A surrogate cannot be encoded either. */
    CHK(wcrtomb(out, 0xD800, &st) == (size_t)-1);

    /* Round trip through both. */
    const char *utf8 = "h\xc3\xa9llo \xe2\x82\xac";
    wchar_t wide[32];
    const char *p = utf8;
    memset(&st, 0, sizeof st);
    size_t n = mbsrtowcs(wide, &p, 32, &st);
    CHK(n == 7 && wide[1] == 0xE9 && wide[6] == 0x20AC);
    char back[32];
    const wchar_t *q = wide;
    memset(&st, 0, sizeof st);
    size_t m = wcsrtombs(back, &q, 32, &st);
    CHK(m == strlen(utf8) && strcmp(back, utf8) == 0);

    /* ---- UTF-16, and the surrogate pair it exists for ------------------ */
    char16_t u16;
    memset(&st, 0, sizeof st);
    CHK(mbrtoc16(&u16, "A", 1, &st) == 1 && u16 == 0x41);
    memset(&st, 0, sizeof st);
    /* A code point above the basic plane takes TWO units: the first
     * call consumes the bytes and returns the high surrogate, the
     * second consumes NOTHING and returns the low one -- which is what
     * -3 means and is the only way a one-unit-at-a-time interface can
     * report it. */
    CHK(mbrtoc16(&u16, "\xf0\x9f\x98\x80", 4, &st) == 4);
    CHK(u16 >= 0xD800 && u16 <= 0xDBFF);
    CHK(mbrtoc16(&u16, "", 0, &st) == (size_t)-3);
    CHK(u16 >= 0xDC00 && u16 <= 0xDFFF);

    memset(&st, 0, sizeof st);
    /* And back: a high surrogate writes nothing yet and returns 0. */
    CHK(c16rtomb(out, 0xD83D, &st) == 0);
    CHK(c16rtomb(out, 0xDE00, &st) == 4);
    /* A lone low surrogate is an error, not a character. */
    memset(&st, 0, sizeof st);
    CHK(c16rtomb(out, 0xDE00, &st) == (size_t)-1);

    char32_t u32;
    memset(&st, 0, sizeof st);
    CHK(mbrtoc32(&u32, "\xf0\x9f\x98\x80", 4, &st) == 4 && u32 == 0x1F600);
    CHK(c32rtomb(out, 0x1F600, &st) == 4);

    /* ---- classification, in the "C" locale ----------------------------- */
    CHK(iswalpha(L'a') && iswalpha(L'Z'));
    CHK(!iswalpha(L'1') && !iswalpha(L' '));
    CHK(iswdigit(L'7') && !iswdigit(L'x'));
    CHK(iswspace(L' ') && iswspace(L'\t'));
    CHK(iswupper(L'A') && iswlower(L'a'));
    CHK(towupper(L'a') == L'A' && towlower(L'Z') == L'z');
    CHK(towupper(L'1') == L'1');
    /* The "C" locale classifies exactly the basic set and says no above
     * it -- which is what the locale MEANS, not a shortcut. */
    CHK(!iswalpha(0xE9));
    CHK(towupper(0xE9) == 0xE9);
    CHK(iswctype(L'a', wctype("alpha")));
    CHK(!iswctype(L'a', wctype("digit")));
    CHK(wctype("nosuch") == 0);
    CHK(towctrans(L'a', wctrans("toupper")) == L'A');

    /* ---- wide numeric conversion --------------------------------------- */
    wchar_t *end;
    CHK(wcstol(L"  -42rest", &end, 10) == -42 && *end == L'r');
    CHK(wcstoul(L"ff", &end, 16) == 255);
    CHK(wcstod(L"1.5", &end) == 1.5 && *end == 0);
    CHK(wcstod(L"abc", &end) == 0.0 && end[0] == L'a');

    /* ---- locale: one, and it says so ----------------------------------- */
    CHK(setlocale(LC_ALL, "C") != NULL);
    CHK(setlocale(LC_ALL, "") != NULL);
    CHK(setlocale(LC_ALL, "fr_FR.UTF-8") == NULL);
    CHK(strcmp(localeconv()->decimal_point, ".") == 0);
    CHK(localeconv()->thousands_sep[0] == 0);

    /* ---- fenv: the rounding mode really reaches the hardware ----------- */
    CHK(fegetround() == FE_TONEAREST);
    feclearexcept(FE_ALL_EXCEPT);
    CHK(fetestexcept(FE_ALL_EXCEPT) == 0);
    feraiseexcept(FE_INEXACT);
    CHK(fetestexcept(FE_INEXACT) == FE_INEXACT);
    CHK(fetestexcept(FE_OVERFLOW) == 0);
    feclearexcept(FE_INEXACT);
    CHK(fetestexcept(FE_INEXACT) == 0);

    /* The mode is set and read BACK from the register, so this fails if
     * the store never reached it. */
    CHK(fesetround(FE_TOWARDZERO) == 0 && fegetround() == FE_TOWARDZERO);
    CHK(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD);
    CHK(fesetround(FE_DOWNWARD) == 0 && fegetround() == FE_DOWNWARD);
    CHK(fesetround(FE_TONEAREST) == 0 && fegetround() == FE_TONEAREST);
    CHK(fesetround(99) != 0);

    /* And it CHANGES arithmetic, which is the only check that proves
     * the bits are the right ones. The values are volatile so the
     * division happens at run time under the mode just set. */
    {
        volatile double a = 1.0, b = 3.0;
        fesetround(FE_DOWNWARD);
        volatile double lo = a / b;
        fesetround(FE_UPWARD);
        volatile double hi = a / b;
        fesetround(FE_TONEAREST);
        CHK(lo < hi);
    }

    fenv_t saved;
    CHK(feholdexcept(&saved) == 0);
    feraiseexcept(FE_DIVBYZERO);
    CHK(feupdateenv(&saved) == 0);
    CHK(fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO);
    feclearexcept(FE_ALL_EXCEPT);

    /* ---- signal: raise reaches the handler, once ----------------------- */
    CHK(signal(SIGINT, handler) != SIG_ERR);
    CHK(raise(SIGINT) == 0);
    CHK(caught == 1);
    /* The handler is cleared before it is called, so a second raise
     * would take the default -- which is why this reinstalls it rather
     * than raising again. */
    CHK(signal(SIGINT, SIG_IGN) != SIG_ERR);
    CHK(raise(SIGINT) == 0);
    CHK(signal(SIGINT, SIG_IGN) == SIG_IGN);
    CHK(signal(999, handler) == SIG_ERR);

    if (!fails)
        printf("wide, locale, fenv and signal: ok\n");
    return fails ? 1 : 42;
}
EOF
if build_run "$out/wide.c" -O1; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the wide/fenv program exited $rc"; exit 1; }
want_line "wide, locale, fenv and signal: ok"
echo "UTF-8 refusing overlong forms, surrogates and split sequences; the
rounding mode reaching the hardware and changing arithmetic"

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
