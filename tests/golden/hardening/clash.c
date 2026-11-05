#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int vla(int n) { char b[n]; memset(b, 1, n); int s = 0; for (int i = 0; i < n; i += 97) s += b[i]; return s; }
int big(int k) { int a[5000]; for (int i = 0; i < 5000; i++) a[i] = i ^ k; long s = 0; for (int i = 0; i < 5000; i++) s += a[i]; return (int)s; }
int al(int n) { char *p = __builtin_alloca(n); p[0] = 3; p[n-1] = 4; return p[0] + p[n-1]; }
int main(int argc, char **argv) { int n = argc > 1 ? atoi(argv[1]) : 20000; printf("%d %d %d\n", vla(n), big(n), al(n)); return 0; }
