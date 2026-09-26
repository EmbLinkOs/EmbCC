#!/bin/sh
# The C23 features, each checked for what it is rather than that it parses.
#
# They are worth having together because new code uses them together:
# `nullptr` instead of NULL, `[[nodiscard]]` instead of
# __attribute__((warn_unused_result)), `auto` instead of writing the
# type twice, `enum e : uint8_t` instead of -fshort-enums, and #embed
# instead of a build step that turns a binary into a .c file.
set -u
echo "TEST-MARKER c23"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/c23
rm -rf "$out"; mkdir -p "$out"
fail=0

printf 'HI!' > "$out/blob.bin"
cat > "$out/p.c" <<'CEOF'
int printf(const char *, ...);

/* enum with a fixed underlying type: the standard's answer to the
 * -fshort-enums question. Instead of a flag that silently resizes
 * every enum in the program, the declaration says. */
enum small : unsigned char { A = 1, B = 200 };
enum wide  : long          { C = 1 };
_Static_assert(sizeof(enum small) == 1, "one byte");
_Static_assert(sizeof(enum wide) == sizeof(long), "long");
/* A plain enum is still int, which is what clang does too. */
enum plain { P };
_Static_assert(sizeof(enum plain) == 4, "plain enums unchanged");

/* #embed: the file's bytes, where an initializer list goes. */
static const unsigned char blob[] = {
#embed "blob.bin"
};
_Static_assert(sizeof blob == 3, "three bytes embedded");

[[nodiscard]] static int must_use(void) { return 7; }
[[deprecated]] static int old_one(void) { return 8; }

static int take([[maybe_unused]] int unused, int used) { return used; }

int main(void)
{
    constexpr int k = 5;
    int arr[k];                    /* constexpr is an integer constant */
    auto a = 5;                    /* C23 type inference */
    auto b = 3L;
    auto int c = 7;                /* and the C89 storage class still */
    int *p = nullptr;
    typeof_unqual(k) unq = k;      /* the type without its qualifiers */

    arr[0] = k * 2;
    printf("%d %ld %d %d\n", a, b, c, unq);
    printf("%d %d\n", k, arr[0]);
    printf("%d\n", p == nullptr);
    printf("%d %d\n", (int)A, (int)B);
    printf("%d %c%c%c\n", (int)sizeof blob, blob[0], blob[1], blob[2]);
    printf("%d %d\n", must_use(), take(1, 9));
    (void)old_one;
    return 0;
}
CEOF

HOSTCC=${CC:-cc}
if "$HOSTCC" -std=c2x -w -o "$out/ref" "$out/p.c" 2>/dev/null; then
    "$out/ref" > "$out/want.txt" 2>/dev/null || { echo "FAIL: reference did not run"; exit 1; }
    have_ref=1
else
    echo "  (the host compiler has no C23 mode; checking EmbCC alone)"
    have_ref=0
fi

for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" "$opt" -c "$out/p.c" -o "$out/p.o" 2> "$out/e.txt" || {
        echo "FAIL $opt: does not compile"
        grep -oE 'error.*' "$out/e.txt" | head -3 | sed 's/^/     | /'
        fail=1; continue; }
    t_link "$out/p" "$out/p.o" 2>/dev/null || continue
    t_run "$out/p" > "$out/got.txt" 2>/dev/null || continue
    if [ "$have_ref" = 1 ]; then
        if cmp -s "$out/want.txt" "$out/got.txt"; then
            echo "  $opt: agrees with the host"
        else
            echo "FAIL $opt: differs from the host"
            diff "$out/want.txt" "$out/got.txt" | head -6 | sed 's/^/     | /'
            fail=1
        fi
    else
        want='5 3 7 5
5 10
1
1 200
3 HI!
7 9'
        if [ "$(cat "$out/got.txt")" = "$want" ]; then
            echo "  $opt: every value is the one C23 defines"
        else
            echo "FAIL $opt:"; cat "$out/got.txt" | sed 's/^/     | /'; fail=1
        fi
    fi
done

# The refusals. Each of these would otherwise be silently wrong.
no() {
    printf '%s\n' "$2" > "$out/n.c"
    if "$EMBCC" -fsyntax-only "$out/n.c" > "$out/n.txt" 2>&1; then
        echo "FAIL: $1 was accepted"; fail=1
    elif grep -q "$3" "$out/n.txt"; then
        printf '  %s -- refused by name\n' "$1"
    else
        echo "FAIL: $1 refused for the wrong reason"
        head -1 "$out/n.txt" | sed 's/^/     | /'; fail=1
    fi
}
no 'a non-constant constexpr' \
   'int g(void); int f(void){ constexpr int k = g(); return k; }' \
   'constant'
no 'an enum with a non-integer underlying type' \
   'enum e : float { A };' 'integer'
no '#embed with parameters' \
   'static const unsigned char b[] = {
#embed "blob.bin" limit(2)
};' 'not supported'

[ "$fail" -eq 0 ] || exit 1
