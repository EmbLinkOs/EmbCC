#!/bin/sh
# Re-run the GCC/Clang gap audit (docs/developer/gaps-vs-gcc-clang.md).
#
# Every claim in that document came from this script, because a gap list
# written from reading source rots the moment something lands -- and two
# findings in the first draft were simply WRONG that way (EmbCC does have
# an inliner; attributes are warned-and-ignored rather than honoured).
# So: probe the built compiler, print YES/NO, and let the document quote
# the output.
#
#   usage: tools/gap-probe.sh [path-to-embcc]
#
# Exits 0 always -- this measures, it does not judge. Diff two runs to
# see what a change bought.
set -u
E=${1:-./embcc}
[ -x "$E" ] || { echo "gap-probe: $E is not executable (run make embcc)" >&2; exit 1; }
T=${TMPDIR:-/tmp}/embcc-gap.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
yes=0; no=0

# p LABEL SOURCE [ext] -- does embcc accept this translation unit?
p() {
    ext=${3:-c}
    printf '%s\n' "$2" > "$T/p.$ext"
    if err=$("$E" -fsyntax-only "$T/p.$ext" 2>&1); then
        printf '  YES  %s\n' "$1"; yes=$((yes + 1))
    else
        printf '  NO   %-36s | %s\n' "$1" \
            "$(printf '%s' "$err" | sed "s|$T/p.$ext||g" \
               | grep -oE '(error|warning).*' | head -1 | cut -c1-58)"
        no=$((no + 1))
    fi
}

# o FLAG -- does the driver accept this option on a real compile?
o() {
    printf 'int g;\n' > "$T/o.c"
    if "$E" $1 -c "$T/o.c" -o "$T/o.o" >/dev/null 2>&1; then
        printf '  YES  %s\n' "$1"; yes=$((yes + 1))
    else
        printf '  NO   %s\n' "$1"; no=$((no + 1))
    fi
}

# a ATTR -- recognised, or parsed and silently dropped?
a() {
    printf '__attribute__((%s)) int g;\n' "$1" > "$T/a.c"
    out=$("$E" -fsyntax-only "$T/a.c" 2>&1)
    # Order matters: a template can BOTH warn that the attribute is
    # unknown and then fail to parse. Checking the warning first labelled
    # those "dropped" and hid the error -- which is how the first draft of
    # the gap document came to claim vector_size was silently ignored when
    # it is actually a parse error.
    if printf '%s' "$out" | grep -q error; then
        printf '  REFUSED  %-24s | %s\n' "$1" \
            "$(printf '%s' "$out" | grep -oE 'error.*' | head -1 | cut -c1-48)"
    elif printf '%s' "$out" | grep -q "is not one EmbCC knows"; then
        printf '  DROPPED  %s\n' "$1"
    else
        printf '  OK       %s\n' "$1"
    fi
}

echo "== preprocessor =================================================="
# The __has_* family, in the form headers actually use. An undefined
# __has_include makes the identifier 0 and the parse then dies on '(<',
# so the DEFENSIVE spelling is a hard error -- which is worse than the
# feature being absent, because the fallback never runs.
p "__has_include (guarded)" '#if defined(__has_include) && __has_include(<stddef.h>)
int a;
#endif'
p "__has_builtin"  '#if __has_builtin(__builtin_expect)
int a;
#endif'
p "__has_attribute" '#if __has_attribute(packed)
int a;
#endif'
p "_Pragma operator" '#define D _Pragma("GCC diagnostic push")
int f(void){ D return 1; }'
p "__VA_OPT__"     '#define M(x,...) f(x __VA_OPT__(,) __VA_ARGS__)
int f(int); int g(void){return M(1);}'
p "#embed"         '#embed "x.bin"'

echo "== typeof, by operand shape ======================================"
p "typeof(global)"   'int x; typeof(x) y;'
p "typeof(local)"    'int f(void){int a=1; typeof(a) b=a; return b;}'
p "typeof(PARAMETER)" 'int f(int a){typeof(a) b=a; return b;}'
p "typeof(a+1)"      'int f(void){int a=1; typeof(a+1) b=a; return b;}'
p "typeof(call)"     'int f(void); typeof(f()) y;'

echo "== builtins ======================================================"
p "__builtin_clz"        'int f(unsigned x){return __builtin_clz(x);}'
p "__builtin_popcountll" 'int f(unsigned long long x){return __builtin_popcountll(x);}'
p "__builtin_bswap32"    'unsigned f(unsigned x){return __builtin_bswap32(x);}'
# C++-only, which a grep of the tree does not tell you: the C front end
# says "is not declared". Probing beats grepping, and this pair is the
# reason that sentence is in the audit.
p "__builtin_add_overflow (C)" 'int f(int a,int b,int*r){return __builtin_add_overflow(a,b,r);}'
p "__builtin_mul_overflow (C)" 'int f(int a,int b,int*r){return __builtin_mul_overflow(a,b,r);}'
p "__builtin_types_compatible_p" 'int f(void){return __builtin_types_compatible_p(int,long);}'
p "__builtin_choose_expr" 'int f(void){return __builtin_choose_expr(1,2,3);}'
p "__builtin_object_size" 'unsigned long f(void*p){return __builtin_object_size(p,0);}'
p "__builtin_fabs"       'double f(double x){return __builtin_fabs(x);}'
p "__builtin_copysign"   'double f(double a,double b){return __builtin_copysign(a,b);}'
p "__builtin_fma"        'double f(double a,double b,double c){return __builtin_fma(a,b,c);}'
p "__builtin_LINE"       'int f(void){return __builtin_LINE();}'
p "__builtin_memcmp"     'int f(const void*a,const void*b){return __builtin_memcmp(a,b,4);}'
p "__builtin_setjmp"     'int f(void*b){return __builtin_setjmp(b);}'

echo "== atomics ======================================================="
p "__atomic_load_n"      'int f(int*p){return __atomic_load_n(p,5);}'
p "__atomic_compare_exchange_n" 'int f(int*p,int*e){return __atomic_compare_exchange_n(p,e,1,0,5,5);}'
p "__sync_synchronize"   'void f(void){__sync_synchronize();}'
p "_Atomic + stdatomic.h" '#include <stdatomic.h>
atomic_int a; int f(void){atomic_store(&a,1); return atomic_load(&a);}'
p "__c11_atomic_load"    '_Atomic int a; int f(void){return __c11_atomic_load(&a,5);}'

echo "== language ======================================================"
p "computed goto"     'int f(int x){void*t[]={&&a,&&b}; goto *t[x]; a: return 1; b: return 2;}'
p "VLA"               'int f(int n){int a[n]; a[0]=1; return a[0];}'
p "statement expr"    'int f(void){return ({int x=1; x+1;});}'
p "nested function"   'int f(void){int g(int x){return x+1;} return g(1);}'
p "__label__"         'int f(void){__label__ l; goto l; l: return 1;}'
p "__auto_type"       'int f(void){__auto_type x=5; return x;}'
p "asm goto"          'int f(void){asm goto("" ::: : l); return 0; l: return 1;}'
p "case ranges"       'int f(int x){switch(x){case 1 ... 5: return 1;} return 0;}'
p "vector_size type"  'typedef int v4 __attribute__((vector_size(16))); v4 f(v4 a,v4 b){return a+b;}'
p "_Float16"          '_Float16 f(_Float16 x){return x+1;}'
p "__float128"        '__float128 f(__float128 x){return x*x;}'
p "__int128"          '__int128 f(__int128 x){return x*x;}'
p "C23 constexpr"     'constexpr int k=5; int f(void){return k;}'
p "C23 auto"          'int f(void){auto x=5; return x;}'
p "C23 nullptr"       'void*f(void){return nullptr;}'
p "C23 [[attr]]"      '[[noreturn]] void f(void);'
p "C23 enum : type"   'enum e : unsigned char { A };'
p "C23 typeof_unqual" 'int x; typeof_unqual(x) y;'

echo "== C++ ==========================================================="
p "lambda"            'int f(){auto g=[](int x){return x+1;}; return g(1);}' cc
p "concepts"          'template<class T> concept C=true; template<C T> T f(T x){return x;} int g(){return f(1);}' cc
p "coroutines"        '#include <coroutine>
int f(){return 0;}' cc
p "<ranges>"          '#include <ranges>
int f(){return 0;}' cc
p "operator<=>"       '#include <compare>
struct S{int a; auto operator<=>(const S&)const=default;};' cc
p "std::string"       '#include <string>
int f(){std::string s="x"; return (int)s.size();}' cc
p "modules"           'export module m;' cc

echo "== attributes: honoured / dropped / refused ======================"
for at in noreturn packed aligned weak used unused deprecated constructor destructor \
          format pure const may_alias warn_unused_result always_inline noinline \
          hot cold nonnull returns_nonnull malloc alloc_size returns_twice flatten \
          nothrow leaf sentinel gnu_inline optimize no_sanitize \
          assume_aligned target transparent_union designated_init counted_by \
          vector_size mode access copy error noclone noipa tls_model weakref ifunc \
          cleanup naked interrupt; do a "$at"; done

echo "== __attribute__ placement ======================================"
# Positional, not per-attribute: a typedef NAME takes no attribute at all,
# which is why every vector-typedef fails.
p "attr on a variable"      'int g __attribute__((aligned(16)));'
p "attr on a function"      'void f(void) __attribute__((noreturn));'
p "attr on a struct def"    'struct s { int a; } __attribute__((packed));'
p "attr on a typedef'd struct" 'typedef struct { int a; } __attribute__((packed)) s;'
p "attr AFTER a typedef name" 'typedef int i __attribute__((aligned(16)));'
p "attr BEFORE a typedef type" 'typedef __attribute__((aligned(16))) int i;'

echo "== driver options a real build passes ============================"
for f in -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
         -fshort-enums -fsigned-char -funsigned-char -fwrapv -fno-strict-aliasing \
         -fPIC -fpie -shared -static -nostdlib -flto \
         -fsanitize=undefined -fprofile-arcs -pg \
         -gdwarf-4 -gdwarf-5 -ggdb -g3 \
         -march=native -mtune=generic -mthumb \
         -v -save-temps -pipe -pedantic; do o "$f"; done
printf '  (-include is documented at src/driver/main.c:122): '
printf 'int x;\n' > "$T/i.c"; printf '\n' > "$T/pre.h"
"$E" -include "$T/pre.h" -fsyntax-only "$T/i.c" >/dev/null 2>&1 \
    && echo "accepted" || echo "REJECTED -- the help text promises it"

echo "== silent acceptance (THE RULE says refuse) ======================"
printf 'int f(void){for(int i=0;i<1;i++){} return 0;}\n' > "$T/s.c"
for bad in -O9 -Wcompletely-made-up -std=c89 -std=bogus; do
    printf '  %-24s ' "$bad"
    "$E" $bad -fsyntax-only "$T/s.c" >/dev/null 2>&1 \
        && echo "accepted silently" || echo "refused"
done

echo "== -g per target ================================================="
printf 'struct s{int x;}; int f(struct s*p){return p->x;}\n' > "$T/g.c"
for t in x86_64-elf aarch64-elf thumbv7m-none-eabi riscv32-unknown-elf riscv64-unknown-elf; do
    printf '  %-22s ' "$t"
    if "$E" --target=$t -g -c "$T/g.c" -o "$T/g.o" >/dev/null 2>&1; then
        echo "yes"
    else
        echo "NO  $("$E" --target=$t -g -c "$T/g.c" -o "$T/g.o" 2>&1 \
              | grep -oE 'error.*' | head -1 | cut -c1-52)"
    fi
done

echo
echo "warnings implemented: $("$E" --help-warnings 2>&1 | grep -c '^  -W')"
echo "probes: $yes accepted, $no rejected"
