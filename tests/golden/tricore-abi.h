/* The shapes tests/golden/tricore-abi-{caller,callee}.c pass between them:
 * what the TriCore EABI places in a way no other target here does
 * (docs/internals/tricore-plan.md). Pointers travel in A4-A7, apart from
 * the integers in D4-D7, and come back in A2; a 64-bit scalar takes the
 * next even pair and a later word fills the register skipped for it; a
 * struct of up to 8 bytes travels as an integer, a larger one as the
 * address of the caller's copy, and comes back through a hidden pointer
 * in A4; every unnamed variadic argument is on the stack. */
struct t3 { char a, b, c; };
struct t8 { int a, b; };
struct t20 { int v[5]; };

/* a D4, b E6 (D5 skipped), c back in D5 */
long long fill(int a, long long b, int c);
/* the five pointers: A4-A7 and the stack; the ints D4, D5 */
int ptrs(char *a, int x, char *b, char *c, int y, char *d, char *e);
/* a pointer result, in A2 */
char *ptr_ret(char *base, int off);
const short *ptr_ret2(int k, const short *tab);
/* floats and doubles in the data registers, pointers beside them */
double fd(float a, int *p, double b, int c);
/* a struct of 20 bytes by reference: the callee writes its copy */
int big(struct t20 s, int k);
/* small structs in D and E, both ways */
struct t3 r3(struct t3 s);
struct t8 r8(int a, struct t8 s);
struct t20 r20(int k, char *tag);
/* ten words: four in D4-D7, six on the stack; then a 64-bit one there */
long long many(int a, int b, int c, int d, int e, int f, int g, int h,
               int i, int j, long long k);
/* variadic: the named in registers, every unnamed one on the stack --
 * pointers, doubles, 64-bit integers and a large struct (its address) */
long long vmixed(int n, char *tag, ...);
/* through a function pointer, with pointer arguments */
typedef int (*cmpfn)(const int *, const int *);
int apply(cmpfn f, const int *a, const int *b);
int cmp_int(const int *a, const int *b);
