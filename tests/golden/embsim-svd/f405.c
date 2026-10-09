/* An STM32F405 driving RCC, GPIO, the USARTs and TIM2, run on QEMU's
 * netduinoplus2 and on EmbSim's stm32f405 board; built from the header,
 * startup and linker script embsvd writes from ST's SVD.
 *
 * Each line is "Q NAME VALUE" or "M NAME VALUE":
 *   Q  QEMU models the register, so EmbSim must print what QEMU prints;
 *   M  QEMU does not model it (RCC and GPIO are unimplemented devices
 *      there, reading 0), or models it otherwise than the reference
 *      manual (RM0090) says, so EmbSim's value is checked against the
 *      manual alone: f405.txt.
 * Output is USART1's, polled: QEMU's first serial port. */
#include "STM32F405.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define ISER0 REG(0xE000E100u)
#define ISER1 REG(0xE000E104u)
#define ICER0 REG(0xE000E180u)
#define ICER1 REG(0xE000E184u)
#define SHCSR REG(0xE000ED24u)
#define CFSR REG(0xE000ED28u)
#define BFAR REG(0xE000ED38u)

static void putc_(char c)
{
    while (!(USART1->SR & (1u << 7)))           /* TXE */
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

static volatile unsigned tim_irqs, usart_irqs, faults;
static volatile uint32_t fault_addr, fault_status;

void TIM2_IRQHandler(void)
{
    if (TIM2->SR & 1u) {
        TIM2->SR = ~1u;                         /* UIF is rc_w0 */
        tim_irqs++;
    }
}

void USART1_IRQHandler(void)
{
    usart_irqs++;
    USART1->CR1 &= ~(1u << 7);                  /* TXEIE off */
}

void bus_fault_c(void)
{
    fault_status = CFSR;
    fault_addr = BFAR;
    CFSR = fault_status;
    faults++;
}

/* past the faulting 16-bit ldr or str, then the C */
__attribute__((naked)) void BusFault_Handler(void)
{
    __asm__ volatile("mrs r0, msp\n\t"
                     "ldr r1, [r0, #24]\n\t"
                     "adds r1, #2\n\t"
                     "str r1, [r0, #24]\n\t"
                     "b bus_fault_c");
}

static uint32_t load(uint32_t a)
{
    register uint32_t r1 __asm__("r1") = a;
    register uint32_t r0 __asm__("r0") = 0;
    __asm__ volatile("ldr r0, [r1]" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void store(uint32_t a, uint32_t v)
{
    register uint32_t r1 __asm__("r1") = a;
    register uint32_t r0 __asm__("r0") = v;
    __asm__ volatile("str r0, [r1]" : : "r"(r0), "r"(r1) : "memory");
}

static void delay(unsigned n)
{
    for (volatile unsigned i = 0; i < n; i++)
        ;
}

/* poll a register for bits, a bounded number of times: 1 if they came */
static int wait_bits(volatile uint32_t *r, uint32_t mask, uint32_t want)
{
    for (int i = 0; i < 10000; i++)
        if ((*r & mask) == want)
            return 1;
    return 0;
}

int main(void)
{
    /* before any clock is on: what reset left */
    uint32_t cr0 = RCC->CR, cfgr0 = RCC->CFGR, ahb1 = RCC->AHB1ENR;
    uint32_t apb1 = RCC->APB1ENR, csr0 = RCC->CSR;
    USART3->BRR = 0x683;                        /* its clock is off */
    uint32_t brr_off = USART3->BRR;

    /* the console */
    RCC->APB2ENR |= 1u << 4;                    /* USART1EN */
    USART1->BRR = 0x8b;
    USART1->CR1 = (1u << 13) | (1u << 3);       /* UE, TE */
    puts_("embsim-svd f405\n");

    /* ---- RCC --------------------------------------------------------- */
    M("RCC.CR.reset", cr0);                     /* HSION HSIRDY HSITRIM=16 */
    M("RCC.CFGR.reset", cfgr0);
    M("RCC.AHB1ENR.reset", ahb1);               /* CCMDATARAMEN */
    M("RCC.APB1ENR.reset", apb1);
    M("RCC.CSR.reset", csr0);                   /* PORRSTF PINRSTF BORRSTF */
    M("USART3.BRR.clock-off", brr_off);
    RCC->CR |= 1u << 16;                        /* HSEON */
    M("RCC.CR.HSERDY", (uint32_t)wait_bits(&RCC->CR, 1u << 17, 1u << 17));
    /* PLL: HSE / 8 * 336 / 2, Q 7 */
    RCC->PLLCFGR = 8u | 336u << 6 | 0u << 16 | 1u << 22 | 7u << 24;
    M("RCC.PLLCFGR", RCC->PLLCFGR);
    RCC->CR |= 1u << 24;                        /* PLLON */
    M("RCC.CR.PLLRDY", (uint32_t)wait_bits(&RCC->CR, 1u << 25, 1u << 25));
    /* APB1 /4, APB2 /2, the system clock from the PLL */
    RCC->CFGR = 5u << 10 | 4u << 13 | 2u;
    M("RCC.CFGR.SWS", (uint32_t)wait_bits(&RCC->CFGR, 3u << 2, 2u << 2));
    M("RCC.CFGR", RCC->CFGR);
    M("RCC.CR", RCC->CR);
    RCC->CR &= ~(1u << 24);                     /* PLLOFF: PLLRDY goes */
    M("RCC.CR.PLLOFF", RCC->CR);
    RCC->CR |= 1u << 24;
    RCC->APB1ENR |= 1u << 18;                   /* USART3EN */
    USART3->BRR = 0x683;
    Q("USART3.BRR.clock-on", USART3->BRR);

    /* ---- GPIO -------------------------------------------------------- */
    RCC->AHB1ENR |= 7u;                         /* GPIOA, GPIOB, GPIOC */
    M("RCC.AHB1ENR", RCC->AHB1ENR);
    M("GPIOA.MODER.reset", GPIOA->MODER);       /* PA13-15: the debug port */
    M("GPIOA.PUPDR.reset", GPIOA->PUPDR);
    M("GPIOA.IDR.reset", GPIOA->IDR);           /* PA13, PA15 pulled up */
    M("GPIOB.MODER.reset", GPIOB->MODER);
    M("GPIOB.IDR.reset", GPIOB->IDR);           /* PB4 pulled up */
    GPIOA->MODER = (GPIOA->MODER & ~(3u << 10)) | 1u << 10;  /* PA5 out */
    GPIOA->BSRR = 1u << 5;
    M("GPIOA.ODR.set5", GPIOA->ODR);
    M("GPIOA.IDR.set5", GPIOA->IDR);
    GPIOA->BSRR = 1u << (16 + 5);
    M("GPIOA.ODR.reset5", GPIOA->ODR);
    GPIOA->BSRR = 1u << 6 | 1u << (16 + 6);     /* both: set wins */
    M("GPIOA.ODR.both6", GPIOA->ODR);
    M("GPIOA.BSRR", GPIOA->BSRR);               /* write-only */
    *(volatile uint16_t *)((uintptr_t)&GPIOA->BSRR + 2) = 1u << 6;
    M("GPIOA.ODR.half-reset6", GPIOA->ODR);
    REG(0x42000000u + (0x40020014u - 0x40000000u) * 32 + 7 * 4) = 1;
    M("GPIOA.ODR.bitband7", GPIOA->ODR);
    *(volatile uint8_t *)((uintptr_t)&GPIOA->ODR + 1) = 0x12;
    M("GPIOA.ODR.byte1", GPIOA->ODR);
    REG(0x40020010u) = 0xffff;                  /* IDR is read-only */
    M("GPIOA.IDR.written", GPIOA->IDR);
    GPIOC->PUPDR = 1u << 26;                    /* PC13 in, pull-up */
    M("GPIOC.IDR.pullup13", GPIOC->IDR);
    GPIOC->PUPDR = 2u << 26;                    /* pull-down */
    M("GPIOC.IDR.pulldown13", GPIOC->IDR);
    /* PC0 open-drain out, pulled up; PC1 analog, pulled up */
    GPIOC->MODER = 1u << 0 | 3u << 2;
    GPIOC->OTYPER = 1u;
    GPIOC->PUPDR = 1u << 0 | 1u << 2;
    GPIOC->ODR = 1u;
    M("GPIOC.IDR.od-released", GPIOC->IDR);
    GPIOC->ODR = 0;
    M("GPIOC.IDR.od-low", GPIOC->IDR);

    /* ---- USART ------------------------------------------------------- */
    RCC->APB1ENR |= 1u << 17;                   /* USART2EN */
    Q("USART2.SR.reset", USART2->SR);           /* TXE TC */
    USART2->BRR = 0x1117;
    Q("USART2.BRR", USART2->BRR);
    USART2->CR1 = (1u << 13) | (1u << 2);       /* UE, RE: no TE */
    Q("USART2.CR1", USART2->CR1);
    USART2->DR = 'X';                           /* TE off: not sent */
    USART1->SR = ~(1u << 6);                    /* TC is rc_w0 */
    uint32_t sr_tc = USART1->SR;
    USART1->SR = 0x3ff;                         /* 1s change nothing */
    uint32_t sr_ones = USART1->SR;
    Q("USART1.SR.TC-cleared", sr_tc);
    M("USART1.SR.ones", sr_ones);
    ISER1 = 1u << (USART1_IRQn - 32);
    USART1->CR1 |= 1u << 7;                     /* TXEIE: TXE is set */
    for (int i = 0; i < 1000 && !usart_irqs; i++)
        ;
    ICER1 = 1u << (USART1_IRQn - 32);
    Q("USART1.TXE-irq", usart_irqs);

    /* ---- TIM2 -------------------------------------------------------- */
    RCC->APB1ENR |= 1u << 0;                    /* TIM2EN */
    Q("TIM2.CR1.reset", TIM2->CR1);
    Q("TIM2.PSC.reset", TIM2->PSC);
    Q("TIM2.ARR.reset", TIM2->ARR);
    TIM2->PSC = 15;
    TIM2->ARR = 999;
    Q("TIM2.PSC", TIM2->PSC);
    Q("TIM2.ARR", TIM2->ARR);
    TIM2->EGR = 1;                              /* UG */
    uint32_t sr_ug = TIM2->SR;
    TIM2->SR = ~1u;
    uint32_t sr_clr = TIM2->SR;
    M("TIM2.SR.UG", sr_ug);                     /* URS 0: UG sets UIF */
    Q("TIM2.SR.cleared", sr_clr);
    M("TIM2.CNT.UG", TIM2->CNT);
    TIM2->CR1 = 1;                              /* CEN */
    uint32_t c0 = TIM2->CNT;
    delay(2000);
    uint32_t c1 = TIM2->CNT;
    TIM2->CR1 = 0;
    uint32_t c2 = TIM2->CNT;
    delay(2000);
    uint32_t c3 = TIM2->CNT;
    Q("TIM2.CNT.counts", c1 != c0);
    M("TIM2.CNT.stopped", c3 == c2);
    /* the update interrupt, three times: PSC 15 and ARR 999 are 16000
     * of the timer's clocks apart. QEMU's TIM2 raised none in seven
     * configurations tried (its alarm is armed by writes to CNT, ARR,
     * PSC and EGR, not by CEN, and is not armed again when it comes
     * before CEN and UIE); so this is checked against the manual */
    TIM2->PSC = 15;
    TIM2->EGR = 1;                              /* PSC is buffered */
    TIM2->SR = 0;
    TIM2->CNT = 0;
    TIM2->DIER = 1;                             /* UIE */
    ISER0 = 1u << TIM2_IRQn;
    TIM2->CR1 = 1;
    for (int i = 0; i < 1000000 && tim_irqs < 3; i++)
        ;
    TIM2->CR1 = 0;
    TIM2->DIER = 0;
    ICER0 = 1u << TIM2_IRQn;
    M("TIM2.irqs", tim_irqs >= 3);
    TIM2->PSC = 0;
    TIM2->EGR = 1;
    TIM2->SR = 0;
    TIM2->CNT = 0;
    TIM2->CR1 = (1u << 3) | 1u;                 /* OPM, CEN */
    M("TIM2.OPM-stops", (uint32_t)wait_bits(&TIM2->CR1, 1u, 0u));
    M("TIM2.OPM.UIF", TIM2->SR & 1u);
    /* the timer's clock against the core's (SysTick): APB1 is /4, so
     * TIM2 runs at twice APB1, half the core's clock */
    TIM2->PSC = 0;
    TIM2->ARR = 0xffffffffu;
    TIM2->EGR = 1;
    TIM2->SR = 0;
    SYST_RVR = 0xffffff;
    SYST_CVR = 0;
    SYST_CSR = 5;                               /* the core's clock */
    TIM2->CR1 = 1;
    uint32_t t0 = TIM2->CNT, s0 = SYST_CVR;
    delay(500);
    uint32_t t1 = TIM2->CNT, s1 = SYST_CVR;
    TIM2->CR1 = 0;
    SYST_CSR = 0;
    uint32_t dt = t1 - t0, ds = s0 - s1;
    M("TIM2.clock-halved", dt * 2 + 8 >= ds && dt * 2 <= ds + 8);

    /* ---- where no register is ---------------------------------------- */
    SHCSR |= 1u << 17;                          /* BUSFAULTENA */
    uint32_t v = load(0x40004440u);             /* USART2 + 0x40 */
    M("hole.read", v);
    M("hole.faults", faults);
    M("hole.BFAR", fault_addr);
    M("hole.CFSR", fault_status);
    faults = 0;
    fault_addr = fault_status = 0;
    store(0x40008000u, 1);                      /* reserved in the map */
    Q("reserved.faults", faults);
    Q("reserved.BFAR", fault_addr);
    Q("reserved.CFSR", fault_status);
    puts_("done\n");
    for (;;)
        ;
}
