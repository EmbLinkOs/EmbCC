/* The o32 shapes tests/golden/mips-abi-{caller,callee}.c pass between
 * them: one is compiled by EmbCC and the other by clang
 * (--target=mipsel-unknown-elf -mcpu=mips32r2 -msoft-float), and every
 * pairing must print the same thing (tests/golden/mips-abi.sh).
 *
 * What is particular to o32 (docs/internals/mips32-plan.md) and so is
 * what this exercises beyond embedded-abi.h:
 *   - the argument block: an 8-byte value is 8-aligned in it, so after
 *     one int a double or long long skips a1 for a2:a3, and after three
 *     it skips a3 for the stack;
 *   - soft float: a float and a double travel exactly as an int and a
 *     long long do, and come back in v0 and v0:v1;
 *   - EVERY struct comes back through the hidden pointer in a0, and a
 *     _Complex float in v0,v1 and a _Complex double in v0:v1,a0:a1;
 *   - a composite of any size travels by value, split across a3 and the
 *     stack when it straddles them;
 *   - a char or short is extended by the side that has it;
 *   - variadic arguments follow the same block, and va_arg of a double
 *     rounds the walking pointer up to 8;
 *   - a HI16/LO16 address pair whose addend crosses 0x8000 (an element far
 *     into an array), which the linker resolves by the AHL rule.
 */
struct d1 { double d; };
struct di { int i; double d; };            /* 16 bytes, 8-aligned */
struct c3 { char a, b, c; };
struct big { int v[9]; };                  /* 36 bytes, by value */
struct fl2 { float a, b; };

double dadd(double a, double b);
float fmul3(float a, float b, float c);
double d_after_int(int a, double b);
double d_after_3(int a, int b, int c, double d);
long long ll_after_3(int a, int b, int c, long long d);
double many_d(double a, double b, double c, double e, int f);
float f_mixed(int a, float b, long long c, float d);
struct d1 ret_d1(double x);
struct di ret_di(int i, double d);
struct c3 ret_c3(int k);
struct big ret_big(int k);
_Complex float ret_cf(float re, float im);
_Complex double ret_cd(double re, double im);
double take_di(int a, struct di s);
int take_c3(int a, int b, int c, struct c3 s);
int take_big(int a, struct big s, int z);
int take_fl2(struct fl2 s, int k);
int take_narrow(signed char a, unsigned char b, short c, unsigned short d,
                int e, signed char f);
signed char ret_sc(int v);
unsigned char ret_uc(int v);
short ret_ss(int v);
unsigned short ret_us(int v);
double vdsum(int n, ...);
int vmixed(int n, ...);
int far_elt(int i);
extern int far_array[10000];
int (*pick(int k))(int);

/* Where the byte order shows (mips-be-abi.sh runs these big-endian): a
 * composite shorter than a word travels LEFT-justified in its register
 * big-endian (its first byte the register's most significant), a
 * long long's or double's HIGH word is the lower-numbered register of its
 * pair and the lower stack word, and a long long comes back high word in
 * v0. Each in the registers, straddling a3 and the stack, and wholly on
 * the stack, named and variadic. */
struct b1 { unsigned char a; };
struct b2 { unsigned char a, b; };
struct h1 { short h; };
struct b5 { unsigned char c[5]; };
struct b6 { short h[3]; };
int take_b1(struct b1 s, int k);
int take_b2_h1(int k, struct b2 s, struct h1 t);
int take_b5_at3(int a, int b, int c, struct b5 s);
int take_small_stack(int a, int b, int c, int d, struct b1 s, struct b2 t,
                     struct h1 u, struct b6 v);
long long ll_first(long long a, int b);
long long ll_stack(int a, int b, int c, int d, long long e, int f);
long long ll_mix(long long a, long long b);
unsigned long long ull_ret(unsigned hi, unsigned lo);
double d_stack(int a, int b, int c, int d, int e, double f);
struct b2 ret_b2(int k);
struct h1 ret_h1(int k);
long long vll(int n, ...);
int vsmall(int n, ...);
