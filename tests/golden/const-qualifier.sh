#!/bin/sh
# const in the type: what it allows, and the one warning it brings.
#
# A pointer conversion that drops a pointee's const draws
# -Wdiscarded-qualifiers, on by default as in GCC, and code that keeps
# its const compiles with no diagnostic at all under -Wall -Werror --
# the refusals themselves are in tests/compile/reject-unimplemented.sh.
set -u
echo "TEST-MARKER const-qualifier"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/const-qualifier
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/drop.c" <<'SRC'
void take(char *p);
char *f(const char *s) { char *t = s; take(s); return s; }
SRC
"$EMBCC" -c "$out/drop.c" -o "$out/drop.o" 2> "$out/drop.err" || {
    echo "FAIL: dropping const is a warning, not an error:"; cat "$out/drop.err"; exit 1; }
for ctx in initialization argument return; do
    grep -q "$ctx discards the 'const' qualifier of const char \* \[-Wdiscarded-qualifiers\]" \
        "$out/drop.err" || {
        echo "FAIL: no warning for the $ctx:"; cat "$out/drop.err"; exit 1; }
done
"$EMBCC" -Wno-discarded-qualifiers -c "$out/drop.c" -o "$out/drop.o" 2> "$out/quiet.err"
[ ! -s "$out/quiet.err" ] || {
    echo "FAIL: -Wno-discarded-qualifiers did not silence it:"; cat "$out/quiet.err"; exit 1; }
if "$EMBCC" -Werror -c "$out/drop.c" -o "$out/drop.o" 2> /dev/null; then
    echo "FAIL: -Werror did not make it an error"; exit 1
fi
echo "const-qualifier: dropping a pointee's const warns, as GCC does"

cat > "$out/keep.c" <<'SRC'
#include <string.h>
/* A const (and a volatile) pointer to a struct declared before its body:
 * the qualified type is a copy, and it must see the body arrive. */
struct later;
int count_later(const struct later *l);
volatile struct later *vl;
struct later { int n; struct later *next; };
int count_later(const struct later *l) { return l->n + (l->next != 0); }
int vn(void) { return vl->n; }
struct S { const int a; int b; };
struct T { int a; };
static const int tbl[] = { 1, 2, 3 };
void use(const int *p);
int keep(char *buf, const char *src)
{
    const int x = 5;
    int y = x + 1;
    const char *s = src;
    s++;
    char *const p = buf;
    *p = 'a';
    struct S st = { 1, 2 };
    st.b = 3;
    const struct T t = { 4 };
    int v = t.a + tbl[1];
    use(&x);
    __typeof__(x + 1) z = 3;
    z = 4;
    __auto_type a = x;
    a = 6;
    for (const char *r = s; *r; r++)
        y++;
    memcpy(buf, src, 2);
    return y + v + z + a + st.a + (int)strlen(s) + (int)*(int *)&x;
}
SRC
"$EMBCC" -Wall -Werror -c "$out/keep.c" -o "$out/keep.o" 2> "$out/keep.err" || {
    echo "FAIL: code that keeps its const was diagnosed:"; cat "$out/keep.err"; exit 1; }
echo "const-qualifier: const objects, pointers to const and const members compile cleanly"
