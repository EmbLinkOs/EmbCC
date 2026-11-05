#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
uint32_t ticks;
volatile uint64_t stamp;
volatile uint32_t flags;
volatile uint32_t counter;
static int buf[8];
static void log_it(int v) { buf[v & 7] = v; }
void __disable_irq(void); void __enable_irq(void);
void SysTick_Handler(void) { ticks++; stamp = stamp + 1; flags |= 1; counter++; log_it(3); printf("tick\n"); }
int main(void) {
    uint32_t t0 = ticks;
    while (ticks == t0) { }
    uint64_t s = stamp;
    counter++;
    __disable_irq(); flags &= ~1u; __enable_irq();
    log_it(5);
    char *p = malloc(10);
    p[0] = 1;
    free(p);
    free(p);
    char *q = malloc(4);
    if (!q) return 1;
    q[1] = 2;
    return (int)s;
}
