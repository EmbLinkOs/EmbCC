#!/bin/sh
# -Wformat: a printf or scanf format string checked against the arguments
# beside it. The judge is gcc, on the same file with the same flags, and
# the invariant runs one way on purpose: every line EmbCC warns about,
# gcc must warn about too. A warning nobody else raises is a false
# positive, and a false positive in a warning that ships inside -Wall is
# worse than a missed one -- it trains the reader to ignore the category.
#
# The declarations live in the test rather than in a header, so both
# compilers are looking at exactly the same attributes.
set -eu
echo "TEST-MARKER format-check"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/format-check-$ARCH
rm -rf "$out"; mkdir -p "$out"

# ---- the calls that are wrong --------------------------------------------
cat > "$out/bad.c" << 'EOF'
typedef __builtin_va_list va_list;
int printf(const char *f, ...) __attribute__((format(printf, 1, 2)));
int sscanf(const char *s, const char *f, ...) __attribute__((format(scanf, 2, 3)));
int vprintf(const char *f, va_list a) __attribute__((format(printf, 1, 0)));

void f(void)
{
    long l = 1; int i = 2; double d = 3; char *s = "x"; long double ld = 4;
    printf("%d\n", l);            /* 9: reads 4, passed 8 */
    printf("%ld\n", i);           /* 10: reads 8, passed 4 */
    printf("%s\n", i);            /* 11: a pointer was wanted */
    printf("%d\n", s);            /* 12: an integer was wanted */
    printf("%d\n", d);            /* 13: an integer was wanted */
    printf("%f\n", i);            /* 14: a double was wanted */
    printf("%f\n", ld);           /* 15: long double needs L */
    printf("%Lf\n", d);           /* 16: L wants a long double */
    printf("%d %d\n", i);         /* 17: too few */
    printf("%d\n", i, i);         /* 18: too many */
    printf("%*d\n", d, i);        /* 19: a '*' takes an int */
    sscanf(s, "%d", &l);          /* 20: writes 4 into 8 bytes */
    sscanf(s, "%d", i);           /* 21: not a pointer */
    sscanf(s, "%s", i);           /* 22: not a pointer */
    sscanf(s, "%f", &d);          /* 23: %f writes a float, not a double */
    sscanf(s, "%d", &d);          /* 24: an integer field into a double */
}
EOF

# ---- the calls that are right --------------------------------------------
# Every one of these is something real code writes, and a checker that
# reports any of them is unusable.
cat > "$out/good.c" << 'EOF'
typedef __builtin_va_list va_list;
int printf(const char *f, ...) __attribute__((format(printf, 1, 2)));
int sscanf(const char *s, const char *f, ...) __attribute__((format(scanf, 2, 3)));
int vprintf(const char *f, va_list a) __attribute__((format(printf, 1, 0)));

void f(const char *runtime_format, va_list ap)
{
    long l = 1; int i = 2; double d = 3; char *s = "x"; long double ld = 4;
    short sh = 5; float fl = 6; char c = 7; unsigned u = 8; void *p = &i;
    unsigned long ul = 9; long long ll = 10; char buf[32];
    printf("no conversions at all\n");
    printf("%%\n");
    printf("%d %i %o %u %x %X\n", i, i, u, u, u, u);
    printf("%ld %lu %lld %llu %zu\n", l, ul, ll, (unsigned long long)ul, sizeof buf);
    printf("%f %e %g %a %F %E %G %A\n", d, d, d, d, d, d, d, d);
    printf("%Lf %Le %Lg\n", ld, ld, ld);
    printf("%s %p %c\n", s, p, c);
    printf("%hhd %hd\n", c, sh);        /* both promote to int */
    printf("%d\n", sh);                 /* promotes to int */
    printf("%f\n", fl);                 /* promotes to double */
    printf("%-8.3f|%+d|% d|%#x|%08d\n", d, i, i, u, i);
    printf("%*d %.*f %*.*f\n", i, i, i, d, i, i, d);
    printf("%s\n", buf);                /* an array decays */
    printf(runtime_format, i);          /* not a literal: nothing to say */
    vprintf("%d %s\n", ap);             /* a va_list: no tail to compare */
    sscanf(s, "%d %ld %f %lf %s %c", &i, &l, &fl, &d, buf, &c);
    sscanf(s, "%*d %d", &i);            /* a suppressed field takes none */
    sscanf(s, "%4s %[a-z] %[^,]", buf, buf, buf);
}
EOF

lines() {  # lines() <file> -> the line numbers warned about
    "$EMBCC" --target="$TARGET" -Wall -c "$out/$1" -o "$out/t.o" 2>&1 |
        sed -n "s|.*$1:\([0-9]*\):.*\[-Wformat\].*|\1|p" | sort -un
}

# 1. Silent unless asked: a warning nobody asked for is noise.
n=$("$EMBCC" --target="$TARGET" -c "$out/bad.c" -o "$out/t.o" 2>&1 |
        grep -c 'Wformat' || true)
[ "$n" = 0 ] || { echo "FAIL: -Wformat fired without -Wall"; exit 1; }

# 2. In -Wall, as gcc has it, and off again with -Wno-format.
lines bad.c > "$out/mine"
[ -s "$out/mine" ] || { echo "FAIL: -Wall did not enable -Wformat"; exit 1; }
n=$("$EMBCC" --target="$TARGET" -Wall -Wno-format -c "$out/bad.c" \
        -o "$out/t.o" 2>&1 | grep -c 'Wformat' || true)
[ "$n" = 0 ] || { echo "FAIL: -Wno-format did not turn it off"; exit 1; }
echo "silent by default, in -Wall, and off again"

# 3. Every wrong line is caught.
for want in 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
    grep -qx "$want" "$out/mine" ||
        { echo "FAIL: line $want of bad.c was not reported"; exit 1; }
done
echo "every one of the 16 wrong calls is reported"

# 4. And no right one is.
lines good.c > "$out/goodwarns"
[ -s "$out/goodwarns" ] &&
    { echo "FAIL: reported a correct call:"
      "$EMBCC" --target="$TARGET" -Wall -c "$out/good.c" -o "$out/t.o" 2>&1 |
          grep 'Wformat'
      exit 1; }
echo "and none of the 20 correct ones is"

# 5. The judge: gcc, on the same file, with the same attributes. Ours
#    must be a SUBSET of what gcc reports -- it reports more kinds (a
#    signedness mismatch, for one, which this deliberately does not).
GCC=$([ "$ARCH" = aarch64 ] && echo aarch64-elf-gcc || echo x86_64-elf-gcc)
if command -v "$GCC" > /dev/null 2>&1; then
    NL=$([ "$ARCH" = aarch64 ] && echo "$AARCH64_NEWLIB" || echo "$X86_NEWLIB")
    "$GCC" -isystem "$NL/include" -Wall -c "$out/bad.c" -o "$out/t.gcc.o" 2>&1 |
        sed -n 's|.*bad.c:\([0-9]*\):.*\[-Wformat.*|\1|p' | sort -un > "$out/gcc"
    for k in $(cat "$out/mine"); do
        grep -qx "$k" "$out/gcc" ||
            { echo "FAIL: EmbCC reports bad.c:$k where gcc does not"; exit 1; }
    done
    "$GCC" -isystem "$NL/include" -Wall -c "$out/good.c" -o "$out/t.gcc.o" 2>&1 |
        sed -n 's|.*good.c:\([0-9]*\):.*\[-Wformat.*|\1|p' | sort -un > "$out/gccgood"
    echo "gcc agrees on all $(wc -l < "$out/mine" | tr -d ' ') lines; \
it raises $(wc -l < "$out/gccgood" | tr -d ' ') on the correct file that we do not"
else
    echo "skipped the gcc comparison: no $GCC"
fi

# 6. C++ too. The attribute survives lowering to C, and the indices move
#    with it: a non-static member's arguments count from two in GCC's
#    definition -- because of `this` -- so lowering does not shift them
#    again, while a return slot this compiler prepends does.
cat > "$out/cpp.cc" << 'EOF'
extern "C" int printf(const char *f, ...) __attribute__((format(printf, 1, 2)));
struct Big { double a, b, c, d; };
struct Log {
    void say(const char *f, ...) __attribute__((format(printf, 2, 3)));
    Big big(const char *f, ...) __attribute__((format(printf, 2, 3)));
};
int main()
{
    long l = 1;
    Log g;
    printf("%d\n", l);        /* 11: a free function */
    g.say("%d", l);           /* 12: a member -- `this` is already counted */
    g.big("%d", l);           /* 13: and one returning through a hidden slot */
    printf("%ld\n", l);       /* silent */
    g.say("%ld", l);          /* silent */
    g.big("%ld", l);          /* silent */
    return 0;
}
EOF
"$EMBCC" --target="$TARGET" -Wall -x c++ -c "$out/cpp.cc" -o "$out/t.o" 2>&1 |
    sed -n 's|.*cpp.cc:\([0-9]*\):.*\[-Wformat\].*|\1|p' | sort -un > "$out/cppwarns"
for want in 11 12 13; do
    grep -qx "$want" "$out/cppwarns" ||
        { echo "FAIL: cpp.cc line $want was not reported"; exit 1; }
done
[ "$(wc -l < "$out/cppwarns" | tr -d ' ')" = 3 ] ||
    { echo "FAIL: C++ reported more than the three wrong calls:"
      cat "$out/cppwarns"; exit 1; }
echo "C++ calls are checked too, through the lowering, with the indices moved"

echo "a format that disagrees with its arguments is caught where the call
is written, and which functions to check comes from the declaration
rather than from a list of names inside the compiler"
