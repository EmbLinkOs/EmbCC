/* Every caller-saved register live, for tests/golden/riscv-isr.sh.
 *
 * torture(n, bad) loads a distinct pattern into each register an
 * interrupt handler must save -- ra, t0-t6, a0-a7 and, with an FPU,
 * ft0-ft11 and fa0-fa7 -- and checks every one of them n times over,
 * while the timer interrupts it. A register the handler wrote and did
 * not restore is a set bit in bad[0] (integer, bit = register number)
 * or bad[1] (floating point). The patterns are XLEN wide, and FLEN wide
 * through fmv.d.x at RV64 with D, so a handler that saved half a
 * register is caught too. Only s1-s6 hold the loop's own state, and are
 * saved here as the ABI says.
 *
 * clobber_all() writes junk into the same registers and returns: what a
 * callee is allowed to do, and what an interrupt handler that calls
 * anything has to assume.
 *
 * GENERATED (the pattern per register is mechanical); the floating-point
 * moves are .word encodings, since this assembler does not take F/D
 * mnemonics: fmv.w.x/fmv.x.w are funct7 0x78/0x70, fmv.d.x/fmv.x.d
 * 0x79/0x71, OP-FP 0x53, rd at bit 7 and rs1 at bit 15. */
#define ENC(f7, rd, rs1) .word (((f7) << 25) | ((rs1) << 15) | ((rd) << 7) | 0x53)
#if __riscv_xlen == 64
#define SX sd
#define LX ld
#else
#define SX sw
#define LX lw
#endif
#if defined(__riscv_flen) && __riscv_flen == 64 && __riscv_xlen == 64
#define F_FROM_X(fd, rs) ENC(0x79, fd, rs)
#define X_FROM_F(rd, fs) ENC(0x71, rd, fs)
#elif defined(__riscv_flen)
#define F_FROM_X(fd, rs) ENC(0x78, fd, rs)
#define X_FROM_F(rd, fs) ENC(0x70, rd, fs)
#endif

    .text
    .globl torture
torture:
    addi sp, sp, -64
    SX ra, 0(sp)
    SX s1, 8(sp)
    SX s2, 16(sp)
    SX s3, 24(sp)
    SX s4, 32(sp)
    SX s5, 40(sp)
    SX s6, 48(sp)
    mv s1, a0
    mv s5, a1
    li s3, 0
    li s4, 0

#ifdef __riscv_flen
#if __riscv_xlen == 64
    li t0, 0x3ff0000000000000
#else
    li t0, 0x3f000000
#endif
    F_FROM_X(0, 5)                 /* ft0 */
#if __riscv_xlen == 64
    li t0, 0x3ff1020304050607
#else
    li t0, 0x3f010203
#endif
    F_FROM_X(1, 5)                 /* ft1 */
#if __riscv_xlen == 64
    li t0, 0x3ff20406080a0c0e
#else
    li t0, 0x3f020406
#endif
    F_FROM_X(2, 5)                 /* ft2 */
#if __riscv_xlen == 64
    li t0, 0x3ff306090c0f1215
#else
    li t0, 0x3f030609
#endif
    F_FROM_X(3, 5)                 /* ft3 */
#if __riscv_xlen == 64
    li t0, 0x3ff4080c1014181c
#else
    li t0, 0x3f04080c
#endif
    F_FROM_X(4, 5)                 /* ft4 */
#if __riscv_xlen == 64
    li t0, 0x3ff50a0f14191e23
#else
    li t0, 0x3f050a0f
#endif
    F_FROM_X(5, 5)                 /* ft5 */
#if __riscv_xlen == 64
    li t0, 0x3ff60c12181e242a
#else
    li t0, 0x3f060c12
#endif
    F_FROM_X(6, 5)                 /* ft6 */
#if __riscv_xlen == 64
    li t0, 0x3ff70e151c232a31
#else
    li t0, 0x3f070e15
#endif
    F_FROM_X(7, 5)                 /* ft7 */
#if __riscv_xlen == 64
    li t0, 0x3ffa141e28323c46
#else
    li t0, 0x3f0a141e
#endif
    F_FROM_X(10, 5)                 /* fa0 */
#if __riscv_xlen == 64
    li t0, 0x3ffb16212c37424d
#else
    li t0, 0x3f0b1621
#endif
    F_FROM_X(11, 5)                 /* fa1 */
#if __riscv_xlen == 64
    li t0, 0x3ffc1824303c4854
#else
    li t0, 0x3f0c1824
#endif
    F_FROM_X(12, 5)                 /* fa2 */
#if __riscv_xlen == 64
    li t0, 0x3ffd1a2734414e5b
#else
    li t0, 0x3f0d1a27
#endif
    F_FROM_X(13, 5)                 /* fa3 */
#if __riscv_xlen == 64
    li t0, 0x3ffe1c2a38465462
#else
    li t0, 0x3f0e1c2a
#endif
    F_FROM_X(14, 5)                 /* fa4 */
#if __riscv_xlen == 64
    li t0, 0x3fff1e2d3c4b5a69
#else
    li t0, 0x3f0f1e2d
#endif
    F_FROM_X(15, 5)                 /* fa5 */
#if __riscv_xlen == 64
    li t0, 0x4000203040506070
#else
    li t0, 0x3f102030
#endif
    F_FROM_X(16, 5)                 /* fa6 */
#if __riscv_xlen == 64
    li t0, 0x4001223344556677
#else
    li t0, 0x3f112233
#endif
    F_FROM_X(17, 5)                 /* fa7 */
#if __riscv_xlen == 64
    li t0, 0x400c3854708ca8c4
#else
    li t0, 0x3f1c3854
#endif
    F_FROM_X(28, 5)                 /* ft8 */
#if __riscv_xlen == 64
    li t0, 0x400d3a577491aecb
#else
    li t0, 0x3f1d3a57
#endif
    F_FROM_X(29, 5)                 /* ft9 */
#if __riscv_xlen == 64
    li t0, 0x400e3c5a7896b4d2
#else
    li t0, 0x3f1e3c5a
#endif
    F_FROM_X(30, 5)                 /* ft10 */
#if __riscv_xlen == 64
    li t0, 0x400f3e5d7c9bbad9
#else
    li t0, 0x3f1f3e5d
#endif
    F_FROM_X(31, 5)                 /* ft11 */
#endif
#if __riscv_xlen == 64
    li ra, 0x1101010b01010101
#else
    li ra, 0x51010101
#endif
#if __riscv_xlen == 64
    li t0, 0x1505050f05050505
#else
    li t0, 0x55050505
#endif
#if __riscv_xlen == 64
    li t1, 0x1606061006060606
#else
    li t1, 0x56060606
#endif
#if __riscv_xlen == 64
    li t2, 0x1707071107070707
#else
    li t2, 0x57070707
#endif
#if __riscv_xlen == 64
    li a0, 0x1a0a0a140a0a0a0a
#else
    li a0, 0x5a0a0a0a
#endif
#if __riscv_xlen == 64
    li a1, 0x1b0b0b150b0b0b0b
#else
    li a1, 0x5b0b0b0b
#endif
#if __riscv_xlen == 64
    li a2, 0x1c0c0c160c0c0c0c
#else
    li a2, 0x5c0c0c0c
#endif
#if __riscv_xlen == 64
    li a3, 0x1d0d0d170d0d0d0d
#else
    li a3, 0x5d0d0d0d
#endif
#if __riscv_xlen == 64
    li a4, 0x1e0e0e180e0e0e0e
#else
    li a4, 0x5e0e0e0e
#endif
#if __riscv_xlen == 64
    li a5, 0x1f0f0f190f0f0f0f
#else
    li a5, 0x5f0f0f0f
#endif
#if __riscv_xlen == 64
    li a6, 0x2010101a10101010
#else
    li a6, 0x60101010
#endif
#if __riscv_xlen == 64
    li a7, 0x2111111b11111111
#else
    li a7, 0x61111111
#endif
#if __riscv_xlen == 64
    li t3, 0x2c1c1c261c1c1c1c
#else
    li t3, 0x6c1c1c1c
#endif
#if __riscv_xlen == 64
    li t4, 0x2d1d1d271d1d1d1d
#else
    li t4, 0x6d1d1d1d
#endif
#if __riscv_xlen == 64
    li t5, 0x2e1e1e281e1e1e1e
#else
    li t5, 0x6e1e1e1e
#endif
#if __riscv_xlen == 64
    li t6, 0x2f1f1f291f1f1f1f
#else
    li t6, 0x6f1f1f1f
#endif
1:
#if __riscv_xlen == 64
    li s2, 0x1101010b01010101
#else
    li s2, 0x51010101
#endif
    beq ra, s2, 2f
    li s2, 2
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1505050f05050505
#else
    li s2, 0x55050505
#endif
    beq t0, s2, 2f
    li s2, 32
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1606061006060606
#else
    li s2, 0x56060606
#endif
    beq t1, s2, 2f
    li s2, 64
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1707071107070707
#else
    li s2, 0x57070707
#endif
    beq t2, s2, 2f
    li s2, 128
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1a0a0a140a0a0a0a
#else
    li s2, 0x5a0a0a0a
#endif
    beq a0, s2, 2f
    li s2, 1024
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1b0b0b150b0b0b0b
#else
    li s2, 0x5b0b0b0b
#endif
    beq a1, s2, 2f
    li s2, 2048
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1c0c0c160c0c0c0c
#else
    li s2, 0x5c0c0c0c
#endif
    beq a2, s2, 2f
    li s2, 4096
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1d0d0d170d0d0d0d
#else
    li s2, 0x5d0d0d0d
#endif
    beq a3, s2, 2f
    li s2, 8192
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1e0e0e180e0e0e0e
#else
    li s2, 0x5e0e0e0e
#endif
    beq a4, s2, 2f
    li s2, 16384
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x1f0f0f190f0f0f0f
#else
    li s2, 0x5f0f0f0f
#endif
    beq a5, s2, 2f
    li s2, 32768
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x2010101a10101010
#else
    li s2, 0x60101010
#endif
    beq a6, s2, 2f
    li s2, 65536
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x2111111b11111111
#else
    li s2, 0x61111111
#endif
    beq a7, s2, 2f
    li s2, 131072
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x2c1c1c261c1c1c1c
#else
    li s2, 0x6c1c1c1c
#endif
    beq t3, s2, 2f
    li s2, 268435456
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x2d1d1d271d1d1d1d
#else
    li s2, 0x6d1d1d1d
#endif
    beq t4, s2, 2f
    li s2, 536870912
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x2e1e1e281e1e1e1e
#else
    li s2, 0x6e1e1e1e
#endif
    beq t5, s2, 2f
    li s2, 1073741824
    or s3, s3, s2
2:
#if __riscv_xlen == 64
    li s2, 0x2f1f1f291f1f1f1f
#else
    li s2, 0x6f1f1f1f
#endif
    beq t6, s2, 2f
    li s2, 1
    slli s2, s2, 31
    or s3, s3, s2
2:
#ifdef __riscv_flen
#if __riscv_xlen == 64
    li s6, 0x3ff0000000000000
#else
    li s6, 0x3f000000
#endif
    X_FROM_F(18, 0)                /* s2 = ft0 */
    beq s2, s6, 2f
    li s2, 1
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff1020304050607
#else
    li s6, 0x3f010203
#endif
    X_FROM_F(18, 1)                /* s2 = ft1 */
    beq s2, s6, 2f
    li s2, 2
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff20406080a0c0e
#else
    li s6, 0x3f020406
#endif
    X_FROM_F(18, 2)                /* s2 = ft2 */
    beq s2, s6, 2f
    li s2, 4
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff306090c0f1215
#else
    li s6, 0x3f030609
#endif
    X_FROM_F(18, 3)                /* s2 = ft3 */
    beq s2, s6, 2f
    li s2, 8
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff4080c1014181c
#else
    li s6, 0x3f04080c
#endif
    X_FROM_F(18, 4)                /* s2 = ft4 */
    beq s2, s6, 2f
    li s2, 16
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff50a0f14191e23
#else
    li s6, 0x3f050a0f
#endif
    X_FROM_F(18, 5)                /* s2 = ft5 */
    beq s2, s6, 2f
    li s2, 32
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff60c12181e242a
#else
    li s6, 0x3f060c12
#endif
    X_FROM_F(18, 6)                /* s2 = ft6 */
    beq s2, s6, 2f
    li s2, 64
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ff70e151c232a31
#else
    li s6, 0x3f070e15
#endif
    X_FROM_F(18, 7)                /* s2 = ft7 */
    beq s2, s6, 2f
    li s2, 128
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ffa141e28323c46
#else
    li s6, 0x3f0a141e
#endif
    X_FROM_F(18, 10)                /* s2 = fa0 */
    beq s2, s6, 2f
    li s2, 1024
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ffb16212c37424d
#else
    li s6, 0x3f0b1621
#endif
    X_FROM_F(18, 11)                /* s2 = fa1 */
    beq s2, s6, 2f
    li s2, 2048
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ffc1824303c4854
#else
    li s6, 0x3f0c1824
#endif
    X_FROM_F(18, 12)                /* s2 = fa2 */
    beq s2, s6, 2f
    li s2, 4096
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ffd1a2734414e5b
#else
    li s6, 0x3f0d1a27
#endif
    X_FROM_F(18, 13)                /* s2 = fa3 */
    beq s2, s6, 2f
    li s2, 8192
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3ffe1c2a38465462
#else
    li s6, 0x3f0e1c2a
#endif
    X_FROM_F(18, 14)                /* s2 = fa4 */
    beq s2, s6, 2f
    li s2, 16384
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x3fff1e2d3c4b5a69
#else
    li s6, 0x3f0f1e2d
#endif
    X_FROM_F(18, 15)                /* s2 = fa5 */
    beq s2, s6, 2f
    li s2, 32768
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x4000203040506070
#else
    li s6, 0x3f102030
#endif
    X_FROM_F(18, 16)                /* s2 = fa6 */
    beq s2, s6, 2f
    li s2, 65536
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x4001223344556677
#else
    li s6, 0x3f112233
#endif
    X_FROM_F(18, 17)                /* s2 = fa7 */
    beq s2, s6, 2f
    li s2, 131072
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x400c3854708ca8c4
#else
    li s6, 0x3f1c3854
#endif
    X_FROM_F(18, 28)                /* s2 = ft8 */
    beq s2, s6, 2f
    li s2, 268435456
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x400d3a577491aecb
#else
    li s6, 0x3f1d3a57
#endif
    X_FROM_F(18, 29)                /* s2 = ft9 */
    beq s2, s6, 2f
    li s2, 536870912
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x400e3c5a7896b4d2
#else
    li s6, 0x3f1e3c5a
#endif
    X_FROM_F(18, 30)                /* s2 = ft10 */
    beq s2, s6, 2f
    li s2, 1073741824
    or s4, s4, s2
2:
#if __riscv_xlen == 64
    li s6, 0x400f3e5d7c9bbad9
#else
    li s6, 0x3f1f3e5d
#endif
    X_FROM_F(18, 31)                /* s2 = ft11 */
    beq s2, s6, 2f
    li s2, 1
    slli s2, s2, 31
    or s4, s4, s2
2:
#endif
    addi s1, s1, -1
    bnez s1, 1b
    SX s3, 0(s5)
    SX s4, (__riscv_xlen / 8)(s5)
    LX ra, 0(sp)
    LX s1, 8(sp)
    LX s2, 16(sp)
    LX s3, 24(sp)
    LX s4, 32(sp)
    LX s5, 40(sp)
    LX s6, 48(sp)
    addi sp, sp, 64
    ret

    .globl clobber_all
clobber_all:
    li t0, -1
    li t1, -1
    li t2, -1
    li a0, -1
    li a1, -1
    li a2, -1
    li a3, -1
    li a4, -1
    li a5, -1
    li a6, -1
    li a7, -1
    li t3, -1
    li t4, -1
    li t5, -1
    li t6, -1
#ifdef __riscv_flen
    F_FROM_X(0, 5)                 /* ft0 = junk */
    F_FROM_X(1, 5)                 /* ft1 = junk */
    F_FROM_X(2, 5)                 /* ft2 = junk */
    F_FROM_X(3, 5)                 /* ft3 = junk */
    F_FROM_X(4, 5)                 /* ft4 = junk */
    F_FROM_X(5, 5)                 /* ft5 = junk */
    F_FROM_X(6, 5)                 /* ft6 = junk */
    F_FROM_X(7, 5)                 /* ft7 = junk */
    F_FROM_X(10, 5)                 /* fa0 = junk */
    F_FROM_X(11, 5)                 /* fa1 = junk */
    F_FROM_X(12, 5)                 /* fa2 = junk */
    F_FROM_X(13, 5)                 /* fa3 = junk */
    F_FROM_X(14, 5)                 /* fa4 = junk */
    F_FROM_X(15, 5)                 /* fa5 = junk */
    F_FROM_X(16, 5)                 /* fa6 = junk */
    F_FROM_X(17, 5)                 /* fa7 = junk */
    F_FROM_X(28, 5)                 /* ft8 = junk */
    F_FROM_X(29, 5)                 /* ft9 = junk */
    F_FROM_X(30, 5)                 /* ft10 = junk */
    F_FROM_X(31, 5)                 /* ft11 = junk */
#endif
    ret
