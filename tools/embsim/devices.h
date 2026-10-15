/* devices.h -- the cores and devices boards.c can build a board from.
 * Each device is a struct dev_ops and a create function returning its
 * context; each core a create function (struct core_type). */
#ifndef EMBSIM_DEVICES_H
#define EMBSIM_DEVICES_H

#include "sim.h"

/* cores */
struct cpu *cortexm_create(struct sim *s, const char *model,
                           const struct board_desc *bd);
struct cpu *riscv_create(struct sim *s, const char *model,
                         const struct board_desc *bd);
int riscv_is(const struct cpu *c);
struct cpu *avr_create(struct sim *s, const char *model,
                       const struct board_desc *bd);
int avr_is(const struct cpu *c);
struct cpu *mips_create(struct sim *s, const char *model,
                        const struct board_desc *bd);
struct cpu *pic24_create(struct sim *s, const char *model,
                         const struct board_desc *bd);

/* devices */
extern const struct dev_ops pl011_ops, cmsdk_uart_ops, nrf51_uart_ops;
extern const struct dev_ops systick_ops, scs_ops, dwt_ops, zero_ops;
void *pl011_create(struct sim *s, const struct dev_desc *d);
void *cmsdk_uart_create(struct sim *s, const struct dev_desc *d);
void *nrf51_uart_create(struct sim *s, const struct dev_desc *d);
void *systick_create(struct sim *s, const struct dev_desc *d);
void *scs_create(struct sim *s, const struct dev_desc *d);
void *dwt_create(struct sim *s, const struct dev_desc *d);
extern const struct dev_ops clint_ops, ns16550a_ops, sifive_test_ops;
extern const struct dev_ops virt_rom_ops;
void *clint_create(struct sim *s, const struct dev_desc *d);
void *ns16550a_create(struct sim *s, const struct dev_desc *d);
void *sifive_test_create(struct sim *s, const struct dev_desc *d);
void *virt_rom_create(struct sim *s, const struct dev_desc *d);
extern const struct dev_ops avr_io_ops, avr_usart_ops, avr_timer16_ops;
void *avr_io_create(struct sim *s, const struct dev_desc *d);
void *avr_usart_create(struct sim *s, const struct dev_desc *d);
void *avr_timer16_create(struct sim *s, const struct dev_desc *d);
extern const struct dev_ops ns16550a_8_ops, malta_fpga_ops, malta_rom_ops;
void *malta_fpga_create(struct sim *s, const struct dev_desc *d);
void *malta_rom_create(struct sim *s, const struct dev_desc *d);

/* the STM32's peripheral models, over its SVD's registers (svd-map.h) */
struct rf_periph;
extern const struct dev_ops stm32_rcc_ops, stm32_gpio_ops, stm32_usart_ops;
extern const struct dev_ops stm32_tim_ops;
void *stm32_rcc_create(struct sim *s, struct rf_periph *p);
void *stm32_gpio_create(struct sim *s, struct rf_periph *p);
void *stm32_usart_create(struct sim *s, struct rf_periph *p);
void *stm32_tim_create(struct sim *s, struct rf_periph *p);

/* a PIC32's peripheral models, over its ATDF's registers (pic32.h) */
extern const struct dev_ops pic32_evic_ops, pic32_cfg_ops, pic32_cru_ops;
extern const struct dev_ops pic32_tmr_ops, pic32_gpio_ops, pic32_uart_ops;
extern const struct dev_ops pic32_pmp_ops, pic32_adc_ops;
void *pic32_evic_create(struct sim *s, struct rf_periph *p);
void *pic32_cfg_create(struct sim *s, struct rf_periph *p);
void *pic32_cru_create(struct sim *s, struct rf_periph *p);
void *pic32_tmr_create(struct sim *s, struct rf_periph *p);
void *pic32_gpio_create(struct sim *s, struct rf_periph *p);
void *pic32_uart_create(struct sim *s, struct rf_periph *p);
void *pic32_pmp_create(struct sim *s, struct rf_periph *p);
void *pic32_adc_create(struct sim *s, struct rf_periph *p);

/* a PIC24's peripheral models, over its EDC's registers (pic24.h) */
extern const struct dev_ops pic24_intc_ops, pic24_adc_ops, pic24_epmp_ops;
extern const struct dev_ops pic24_spi_ops, pic24_uart_ops, pic24_tmr_ops;
extern const struct dev_ops pic24_osc_ops, pic24_pps_ops, pic24_nvm_ops;
extern const struct dev_ops pic24_rtcc_ops;
void *pic24_intc_create(struct sim *s, struct rf_periph *p);
void *pic24_uart_create(struct sim *s, struct rf_periph *p);
void *pic24_tmr_create(struct sim *s, struct rf_periph *p);
void *pic24_osc_create(struct sim *s, struct rf_periph *p);
void *pic24_pps_create(struct sim *s, struct rf_periph *p);
void *pic24_adc_create(struct sim *s, struct rf_periph *p);
void *pic24_epmp_create(struct sim *s, struct rf_periph *p);
void *pic24_spi_create(struct sim *s, struct rf_periph *p);
void *pic24_nvm_create(struct sim *s, struct rf_periph *p);
void *pic24_rtcc_create(struct sim *s, struct rf_periph *p);
/* the hardware sets interrupt irq's flag (IFSx), as the EDC numbers it;
 * the flag; an interrupt's number by the EDC's name (T1, ADC1), or -1 */
void pic24_irq(struct sim *s, int irq);
int pic24_irq_flag(struct sim *s, int irq);
int pic24_irq_enabled(struct sim *s, int irq);
int pic24_irq_named(struct sim *s, const char *name);
/* fn told of each interrupt event as it happens, whatever its flag (the
 * DMA's triggers): one listener */
void pic24_irq_listen(struct sim *s, void (*fn)(void *ctx, int irq), void *ctx);
extern const struct dev_ops pic24_dma_ops;
void *pic24_dma_create(struct sim *s, struct rf_periph *p);
extern const struct dev_ops pic24_i2c_ops;
void *pic24_i2c_create(struct sim *s, struct rf_periph *p);

/* the PIC32 and PIC24 boards' own: LEDs, buttons, the Explorer 16/32's
 * sensors, LCD and EEPROM (a dev_desc's type names the board) */
extern const struct dev_ops pic_board_ops;
void *pic_board_create(struct sim *s, const struct dev_desc *d);

#endif
