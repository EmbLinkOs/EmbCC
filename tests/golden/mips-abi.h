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
