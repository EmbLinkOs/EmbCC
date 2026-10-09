/* What is particular to LP64S, for tests/golden/loongarch-abi-{caller,
 * callee}.c: one side compiled by EmbCC and the other by clang
 * (--target=loongarch64-unknown-elf -msoft-float), and the two must still
 * agree. Each shape is one a backend could read consistently wrong:
 *
 *   a 32-bit value travels SIGN-EXTENDED in its 64-bit register, unsigned
 *   included (u_ret's 0x80000001 must compare equal to the caller's, and
 *   u_take's callee reads all 64 bits of its argument);
 *   plain char is SIGNED here, where RISC-V's is not;
 *   floating point is soft: a float or a double is its bits in an INTEGER
 *   register, a variadic float is promoted to double, and a struct of
 *   floats is a small aggregate like any other;
 *   a 16-byte aggregate is a register pair, odd-first when named and
 *   splitting across a7 and the stack; a larger one goes BY REFERENCE to
 *   a copy the caller owns (big_mod's write must not reach the caller);
 *   a variadic 16-byte-aligned scalar or aggregate takes an EVEN pair;
 *   stack arguments of mixed widths each take an eight-byte slot;
 *   _Complex float and double come back packed like the structs they are;
 *   and globals and functions cross the units -- clang reaches another
 *   unit's through its GOT and calls through CALL36, which embld rewrites
 *   and patches (it builds no GOT). */
struct f2 { float a, b; };
struct d2 { double a, b; };
struct fd { float f; double d; };
struct big { long a, b, c; };
struct mixc { signed char c; unsigned char u; short s; int i; };
struct a16 { _Alignas(16) long lo; long hi; };

unsigned u_ret(unsigned x);
long u_take(unsigned x, int y);
int c_take(char c, signed char sc, unsigned char uc, short s,
           unsigned short us);
char c_ret(int v);
float f_sum(float a, double b, float c, int d);
double d_var(int n, ...);                 /* n doubles (floats promoted) */
struct f2 f2_make(float a, float b);
struct d2 d2_make(double a, double b);
struct fd fd_make(float f, double d);
double d2_take(int a, struct d2 x, struct d2 y);
double d2_split(int a, int b, int c, int d, int e, int f, int g,
                struct d2 x);
long big_sum(struct big b, int k);
void big_mod(struct big b);
long many(long a, long b, long c, long d, long e, long f, long g, long h,
          int s0, long s1, char s2, short s3, unsigned s4, long s5);
long v_mix(int n, ...);   /* int, __int128, int, struct a16, long */
struct mixc mixc_make(int k);
_Complex float cf_make(float re, float im);
_Complex double cd_make(double re, double im);

extern int g_counter;
extern long g_arr[4];
int bump(int by);
int (*pick_fn(int which))(int);
int call_fn(int (*f)(int), int x);
