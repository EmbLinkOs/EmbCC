#!/bin/sh
# sizeof, _Alignof and offsetof have the target's size_t, and a pointer
# difference its ptrdiff_t -- the types <stddef.h> names through
# __SIZE_TYPE__ and __PTRDIFF_TYPE__.
#
# They were unsigned long and long on every target. On ILP32 that is
# only the wrong type (a _Generic told size_t from sizeof); on AVR it is
# the wrong VALUE as well, because unsigned long is four bytes there and
# size_t two: `long x = -sizeof(int)` was -2 where avr-gcc and clang
# give 65534.
#
# Each target is checked twice over: the _Static_asserts are folded by
# the parser before sema runs, and the functions' _Generics have no
# default association, so sema refuses them unless its own type matches.
set -u
echo "TEST-MARKER size-types"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/size-types
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/t.c" <<'SRC'
struct s { char a; int b; };
#define IS(T, x) _Generic((x), T: 1, default: 0)
_Static_assert(IS(__SIZE_TYPE__, sizeof(int)), "sizeof is size_t");
_Static_assert(IS(__SIZE_TYPE__, _Alignof(int)), "_Alignof is size_t");
_Static_assert(IS(__SIZE_TYPE__, __builtin_offsetof(struct s, b)),
               "offsetof is size_t");
_Static_assert(IS(__PTRDIFF_TYPE__, (char *)0 - (char *)0),
               "a pointer difference is ptrdiff_t");
_Static_assert((unsigned long long)-sizeof(char) ==
               (unsigned long long)(__SIZE_TYPE__)-1,
               "-sizeof(char) is computed in size_t");
enum e { NEG_SIZE = -sizeof(char) };
_Static_assert(NEG_SIZE > 0, "an enumerator of -sizeof is not negative");

__SIZE_TYPE__ f1(void) { return _Generic(sizeof(int), __SIZE_TYPE__: 1); }
__SIZE_TYPE__ f2(void) { return _Generic(_Alignof(int), __SIZE_TYPE__: 2); }
__SIZE_TYPE__ f3(void)
{
    return _Generic(__builtin_offsetof(struct s, b), __SIZE_TYPE__: 3);
}
__PTRDIFF_TYPE__ f4(char *p, char *q) { return _Generic(p - q, __PTRDIFF_TYPE__: p - q); }
long long f5(void) { return -sizeof(int); }
SRC

for t in x86_64-elf aarch64-elf aarch64-apple-darwin thumbv7m-none-eabi \
         riscv32-unknown-elf riscv64-unknown-elf avr; do
    "$EMBCC" --target=$t -c "$out/t.c" -o "$out/t-$t.o" \
        > "$out/$t.err" 2>&1 || {
        echo "FAIL $t:"; cat "$out/$t.err"; exit 1; }
done
echo "size-types: sizeof, _Alignof, offsetof and p - q typed as the target's"
