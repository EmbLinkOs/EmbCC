/* The n64 shapes tests/golden/mips64-abi-{caller,callee}.c pass between
 * them: one is compiled by EmbCC and the other by clang
 * (--target=mips64el-unknown-elf / mips64-unknown-elf -mcpu=mips64r2
 * -msoft-float -mno-abicalls -G0), and every pairing must print the same
 * thing (tests/golden/mips64-abi.sh).
 *
 * What is particular to n64 (docs/internals/mips64-plan.md) and so is
 * what this exercises beyond embedded-abi.h:
 *   - eight argument registers, doubleword slots, and the stack from the
 *     caller's sp with no home area;
 *   - a 32-bit value travels SIGN-EXTENDED, unsigned too, both ways;
 *   - soft float: a float and a double travel as an int and a long do,
 *     and a long double (binary128) as an even-aligned register pair --
 *     where an __int128 takes the next slot, aligned or not (clang);
 *   - a composite of any size by value, packed as `ld` reads it (a short
 *     one left-justified big-endian), split across a7 and the stack;
 *     one aligned to 16 at an even slot;
 *   - results: a composite of at most 16 bytes in v0:v1 -- as its
 *     doublewords, or field by field when it is one or two floating
 *     fields (a float sign-extended in its own register) or a _Complex
 *     float or double -- and a larger one through the pointer in a0;
 *   - variadic: the same slots, a long double rounded to an even one,
 *     a float promoted to double;
 *   - a 64-bit address in four relocated pieces (%highest..%lo) whose
 *     carries ripple, and a function pointer returned.
 */
struct c3 { char a, b, c; };
struct i3 { int a, b, c; };                 /* 12 bytes */
struct l3 { long a, b, c; };                /* 24 bytes: registers */
struct big { long v[10]; };                 /* 80: a2..a7 and the stack */
struct al16 { _Alignas(16) long a; long b; };
struct i16 { __int128 x; };
struct ld1 { long double x; };
struct f1 { float a; };
struct f2 { float a, b; };
struct df { double a; float b; };
struct fd { float a; double b; };
struct d2 { double a, b; };
struct fi { float a; int b; };              /* not all floating: doublewords */
struct s16 { short h[8]; };

long many_l(long a, long b, long c, long d, long e, long f, long g, long h,
            long i, long j);
double many_d(double a, double b, double c, double d, double e, double f,
              double g, double h, double i, int j);
int many_i(int a, int b, int c, int d, int e, int f, int g, int h, int i,
           unsigned j);
float fmul3(float a, float b, float c);
unsigned uret(unsigned a, unsigned b);
int is_max(unsigned u);
long sext_back(int x);
signed char ret_sc(int v);
unsigned char ret_uc(int v);
short ret_ss(int v);
unsigned short ret_us(int v);
long double ld_add(long double a, long double b);
long double ld_after_int(int a, long double b);
long double ld_stack(int a, int b, int c, int d, int e, int f, int g,
                     long double h, int i);
__int128 i128_after_int(int a, __int128 b);
__int128 i128_at7(int a, int b, int c, int d, int e, int f, int g,
                  __int128 h);
unsigned __int128 i128_mul(unsigned __int128 a, unsigned __int128 b);
int take_c3(int a, struct c3 s, int k);
int take_i3(struct i3 s, int k);
long take_l3(int a, struct l3 s);
long take_big(int a, int b, struct big s, int z);
long take_al16(int a, struct al16 s, int z);
long take_i16(int a, struct i16 s);
int take_c3_stack(long a, long b, long c, long d, long e, long f, long g,
                  long h, struct c3 s, struct s16 t);
struct c3 ret_c3(int k);
struct i3 ret_i3(int k);
struct l3 ret_l3(long k);
struct big ret_big(long k);
struct f1 ret_f1(float x);
struct f2 ret_f2(float x, float y);
struct df ret_df(double x, float y);
struct fd ret_fd(float x, double y);
struct d2 ret_d2(double x, double y);
struct fi ret_fi(float x, int y);
struct s16 ret_s16(int k);
struct ld1 ret_ld1(long double x);
_Complex float ret_cf(float re, float im);
_Complex double ret_cd(double re, double im);
_Complex long double ret_cld(long double re, long double im);
long vsum(int n, ...);
double vdsum(int n, ...);
long double vldsum(int n, ...);
long vmixed(int n, ...);
extern long far_array[300000];
long far_elt(long i);
int (*pick(int k))(int);
