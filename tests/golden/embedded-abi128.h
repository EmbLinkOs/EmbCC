/* The 128-bit shapes tests/golden/embedded-abi128-{caller,callee}.c pass
 * between them at RV64, so that one may be compiled by EmbCC and the
 * other by clang and the two must still agree -- the psABI's rules for a
 * 2*XLEN scalar, which an __int128 and a long double both are there:
 *
 *   a NAMED one takes the next two argument registers, odd-first if that
 *   is where they start (p_odd: a1:a2);
 *   with one register left it SPLITS, the low word in a7 and the high
 *   at the bottom of the stack area (p_split);
 *   wholly on the stack it is 16-aligned (p_stack: after an int at sp+0
 *   it is at sp+16, not sp+8);
 *   a VARIADIC one takes an EVEN pair (v128: a2:a3 after an int in a1);
 *   and it comes back in a0:a1.
 *
 * A struct holding one is two words packed like any small aggregate. */
typedef __int128 s128;

struct w128 { s128 v; };
struct wld { long double d; };

s128 r_ret(long k);
s128 p_odd(int a, s128 x);
s128 p_split(int a, int b, int c, int d, int e, int f, int g, s128 x);
s128 p_stack(int a, int b, int c, int d, int e, int f, int g, int h,
             int i, s128 x, s128 y);
long double l_odd(int a, long double x, long double y);
long double l_split(int a, int b, int c, int d, int e, int f, int g,
                    long double x, long double y);
s128 v128(int n, ...);              /* int, s128, int, s128, ... */
long double vld(int n, ...);        /* int, long double, ... */
struct w128 s_odd(int a, struct w128 x, struct wld y);
