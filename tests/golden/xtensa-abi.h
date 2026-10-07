/* The windowed-ABI shapes tests/golden/xtensa-abi-{caller,callee}.c pass
 * between them: one is compiled by EmbCC and the other by Espressif's GCC
 * (xtensa-esp32s2-elf-gcc), and every pairing must print the same thing
 * (tests/golden/xtensa-abi.sh).
 *
 * What is particular to Xtensa (docs/internals/xtensa-plan.md) and so is
 * what this exercises beyond embedded-abi.h:
 *   - six argument words, a2-a7 in the callee; a type aligned beyond 4
 *     starts at an even word; one that does not fit WHOLLY in the words
 *     left goes on the stack, and so does every argument after it, even
 *     one that would have fitted (a7 left empty);
 *   - stack arguments at the caller's sp, each at its own alignment;
 *   - soft float: a float and a double travel as an int and a long long;
 *   - a result of up to 16 bytes comes back in a2-a5, a struct included,
 *     and a larger one through a hidden first argument;
 *   - a _Complex argument is its two parts, each placed on its own;
 *   - a char or short is extended by the side that has it;
 *   - variadic arguments, including the jump of va_arg's index from the
 *     register words to the stack, and a va_list passed BY VALUE (GCC's
 *     12-byte record) to a function of the other compiler.
 */
#include <stdarg.h>
struct d1 { double d; };
struct di { int i; double d; };            /* 16 bytes, 8-aligned */
struct c3 { char a, b, c; };
struct s12 { int a, b, c; };
struct s16 { int a, b, c, d; };
struct s20 { int v[5]; };
struct big { int v[9]; };                  /* 36 bytes, by value */
struct fl2 { float a, b; };

double dadd(double a, double b);
float fmul3(float a, float b, float c);
double d_after_int(int a, double b);
long long ll_after_5(int a, int b, int c, int d, int e, long long f, int g);
int s12_after_4(int a, int b, int c, int d, struct s12 s, int z);
long long ll_stack(int a, int b, int c, int d, int e, int f, int g,
                   long long x, int h, long long y);
double many_d(double a, double b, double c, double e, int f);
float f_mixed(int a, float b, long long c, float d);
struct d1 ret_d1(double x);
struct di ret_di(int i, double d);
struct c3 ret_c3(int k);
struct s12 ret_s12(int k);
struct s16 ret_s16(int k);
struct s20 ret_s20(int k);
struct big ret_big(int k);
_Complex float ret_cf(float re, float im);
_Complex double ret_cd(double re, double im);
double take_cd(int a, _Complex double z, int b);
float take_cf(int a, int b, int c, int d, int e, _Complex float z);
double take_di(int a, struct di s);
int take_c3(int a, int b, int c, struct c3 s);
int take_big(int a, struct big s, int z);
int take_fl2(struct fl2 s, int k);
int take_narrow(signed char a, unsigned char b, short c, unsigned short d,
                int e, signed char f, short g, unsigned char h);
signed char ret_sc(int v);
unsigned char ret_uc(int v);
short ret_ss(int v);
unsigned short ret_us(int v);
double vdsum(int n, ...);
int vmixed(int n, ...);
int vlate(int a, int b, int c, int d, int e, ...);
/* named arguments that end on the stack: va_start's index starts past the
 * register words' gap (GCC's +2 words), and after a composite that did
 * not fit in a4-a7, at six words plus its own */
int vsix(int a, int b, int c, int d, int e, int f, ...);
int vstraddle(int a, int b, int c, int d, struct s12 s, ...);
int vlist(int n, va_list ap);              /* defined by the callee */
int vforward(int n, ...);                  /* the callee's, calling vlist */
int caller_vlist(int n, va_list ap);       /* defined by the CALLER */
int (*pick(int k))(int);
