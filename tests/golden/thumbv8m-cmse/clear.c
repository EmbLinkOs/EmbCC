/* The CMSE shapes tests/golden/thumbv8m-cmse.sh compares with clang's:
 * entry functions with each width of result, and calls through Non-secure
 * pointers with zero to four arguments and narrow results. */
typedef void __attribute__((cmse_nonsecure_call)) ns0_t(void);
typedef int __attribute__((cmse_nonsecure_call)) ns1_t(int);
typedef long long __attribute__((cmse_nonsecure_call)) ns2_t(int, int);
typedef short __attribute__((cmse_nonsecure_call)) ns3_t(int, int, int);
typedef unsigned char __attribute__((cmse_nonsecure_call)) ns4_t(int, int, int, int);

int g;

void __attribute__((cmse_nonsecure_entry)) e_void(void) { g++; }
int __attribute__((cmse_nonsecure_entry)) e_int(int a, int b) { return a * b + g; }
long long __attribute__((cmse_nonsecure_entry)) e_ll(int a) { return (long long)a * g; }
short __attribute__((cmse_nonsecure_entry)) e_short(short a, unsigned char b) { return (short)(a + b); }
struct pair { short x, y; };
struct pair __attribute__((cmse_nonsecure_entry)) e_struct(int a) { struct pair p = { (short)a, (short)g }; return p; }

void c0(ns0_t *f) { f(); }
int c1(ns1_t *f) { return f(g) + 1; }
long long c2(ns2_t *f) { return f(g, 2) + 3; }
int c3(ns3_t *f) { return f(1, 2, g) + 4; }
int c4(ns4_t *f) { return f(1, 2, 3, g) + 5; }
