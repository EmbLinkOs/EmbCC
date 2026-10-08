/* Checks the machine code lib/rt/atomic8.h writes out by hand -- the
 * interrupt-mask routines of the targets EmbCC has no inline assembler
 * for -- against the backends' own encoders, so the bytes are derived
 * once and not restated: ColdFire (src/arch/coldfire/emit.c), Xtensa
 * (src/arch/xtensa/emit.c) and RX (src/arch/rx/emit.c). SPARC and
 * PowerPC are checked against llvm-mc by tests/golden/atomic8.sh.
 *
 *   rtblobs        prints each routine as the encoder makes it and as
 *                  atomic8.h has it; exits 1 if any differs
 *
 * Built by tests/golden/atomic8.sh. */
#include <stdio.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/coldfire/emit.h"
#include "../../src/arch/xtensa/emit.h"
#include "../../src/arch/rx/emit.h"
#include "../../lib/rt/atomic8.h"

static int bad;

static void cmp(const char *name, const struct code *c,
                const unsigned char *want, int n)
{
    int same = c->len == n && memcmp(c->p, want, (size_t)n) == 0;
    printf("%-8s encoder:", name);
    for (int k = 0; k < c->len; k++)
        printf(" %02x", c->p[k]);
    printf("\n%-8s header: ", name);
    for (int k = 0; k < n; k++)
        printf(" %02x", want[k]);
    printf("  %s\n", same ? "same" : "DIFFERENT");
    if (!same)
        bad = 1;
}

/* a big-endian halfword list as bytes */
static int be16(const unsigned short *w, int n, unsigned char *out)
{
    for (int k = 0; k < n; k++) {
        out[2 * k] = (unsigned char)(w[k] >> 8);
        out[2 * k + 1] = (unsigned char)w[k];
    }
    return 2 * n;
}

int main(void)
{
    struct code c;
    unsigned char b[64];

    /* ColdFire */
    {
        static const unsigned short off[] = { RT_CF_OFF }, on[] = { RT_CF_ON };
        memset(&c, 0, sizeof c);
        cf_move_from_sr(&c, 0);
        cf_move(&c, 4, cf_dreg(0), cf_dreg(1));
        cf_alu_imm(&c, CF_OR, 0x700, 1);
        cf_move_to_sr(&c, 1);
        cf_alu_imm(&c, CF_AND, 0x700, 0);
        cf_rts(&c);
        cmp("cf.off", &c, b, be16(off, (int)(sizeof off / sizeof off[0]), b));
        memset(&c, 0, sizeof c);
        cf_move_from_sr(&c, 0);
        cf_alu_imm(&c, CF_AND, (long)0xfffff8ffL, 0);
        cf_alu(&c, CF_OR, cf_disp16(CF_SP, 4), 0);
        cf_move_to_sr(&c, 0);
        cf_rts(&c);
        cmp("cf.on", &c, b, be16(on, (int)(sizeof on / sizeof on[0]), b));
    }
    /* Xtensa */
    {
        static const unsigned char off[] = { RT_XT_OFF }, on[] = { RT_XT_ON };
        memset(&c, 0, sizeof c);
        xt_entry(&c, XT_A1, 32);
        xt_rsil(&c, XT_A2, 15);
        xt_retw(&c);
        cmp("xt.off", &c, off, (int)sizeof off);
        memset(&c, 0, sizeof c);
        xt_entry(&c, XT_A1, 32);
        xt_wsr(&c, XT_A2, XT_SR_PS);
        xt_rsync(&c);
        xt_retw(&c);
        cmp("xt.on", &c, on, (int)sizeof on);
    }
    /* RX: PSW is control register 0, r1 the return register */
    {
        static const unsigned char off[] = { RT_RX_OFF }, on[] = { RT_RX_ON };
        memset(&c, 0, sizeof c);
        rx_mvfc(&c, 0, 1);
        rx_clrpsw(&c, 8);
        rx_rts(&c);
        cmp("rx.off", &c, off, (int)sizeof off);
        memset(&c, 0, sizeof c);
        rx_setpsw(&c, 8);
        rx_rts(&c);
        cmp("rx.on", &c, on, (int)sizeof on);
    }
    return bad;
}
