/* <sys/l1cache.h> -- the MIPS32 L1 cache operations, as XC32's header of
 * this name gives them to PIC32MZ code: the `cache` instruction's
 * operation codes, and a line written back or invalidated by address. A
 * DMA transfer does not see the data cache, so a buffer it reads is
 * written back first and one it writes is invalidated before reading.
 */
#ifndef _SYS_L1CACHE_H
#define _SYS_L1CACHE_H

/* the cache instruction's op field: the cache (bits 1:0) and the
 * operation (bits 4:2) */
#define Index_Invalidate_I               0x00
#define Index_Writeback_Inv_D            0x01
#define Index_Load_Tag_I                 0x04
#define Index_Load_Tag_D                 0x05
#define Index_Store_Tag_I                0x08
#define Index_Store_Tag_D                0x09
#define Hit_Invalidate_I                 0x10
#define Hit_Invalidate_D                 0x11
#define Fill_I                           0x14
#define Hit_Writeback_Inv_D              0x15
#define Hit_Writeback_D                  0x19

#if defined(__LANGUAGE_C__) || defined(__LANGUAGE_C_PLUS_PLUS__)
#define _cache_line(op, addr) \
    __asm__ __volatile__("cache %0, 0(%1)" : : "i"(op), "d"(addr) : "memory")
#endif

#endif
