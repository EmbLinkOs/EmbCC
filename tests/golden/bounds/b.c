#include <stdio.h>
#include <stdlib.h>
struct msg { int len; int pad; int data[] __attribute__((counted_by(len))); };
struct buf { int n; char *p __attribute__((counted_by(n))); };
int g[10];
int at(int i) { return g[i]; }
int *end_of(void) { return &g[10]; }
int m2(int i, int j) { int m[3][4] = { 0 }; return m[i][j]; }
int fam(struct msg *m, int i) { return m->data[i]; }
int ptr(struct buf *b, int i) { return b->p[i]; }
int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    struct msg *m = malloc(sizeof *m + 4 * sizeof(int)); m->len = 4;
    char store[8] = "abcdefg"; struct buf b = { 8, store };
    int r = at(3) + (end_of() - g) + m2(2, 3) + fam(m, 3) + ptr(&b, 7);
    if (mode == 1) r += at(10);
    if (mode == 2) r += m2(1, 4);
    if (mode == 3) r += fam(m, 4);
    if (mode == 4) r += ptr(&b, 8);
    if (mode == 5) r += at(-1);
    printf("ok %d\n", r);
    return 0;
}
