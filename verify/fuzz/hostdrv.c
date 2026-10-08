#include <stdio.h>
extern unsigned long long ck;
int prog_main(void);
int main(void) { prog_main(); printf("%llu\n", ck); return 0; }
