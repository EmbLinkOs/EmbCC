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

/* the STM32's peripheral models, over its SVD's registers (svd-map.h) */
struct rf_periph;
extern const struct dev_ops stm32_rcc_ops, stm32_gpio_ops, stm32_usart_ops;
extern const struct dev_ops stm32_tim_ops;
void *stm32_rcc_create(struct sim *s, struct rf_periph *p);
void *stm32_gpio_create(struct sim *s, struct rf_periph *p);
void *stm32_usart_create(struct sim *s, struct rf_periph *p);
void *stm32_tim_create(struct sim *s, struct rf_periph *p);

#endif
