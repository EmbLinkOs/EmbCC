/* What a MIPS .S file writes, for tests/golden/mips-gas.sh to assemble
 * with EmbCC and with clang and compare: modes, labels, the symbol forms
 * and their relocations, expressions, sections and data. */
#define FRAME (16 + 4 * 6)
#define SLOT(k) (16 + 4 * (k))
    .equ  UART, 0xbf000900
    .equ  BIG, 0x12348000

    .text
    .globl  f_reorder
    .ent    f_reorder
    .type   f_reorder, @function
f_reorder:                       # GNU as's default mode: slots are filled
    addiu   $sp, $sp, -FRAME
    sw      $ra, SLOT(5)($sp)
    sw      $s0, SLOT(4)($sp)
    move    $s0, $a0
    beqz    $a0, 1f
    addiu   $s0, $s0, 1
    bnez    $s0, .Lskip
    jal     ext_fn
    nop
.Lskip:
    jal     f_noreorder
1:  bgez    $s0, 2f
    blez    $a1, 1b
    bltz    $a2, 1b
    bgtz    $a3, 2f
2:  beq     $a0, $a1, f_reorder
    bne     $a0, $zero, .Lskip
    b       3f
    bal     f_reorder
3:  lw      $ra, SLOT(5)($sp)
    lw      $s0, SLOT(4)($sp)
    addiu   $sp, $sp, FRAME
    jr      $ra
    .end    f_reorder
    .size   f_reorder, .-f_reorder

    .globl  f_noreorder
    .set    push
    .set    noreorder
f_noreorder:
    beq     $a0, $zero, 1f
    addiu   $v0, $zero, 7          # in the slot
    j       local_tail
    addiu   $v0, $v0, 1
1:  jr      $ra
    move    $v0, $a1
    .set    pop
local_tail:
    jalr    $t9
    jr      $ra

    .globl  f_symbols
f_symbols:
    lui     $t0, %hi(gvar)
    addiu   $t0, $t0, %lo(gvar)
    lui     $t1, %hi(gvar+0x8004)
    lw      $t2, %lo(gvar+0x8004)($t1)
    sw      $t2, %lo(gvar+0x8004)($t1)
    lbu     $t3, %lo(ext_data)($t1)
    lui     $t4, %hi(ext_data+12)
    sh      $t3, %lo(ext_data+12)($t4)
    la      $a0, gvar+8
    la      $a1, ext_data
    la      $a2, f_reorder
    lui     $t5, %hi(BIG)
    addiu   $t5, $t5, %lo(BIG)
    lui     $t6, %hi(0x7fff8000)
    ori     $t6, $t6, 0x8000
    li      $t7, UART
    li      $t8, -32768
    li      $t9, 0x10000
    li      $v1, 65535
    j       ext_fn
    jal     f_noreorder

    .globl  f_misc
f_misc:
    .set    noat
    addu    $at, $a0, $a1
    .set    at
    mfc0    $t0, $12
    mtc0    $t0, $12, 0
    mfc0    $t1, $15, 1
    ehb
    di      $t2
    ei
    ext     $t3, $t0, 3, 5
    ins     $t3, $zero, 0, 2
    seb     $t4, $t3
    wsbh    $t5, $t4
    rotr    $t5, $t5, 16
    clz     $t6, $t5
    mult    $a0, $a1
    mflo    $v0
    mfhi    $v1
    div     $zero, $a0, $a1
    teq     $a1, $zero, 7
    ll      $t7, 0($a0)
    sc      $t7, 0($a0)
    sync
    lwl     $t8, 3($a0)
    lwr     $t8, 0($a0)
    sw      $t8, ((1 << 4) | 2)($sp)
    lw      $t8, -(2 * 8)($sp)
    syscall
    break   5
    wait
    eret

    .data
    .globl  gvar
    .p2align 2
gvar:   .word   1, 2, 3
        .word   f_reorder, local_tail, gvar+4, ext_data
        .2byte  0x1234
        .byte   1, 2
        .align  2
table:  .word   1b, 3b
        .ascii  "mips\n"
        .asciz  "o32"

    .section .rodata
    .p2align 3
ro:     .word   table, ro
        .space  8

    .bss
    .p2align 2
    .globl  counter
counter: .space 16
