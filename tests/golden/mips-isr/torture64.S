/* Every caller-saved register live, 64 bits wide, for
 * tests/golden/mips-isr.sh on MIPS64 (n64) -- torture.S's twin.
 *
 * torture(n, bad) loads a distinct 64-bit pattern -- its upper half not
 * the sign extension of its lower, so a handler that saved a register's
 * low word alone (sw/lw, or mfc0 for EPC) shows -- into each register an
 * interrupt handler must save -- at, v0-v1, a0-a7, t0-t3, t8, t9, ra,
 * and HI and LO -- and checks every one of them n times over while the
 * timer interrupts it. A register that changed is a set bit in bad[0]
 * (bit = register number; bit 0 for HI) or in bad[1] (bit 0 LO). Only
 * s0-s6 hold the loop's own state, saved here as n64 says.
 *
 * clobber_all() writes junk into the same registers, HI and LO.
 *
 * GENERATED: the pattern per register is mechanical. In `.set reorder`
 * mode, so the assembler fills each delay slot, and `noat`: $at is one of
 * the registers under test. */
    .text
    .set noat

    .globl torture
    .ent torture
torture:
    daddiu  $sp, $sp, -64
    sd      $ra, 56($sp)
    sd      $s0, 48($sp)
    sd      $s1, 40($sp)
    sd      $s2, 32($sp)
    sd      $s3, 24($sp)
    sd      $s4, 16($sp)
    sd      $s5, 8($sp)
    sd      $s6, 0($sp)
    move    $s0, $a0
    move    $s5, $a1
    li      $s3, 0
    li      $s4, 0
    dli     $s2, 0x4848484821212121
    mthi    $s2
    dli     $s2, 0x4c4c4c4c37373737
    mtlo    $s2
    dli     $1, 0x5101a1013101c101
    dli     $2, 0x5202a2023202c202
    dli     $3, 0x5303a3033303c303
    dli     $4, 0x5404a4043404c404
    dli     $5, 0x5505a5053505c505
    dli     $6, 0x5606a6063606c606
    dli     $7, 0x5707a7073707c707
    dli     $8, 0x5808a8083808c808
    dli     $9, 0x5909a9093909c909
    dli     $10, 0x5a0aaa0a3a0aca0a
    dli     $11, 0x5b0bab0b3b0bcb0b
    dli     $12, 0x5c0cac0c3c0ccc0c
    dli     $13, 0x5d0dad0d3d0dcd0d
    dli     $14, 0x5e0eae0e3e0ece0e
    dli     $15, 0x5f0faf0f3f0fcf0f
    dli     $24, 0x6818b8184818d818
    dli     $25, 0x6919b9194919d919
    dli     $31, 0x6f1fbf1f4f1fdf1f
1:
    dli     $s2, 0x5101a1013101c101
    beq     $1, $s2, 2f
    dli     $s2, 0x2
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5202a2023202c202
    beq     $2, $s2, 2f
    dli     $s2, 0x4
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5303a3033303c303
    beq     $3, $s2, 2f
    dli     $s2, 0x8
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5404a4043404c404
    beq     $4, $s2, 2f
    dli     $s2, 0x10
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5505a5053505c505
    beq     $5, $s2, 2f
    dli     $s2, 0x20
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5606a6063606c606
    beq     $6, $s2, 2f
    dli     $s2, 0x40
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5707a7073707c707
    beq     $7, $s2, 2f
    dli     $s2, 0x80
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5808a8083808c808
    beq     $8, $s2, 2f
    dli     $s2, 0x100
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5909a9093909c909
    beq     $9, $s2, 2f
    dli     $s2, 0x200
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5a0aaa0a3a0aca0a
    beq     $10, $s2, 2f
    dli     $s2, 0x400
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5b0bab0b3b0bcb0b
    beq     $11, $s2, 2f
    dli     $s2, 0x800
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5c0cac0c3c0ccc0c
    beq     $12, $s2, 2f
    dli     $s2, 0x1000
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5d0dad0d3d0dcd0d
    beq     $13, $s2, 2f
    dli     $s2, 0x2000
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5e0eae0e3e0ece0e
    beq     $14, $s2, 2f
    dli     $s2, 0x4000
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x5f0faf0f3f0fcf0f
    beq     $15, $s2, 2f
    dli     $s2, 0x8000
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x6818b8184818d818
    beq     $24, $s2, 2f
    dli     $s2, 0x1000000
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x6919b9194919d919
    beq     $25, $s2, 2f
    dli     $s2, 0x2000000
    or      $s3, $s3, $s2
2:
    dli     $s2, 0x6f1fbf1f4f1fdf1f
    beq     $31, $s2, 2f
    dli     $s2, 0x80000000
    or      $s3, $s3, $s2
2:
    mfhi    $s2
    dli     $s6, 0x4848484821212121
    beq     $s2, $s6, 2f
    ori     $s3, $s3, 1
2:
    mflo    $s2
    dli     $s6, 0x4c4c4c4c37373737
    beq     $s2, $s6, 2f
    ori     $s4, $s4, 1
2:
    daddiu  $s0, $s0, -1
    bne     $s0, $zero, 1b
    sd      $s3, 0($s5)
    sd      $s4, 8($s5)
    ld      $ra, 56($sp)
    ld      $s0, 48($sp)
    ld      $s1, 40($sp)
    ld      $s2, 32($sp)
    ld      $s3, 24($sp)
    ld      $s4, 16($sp)
    ld      $s5, 8($sp)
    ld      $s6, 0($sp)
    daddiu  $sp, $sp, 64
    jr      $ra
    .end torture

    .globl clobber_all
    .ent clobber_all
clobber_all:
    dli     $1, 0x7eadbeefdeadbeef
    dli     $2, 0x7eadbeefdeadbeef
    dli     $3, 0x7eadbeefdeadbeef
    dli     $4, 0x7eadbeefdeadbeef
    dli     $5, 0x7eadbeefdeadbeef
    dli     $6, 0x7eadbeefdeadbeef
    dli     $7, 0x7eadbeefdeadbeef
    dli     $8, 0x7eadbeefdeadbeef
    dli     $9, 0x7eadbeefdeadbeef
    dli     $10, 0x7eadbeefdeadbeef
    dli     $11, 0x7eadbeefdeadbeef
    dli     $12, 0x7eadbeefdeadbeef
    dli     $13, 0x7eadbeefdeadbeef
    dli     $14, 0x7eadbeefdeadbeef
    dli     $15, 0x7eadbeefdeadbeef
    dli     $24, 0x7eadbeefdeadbeef
    dli     $25, 0x7eadbeefdeadbeef
    mthi    $1
    mtlo    $1
    jr      $ra
    .end clobber_all
