/* Where features.svd puts every kind of member, worked out by hand from
 * the SVD and the CMSIS-SVD specification -- not from embsvd's output.
 * Compiled against the header embsvd writes, by EmbCC and by clang, for a
 * Cortex-M: a member in the wrong place, of the wrong size, or missing
 * fails the build. */
#include <stddef.h>
#include "features.h"

#define AT(type, member, off) \
    _Static_assert(offsetof(type, member) == (off), #type "." #member)
#define SIZE(x, n) _Static_assert(sizeof(x) == (n), "sizeof " #x)
#define MSIZE(type, member, n) \
    _Static_assert(sizeof(((type *)0)->member) == (n), "sizeof " #member)

/* DMA */
_Static_assert(DMA_BASE == 0x40001000UL, "DMA_BASE");
AT(DMA_Type, CTRL, 0x000);
AT(DMA_Type, STATUS, 0x004);                /* read-only and write-only */
AT(DMA_Type, CLEAR, 0x004);                 /* at one offset: a union */
/* CH[%s], 4 x 0x20 at 0x100: a 16-byte struct padded to its increment */
SIZE(DMA_CH_Type, 0x20);
AT(DMA_Type, CH, 0x100);
AT(DMA_Type, CH[1].DST, 0x124);
AT(DMA_Type, CH[2].CNT, 0x148);
MSIZE(DMA_Type, CH[2].CNT, 2);
AT(DMA_Type, CH[3].CFG, 0x16c);
MSIZE(DMA_Type, CH, 0x80);
/* DATA[%s], packed: a C array */
AT(DMA_Type, DATA[3], 0x20c);
MSIZE(DMA_Type, DATA, 16);
/* PRIO[%s], 4 bytes every 8: one member per element */
AT(DMA_Type, PRIO0, 0x210);
AT(DMA_Type, PRIO1, 0x218);
AT(DMA_Type, PRIO2, 0x220);
/* GPIO%s_CTRL with dimIndex A,B,C */
AT(DMA_Type, GPIOA_CTRL, 0x230);
AT(DMA_Type, GPIOB_CTRL, 0x234);
AT(DMA_Type, GPIOC_CTRL, 0x238);
/* IRQ%s with dimIndex 8-15 */
AT(DMA_Type, IRQ8, 0x240);
AT(DMA_Type, IRQ12, 0x250);
AT(DMA_Type, IRQ15, 0x25c);
/* MODE%s with dimIndex A-D, 16 bits each */
AT(DMA_Type, MODEA, 0x260);
AT(DMA_Type, MODED, 0x266);
MSIZE(DMA_Type, MODEC, 2);
/* derivedFrom: CTRL2 is CTRL, fields and all; DATAX is one register */
AT(DMA_Type, CTRL2, 0x270);
AT(DMA_Type, DATAX, 0x280);
MSIZE(DMA_Type, DATAX, 4);
SIZE(DMA_Type, 0x284);
_Static_assert(DMA_CTRL_EN_Msk == 0x1UL, "CTRL.EN");
_Static_assert(DMA_CTRL_MODE_Msk == 0x6UL, "CTRL.MODE");
_Static_assert(DMA_CTRL_PRI_Msk == 0x70UL, "CTRL.PRI");
_Static_assert(DMA_CTRL_EN2_Msk == 0x100UL, "CTRL.EN2 derivedFrom EN");
_Static_assert(DMA_CTRL_CH0_IE_Pos == 16 && DMA_CTRL_CH3_IE_Pos == 19, "CH%s_IE");
_Static_assert(DMA_CTRL2_EN2_Msk == 0x100UL, "CTRL2 has CTRL's fields");
_Static_assert(DMA_CTRL2_CH3_IE_Msk == (1UL << 19), "CTRL2.CH3_IE");

/* NET: its headerStructName is NETIF */
_Static_assert(NET_BASE == 0x40002000UL, "NET_BASE");
AT(NETIF_Type, ID, 0x000);
/* PORT[%s], 2 x 0x100 at 0x100 */
SIZE(NETIF_PORT_Type, 0x100);
AT(NETIF_Type, PORT[0].CFG, 0x100);
AT(NETIF_Type, PORT[1].CFG, 0x200);
/* QUEUE[%s], 4 x 0x20 at PORT + 0x10; a 0x18-byte struct padded */
SIZE(NET_QUEUE_Type, 0x20);
AT(NETIF_Type, PORT[0].QUEUE[0].HEAD, 0x110);
AT(NETIF_Type, PORT[1].QUEUE[3].TAIL, 0x274);
/* DESC[%s], 2 x 8 at QUEUE + 8 */
SIZE(NET_QUEUE_DESC_Type, 8);
AT(NETIF_Type, PORT[0].QUEUE[0].DESC[0].ADDR, 0x118);
AT(NETIF_Type, PORT[1].QUEUE[2].DESC[1].FLAGS, 0x266);
MSIZE(NETIF_Type, PORT[1].QUEUE[2].DESC[1].LEN, 2);
/* STAT at PORT + 0x90: 16-bit read-only registers, from the cluster */
SIZE(NETIF_PORT_STAT_Type, 4);
AT(NETIF_Type, PORT[0].STAT.TX, 0x192);
AT(NETIF_Type, PORT[1].STAT.RX, 0x290);
MSIZE(NETIF_Type, PORT[0].STAT.TX, 2);
/* LANE%s with dimIndex L,R, 0x10 apart: one member per element */
SIZE(NETIF_LANE_Type, 8);
AT(NETIF_Type, LANEL, 0x400);
AT(NETIF_Type, LANER.LVL, 0x414);
/* SPARE derivedFrom LANE: one, at its own offset, LANE's registers */
AT(NETIF_Type, SPARE.EN, 0x500);
AT(NETIF_Type, SPARE.LVL, 0x504);
MSIZE(NETIF_Type, SPARE, 8);
/* MODE_B is MODE_A's alternateCluster: a union */
AT(NETIF_Type, MODE_A.REGB, 0x604);
AT(NETIF_Type, MODE_B.Z, 0x608);
AT(NETIF_Type, AFTER, 0x610);
SIZE(NETIF_Type, 0x614);
_Static_assert(NETIF_ID_VENDOR_Msk == 0x300UL, "ID.VENDOR derivedFrom DMA.CTRL.MODE");
_Static_assert(NET_QUEUE_DESC_LEN_N_Msk == 0xfffUL, "DESC.LEN.N");

/* NET2 derivedFrom NET */
_Static_assert(NET2_BASE == 0x40003000UL, "NET2_BASE");

/* TIMER%s: three peripherals 0x400 apart, one struct */
_Static_assert(TIMER0_BASE == 0x40004000UL, "TIMER0_BASE");
_Static_assert(TIMER1_BASE == 0x40004400UL, "TIMER1_BASE");
_Static_assert(TIMER2_BASE == 0x40004800UL, "TIMER2_BASE");
AT(TIMER_Type, VAL, 0x04);
SIZE(TIMER_CC_Type, 4);
AT(TIMER_Type, CC[3].VALUE, 0x1c);
/* COPY derivedFrom NET.PORT.CFG; LANEX derivedFrom NET.LANE */
AT(TIMER_Type, COPY, 0x20);
_Static_assert(TIMER_COPY_SPEED_Msk == 0x3UL, "COPY has CFG's fields");
AT(TIMER_Type, LANEX.LVL, 0x34);
SIZE(TIMER_Type, 0x38);

/* and the instances are usable */
unsigned read_all(void)
{
    return DMA->CH[2].CFG + DMA->GPIOB_CTRL + NET->PORT[1].QUEUE[2].DESC[1].ADDR +
           NET2->LANER.LVL + NET->MODE_B.Z + TIMER2->CC[3].VALUE +
           TIMER1->LANEX.EN + DMA->STATUS;
}
