#!/bin/sh
# The C++ standard library's foundation: <type_traits>, <utility>,
# <limits>, and the <c*> headers.
#
# Almost all of it is compile-time, so almost all of the test is too: a
# static_assert that fails is a compile error, which is the strongest
# check available and the one these headers deserve. A trait that answers
# the wrong question does not crash -- it silently selects the wrong
# overload three layers up.
set -eu
echo "TEST-MARKER libcxx-std"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/libcxx-std
rm -rf "$out"; mkdir -p "$out"
INC="-Ilib/libcxx/include -Ilib/libc/include"
TGT=$( [ "${ARCH:-x86_64}" = aarch64 ] && echo --target=aarch64-elf || echo "" )

check() {                  # check <name> <file>
    "$EMBCC" -c -x c++ -O1 $TGT $INC "$2" -o "$out/o.o" 2> "$out/err.txt" ||
        { head -5 "$out/err.txt"; echo "FAIL: $1"; exit 1; }
    echo "$1"
}

# ---- every header, on its own --------------------------------------------
# A header that only works after some other header has been included is a
# trap that hides until the one program that includes it first. Each is
# compiled alone, so an include this library forgot to make is a failure
# here rather than a surprise in a user's translation unit.
nsolo=0
for h in lib/libcxx/include/*; do
    b=$(basename "$h")
    printf '#include <%s>\nint main(){return 42;}\n' "$b" > "$out/solo.cc"
    "$EMBCC" -c -x c++ -O0 $TGT $INC "$out/solo.cc" -o "$out/solo.o" \
        2> "$out/err.txt" || { head -3 "$out/err.txt"
                               echo "FAIL: <$b> does not compile alone"
                               exit 1; }
    nsolo=$((nsolo + 1))
done
echo "all $nsolo headers compile on their own"

# ---- <type_traits> -------------------------------------------------------
cat > "$out/traits.cc" << 'EOF'
#include <type_traits>
using namespace std;
struct Pod { int a; };
struct Poly { virtual ~Poly(); };
struct Der : Poly {};
enum class E : short { x };
struct F { int operator()(int, double) const { return 0; } };
struct M { int v; int f(int) const { return v; } };

static_assert(is_integral_v<const int> && !is_integral_v<float>);
static_assert(is_floating_point_v<volatile double>);
static_assert(is_arithmetic_v<char> && !is_arithmetic_v<int *>);
static_assert(is_void_v<const void> && !is_void_v<int>);
static_assert(is_null_pointer_v<decltype(nullptr)>);
/* is_signed is about ARITHMETIC types: a pointer is neither signed nor
 * unsigned, and T(-1) < T(0) on one is not false but ill-formed. */
static_assert(is_signed_v<int> && !is_signed_v<unsigned> && !is_signed_v<int *>);
static_assert(is_unsigned_v<unsigned long> && !is_unsigned_v<Pod>);
static_assert(is_same_v<remove_cvref_t<const int &>, int>);
/* decay is what a by-value parameter does to its argument's type. */
static_assert(is_same_v<decay_t<int[5]>, int *>);
static_assert(is_same_v<decay_t<int(int)>, int (*)(int)>);
static_assert(is_same_v<decay_t<const int &>, int>);
/* SFINAE-friendly: void has no reference, and that must be `void`, not an
 * error -- generic code applies this to return types. */
static_assert(is_same_v<add_lvalue_reference_t<void>, void>);
static_assert(is_same_v<add_pointer_t<int &>, int *>);
/* The cv-qualifiers come back on, which is the half everyone forgets. */
static_assert(is_same_v<make_unsigned_t<const long>, const unsigned long>);
static_assert(is_same_v<make_signed_t<volatile unsigned char>, volatile signed char>);
static_assert(is_same_v<underlying_type_t<E>, short>);
static_assert(is_same_v<common_type_t<int, long, char>, long>);
static_assert(is_base_of_v<Poly, Der> && !is_base_of_v<Der, Poly>);
static_assert(is_polymorphic_v<Poly> && !is_polymorphic_v<Pod>);
static_assert(is_trivially_copyable_v<Pod> && !is_trivially_copyable_v<Poly>);
static_assert(rank_v<int[2][3]> == 2 && extent_v<int[2][3], 1> == 3);
static_assert(alignment_of_v<double> == alignof(double));
static_assert(is_copy_constructible_v<Pod> && is_move_assignable_v<Pod>);
static_assert(is_nothrow_destructible_v<Pod>);
/* conjunction SHORT-CIRCUITS. underlying_type_t<int> is ill-formed, so a
 * plain && of two ::value expressions would not compile at all. */
template <class T> struct UnderlyingIsShort
    : bool_constant<is_same_v<underlying_type_t<T>, short>> {};
static_assert(!conjunction_v<is_enum<int>, UnderlyingIsShort<int>>);
static_assert(conjunction_v<is_enum<E>, UnderlyingIsShort<E>>);
static_assert(disjunction_v<is_enum<E>, is_void<int>>);
static_assert(negation_v<is_void<int>>);
/* INVOKE: a callable, a member function through an object and through a
 * pointer, and a data member -- four spellings of "call it". */
static_assert(is_invocable_v<F, int, double> && !is_invocable_v<F, const char *>);
static_assert(is_same_v<invoke_result_t<F, int, double>, int>);
static_assert(is_invocable_r_v<long, F, int, double>);
static_assert(is_same_v<invoke_result_t<decltype(&M::f), M, int>, int>);
static_assert(is_same_v<invoke_result_t<decltype(&M::f), M *, int>, int>);
static_assert(is_same_v<invoke_result_t<decltype(&M::v), M &>, int &>);
int main() { return 42; }
EOF
check "<type_traits>: categories, transformations, relationships, and INVOKE" \
      "$out/traits.cc"

# ---- <limits> ------------------------------------------------------------
cat > "$out/limits.cc" << 'EOF'
#include <limits>
using namespace std;
struct NotANumber {};
static_assert(numeric_limits<int>::max() == 2147483647);
static_assert(numeric_limits<int>::min() == -2147483647 - 1);
static_assert(numeric_limits<int>::lowest() == numeric_limits<int>::min());
static_assert(numeric_limits<unsigned>::max() == 4294967295u);
static_assert(numeric_limits<int>::digits == 31);
static_assert(numeric_limits<unsigned>::digits == 32);
static_assert(numeric_limits<int>::digits10 == 9);
static_assert(numeric_limits<unsigned long long>::digits10 == 19);
/* Unsigned arithmetic wraps; signed overflow is undefined. */
static_assert(numeric_limits<unsigned>::is_modulo);
static_assert(!numeric_limits<int>::is_modulo);
static_assert(numeric_limits<bool>::digits == 1);
static_assert(numeric_limits<const int>::max() == 2147483647);
static_assert(numeric_limits<double>::is_iec559);
static_assert(numeric_limits<double>::digits == 53);
static_assert(numeric_limits<double>::max() > 1e308);
/* min() is the smallest POSITIVE NORMAL value for a floating type and the
 * most negative one for an integer -- the asymmetry lowest() exists to fix. */
static_assert(numeric_limits<double>::lowest() < 0);
static_assert(numeric_limits<double>::min() > 0);
static_assert(numeric_limits<long double>::max() >
              numeric_limits<double>::max());
/* The primary template answers "no", so a caller can tell a type with no
 * limits from one whose maximum is zero. */
static_assert(!numeric_limits<NotANumber>::is_specialized);
int main() { return 42; }
EOF
check "<limits>: every integer and floating type, and the unspecialized answer" \
      "$out/limits.cc"

# ---- long double in constant expressions ---------------------------------
# <limits> needs this and it did not work: the constant evaluator had no
# case for long double at all, so DBL_MAX -- which the compiler spells as a
# long double literal cast to double -- was not a constant expression.
cat > "$out/ld.cc" << 'EOF'
#include <cfloat>
static_assert(DBL_MAX > 1e308);
static_assert(LDBL_MAX > DBL_MAX);
/* The point of doing this EXACTLY rather than at double precision:
 * LDBL_EPSILON is below a double's resolution at 1.0, so an evaluator that
 * folded long doubles as doubles would answer false here -- silently. */
static_assert(1.0L + LDBL_EPSILON > 1.0L);
static_assert(1.0 + DBL_EPSILON / 2 == 1.0);
int main() { return 42; }
EOF
check "long double constant expressions, folded exactly" "$out/ld.cc"

# ---- <utility>, and it RUNS ----------------------------------------------
cat > "$out/utility.cc" << 'EOF'
#include <utility>
#include <cstdio>
using namespace std;
struct Tracked {
    int v; static int copies, moves;
    Tracked(int x = 0) : v(x) {}
    Tracked(const Tracked &o) : v(o.v) { copies++; }
    Tracked(Tracked &&o) noexcept : v(o.v) { moves++; o.v = -1; }
    Tracked &operator=(const Tracked &o) { v = o.v; copies++; return *this; }
    Tracked &operator=(Tracked &&o) noexcept
    { v = o.v; moves++; o.v = -1; return *this; }
};
int Tracked::copies, Tracked::moves;

static_assert(is_same_v<decltype(move(declval<int &>())), int &&>);
static_assert(is_same_v<decltype(forward<int &>(declval<int &>())), int &>);
/* The oldest trap in C: the signed operand is converted, so -1 < 1u is
 * FALSE. These compare the mathematical values instead. */
static_assert(cmp_less(-1, 1u) && !(-1 < 1u));
static_assert(cmp_greater(1u, -1));
static_assert(!cmp_equal(-1, (unsigned)-1));
static_assert(tuple_size<pair<int, char>>::value == 2);
static_assert(is_same_v<tuple_element_t<1, pair<int, char>>, char>);
static_assert(is_same_v<make_index_sequence<3>, index_sequence<0, 1, 2>>);

int main()
{
    Tracked a(1), b(2);
    swap(a, b);
    printf("swap %d %d copies %d\n", a.v, b.v, Tracked::copies);
    printf("exchange %d %d\n", exchange(a.v, 9), a.v);

    auto p = make_pair(1, 'x');
    static_assert(is_same_v<decltype(p), pair<int, char>>);
    auto [k, v] = p;                        /* structured binding */
    printf("pair %d %c\n", k, v);
    printf("order %d %d\n", make_pair(1, 2) < make_pair(1, 3),
           make_pair(2, 0) < make_pair(1, 9));

    Tracked::moves = Tracked::copies = 0;
    Tracked c(5);
    Tracked d(move_if_noexcept(c));
    printf("moveifnoexcept moves %d copies %d v %d\n",
           Tracked::moves, Tracked::copies, d.v);
    return 42;
}
EOF
check "<utility>: move, forward, swap, pair, comparisons, sequences" \
      "$out/utility.cc"

# ---- the <c*> headers ----------------------------------------------------
cat > "$out/cheaders.cc" << 'EOF'
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
int main()
{
    /* Both spellings have to work: the standard requires std::, and every
     * real implementation also leaves the global one, which code relies on. */
    std::size_t n = std::strlen("abc");
    /* nine ints: our <time.h> lays tm out as C11 names it and nothing
     * more -- glibc's is larger because it adds two non-standard fields. */
    std::printf("%zu %d %g %d\n", n, std::toupper('a'), std::sqrt(4.0),
                (int)sizeof(std::tm));
    assert(n == 3);
    return strlen("ab") == 2 ? 42 : 1;
}
EOF
check "the <c*> headers: names in std:: and at global scope" "$out/cheaders.cc"

# ---- and the two that run -----------------------------------------------
[ "${ARCH:-x86_64}" = x86_64 ] ||
    { echo "skipped the run: the harness link here is x86-64"; exit 0; }
[ -f build/libcxx/x86_64/libcxx.a ] ||
    { echo "skipped the run: no libcxx (make libcxx)"; exit 0; }
command -v x86_64-elf-ld > /dev/null 2>&1 ||
    { echo "skipped the run: no x86_64-elf-ld"; exit 0; }

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
LIBGCC=$(dirname "$($GCC -print-libgcc-file-name)")
H=tests/harness/x86_64
for part in start sys crt; do
    src=$H/$part.S; [ "$part" = sys ] && src=$H/sys.c
    [ "$part" = crt ] && src=$H/../crt.c
    $GCC -ffreestanding -mno-red-zone -isystem "$X86_NEWLIB/include" \
        -c "$src" -o "$out/$part.o"
done
run() {
    "$EMBCC" -c -x c++ -O2 $INC -Itests/golden/libcxx-std "$1" -o "$out/p.o"
    x86_64-elf-ld -n -z max-page-size=0x1000 -T "$H/link.ld" -o "$out/p.64" \
        "$out/start.o" "$out/p.o" "$out/sys.o" "$out/crt.o" \
        build/libcxx/x86_64/libcxx.a build/libc/x86_64/libc.a \
        -L"$LIBGCC" -lgcc 2>&1 | grep -v 'RWX permissions' >&2 || true
    x86_64-elf-objcopy -I elf64-x86-64 -O elf32-i386 "$out/p.64" "$out/p.elf"
    "$H/run.sh" "$out/p.elf" > "$out/run.txt" 2>&1
    return $?
}
if run "$out/utility.cc"; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: <utility> exited $rc"; exit 1; }
want() { grep -qx "$1" "$out/run.txt" || { echo "FAIL: expected \"$1\""; exit 1; }; }
# swap moves; it must not copy.
want "swap 2 1 copies 0"
want "exchange 2 9"
want "pair 1 x"
want "order 1 0"
# move_if_noexcept moves when the move cannot throw -- which is what makes a
# vector's reallocation exception-safe.
want "moveifnoexcept moves 1 copies 0 v 5"
# ---- <iterator>, <memory>, <functional>, <array> ------------------------
# These four only make sense running: they are about what happens to
# objects, not about what a type is. Each program CHECKS ITSELF and exits
# 42, so every expectation lives beside the code it is about and a
# failure names its own line -- see libcxx-std/check.h.
for prog in iterator memory functional array vector string algorithm \
            vocabulary associative iostreams smartptr sequences views timing \
            files containers compare erasure formatting rangeviews atomics \
            regexes concurrency langsupport callable numerics lastmile; do
    if run "tests/golden/libcxx-std/$prog.cc"; then rc=0; else rc=$?; fi
    [ "$rc" = 42 ] || {
        cat "$out/run.txt"
        echo "FAIL: <$prog> exited $rc"; exit 1; }
    [ "$prog" = iostreams ] && cp "$out/run.txt" "$out/iostreams.txt"
    echo "<$prog>: every check passed"
done

# The iostreams program is the one whose result is also OUTPUT: the
# checks above prove the string streams format correctly, and these two
# lines prove std::cout reaches the terminal through our own streambuf,
# our own FILE and our own write.
grep -qx "iostreams: 1 2.5 ok" "$out/iostreams.txt" ||
    { cat "$out/iostreams.txt"; echo "FAIL: cout did not print"; exit 1; }
grep -qx "\.\.\.\.42" "$out/iostreams.txt" ||
    { echo "FAIL: setw/setfill did not reach cout"; exit 1; }
echo "and std::cout really prints, through our streambuf and our FILE"

if run "$out/cheaders.cc"; then rc=0; else rc=$?; fi
cat "$out/run.txt"
[ "$rc" = 42 ] || { echo "FAIL: the <c*> program exited $rc"; exit 1; }
want "3 65 2 36"
echo "and both run on our own C++ runtime and C library"
