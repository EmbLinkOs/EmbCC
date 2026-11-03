#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct S { int a, b; };
int pick(int x) { return x * 2; }
int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    int x;
    int y = 5;
    int *h = malloc(4 * sizeof *h);
    char buf[32];
    struct S s;
    s.a = 1;
    snprintf(buf, sizeof buf, "%d", y);
    memcpy(h, &y, sizeof y);
    if (mode == 1 && x > 3) puts("x");              /* uninit local */
    if (mode == 2 && h[1]) puts("h");               /* uninit heap */
    if (mode == 3 && pick(s.b)) puts("s");          /* uninit field through a call */
    int v = 0;
    if (mode == 4) { int q; sscanf("12", "%d", &q); v = q; }
    if (mode == 5) { char *d = strdup(buf); v = d[0]; free(d); }
    if (buf[0] == '5' && h[0] == 5 && s.a == 1) printf("ok %d\n", v);
    free(h);
    return 0;
}
