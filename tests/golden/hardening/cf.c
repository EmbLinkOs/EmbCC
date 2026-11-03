#include <setjmp.h>
int g(int);
jmp_buf jb;
int sw(int x) { switch (x) { case 0: return g(1); case 1: return g(7); case 2: return g(9); case 3: return 4; case 4: return g(2); case 5: return 77; default: return 0; } }
int cg(int x) { static void *t[] = { &&a, &&b }; goto *t[x & 1]; a: return g(1); b: return g(2); }
int sj(void) { if (setjmp(jb)) return 1; return g(0); }
int leaf(int x) { return x + 1; }
