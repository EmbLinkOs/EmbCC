/* Every caller-saved register live, for tests/golden/mips-isr.sh.
 *
 * torture(n, bad) loads a distinct pattern into each register a MIPS32
 * interrupt handler must save -- at, v0-v1, a0-a3, t0-t9, ra, and HI and
 * LO -- and checks every one of them n times over while the timer
 * interrupts it. A register that changed is a set bit in bad[0] (bit =
 * register number; bit 0 for HI, which $0 cannot use) or in bad[1] (bit 0
 * LO). Only s0-s6 hold the loop's own state, saved here as o32 says.
 *
 * clobber_all() writes junk into the same registers, HI and LO: what a
 * callee may do, and what a handler that calls anything must assume.
 *
 * GENERATED: the pattern per register is mechanical. In `.set reorder`
 * mode (GNU as's default), so the assembler fills each delay slot, and
 * `noat`: $at is one of the registers under test. */
    .text
    .set noat

    .globl torture
    .ent torture
torture:
    addiu   $sp, $sp, -40
    sw      $ra, 36($sp)
    sw      $s0, 32($sp)
    sw      $s1, 28($sp)
    sw      $s2, 24($sp)
    sw      $s3, 20($sp)
    sw      $s4, 16($sp)
    sw      $s5, 12($sp)
    sw      $s6, 8($sp)
    move    $s0, $a0
    move    $s5, $a1
    li      $s3, 0
    li      $s4, 0
    li      $s2, 0x48484848
    mthi    $s2
    li      $s2, 0x4c4c4c4c
    mtlo    $s2

    li      $1, 0x51010101
    li      $2, 0x52020202
    li      $3, 0x53030303
    li      $4, 0x54040404
    li      $5, 0x55050505
    li      $6, 0x56060606
    li      $7, 0x57070707
    li      $8, 0x58080808
    li      $9, 0x59090909
    li      $10, 0x5a0a0a0a
    li      $11, 0x5b0b0b0b
    li      $12, 0x5c0c0c0c
    li      $13, 0x5d0d0d0d
    li      $14, 0x5e0e0e0e
    li      $15, 0x5f0f0f0f
    li      $24, 0x68181818
    li      $25, 0x69191919
    li      $31, 0x6f1f1f1f
1:
    li      $s2, 0x51010101
    beq     $1, $s2, 2f
    li      $s2, 0x2
    or      $s3, $s3, $s2
2:
    li      $s2, 0x52020202
    beq     $2, $s2, 2f
    li      $s2, 0x4
    or      $s3, $s3, $s2
2:
    li      $s2, 0x53030303
    beq     $3, $s2, 2f
    li      $s2, 0x8
    or      $s3, $s3, $s2
2:
    li      $s2, 0x54040404
    beq     $4, $s2, 2f
    li      $s2, 0x10
    or      $s3, $s3, $s2
2:
    li      $s2, 0x55050505
    beq     $5, $s2, 2f
    li      $s2, 0x20
    or      $s3, $s3, $s2
2:
    li      $s2, 0x56060606
    beq     $6, $s2, 2f
    li      $s2, 0x40
    or      $s3, $s3, $s2
2:
    li      $s2, 0x57070707
    beq     $7, $s2, 2f
    li      $s2, 0x80
    or      $s3, $s3, $s2
2:
    li      $s2, 0x58080808
    beq     $8, $s2, 2f
    li      $s2, 0x100
    or      $s3, $s3, $s2
2:
    li      $s2, 0x59090909
    beq     $9, $s2, 2f
    li      $s2, 0x200
    or      $s3, $s3, $s2
2:
    li      $s2, 0x5a0a0a0a
    beq     $10, $s2, 2f
    li      $s2, 0x400
    or      $s3, $s3, $s2
2:
    li      $s2, 0x5b0b0b0b
    beq     $11, $s2, 2f
    li      $s2, 0x800
    or      $s3, $s3, $s2
2:
    li      $s2, 0x5c0c0c0c
    beq     $12, $s2, 2f
    li      $s2, 0x1000
    or      $s3, $s3, $s2
2:
    li      $s2, 0x5d0d0d0d
    beq     $13, $s2, 2f
    li      $s2, 0x2000
    or      $s3, $s3, $s2
2:
    li      $s2, 0x5e0e0e0e
    beq     $14, $s2, 2f
    li      $s2, 0x4000
    or      $s3, $s3, $s2
2:
    li      $s2, 0x5f0f0f0f
    beq     $15, $s2, 2f
    li      $s2, 0x8000
    or      $s3, $s3, $s2
2:
    li      $s2, 0x68181818
    beq     $24, $s2, 2f
    li      $s2, 0x1000000
    or      $s3, $s3, $s2
2:
    li      $s2, 0x69191919
    beq     $25, $s2, 2f
    li      $s2, 0x2000000
    or      $s3, $s3, $s2
2:
    li      $s2, 0x6f1f1f1f
    beq     $31, $s2, 2f
    li      $s2, 0x80000000
    or      $s3, $s3, $s2
2:
    mfhi    $s2
    li      $s6, 0x48484848
    beq     $s2, $s6, 2f
    ori     $s3, $s3, 1
2:
    mflo    $s2
    li      $s6, 0x4c4c4c4c
    beq     $s2, $s6, 2f
    ori     $s4, $s4, 1
2:
    addiu   $s0, $s0, -1
    bne     $s0, $zero, 1b
    sw      $s3, 0($s5)
    sw      $s4, 4($s5)
    lw      $ra, 36($sp)
    lw      $s0, 32($sp)
    lw      $s1, 28($sp)
    lw      $s2, 24($sp)
    lw      $s3, 20($sp)
    lw      $s4, 16($sp)
    lw      $s5, 12($sp)
    lw      $s6, 8($sp)
    addiu   $sp, $sp, 40
    jr      $ra
    .end torture

    .globl clobber_all
    .ent clobber_all
clobber_all:
    li      $1, -1
    li      $2, -1
    li      $3, -1
    li      $4, -1
    li      $5, -1
    li      $6, -1
    li      $7, -1
    li      $8, -1
    li      $9, -1
    li      $10, -1
    li      $11, -1
    li      $12, -1
    li      $13, -1
    li      $14, -1
    li      $15, -1
    li      $24, -1
    li      $25, -1
    mthi    $1
    mtlo    $1
    jr      $ra
    .end clobber_all
