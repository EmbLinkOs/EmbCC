/* <sys/kmem.h> -- the MIPS32 kernel segments' address arithmetic, as
 * XC32's header of this name gives it to PIC32 code. KSEG0 (0x80000000)
 * reaches memory through the cache, KSEG1 (0xA0000000) around it, and both
 * are the physical address with its top three bits set: a DMA controller
 * is given the physical address, and a buffer it writes is read through
 * KSEG1 to see what it wrote.
 */
#ifndef _SYS_KMEM_H
#define _SYS_KMEM_H

#define KSEG0_BASE 0x80000000u
#define KSEG1_BASE 0xA0000000u
#define KSEG2_BASE 0xC0000000u

#define _KVA_PA_MASK 0x1FFFFFFFu

#define KVA_TO_PA(v)    ((unsigned long)(v) & _KVA_PA_MASK)
#define PA_TO_KVA0(pa)  ((unsigned long)(pa) | KSEG0_BASE)
#define PA_TO_KVA1(pa)  ((unsigned long)(pa) | KSEG1_BASE)
#define KVA0_TO_KVA1(v) ((unsigned long)(v) | 0x20000000u)
#define KVA1_TO_KVA0(v) ((unsigned long)(v) & ~0x20000000u)

#define IS_KVA(v)   ((int)(v) < 0)
#define IS_KVA0(v)  (((unsigned long)(v) & 0xE0000000u) == KSEG0_BASE)
#define IS_KVA1(v)  (((unsigned long)(v) & 0xE0000000u) == KSEG1_BASE)
#define IS_KVA01(v) (IS_KVA0(v) || IS_KVA1(v))

#endif
