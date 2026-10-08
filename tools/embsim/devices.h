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

#endif
