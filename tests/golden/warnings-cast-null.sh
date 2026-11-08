#!/bin/sh
# -Wcast-align and -Wnull-dereference, which an RTOS's build passes and
# EmbCC used to answer with "not a warning EmbCC has" (#184).
#
#  1. --warn=cast-align: a pointer cast that raises the alignment, on a
#     target where a misaligned word access traps -- the lines GCC for
#     Xtensa reports (char * and unsigned char * to int, struct, short),
#     not from void *; the same on ARMv6-M; nothing on a Cortex-M4 or
#     x86-64, which take the access; everywhere with -Wcast-align=strict.
#  2. --warn=null-dereference: a pointer null on every path, or on some,
#     dereferenced -- and the patterns that are safe stay quiet: a test
#     first, a loop over a list, `p && p->v`, a guard correlated with how
#     the pointer was set, offsetof's idiom, a volatile read of address
#     0, a call that never returns (`cerr` below ends in abort()).
set -u
echo "TEST-MARKER warnings-cast-null"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/warnings-cast-null
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/ca.c" << 'EOF2'
struct s { int a; };
int f1(char *c) { return *(int *)c; }
int f2(void *v) { return *(int *)v; }
int f3(unsigned char *c) { return ((struct s *)c)->a; }
short f4(char *c) { return *(short *)c; }
char f5(int *p) { return *(char *)p; }
int f6(char *c) { return *(int *)(void *)c; }
EOF2
lines() { sed -n "s|.*$1:\([0-9]*\):.*\[--warn=$2\].*|\1|p" | sort -un | tr '\n' ' '; }
for tw in "xtensa-esp32-elf:2 4 5 " "thumbv6m-none-eabi:2 4 5 " "thumbv7em-none-eabi:" "x86_64-elf:"; do
    t=${tw%%:*} w=${tw#*:}
    got=$("$EMBCC" --target=$t -Wcast-align -c "$out/ca.c" -o "$out/ca.o" 2>&1 | lines ca.c cast-align)
    [ "$got" = "$w" ] || fail "$t -Wcast-align: lines '$got', not '$w'"
done
got=$("$EMBCC" --target=x86_64-elf -Wcast-align=strict -c "$out/ca.c" -o "$out/ca.o" 2>&1 | lines ca.c cast-align-strict)
[ "$got" = "2 4 5 " ] || fail "x86-64 -Wcast-align=strict: lines '$got'"
"$EMBCC" --target=thumbv6m-none-eabi -Wcast-align -c "$out/ca.c" -o "$out/ca.o" 2>&1 |
    grep -q "from 1 to 4 bytes (char \* to int \*)" || fail "the message names the types"
echo "cast-align: GCC's lines where a misaligned access traps, none elsewhere, all with =strict"

cat > "$out/nd.c" << 'EOF2'
void abort(void);
struct n { int v; struct n *next; };
static void cerr(const char *m) { (void)m; abort(); }
int n1(void) { int *p = 0; return *p; }
int n2(int k, int *q) { int *p = k ? q : 0; return *p; }
int n3(int *p) { if (p) return 1; return *p; }
int ok1(int *p) { if (!p) return 0; return *p; }
int ok2(struct n *h) { int s = 0; for (struct n *p = h; p; p = p->next) s += p->v; return s; }
int ok3(struct n *p) { return p && p->v; }
int ok4(struct n *p, int c) { struct n *q = 0; if (c) q = p; while (q) { c += q->v; q = q->next; } return c; }
unsigned ok5(void) { return *(volatile unsigned *)0; }
long ok6(void) { return (long)&((struct n *)0)->next; }
int n5(struct n *p) { struct n *q = 0; if (p->v) q = p; return q->v; }
int ok7(int *p) { if (p == 0) return 0; return p[2]; }
int n6(int *p) { if (0 != p) return 1; return p[1]; }
int ok8(struct n *a, int k) { struct n *x = k < 3 ? a : 0; int use = x && k; if (use) return x->v; return 0; }
int ok9(struct n *a) { if (!a) cerr("none"); return a->v; }
const char *ok10(const char *v, const char *d) { const char *p = v ? v : d; return p; }
int ok11(const char *v, const char *d) { const char *p = v ? v : d; return *p; }
EOF2
for t in thumbv7em-none-eabi xtensa-esp32-elf avr x86_64-elf; do
    got=$("$EMBCC" --target=$t -Wnull-dereference -c "$out/nd.c" -o "$out/nd.o" 2>&1 | lines nd.c null-dereference)
    [ "$got" = "4 5 6 13 15 " ] || fail "$t -Wnull-dereference: lines '$got', not '4 5 6 13 15 '"
done
"$EMBCC" --target=thumbv7em-none-eabi -Wnull-dereference -c "$out/nd.c" -o "$out/nd.o" 2> "$out/nd.err"
grep -q "nd.c:4:.*null pointer dereference: 'p' is null here" "$out/nd.err" || fail "the definite form"
grep -q "nd.c:5:.*potential null pointer dereference: 'p' is null on some path" "$out/nd.err" || fail "the potential form"
"$EMBCC" --target=thumbv7em-none-eabi -Wall -Wextra -c "$out/nd.c" -o "$out/nd.o" 2>&1 |
    grep -q 'null-dereference' && fail "-Wall -Wextra turned it on (GCC's sets do not)"
echo "null-dereference: every null read found, the safe patterns quiet, on four targets"
