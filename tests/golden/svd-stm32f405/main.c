/* An STM32F405 program built from what embsvd generates alone -- the
 * device header, the startup, the linker script -- and CMSIS-Core, run on
 * QEMU's netduinoplus2. */
#include "STM32F405.h"

static volatile uint32_t ticks;
static uint32_t initialized = 0x5a5a;   /* .data, copied by the startup */
static uint32_t zeroed[8];              /* .bss, zeroed by it */
static int ctor_ran;

__attribute__((constructor)) static void init(void) { ctor_ran = 1; }

void SysTick_Handler(void) { ticks++; }  /* takes the startup's weak one */

/* a device interrupt: its slot in the vector table is 16 + USART1_IRQn,
 * and it is reached only if the generated table puts it there */
static volatile int usart_irqs;
void USART1_IRQHandler(void) { usart_irqs++; }

static void putc_(char c)
{
    while (!(USART1->SR & USART6_SR_TXE_Msk))
        ;
    USART1->DR = (uint32_t)c;
}

static void puts_(const char *s) { while (*s) putc_(*s++); }

static void hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 0xf]);
    putc_(' ');
}

int main(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN_Msk;
    USART1->CR1 = USART6_CR1_UE_Msk | USART6_CR1_TE_Msk;
    puts_("STM32F405 via embsvd\n");
    hex(initialized);
    hex(zeroed[5]);
    hex((uint32_t)ctor_ran);
    hex(SCB->CPUID >> 4 & 0xfff);
    hex(USART1_IRQn);
    puts_("\n");
    if (SysTick_Config(16000) != 0)
        puts_("systick config failed\n");
    __enable_irq();
    while (ticks < 3)
        __WFI();
    __disable_irq();
    NVIC_EnableIRQ(USART1_IRQn);
    NVIC_SetPendingIRQ(USART1_IRQn);
    __enable_irq();
    for (int i = 0; i < 1000 && !usart_irqs; i++)
        ;
    __disable_irq();
    puts_("ticks ");
    hex(ticks >= 3);
    hex((uint32_t)usart_irqs);
    puts_("\ndone\n");
    for (;;)
        ;
}
