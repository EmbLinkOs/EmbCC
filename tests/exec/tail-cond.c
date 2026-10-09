/* A call returned from one arm of a choice -- `return c < 0x80 ?
 * isdigit(c) : 0;` -- is a tail call once its return is copied to it:
 * the function leaves by a branch, with no frame, and the other arm
 * returns on its own. Values, void calls, arguments in a new order, a
 * 64-bit result, and a function whose other arm makes an ordinary call.
 * Exit 0, or the number of the first wrong result. */
// expect-exit: 0

typedef __INT32_TYPE__ i32;
typedef long long i64;

static volatile i32 vk = 5;
static i32 hits;

__attribute__((noinline)) static i32 dbl(i32 x) { return x * 2; }
__attribute__((noinline)) static i32 sub(i32 a, i32 b) { return a - b; }
__attribute__((noinline)) static i64 wide(i64 x) { return x << 3 | 1; }
__attribute__((noinline)) static void note(i32 x) { hits += x; }

__attribute__((noinline)) static i32 guard(i32 c) { return c < 0x80 ? dbl(c) : 0; }
__attribute__((noinline)) static i32 swap_or(i32 a, i32 b) { return a > b ? sub(b, a) : -1; }
__attribute__((noinline)) static i64 wide_or(i64 x) { return x ? wide(x) : 7; }
__attribute__((noinline)) static void maybe(i32 x) { if (x & 1) note(x); }
__attribute__((noinline)) static i32 both(i32 a, i32 b)
{
    if (a)
        return sub(a, b);
    return dbl(b) + 1;
}
__attribute__((noinline)) static i32 chain(i32 s, i32 x)
{
    switch (s) {
    case 0: return dbl(x);
    case 1: return sub(x, 3);
    case 2: return sub(3, x);
    default: return x;
    }
}

static int nth, first_bad;

static void check(i64 got, i64 want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    i32 k = vk;
    check(guard(k), 2 * k);
    check(guard(k + 0x100), 0);
    check(swap_or(k + 4, k), -4);
    check(swap_or(k, k + 4), -1);
    check(wide_or((i64)k << 34), ((i64)k << 37) | 1);
    check(wide_or(0), 7);
    maybe(k);
    maybe(k + 1);
    check(hits, k);
    check(both(k, 2), k - 2);
    check(both(0, k), 2 * k + 1);
    for (i32 s = 0; s < 4; s++)
        check(chain(s, k), s == 0 ? 2 * k : s == 1 ? k - 3 : s == 2 ? 3 - k : k);
    return first_bad;
}
