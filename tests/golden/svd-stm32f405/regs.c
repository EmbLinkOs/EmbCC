/* The same table of register addresses, compiled once against ST's
 * stm32f4xx.h and once against the header embsvd generates from ST's
 * SVD: the two objects' .rodata must be the same bytes. The instance
 * macros name the same peripherals in both headers, so __typeof__(*P)
 * is ST's USART_TypeDef in one and embsvd's USART6_Type in the other. */
#include HEADER
#include <stddef.h>

#define R(P, REG) (unsigned long)&P->REG
const unsigned long regs[] = {
    R(RCC, CR), R(RCC, PLLCFGR), R(RCC, CFGR), R(RCC, CIR), R(RCC, AHB1RSTR),
    R(RCC, AHB2RSTR), R(RCC, APB1RSTR), R(RCC, APB2RSTR), R(RCC, AHB1ENR),
    R(RCC, AHB2ENR), R(RCC, APB1ENR), R(RCC, APB2ENR), R(RCC, AHB1LPENR),
    R(RCC, BDCR), R(RCC, CSR), R(RCC, SSCGR), R(RCC, PLLI2SCFGR),
    R(GPIOA, MODER), R(GPIOA, OTYPER), R(GPIOA, OSPEEDR), R(GPIOA, PUPDR),
    R(GPIOA, IDR), R(GPIOA, ODR), R(GPIOA, LCKR), R(GPIOD, MODER),
    R(GPIOD, ODR), R(GPIOI, LCKR),
    R(USART1, SR), R(USART1, DR), R(USART1, BRR), R(USART1, CR1),
    R(USART1, CR2), R(USART1, CR3), R(USART1, GTPR), R(USART2, CR1),
    R(USART6, GTPR), R(UART4, CR3),
    R(SPI1, CR1), R(SPI1, CR2), R(SPI1, SR), R(SPI1, DR), R(SPI1, CRCPR),
    R(SPI1, RXCRCR), R(SPI1, TXCRCR), R(SPI1, I2SCFGR), R(SPI1, I2SPR),
    R(SPI3, I2SPR),
    R(I2C1, CR1), R(I2C1, CR2), R(I2C1, OAR1), R(I2C1, OAR2), R(I2C1, DR),
    R(I2C1, SR1), R(I2C1, SR2), R(I2C1, CCR), R(I2C1, TRISE), R(I2C3, TRISE),
    R(TIM1, CR1), R(TIM1, CR2), R(TIM1, SMCR), R(TIM1, DIER), R(TIM1, SR),
    R(TIM1, EGR), R(TIM1, CCER), R(TIM1, CNT), R(TIM1, PSC), R(TIM1, ARR),
    R(TIM1, RCR), R(TIM1, CCR1), R(TIM1, CCR4), R(TIM1, BDTR), R(TIM1, DCR),
    R(TIM1, DMAR),
    R(TIM2, CR1), R(TIM2, CNT), R(TIM2, PSC), R(TIM2, ARR), R(TIM2, CCR1),
    R(TIM2, CCR4), R(TIM2, OR), R(TIM5, OR), R(TIM6, ARR), R(TIM14, CCR1),
    R(ADC1, SR), R(ADC1, CR1), R(ADC1, CR2), R(ADC1, SMPR1), R(ADC1, SMPR2),
    R(ADC1, HTR), R(ADC1, LTR), R(ADC1, SQR1), R(ADC1, SQR3), R(ADC1, JSQR),
    R(ADC1, JDR4), R(ADC1, DR), R(ADC3, DR),
    R(FLASH, ACR), R(FLASH, KEYR), R(FLASH, OPTKEYR), R(FLASH, SR),
    R(FLASH, CR), R(FLASH, OPTCR),
    R(PWR, CR), R(PWR, CSR),
    R(EXTI, IMR), R(EXTI, EMR), R(EXTI, RTSR), R(EXTI, FTSR), R(EXTI, SWIER),
    R(EXTI, PR),
    R(SYSCFG, PMC), R(SYSCFG, CMPCR),
    R(DMA2, LISR), R(DMA2, HISR), R(DMA2, LIFCR), R(DMA2, HIFCR),
    R(DMA1, HIFCR),
    R(IWDG, KR), R(IWDG, PR), R(IWDG, RLR), R(IWDG, SR),
    R(WWDG, CR), R(WWDG, CFR), R(WWDG, SR),
    R(CRC, DR), R(CRC, IDR), R(CRC, CR),
    R(RTC, TR), R(RTC, DR), R(RTC, CR), R(RTC, ISR), R(RTC, PRER),
    R(RTC, WUTR), R(RTC, CALIBR), R(RTC, ALRMAR), R(RTC, ALRMBR), R(RTC, WPR),
    R(RTC, TAFCR), R(RTC, BKP0R), R(RTC, BKP19R),
    R(DAC, CR), R(DAC, SWTRIGR), R(DAC, DHR12R1), R(DAC, DOR1), R(DAC, DOR2),
    R(DAC, SR),
    R(RNG, CR), R(RNG, SR), R(RNG, DR),
};
