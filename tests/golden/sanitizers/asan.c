#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int g[10];
int idx(int i) { return i; }
int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    int *p = malloc(10 * sizeof *p);
    int a[8];
    memset(a, 0, sizeof a);
    for (int i = 0; i < 10; i++) p[i] = i;
    if (mode == 1) p[idx(10)] = 1;            /* heap overflow */
    if (mode == 2) a[idx(8)] = 1;             /* stack overflow */
    if (mode == 3) g[idx(10)] = 1;            /* global overflow */
    if (mode == 4) { free(p); return p[3]; }  /* use after free */
    if (mode == 5) { free(p); free(p); }      /* double free */
    char *s = strdup("hello");
    printf("ok %d %d %s\n", p[9], a[3], s);
    free(s);
    free(p);
    return 0;
}
