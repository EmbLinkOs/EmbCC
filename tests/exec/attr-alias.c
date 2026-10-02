/* __attribute__((alias("t"))) makes a function another name for t,
 * defined in the same file -- how CMSIS startup code gives every IRQ
 * handler a weak default: `void USART1_IRQHandler(void)
 * __attribute__((weak, alias("Default_Handler")));`. EmbCC warned that
 * it did not know the attribute and left the name undefined, so the
 * link failed (or, weak, resolved to nothing). Calls, a vector table of
 * them, a static alias, at every optimization level. */
// expect-exit: 42
void Default_Handler(void) { }
static int counter;
void count_handler(void) { counter++; }
void USART1_IRQHandler(void) __attribute__((weak, alias("count_handler")));
void TIM2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
int real(void) { return 40; }
int al(void) __attribute__((alias("real")));
static int sreal(int x) { return x + 1; }
static int salias(int) __attribute__((alias("sreal")));
void (*const handlers[])(void) = { USART1_IRQHandler, TIM2_IRQHandler };
int main(void)
{
    handlers[0]();
    USART1_IRQHandler();
    TIM2_IRQHandler();
    return al() + counter + salias(-1) + (handlers[1] == TIM2_IRQHandler ? 0 : 100);
}
