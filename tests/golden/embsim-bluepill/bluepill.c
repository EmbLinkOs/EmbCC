/* A "Blue Pill" (an STM32F103C8) driving RCC, the GPIO ports, AFIO,
 * USART1, TIM2 and SysTick, run on QEMU's stm32vldiscovery (an STM32F100:
 * the F1 family's USARTs at the same addresses) and on EmbSim's bluepill;
 * built from the header, startup and linker script embsvd writes from
 * ST's STM32F103xx.svd, with the 8 KiB of SRAM both have.
 *
 * Each line is "Q NAME VALUE" or "M NAME VALUE":
 *   Q  QEMU models the register: EmbSim must print what QEMU prints;
 *   M  QEMU does not model it (RCC, GPIO, AFIO and the timers are
 *      unimplemented there, reading 0), or models it otherwise than the
 *      reference manual (RM0008) says: bluepill.txt holds the manual's
 *      values.
 * Waits are bounded, so the run ends on QEMU too. Output is USART1's,
 * polled: QEMU's first serial port. */
#include "STM32F103xx.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define ISER0 REG(0xE000E100u)
#define ISER1 REG(0xE000E104u)
#define ICER1 REG(0xE000E184u)
#define ICPR0 REG(0xE000E280u)
#define ICPR1 REG(0xE000E284u)
#define SHCSR REG(0xE000ED24u)
#define CFSR REG(0xE000ED28u)
#define BFAR REG(0xE000ED38u)

static void putc_(char c)
{
    for (int i = 0; i < 100000 && !(USART1->SR & (1u << 7)); i++)   /* TXE */
        ;
    USART1->DR = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void line(char tag, const char *name, uint32_t v)
{
    putc_(tag);
    putc_(' ');
    puts_(name);
    putc_(' ');
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 0xf]);
    putc_('\n');
}

#define Q(n, v) line('Q', n, v)
#define M(n, v) line('M', n, v)

static volatile unsigned tim_irqs, usart_irqs, ticks, faults;
static volatile uint32_t fault_addr, fault_status;

void TIM2_IRQHandler(void)
{
    if (TIM2->SR & 1u) {
        TIM2->SR = ~1u;                         /* UIF is rc_w0 */
        GPIOC->ODR ^= 1u << 13;                 /* the LED */
        tim_irqs++;
    }
}

void USART1_IRQHandler(void)
{
    usart_irqs++;
    USART1->CR1 &= ~(1u << 7);                  /* TXEIE off: it stays set */
}

void SysTick_Handler(void)
{
    ticks++;
}

void bus_fault_c(void)
{
    faults++;
    fault_addr = BFAR;
    fault_status = CFSR;
    CFSR = CFSR;
}

/* returns past the faulting 16-bit store */
__attribute__((naked)) void BusFault_Handler(void)
{
    __asm__ volatile("mrs r0, msp\n\t"
                     "ldr r1, [r0, #24]\n\t"
                     "adds r1, #2\n\t"
                     "str r1, [r0, #24]\n\t"
                     "b bus_fault_c");
}

static void store(uint32_t a, uint32_t v)
{
    register uint32_t r1 __asm__("r1") = a;
    register uint32_t r0 __asm__("r0") = v;
    __asm__ volatile("str r0, [r1]" : : "r"(r0), "r"(r1) : "memory");
}

static void wait_bit(volatile uint32_t *r, uint32_t m)
{
    for (int i = 0; i < 1000 && !(*r & m); i++)
        ;
}

int main(void)
{
    /* ---- out of reset ------------------------------------------------- */
    uint32_t cr0 = RCC->CR & 0xffff00ffu, cfgr0 = RCC->CFGR, ahb0 = RCC->AHBENR;
    uint32_t apb20 = RCC->APB2ENR;
    uint32_t idr_off = GPIOA->IDR;              /* its clock is off */

    /* ---- USART1 on PA9, as the board's console --------------------------- */
    RCC->APB2ENR = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 14);
    GPIOA->CRH = (GPIOA->CRH & ~(0xfu << 4)) | (0xbu << 4);   /* PA9: AF push-pull 50 MHz */
    USART1->BRR = 0x45;                          /* 8 MHz (HSI) / 115200 */
    USART1->CR1 = (1u << 13) | (1u << 3) | (1u << 2);           /* UE, TE, RE */
    puts_("bluepill\n");
    Q("USART1.SR-reset", USART1->SR);
    Q("USART1.BRR-hsi", USART1->BRR);
    M("RCC.CR-reset", cr0);
    M("RCC.CFGR-reset", cfgr0);
    M("RCC.AHBENR-reset", ahb0);
    M("RCC.APB2ENR-reset", apb20);
    M("GPIOA.IDR-clock-off", idr_off);
    M("RCC.APB2ENR", RCC->APB2ENR);

    /* ---- the ports out of reset: the debug port's pins pulled ------------ */
    M("GPIOA.CRL", GPIOA->CRL);
    M("GPIOA.CRH", GPIOA->CRH);
    M("GPIOA.IDR", GPIOA->IDR);
    M("GPIOB.IDR", GPIOB->IDR);
    M("GPIOC.IDR", GPIOC->IDR);

    /* ---- 72 MHz: HSE, the PLL x9 ---------------------------------------- */
    FLASH->ACR = (FLASH->ACR & ~7u) | 2;        /* two wait states */
    RCC->CFGR = (4u << 8) | (1u << 16) | (7u << 18);   /* APB1 /2, PLL from HSE, x9 */
    RCC->CFGR |= 2;                             /* SW: PLL, not ready yet */
    M("RCC.CFGR-pll-off", RCC->CFGR);
    RCC->CR |= 1u << 24;                        /* PLLON, but HSE is off */
    M("RCC.CR-pll-no-hse", RCC->CR & 0x03030003u);
    RCC->CR |= 1u << 16;                        /* HSEON */
    wait_bit(&RCC->CR, 1u << 17);
    wait_bit(&RCC->CR, 1u << 25);
    M("RCC.CR-pll", RCC->CR & 0x03030003u);
    for (int i = 0; i < 1000 && (RCC->CFGR & 0xc) != 8; i++)
        ;
    M("RCC.CFGR-pll", RCC->CFGR);
    USART1->BRR = 0x271;                        /* 72 MHz / 115200 = 39.0625 */
    Q("USART1.BRR", USART1->BRR);
    puts_("at 72 MHz\n");

    /* ---- USART1's flags and its interrupt -------------------------------- */
    Q("USART1.CR1", USART1->CR1);
    Q("USART1.SR", USART1->SR);
    USART1->SR = ~(1u << 6);                    /* TC: rc_w0 */
    Q("USART1.SR-tc-cleared", USART1->SR & 0xc0);
    USART1->SR = 0x3ff;                         /* ones leave rc_w0 flags */
    M("USART1.SR-ones", USART1->SR);
    ISER1 = 1u << (USART1_IRQn - 32);
    USART1->CR1 |= 1u << 7;                     /* TXEIE: TXE is set */
    for (int i = 0; i < 1000 && !usart_irqs; i++)
        ;
    ICER1 = 1u << (USART1_IRQn - 32);
    Q("USART1.irqs", usart_irqs);
    /* USART2's clock is off: its registers ignore writes, read 0 */
    USART2->BRR = 0x1234;
    M("USART2.BRR-clock-off", USART2->BRR);

    /* ---- the pins ----------------------------------------------------------- */
    GPIOA->CRL = (GPIOA->CRL & ~0xffu) | 0x88u; /* PA0, PA1: inputs with pulls */
    GPIOA->BSRR = 1u << 0;                      /* PA0 pulled up */
    GPIOA->BRR = 1u << 1;                       /* PA1 pulled down */
    M("GPIOA.IDR-pulls", GPIOA->IDR & 0x3u);
    M("GPIOA.IDR-tx", GPIOA->IDR >> 9 & 1);    /* USART1's TX, idle high */
    GPIOA->CRL = (GPIOA->CRL & ~0xf0u) | 0x00u; /* PA1: analog */
    GPIOA->BSRR = 1u << 1;
    M("GPIOA.IDR-analog", GPIOA->IDR >> 1 & 1);
    /* PA15 is JTDI, pulled up, whatever it is set to, until SWJ_CFG frees it */
    GPIOA->CRH = (GPIOA->CRH & ~(0xfu << 28)) | (0x2u << 28);  /* output, push-pull */
    GPIOA->BRR = 1u << 15;
    M("GPIOA.IDR15-jtdi", GPIOA->IDR >> 15 & 1);
    AFIO->MAPR = 2u << 24;                      /* SW-DP only */
    M("GPIOA.IDR15-freed", GPIOA->IDR >> 15 & 1);
    M("GPIOA.IDR13-swdio", GPIOA->IDR >> 13 & 1);
    /* PB5: open-drain output, driving low */
    GPIOB->CRL = (GPIOB->CRL & ~(0xfu << 20)) | (0x6u << 20);
    GPIOB->BSRR = 1u << (16 + 5);
    M("GPIOB.IDR5-od-low", GPIOB->IDR >> 5 & 1);
    GPIOB->BSRR = 1u << 5;                      /* let go: floating */
    M("GPIOB.IDR5-od-released", GPIOB->IDR >> 5 & 1);
    GPIOB->BSRR = 3u << 6;                      /* ODR6, ODR7 set */
    GPIOB->BSRR = (1u << 6) | (1u << (16 + 6)); /* set wins */
    M("GPIOB.ODR", GPIOB->ODR & 0xc0u);
    GPIOB->BRR = 1u << 7;
    M("GPIOB.ODR-brr", GPIOB->ODR & 0xc0u);
    /* the LED, PC13: an output, push-pull, 2 MHz */
    GPIOC->CRH = (GPIOC->CRH & ~(0xfu << 20)) | (0x2u << 20);
    GPIOC->BSRR = 1u << 13;                     /* high: off */
    M("GPIOC.ODR", GPIOC->ODR);
    M("GPIOC.IDR", GPIOC->IDR);

    /* ---- TIM2 at 1 ms, toggling the LED; SysTick at 1 ms --------------- */
    RCC->APB1ENR |= 1u << 0;                    /* TIM2 */
    TIM2->PSC = 71;                             /* 72 MHz / 72: 1 MHz */
    TIM2->ARR = 999;                            /* 1 ms */
    TIM2->EGR = 1;                              /* PSC in, UIF set */
    M("TIM2.SR-ug", TIM2->SR);
    TIM2->SR = 0;
    TIM2->DIER = 1;                             /* UIE */
    ICPR0 = 1u << TIM2_IRQn;
    ISER0 = 1u << TIM2_IRQn;
    SYST_RVR = 72000 - 1;
    SYST_CVR = 0;
    SYST_CSR = 7;                               /* HCLK, TICKINT, ENABLE */
    TIM2->CR1 = 1;                              /* CEN */
    while (tim_irqs < 6 && ticks < 20)         /* QEMU's TIM2 never fires */
        __asm__ volatile("wfi");
    TIM2->CR1 = 0;
    SYST_CSR = 0;
    M("TIM2.irqs", tim_irqs);
    M("SysTick.ticks-in-6ms", ticks >= 5 && ticks <= 6);
    /* HCLK / 2: 36 MHz, and TIM2's clock with it: its 1000 counts take 2 ms */
    RCC->CFGR |= 8u << 4;                       /* HPRE: /2 */
    RCC->CFGR &= ~(7u << 8);                    /* APB1: /1 */
    USART1->BRR = 0x138;                        /* 36 MHz / 115200 */
    TIM2->CR1 = 1;
    ticks = 0;
    SYST_CVR = 0;
    SYST_CSR = 7;                               /* the bound on QEMU */
    while (tim_irqs < 8 && ticks < 20)
        __asm__ volatile("wfi");
    TIM2->CR1 = 0;
    SYST_CSR = 0;
    M("TIM2.irqs-36MHz", tim_irqs);

    /* ---- the console's input (--input), a line -------------------------- */
    puts_("rx '");
    for (int i = 0; i < 100000; i++) {
        if (!(USART1->SR & (1u << 5)))          /* RXNE */
            continue;
        char c = (char)USART1->DR;
        if (c == '\n')
            break;
        if (c >= ' ')                           /* QEMU's sets RXNE with SR */
            putc_(c);
        i = 0;
    }
    puts_("'\n");

    /* ---- where no register is -------------------------------------------- */
    SHCSR |= 1u << 17;                          /* BUSFAULTENA */
    store(0x40024000u, 1);                      /* reserved in the F1's map */
    Q("reserved.faults", faults);
    Q("reserved.BFAR", fault_addr);
    Q("reserved.CFSR", fault_status);
    ICPR0 = ICPR1 = 0xffffffffu;
    puts_("done\n");
    for (;;)
        ;
}
