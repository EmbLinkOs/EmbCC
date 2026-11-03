#include <stdint.h>
volatile uint32_t ticks;
static int sum(const int *a, int n) { int s = 0; for (int i = 0; i < 16; i++) s += a[i] * n; return s; }
static int deep(int x) { int buf[32]; for (int i = 0; i < 32; i++) buf[i] = x + i; return sum(buf, x); }
long long div64(long long a, long long b) { return a / b; }
int rec(int n) { return n ? rec(n - 1) + 1 : 0; }
int (*fp)(int) = rec;
void SysTick_Handler(void) { ticks++; }
int main(void) { int v = deep(3); v += (int)div64(v, 7); v += fp(2); while (ticks < 10) { } return v; }
