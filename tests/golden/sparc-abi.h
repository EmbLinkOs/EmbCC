/* The SPARC shapes tests/golden/sparc-abi-{caller,callee}.c pass between
 * them: one is compiled by EmbCC and the other by clang
 * (--target=sparc-none-elf -mcpu=leon3 -msoft-float), and every pairing
 * must print the same thing (tests/golden/sparc-abi.sh).
 *
 * What is particular to SPARC (docs/internals/sparc-plan.md) and so is
 * what this exercises beyond embedded-abi.h:
 *   - the argument WORDS: no padding, so a double or long long after one
 *     int is in %o1:%o2, after five it straddles %o5 and %sp+92, after six
 *     it is wholly at %sp+92, high word first everywhere;
 *   - EVERY aggregate by reference to the caller's copy -- a 1-, 3-, 8-
 *     and 36-byte struct, a union, a _Complex, a long double -- in a
 *     register word and on the stack, and a callee that writes its
 *     parameter must not change the caller's object;
 *   - a struct, a union and a long double returned through the buffer at
 *     %sp+64 with the caller's `unimp` after the call, the callee returning
 *     past it; a _Complex float in %o0:%o1 and a _Complex double in
 *     %o0-%o3;
 *   - a char or short extended by the side that has it;
 *   - variadic doubles at odd words, long longs, and structs (their
 *     addresses) among them;
 *   - an address whose addend needs sethi's high 22 bits and a nonzero low
 *     ten (an element far into an array), and a function pointer.
 * Nothing here does long double ARITHMETIC: clang calls __addtf3 with its
 * operands in registers, which no SPARC runtime implements, so only the
 * passing of the values is compared.
 */
struct s1 { unsigned char a; };
struct s3 { char a, b, c; };
struct s8 { int a, b; };
struct sd { double d; };
struct big { int v[9]; };                  /* 36 bytes */
union un { int i; char c[6]; };

double d_after_1(int a, double b);
double d_split(int a, int b, int c, int d, int e, double f);
double d_stack(int a, int b, int c, int d, int e, int f, double g);
long long ll_after_1(int a, long long b);
long long ll_split(int a, int b, int c, int d, int e, long long f);
long long ll_stack(int a, int b, int c, int d, int e, int f, long long g,
                   int h);
long long ll_ret(unsigned hi, unsigned lo);
float f_mixed(int a, float b, long long c, float d);
int take_s1(struct s1 s, int k);
int take_s3(int a, struct s3 s);
int take_s8_write(struct s8 s);
int take_big(int a, struct big s, int z);
int take_un(union un u, int k);
int take_many(int a, int b, int c, int d, int e, int f, struct s3 g,
              struct s8 h, int i);
double take_cd(_Complex double z, int k);
float take_cf(int k, _Complex float z);
struct s1 ret_s1(int k);
struct s3 ret_s3(int k);
struct sd ret_sd(double x);
struct big ret_big(int k);
union un ret_un(int k);
_Complex float ret_cf(float re, float im);
_Complex double ret_cd(double re, double im);
long double ld_pick(int which, long double a, long double b);
long double ld_stack(int a, int b, int c, int d, int e, int f,
                     long double g);
signed char ret_sc(int v);
unsigned char ret_uc(int v);
short ret_ss(int v);
unsigned short ret_us(int v);
int take_narrow(signed char a, unsigned char b, short c, unsigned short d,
                int e, signed char f, short g);
double vdsum(int n, ...);
long long vll(int n, ...);
int vstruct(int n, ...);
int far_elt(int i);
extern int far_array[10000];
int (*pick(int k))(int);
