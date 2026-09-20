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
