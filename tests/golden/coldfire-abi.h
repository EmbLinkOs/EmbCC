/* The shapes tests/golden/coldfire-abi-{caller,callee}.c pass between them:
 * what the m68k convention does that no other target here does
 * (docs/internals/coldfire-plan.md). Every argument is on the stack in
 * whole words -- a char or short promoted, its byte the word's last; a
 * composite smaller than a word right-justified in it, a larger one padded
 * after; a 64-bit scalar two words, only 4-aligned. Every structure comes
 * back through the buffer whose address is in a1; a _Complex float in d0
 * and d1, a _Complex double in d0-d3; a pointer in a0 and d0. Nothing is
 * aligned beyond two bytes, so an int inside a struct may sit at offset 2. */
struct c1 { char a; };
struct c2 { char a, b; };
struct c3 { char a, b, c; };
struct c5 { char a; int b; };            /* 6 bytes: the int at offset 2 */
struct c7 { short a; char b, c, d, e, f; };
struct c10 { char a; long long b; char c; }; /* 12: the long long at 2 */

int narrow(signed char a, unsigned char b, short c, unsigned short d, int e);
int smalls(struct c1 a, int x, struct c2 b, struct c3 c, int y);
long long mids(struct c5 a, struct c7 b, long long z, struct c10 c);
struct c3 r3(int k);
struct c5 r5(struct c5 s, int k);
struct c10 r10(long long v);
_Complex float cf(float re, float im);
_Complex double cd(double re, double im, int k);
char *ptr_ret(char *base, int off);
double dmix(char a, double b, short c, float d, long long e);
long long vstructs(int n, ...);
int keep_across(int a, int b, int c, int d);
