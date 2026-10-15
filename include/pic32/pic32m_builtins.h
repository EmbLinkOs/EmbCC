/* <pic32m_builtins.h> -- XC32's PIC32 builtins, as EmbCC gives them: each
 * a macro over the instruction it names, so a call to one compiles to that
 * instruction inline as XC32's does. <xc.h> (the device pack's, through
 * <xc-pic32m.h> and <cp0defs.h>) reaches them; the _CP0_GET_STATUS()
 * family is written over the first six.
 *
 * Each coprocessor 0 write is followed by `ehb`, so the next instruction
 * sees it. A read-modify-write (mxc0, bsc0, bcc0, bcsc0) is not atomic
 * against an interrupt that changes the register in between, as XC32's
 * is not either; mask interrupts around one that must be.
 */
#ifndef _PIC32M_BUILTINS_H
#define _PIC32M_BUILTINS_H

/* the register (0..31) and select (0..7) are constants */
#define __builtin_mfc0(rn, sel) __extension__({                         \
    unsigned int __mfc0_v;                                              \
    __asm__ __volatile__("mfc0 %0, $%1, %2"                            \
                         : "=d"(__mfc0_v) : "i"(rn), "i"(sel));       \
    __mfc0_v; })

#define __builtin_mtc0(rn, sel, v) __extension__({                      \
    __asm__ __volatile__("mtc0 %z0, $%1, %2\n\tehb"                    \
                         : : "dJ"((unsigned int)(v)), "i"(rn), "i"(sel) \
                         : "memory");                                   \
    (void)0; })

#define __builtin_mxc0(rn, sel, v) __extension__({                      \
    unsigned int __mxc0_old = __builtin_mfc0(rn, sel);                  \
    __builtin_mtc0(rn, sel, (v));                                       \
    __mxc0_old; })

#define __builtin_bsc0(rn, sel, set) __extension__({                    \
    unsigned int __bsc0_old = __builtin_mfc0(rn, sel);                  \
    __builtin_mtc0(rn, sel, __bsc0_old | (unsigned int)(set));          \
    __bsc0_old; })

#define __builtin_bcc0(rn, sel, clr) __extension__({                    \
    unsigned int __bcc0_old = __builtin_mfc0(rn, sel);                  \
    __builtin_mtc0(rn, sel, __bcc0_old & ~(unsigned int)(clr));         \
    __bcc0_old; })

#define __builtin_bcsc0(rn, sel, clr, set) __extension__({              \
    unsigned int __bcsc0_old = __builtin_mfc0(rn, sel);                 \
    __builtin_mtc0(rn, sel, (__bcsc0_old & ~(unsigned int)(clr)) |      \
                            (unsigned int)(set));                       \
    __bcsc0_old; })

/* di / ei: the Status register as it was */
#define __builtin_disable_interrupts() __extension__({                  \
    unsigned int __di_v;                                                \
    __asm__ __volatile__("di %0\n\tehb" : "=d"(__di_v) : : "memory");  \
    __di_v; })

#define __builtin_enable_interrupts() __extension__({                   \
    unsigned int __ei_v;                                                \
    __asm__ __volatile__("ei %0\n\tehb" : "=d"(__ei_v) : : "memory");  \
    __ei_v; })

/* The interrupt state, Status's IPL and IE bits, to put back later with
 * __builtin_set_isr_state */
#define __builtin_get_isr_state()                                       \
    (__builtin_mfc0(12, 0) & 0x0000FC01u)

#define __builtin_set_isr_state(s) __extension__({                      \
    unsigned int __isr_s = (unsigned int)(s) & 0x0000FC01u;             \
    unsigned int __isr_st = __builtin_disable_interrupts();             \
    __builtin_mtc0(12, 0, (__isr_st & ~0x0000FC01u) | __isr_s);         \
    (void)0; })

#define __builtin_software_breakpoint()                                 \
    __asm__ __volatile__("sdbbp 0" : : : "memory")

#define __builtin_nop() __asm__ __volatile__("nop")

#endif
