#define OFF 8
    .text
    .globl asm_add
    .type asm_add, @function
asm_add:
    add.d   $a0, $a0, $a1
    jr      $ra
    .size asm_add, .-asm_add

    .globl asm_call
asm_call:
    addi.d  $sp, $sp, -16
    st.d    $ra, $sp, OFF
    bl      c_twice           # external: R_LARCH_B26
    addi.d  $a0, $a0, 1
    beqz    $a0, 1f
    b       2f
1:  li.w    $a0, -1
2:  ld.d    $ra, $sp, OFF
    addi.d  $sp, $sp, 16
    ret

    .globl asm_glob
asm_glob:
    la.pcrel $t0, counter
    ld.d    $a0, $t0, 0
    pcalau12i $t1, %pc_hi20(table+8)
    ld.d    $t1, $t1, %pc_lo12(table+8)
    add.d   $a0, $a0, $t1
    la.global $t2, counter
    ld.d    $t2, $t2, 0
    add.d   $a0, $a0, $t2
    jr      $ra

    .data
    .globl table
    .p2align 3
table:
    .dword counter
    .dword 1000
