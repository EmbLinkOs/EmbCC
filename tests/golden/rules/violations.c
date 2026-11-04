#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern int ext_arr[];
static int g[10];
union U { int a; float b; };
int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
int unused_param(int a, int b) { return a; }
void f(uint8_t u8, int s, unsigned u, char *p) {
    int x = 010;
    long l = 5l;
    unsigned v = 0xffffffff;
    char *q = "lit";
    if (s) x = 1;
    if (s > 0) x = 2; else if (s < 0) x = 3;
    while (u) u--;
    x = u8 + 1;
    x = s & 3;
    u = s;
    x = (s, 2);
    g[10] = 1;
    s++;
    malloc(4);
    p = malloc(8); free(p); p[0] = 1;
    switch (s) { case 1: x = 4; case 2: x = 5; break; }
    for (float fl = 0; fl < 1; fl += 0.5f) { }
    goto out;
out:
    x = rand();
    (void)x; (void)l; (void)v; (void)q;
}
