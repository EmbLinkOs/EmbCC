/* The AAPCS shapes tests/golden/arm-a32-abi-{caller,callee}.c pass
 * between them: one is compiled by EmbCC and the other by clang
 * (--target=armv7a-none-eabi -mfloat-abi=soft, in ARM state or, for the
 * interworking pairs, Thumb), and every pairing must print the same
 * thing (tests/golden/arm-a32-abi.sh).
 *
 * What embedded-abi.h does not reach, and so what this exercises:
 *   - soft float: a float travels as an int and a double as a long long
 *     does, in r0-r3 or on the stack, and comes back in r0 or r0:r1;
 *   - an eight-byte value takes an EVEN register pair (r0:r1 or r2:r3) or
 *     an 8-aligned stack slot, so after one int a double skips r1, and
 *     after three it goes to the stack, leaving r3 unused -- and nothing
 *     later back-fills r3 (the NCRN is past it);
 *   - a composite up to four bytes comes back in r0 (struct c3), a larger
 *     one through the hidden pointer in r0 -- including _Complex float and
 *     _Complex double, which AAPCS treats as two-member structs;
 *   - a composite of any size is passed by value, split across r3 and the
 *     stack when it straddles them, 8-aligned when it holds a double;
 *   - a char or short is extended by the side that has it;
 *   - variadic arguments follow the same rules (a double 8-aligned in the
 *     register file or on the stack), and va_arg walks them;
 *   - movw/movt of an address far into an array (an addend past 16 bits),
 *     and a function pointer returned across the call -- which, in the
 *     interworking pairs, is a pointer to the other instruction set's
 *     code, called through blx.
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
