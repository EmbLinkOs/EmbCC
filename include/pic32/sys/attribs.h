/* <sys/attribs.h> -- XC32's attribute shorthands for PIC32 code, as EmbCC
 * gives them. __ISR(v, IPLnSOFT) and the other vector forms name attributes
 * EmbCC refuses by name until it places handlers at their vectors itself:
 * until then a handler's vector stub is written in assembly, in section
 * .vector_N (Harmony's interrupts_a.S does this). The rest are accepted:
 * EmbCC emits MIPS32 code only, so the MIPS16e and microMIPS ones change
 * nothing, and a RAM function is one placed in .ramfunc.
 */
#ifndef _SYS_ATTRIBS_H
#define _SYS_ATTRIBS_H

#define __ISR(v, ...) \
    __attribute__((vector(v), interrupt(__VA_ARGS__), nomips16))
#define __ISR_AT_VECTOR(v, ...) \
    __attribute__((at_vector(v), interrupt(__VA_ARGS__), nomips16))
#define __ISR_SINGLE(...) __attribute__((interrupt(__VA_ARGS__), nomips16))
#define __ISR_SINGLE_AT_VECTOR(...) \
    __attribute__((at_vector(0), interrupt(__VA_ARGS__), nomips16))

#define __mips16 __attribute__((mips16))
#define __nomips16 __attribute__((nomips16))
#define __micromips __attribute__((micromips))
#define __nomicromips __attribute__((nomicromips))
#define __longcall__ __attribute__((long_call))
#define __unique_section__ __attribute__((section(".text.unique")))
#define __ramfunc__ __attribute__((section(".ramfunc"), noinline))
#define __longramfunc__ __attribute__((section(".ramfunc"), long_call, noinline))
#define __persistent __attribute__((section(".persist")))

#endif
