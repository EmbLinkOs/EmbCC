#include <stdio.h>
#include <string.h>
#include <stdlib.h>
__attribute__((noinline)) void fill(char *b, int n) { memset(b, 'A', n); }
int work(int n) { char buf[16]; fill(buf, n); return buf[0] + buf[15]; }
int safe(int x) { return x * 3; }
int main(int argc, char **argv) { int n = argc > 1 ? atoi(argv[1]) : 16; printf("%d %d\n", work(n), safe(n)); return 0; }
