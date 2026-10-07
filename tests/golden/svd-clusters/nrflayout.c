/* Where nrfshape.svd puts its registers, worked out by hand from the SVD:
 * Nordic's shape of header -- NRF_-prefixed peripheral structs and
 * instances, clusters named by their headerStructName -- checked by EmbCC
 * and by clang for a Cortex-M33. */
#include <stddef.h>
#include "nrfshape.h"

#define AT(type, member, off) \
    _Static_assert(offsetof(type, member) == (off), #type "." #member)
#define SIZE(x, n) _Static_assert(sizeof(x) == (n), "sizeof " #x)

/* three clusters deep, each an array, around a register array */
SIZE(CACHEDATA_SET_WAY_DU_Type, 8);
SIZE(CACHEDATA_SET_WAY_Type, 0x20);
SIZE(CACHEDATA_SET_Type, 0x40);
AT(NRF_CACHEDATA_Type, SET[3].WAY[1].DU[2].DATA[1], 0xf4);
SIZE(NRF_CACHEDATA_Type, 0x100);
_Static_assert(NRF_ICACHEDATA_S_BASE == 0x12F00000UL, "ICACHEDATA_S");

/* SPU: a 64-element cluster array of one register, and FEATURE with a
 * cluster array and an alternate cluster at one offset */
AT(NRF_SPU_Type, EVENTS_PERIPHACCERR, 0x100);
SIZE(SPU_PERIPH_Type, 4);
AT(NRF_SPU_Type, PERIPH[63].PERM, 0x5fc);
AT(NRF_SPU_Type, FEATURE.DPPIC.CH[23], 0x6dc);
AT(NRF_SPU_Type, FEATURE.DPPIC.CHG[7], 0x6fc);
AT(NRF_SPU_Type, FEATURE.GPIO[1].PIN[31], 0x7fc);
AT(NRF_SPU_Type, FEATURE.CRACEN.CRACENCORE, 0x704);
SIZE(SPU_FEATURE_GPIO_Type, 0x80);
SIZE(CRACEN_Type, 8);
SIZE(SPU_FEATURE_Type, 0x200);

/* the cluster CRACEN_Type and the peripheral NRF_CRACEN_Type */
AT(NRF_CRACEN_Type, ENABLE, 0x400);

/* DPPIC: task clusters, one named by headerStructName, one not */
AT(NRF_DPPIC_Type, TASKS_CHG[5].DIS, 0x2c);
AT(NRF_DPPIC_Type, SUBSCRIBE_CHG[2].EN, 0x90);
SIZE(DPPIC_SUBSCRIBE_CHG_Type, 8);
AT(NRF_DPPIC_Type, CHEN, 0x500);
AT(NRF_DPPIC_Type, CHG[5], 0x814);
_Static_assert(NRF_DPPIC00_S_BASE == 0x50042000UL, "DPPIC00_S");

/* UARTE: clusters in clusters, not arrays */
AT(NRF_UARTE_Type, EVENTS_DMA.TX.READY, 0x16c);
AT(NRF_UARTE_Type, ERRORSRC, 0x480);
AT(NRF_UARTE_Type, PSEL.RTS, 0x610);
AT(NRF_UARTE_Type, DMA.RX.PTR, 0x704);
AT(NRF_UARTE_Type, DMA.TX.AMOUNT, 0x744);
_Static_assert(UARTE_PSEL_TXD_CONNECT_Msk == 0x80000000UL, "PSEL.TXD.CONNECT");
_Static_assert(UARTE_ENABLE_ENABLE_Msk == 0xfUL, "ENABLE.ENABLE");

/* GPIO, and P1_S derivedFrom P1_NS derivedFrom P0_NS */
AT(NRF_GPIO_Type, IN, 0x10);
AT(NRF_GPIO_Type, PIN_CNF[31], 0xfc);
_Static_assert(GPIO_PIN_CNF_PULL_Msk == 0xcUL, "PIN_CNF.PULL");
_Static_assert(SPU_PERIPH_PERM_SECUREMAPPING_Msk == 0x3UL, "PERM.SECUREMAPPING");
_Static_assert(CACHEDATA_SET_WAY_DU_DATA_Data_Msk == 0xffffffffUL, "DATA.Data");
_Static_assert(NRF_P1_S_BASE == 0x500D8200UL, "P1_S");

unsigned touch(void)
{
    NRF_P1_S->PIN_CNF[3] = GPIO_PIN_CNF_PULL_Msk;
    NRF_UARTE00_S->PSEL.TXD = 6;
    NRF_SPU00_S->FEATURE.GPIO[1].PIN[2] = 1;
    return NRF_ICACHEDATA_S->SET[1].WAY[0].DU[3].DATA[1] +
           NRF_DPPIC00_S->TASKS_CHG[2].EN + NRF_GLOBAL_CRACEN_S->ENABLE;
}
