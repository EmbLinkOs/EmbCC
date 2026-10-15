#!/bin/sh
# The GNU forms system headers are written in -- glibc's and Linux's, which
# --libc=system compiles (tests/golden/libc-system.sh) -- each on its own,
# so that one breaking is named without a glibc to hand:
#
#   a function-like macro whose '(' is on the next line (glibc <signal.h>:
#       __attribute_deprecated_msg__
#         ("Use the signal and sigprocmask functions instead"))
#   __signed__ and __const (Linux's asm-generic/int-ll64.h)
#   __attribute__((mode(QI|HI|SI|DI|word))) (glibc <sys/types.h>)
#   __attribute__((transparent_union)) and a call through it
#       (glibc <sys/socket.h> under _GNU_SOURCE)
#   `typedef float _Float32;` (glibc's <bits/floatn-common.h>, for a
#       compiler before GCC 7): a typedef naming a keyword as itself
#
# Run on x86-64 and compared with GCC's build of the same program.
set -u
echo "TEST-MARKER gnu-header-forms"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
out=$PWD/tests/golden/out/gnu-header-forms
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/forms.c" <<'EOF'
#include <stdio.h>

#define TWICE(x) ((x) * 2)
#define deprecated_msg(m) __attribute__((__deprecated__(m)))

typedef int i8 __attribute__((__mode__(__QI__)));
typedef unsigned u16m __attribute__((mode(HI)));
typedef int i32m __attribute__((__mode__(__SI__)));
typedef unsigned long long u64m __attribute__((mode(DI)));
typedef int wordt __attribute__((__mode__(__word__)));
typedef __signed__ char s8;
typedef __signed__ short s16;
typedef __const int cint;

struct sa { int family; };
struct sa_in { int family; int port; };
typedef union {
    struct sa *__restrict sa;
    struct sa_in *__restrict sin;
} sa_arg __attribute__((__transparent_union__));

static int family(sa_arg a) { return a.sa->family; }

extern int old_api(int)
    deprecated_msg
      ("use new_api");
int old_api(int x) { return x; }

int main(void)
{
    struct sa_in in = { 2, 80 };
    struct sa plain = { 10 };
    int v = TWICE
        (3);
    cint c = 1;
    int r = family(&in) + family(&plain) + family((struct sa *)&in);
    printf("%d %d %d %d %d %d %d %d %d\n", (int)sizeof(i8), (int)sizeof(u16m),
           (int)sizeof(i32m), (int)sizeof(u64m), (int)sizeof(wordt),
           (s8)-1 < 0, (u16m)-1 == 65535, v, r);
    return v + r + c + 21;     /* 6 + 14 + 1 + 21 */
}
EOF

if [ "$TARGET" = x86_64-elf ]; then
    "$EMBCC" --target=x86_64-elf -c "$out/forms.c" -o "$out/e.o" 2> "$out/e.err" ||
        { cat "$out/e.err"; fail "EmbCC does not compile the forms"; }
    t_link "$out/e" "$out/e.o" || fail "the forms do not link"
    got=$(t_run "$out/e"); st=$?
    [ "$st" = 42 ] || fail "EmbCC's build exits $st: $got"
    if t_gcc_c "$out/forms.c" -o "$out/g.o" 2> /dev/null &&
       t_link "$out/g" "$out/g.o"; then
        want=$(t_run "$out/g")
        [ "$got" = "$want" ] || fail "EmbCC printed '$got', GCC '$want'"
        echo "gnu-header-forms: each form compiles and runs as GCC's build does"
    else
        echo "gnu-header-forms: each form compiles and runs (no GCC to compare)"
    fi
fi

# a transparent_union parameter is received only where it arrives as its
# first member does; elsewhere a definition of one is refused
"$EMBCC" --target=sparc-elf -c "$out/forms.c" -o "$out/s.o" 2> "$out/s.err" &&
    fail "a transparent_union parameter was defined for SPARC"
grep -q "transparent_union parameter, which EmbCC receives only on x86-64 and AArch64" "$out/s.err" ||
    { cat "$out/s.err"; fail "the SPARC refusal does not say why"; }

# glibc's interchange-type typedefs for a compiler before GCC 7: the
# keyword named as itself passes; named as another type it is refused
printf 'typedef float _Float32;\ntypedef double _Float64;\n_Float32 f = 1;\n' > "$out/fn.c"
"$EMBCC" --target=x86_64-elf -fsyntax-only "$out/fn.c" ||
    fail "glibc's typedef float _Float32 was refused"
printf 'typedef double _Float32;\n' > "$out/fn2.c"
"$EMBCC" --target=x86_64-elf -fsyntax-only "$out/fn2.c" 2> "$out/fn2.err" &&
    fail "typedef double _Float32 was accepted"
grep -q "_Float32 is a keyword, and this is not its type" "$out/fn2.err" ||
    { cat "$out/fn2.err"; fail "the _Float32 refusal does not say why"; }
echo "gnu-header-forms: a transparent_union definition refused off x86-64/AArch64, _FloatN typedefs as glibc writes them"
