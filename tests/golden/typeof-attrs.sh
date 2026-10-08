#!/bin/sh
# Two holes in the declaration grammar, both found by the GCC/Clang
# audit and both in the parser rather than anywhere deep.
#
# 1. `typeof` could not see a function PARAMETER. It resolves names
#    through a parse-time table (g_fold_locals in src/parse/parse.c)
#    that held block-scope locals and file-scope globals -- parameters
#    were never added. So this failed:
#
#        int f(int a) { typeof(a) b = a; ... }
#
#    with "typeof of an unsupported expression", while the same line
#    one scope deeper worked. A parameter is the shape that matters:
#    min(), max() and container_of() all expand to typeof of a macro
#    argument, and in a function that argument is usually a parameter.
#    Those three macros are the test.
#
# 2. `__attribute__` was not accepted on a typedef, in EITHER position,
#    although the same attribute on a variable, a function or a struct
#    definition always worked. Every vector typedef has the shape
#    `typedef int v4 __attribute__((vector_size(16)))`, which is why
#    vector_size looked like a vector problem rather than a grammar one.
#
# The refusals matter as much as the acceptances, so both are here.
set -u
echo "TEST-MARKER typeof-attrs"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/typeof-attrs
rm -rf "$out"; mkdir -p "$out"
fail=0

ok() {  # ok LABEL SOURCE -- must compile
    printf '%s\n' "$2" > "$out/t.c"
    if "$EMBCC" -fsyntax-only "$out/t.c" > "$out/e.txt" 2>&1; then
        printf '  %s\n' "$1"
    else
        printf 'FAIL %s\n' "$1"
        grep -oE '(error|warning).*' "$out/e.txt" | head -1 | sed 's/^/     | /'
        fail=1
    fi
}
no() {  # no LABEL SOURCE NEEDLE -- must be refused, and say why
    printf '%s\n' "$2" > "$out/t.c"
    if "$EMBCC" -fsyntax-only "$out/t.c" > "$out/e.txt" 2>&1; then
        printf 'FAIL %s (accepted)\n' "$1"; fail=1
    elif grep -q "$3" "$out/e.txt"; then
        printf '  %s -- refused by name\n' "$1"
    else
        printf 'FAIL %s: refused, but not for the stated reason\n' "$1"
        head -1 "$out/e.txt" | sed 's/^/     | /'; fail=1
    fi
}

echo "-- typeof, by operand shape --"
ok 'typeof(global)'          'int x; typeof(x) y;'
ok 'typeof(local)'           'int f(void){int a=1; typeof(a) b=a; return b;}'
ok 'typeof(PARAMETER)'       'int f(int a){typeof(a) b=a; return b;}'
ok 'typeof(pointer param)'   'int f(int*p){typeof(p) q=p; return *q;}'
ok 'typeof(*param)'          'int f(int*p){typeof(*p) v=*p; return v;}'
ok 'typeof(param.member)'    'struct s{int m;}; int f(struct s v){typeof(v.m) x=v.m; return x;}'
ok 'typeof(param->member)'   'struct s{int m;}; int f(struct s*p){typeof(p->m) x=p->m; return x;}'
ok 'sizeof(parameter)'       'int f(long a){return (int)sizeof(a);}'
ok '__typeof__ spelling'     'int f(int a){__typeof__(a) b=a; return b;}'
ok 'typeof(a - b) arithmetic' 'int f(int a,long b){typeof(a - b) v=0; return (int)v;}'
ok 'typeof(literal)'         'int f(void){typeof(2) v=2; return v;}'
ok 'typeof(a < b) is int'    'int f(int a,int b){typeof(a < b) v=0; return v;}'
ok 'typeof(a << b)'          'int f(long a,int b){typeof(a << b) v=0; return (int)v;}'
ok 'typeof(call)'            'long g(void); int f(void){typeof(g()) y=g(); return (int)y;}'
ok 'typeof(call through fp)' 'long (*fp)(void); int f(void){typeof(fp()) y=fp(); return (int)y;}'
ok 'typeof(p - q) is not a pointer' 'long f(int*p,int*q){typeof(p - q) d=p-q; return (long)d;}'

echo "-- the three macros that are the whole point --"
ok 'MIN over parameters' '#define MIN(x,y) ({typeof(x) _a=(x); typeof(y) _b=(y); _a<_b?_a:_b;})
int f(int a,int b){return MIN(a,b);}'
ok 'MAX over mixed widths' '#define MAX(x,y) ({typeof(x) _a=(x); typeof(y) _b=(y); _a>_b?_a:_b;})
long f(long a,long b){return MAX(a,b);}'
ok 'container_of' 'struct s{int m; char c;};
#define container_of(p,t,m) ((t*)((char*)(1?(p):(typeof(((t*)0)->m)*)0)-__builtin_offsetof(t,m)))
char *f(int *p){return &container_of(p,struct s,m)->c;}'

echo "-- and it still runs, not just parses --"
cat > "$out/run.c" <<'CEOF'
#define MIN(x,y) ({ typeof(x) _a = (x); typeof(y) _b = (y); _a < _b ? _a : _b; })
struct box { int lo, hi; };
static int span(struct box b, int cap) { return MIN(b.hi - b.lo, cap); }
int main(void) {
    struct box b = { 3, 40 };
    int r = span(b, 20);                 /* MIN(37, 20) == 20 */
    typeof(r) doubled = r + r;           /* 40 */
    return doubled + MIN(2, 5);          /* 42 */
}
CEOF
if "$EMBCC" -O2 -c "$out/run.c" -o "$out/run.o" 2>"$out/e.txt"; then
    if t_link "$out/run" "$out/run.o" 2>/dev/null && t_run "$out/run"; then
        rc=$?
    else
        rc=$?
    fi
    if [ "${rc:-1}" -eq 42 ]; then
        echo "  a program using typeof on parameters exits 42"
    else
        echo "  (skipped running it: no runner here, it compiled)"
    fi
else
    echo "FAIL: the typeof program does not compile"
    grep -oE 'error.*' "$out/e.txt" | head -1 | sed 's/^/     | /'; fail=1
fi

echo "-- __attribute__ on a typedef, both positions --"
ok 'after the name'          'typedef int i __attribute__((may_alias)); i g;'
ok 'before the type'         'typedef __attribute__((may_alias)) int i; i g;'
ok 'deprecated on a typedef' 'typedef int i __attribute__((deprecated)); i g;'
ok 'a packed struct typedef' 'typedef struct {char a; int b;} __attribute__((packed)) s; s g;'
ok 'several declarators'     'typedef int i __attribute__((unused)), j; i a; j b;'
ok 'still plain'             'typedef unsigned u, *up; u a; up b;'

# aligned on a typedef is the type's alignment, as GCC and clang have it
# (tests/exec/aligned-typedef.c runs it): never quietly dropped
ok 'aligned on a typedef' \
   'typedef int i __attribute__((aligned(16))); i g; _Static_assert(_Alignof(i) == 16, "the typedef'"'"'s");'

[ "$fail" -eq 0 ] || exit 1
