#!/bin/sh
# The GNU C extensions this round added, each against the host compiler.
#
# They are grouped because they are one thing in practice: the shapes a
# kernel or libc macro takes. __builtin_types_compatible_p with
# __builtin_choose_expr is how C dispatched on a type before _Generic;
# __auto_type and __label__ are what a statement-expression macro needs
# to be written twice in one function; case ranges are how a character
# classifier is written.
#
# The reference is the host compiler on the same source, because the
# answers are defined by the language and not by anything in this tree.
set -u
echo "TEST-MARKER gnu-c"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/gnu-c
rm -rf "$out"; mkdir -p "$out"
fail=0

cat > "$out/p.c" <<'CEOF'
int printf(const char *, ...);

/* types_compatible_p + choose_expr: the pre-_Generic type switch. The
 * arm NOT taken is never type-checked, which is the whole point --
 * `?:` cannot stand in because both its arms must type. */
#define is_int(x)  __builtin_types_compatible_p(typeof(x), int)
#define widen(x)   __builtin_choose_expr(is_int(x), (long)(x), (x))

/* __label__: the same macro twice in one function. Without it the
 * second `done:` is a duplicate label. */
#define FIND(arr, n, want) ({ __label__ done; int r = -1;                 \
    for (int i = 0; i < (n); i++) if ((arr)[i] == (want)) { r = i; goto done; } \
    done: r; })

/* __auto_type: min() without naming the type. */
#define MIN(a,b) ({ __auto_type _x = (a); __auto_type _y = (b); \
                    _x < _y ? _x : _y; })

static int cls(int c)
{
    switch (c) {
    case '0' ... '9':                 return 1;
    case 'a' ... 'z': case 'A' ... 'Z': return 2;
    case ' ': case '\t':              return 3;
    default:                          return 0;
    }
}

int main(void)
{
    int a[] = { 4, 8, 15, 16 };
    struct { char d[24]; } blob;
    int i = 7; long l = 9;
    (void)blob;

    printf("%d %d\n", is_int(i), is_int(l));
    printf("%ld %ld\n", widen(i), widen(l));
    printf("%d %d %d\n", FIND(a, 4, 15), FIND(a, 4, 4), FIND(a, 4, 99));
    printf("%d %ld\n", (int)MIN(7, 4), (long)MIN(3L, 9L));
    printf("%d %d %d %d %d\n", cls('5'), cls('q'), cls('Q'), cls(' '), cls('!'));
    /* __builtin_LINE is the line of the call, not of the macro. */
    printf("%d\n", __builtin_LINE());
    /* object_size, for the shape EmbCC answers: the address of a
     * named object. A decayed array or an interior pointer gets the
     * DEFINED "unknown" instead -- see the note below. */
    printf("%d\n", (int)(long)__builtin_object_size(&blob, 0));
    return 0;
}
CEOF

HOSTCC=${CC:-cc}
"$HOSTCC" -O1 -w -o "$out/ref" "$out/p.c" 2>/dev/null || {
    echo "skipped: the host compiler could not build the reference"; exit 0; }
"$out/ref" > "$out/want.txt" || { echo "FAIL: the reference did not run"; exit 1; }

for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" "$opt" -c "$out/p.c" -o "$out/p.o" 2> "$out/e.txt" || {
        echo "FAIL $opt: does not compile"
        grep -oE 'error.*' "$out/e.txt" | head -2 | sed 's/^/     | /'
        fail=1; continue; }
    if t_link "$out/p" "$out/p.o" 2>/dev/null &&
       t_run "$out/p" > "$out/got.txt" 2>/dev/null; then
        if cmp -s "$out/want.txt" "$out/got.txt"; then
            echo "  $opt: agrees with the host"
        else
            echo "FAIL $opt: differs from the host"
            diff "$out/want.txt" "$out/got.txt" | head -6 | sed 's/^/     | /'
            fail=1
        fi
    else
        echo "  (skipped running $opt: no runner here; it compiled)"
    fi
done

# _Pragma: the operator form, whose reason for existing is that a MACRO
# can carry it. Both spellings must compile and neither may reach the
# parser as an undeclared call.
printf '#define D _Pragma("GCC diagnostic push")\nint f(void){ D return 1; }\n' \
    > "$out/pr.c"
"$EMBCC" -fsyntax-only "$out/pr.c" 2>/dev/null || {
    echo "FAIL: _Pragma inside a macro does not compile"; fail=1; }
printf '_Pragma("GCC diagnostic push")\nint x;\n' > "$out/pr2.c"
"$EMBCC" -fsyntax-only "$out/pr2.c" 2>/dev/null || {
    echo "FAIL: a bare _Pragma does not compile"; fail=1; }
[ "$fail" -eq 0 ] && echo "  _Pragma compiles bare and inside a macro"

# #pragma pack changes LAYOUT. It used to be dropped in silence, so a
# struct the programmer packed came out padded -- differently from every
# other compiler, with nothing said; then it was refused. It is honoured
# now (tests/exec/pragma-pack.c has the layouts): the size proves it.
printf '#pragma pack(1)\nstruct s { char a; int b; };\n_Static_assert(sizeof(struct s) == 1 + sizeof(int), "packed");\n' > "$out/pk.c"
if "$EMBCC" -fsyntax-only "$out/pk.c" 2> "$out/pk.txt"; then
    echo "  #pragma pack(1) packs the struct after it"
else
    echo "FAIL: #pragma pack(1) did not pack the struct:"
    sed 's/^/     | /' "$out/pk.txt"
    fail=1
fi

# A case range that would expand to a million labels is refused with the
# count rather than appearing to hang.
printf 'int f(int c){switch(c){case 0 ... 2000000: return 1;} return 0;}\n' \
    > "$out/cr.c"
if "$EMBCC" -fsyntax-only "$out/cr.c" 2> "$out/cr.txt"; then
    echo "FAIL: an unbounded case range was accepted"; fail=1
else
    grep -q '2000001' "$out/cr.txt" || {
        echo "FAIL: the refusal does not say how many values"; fail=1; }
    echo "  an oversized case range is refused with its count"
fi

# What object_size does NOT answer, stated rather than left to be
# discovered. GCC says 16 for a decayed `int[4]` and 12 for `&a[1]`;
# EmbCC says "unknown" for both -- (size_t)-1 for types 0/1 and 0 for
# 2/3, which is the interface's defined answer and the one that
# disables a _FORTIFY_SOURCE check. A SMALL answer would be worse than
# none: it would reject a write that fits.
cat > "$out/os.c" <<'CEOF'
int printf(const char *, ...);
int main(void){ int a[4];
    printf("%d %d %d\n", (int)(long)__builtin_object_size(a, 0),
                         (int)(long)__builtin_object_size(&a[1], 0),
                         (int)(long)__builtin_object_size(&a, 0));
    return 0; }
CEOF
if "$EMBCC" -O1 -c "$out/os.c" -o "$out/os.o" 2>/dev/null &&
   t_link "$out/os" "$out/os.o" 2>/dev/null; then
    got=$(t_run "$out/os" 2>/dev/null)
    if [ "$got" = "-1 -1 -1" ]; then
        echo "  object_size says 'unknown' for a decayed array, an interior"
        echo "  pointer and &array, rather than a number that is too small"
    else
        echo "FAIL: object_size answered '$got' for the cases it cannot know"
        fail=1
    fi
fi

[ "$fail" -eq 0 ] || exit 1
