/* STM32F405 (QEMU netduinoplus2), written against ST's stm32f4xx.h and
 * ARM's CMSIS-Core, as a CubeMX project's main would be. */
#include "stm32f4xx.h"

static volatile uint32_t ticks;
static uint32_t initialized = 0x1234;   /* .data */
static uint32_t zeroed[8];              /* .bss */

void SysTick_Handler(void) { ticks++; }

static void uart_putc(char c)
{
    while (!(USART1->SR & USART_SR_TXE))
        ;
    USART1->DR = (uint32_t)c;
}

static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }

static void uart_hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        uart_putc("0123456789abcdef"[(v >> i) & 0xf]);
    uart_putc(' ');
}

void __libc_init_array(void) { }        /* no C++ constructors here */

int main(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE;
    SystemCoreClockUpdate();
    uart_puts("STM32F405 via CMSIS\n");
    uart_hex(SystemCoreClock);
    uart_hex(initialized);
    uart_hex(zeroed[3]);
    uart_hex(__REV(0x11223344));
    uart_hex(__CLZ(0x00010000));
    uart_hex(__get_PRIMASK());
    uart_hex(SCB->CPUID >> 4 & 0xfff);   /* part number: c24 is a Cortex-M4 */
    uart_puts("\n");
    NVIC_SetPriority(SysTick_IRQn, 3);
    if (SysTick_Config(SystemCoreClock / 1000) != 0)
        uart_puts("systick config failed\n");
    __enable_irq();
    while (ticks < 5)
        __WFI();
    __disable_irq();
    uart_puts("ticks ");
    uart_hex(ticks >= 5);
    uart_puts("\ndone\n");
    for (;;)
        ;
}
