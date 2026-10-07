/* The shapes tests/golden/rx-abi-{caller,callee}.c pass between them, for
 * what is particular to GCC's RX convention (docs/internals/rx-plan.md):
 * each is a rule a backend could read consistently wrong and still agree
 * with itself through the whole exec corpus. */
struct c6 { char v[6]; };                 /* not whole words: stacked */
struct s4c { char a, b, c, d; };          /* one word: r1 */
struct s12 { int a, b, c; };
struct s16 { int a, b, c, d; };
struct al16 { long x; } __attribute__((aligned(16)));
struct msbf { char a; int b : 4; };      /* 8 bytes in the MS layout */

/* A stacked argument still advances the register count: `a` is in r3. */
int after_c6(struct c6 s, int a);
/* r1-r3, then a long long that does not fit -- stacked, and so is e. */
long long after3(int a, int b, int c, long long d, int e);
/* A twelve-byte struct after one int: r2-r4. After two: the stack. */
int s12_regs(int a, struct s12 s);
int s12_stk(int a, int b, struct s12 s, int z);
/* Narrow arguments, four in registers and three stacked one after the
 * other at their own sizes (sp+0, +2, +4). */
int narrow(signed char a, unsigned char b, short c, unsigned short d,
           signed char e, short f, unsigned char g);
/* Narrow results, extended by the callee. */
signed char ret_sc(int x);
unsigned short ret_us(int x);
/* Results: up to 16 bytes in words in r1-r4, anything else through r15. */
struct s16 ret16(int k);
struct c6 ret6(int k);
struct s4c ret4c(int k);
_Complex float retcf(float re, float im);
long long ret64(long long a, long long b);
/* binary32 doubles in single registers. */
double dmul(double a, float b, double c);
/* The last named argument of a variadic function is on the stack too. */
int vlast(int a, int b, ...);
double vdbl(int n, ...);
int vstruct(int n, ...);
/* An over-aligned struct is stacked at a multiple of 4, not 16. */
long stk_al16(int a, int b, int c, int d, struct al16 s, int z);
int bf_byval(int k, struct msbf s);
