/* The interrupt-mask routines lib/rt/atomic8.c cannot write in C on the
 * targets where EmbCC has no inline assembler yet, as their machine code.
 * Each pair is two leaf functions in the target's own calling convention:
 *
 *   u32 off(void)     interrupts masked; returns the mask bits as they were
 *   void on(u32 m)    the mask bits put back from m (RX: interrupts on)
 *
 * The bytes are NOT the place they are checked: tests/golden/atomic8.sh
 * encodes the same instructions with the backends' own encoders
 * (tools/rtblobs) for ColdFire, Xtensa and RX, and with llvm-mc for SPARC
 * and PowerPC, and compares. Every one is defined for every target here,
 * so that check can see them all. */
#ifndef EMBCC_RT_ATOMIC8_H
#define EMBCC_RT_ATOMIC8_H

/* SPARC V8, big-endian words. off: rd %psr,%o0; or %o0,0xf00,%o1;
 * wr %o1,0,%psr; nop; nop; nop; retl; and %o0,0xf00,%o0.
 * on: rd %psr,%o1; andn %o1,0xf00,%o1; or %o1,%o0,%o1; wr %o1,0,%psr;
 * nop; nop; retl; nop -- three instructions pass before a wr %psr
 * takes effect. */
#define RT_SPARC_OFF 0x91480000u, 0x92122f00u, 0x818a6000u, 0x01000000u, \
                     0x01000000u, 0x01000000u, 0x81c3e008u, 0x900a2f00u
#define RT_SPARC_ON  0x93480000u, 0x922a6f00u, 0x92124008u, 0x818a6000u, \
                     0x01000000u, 0x01000000u, 0x81c3e008u, 0x01000000u

/* PowerPC, big-endian words. off: mfmsr r3; rlwinm r4,r3,0,17,15 (EE
 * clear); mtmsr r4; andi. r3,r3,0x8000; blr.
 * on: mfmsr r4; rlwinm r4,r4,0,17,15; or r4,r4,r3; mtmsr r4; blr. */
#define RT_PPC_OFF 0x7c6000a6u, 0x5464045eu, 0x7c800124u, 0x70638000u, \
                   0x4e800020u
#define RT_PPC_ON  0x7c8000a6u, 0x5484045eu, 0x7c841b78u, 0x7c800124u, \
                   0x4e800020u

/* ColdFire, big-endian halfwords. off: move.w %sr,%d0; move.l %d0,%d1;
 * ori.l #0x700,%d1; move.w %d1,%sr; andi.l #0x700,%d0; rts.
 * on (m at 4(%sp)): move.w %sr,%d0; andi.l #0xfffff8ff,%d0;
 * or.l 4(%sp),%d0; move.w %d0,%sr; rts. */
#define RT_CF_OFF 0x40c0u, 0x2200u, 0x0081u, 0x0000u, 0x0700u, 0x46c1u, \
                  0x0280u, 0x0000u, 0x0700u, 0x4e75u
#define RT_CF_ON  0x40c0u, 0x0280u, 0xffffu, 0xf8ffu, 0x80afu, 0x0004u, \
                  0x46c0u, 0x4e75u

/* Xtensa, windowed ABI, bytes. off: entry a1,32; rsil a2,15; retw.
 * on: entry a1,32; wsr a2,ps; rsync; retw. */
#define RT_XT_OFF 0x36, 0x41, 0x00, 0x20, 0x6f, 0x00, 0x90, 0x00, 0x00
#define RT_XT_ON  0x36, 0x41, 0x00, 0x20, 0xe6, 0x13, 0x10, 0x20, 0x00, \
                  0x90, 0x00, 0x00

/* RX, bytes. off: mvfc psw,r1; clrpsw i; rts (the whole PSW back).
 * on: setpsw i; rts. */
#define RT_RX_OFF 0xfd, 0x6a, 0x01, 0x7f, 0xb8, 0x02
#define RT_RX_ON  0x7f, 0xa8, 0x02

#endif
