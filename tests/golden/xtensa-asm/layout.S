/* A file whose bytes, symbols and relocations EmbCC's assembler must
 * produce exactly as Espressif's GNU as does (--text-section-literals),
 * compared by tests/golden/xtensa-asm.sh. GNU as narrows instructions to
 * the density option's 16-bit forms wherever it may, and EmbCC emits
 * none, so the whole file is in a no-transform block: what it checks is
 * the layout -- where .literal_position, .literal and a function's entry
 * put a pool, the zero padding before a pool, .align counted in bytes,
 * labels local and global, branches, loops and calls to labels of this
 * file and to symbols defined elsewhere, and data words naming them. */
        .begin  no-transform
        .text
        nop
        .literal_position
        .literal .Lone, 1, ext_data + 8
        .align  4
        .global lay_a
        .type   lay_a, @function
lay_a:
        entry   a1, 32
        l32r    a2, .Lone
        l32r    a3, .Lone + 4
        beqz    a2, 1f
        bbsi    a3, 7, 2f
        loopnez a2, 2f
1:      addi    a2, a2, 1
        add     a2, a2, a3
2:      call8   lay_b
        call8   ext_fn
        call4   lay_b
        retw

        .literal .Ltwo, lay_a, 0x7fffffff
        .align  4
        .global lay_b
        .type   lay_b, @function
lay_b:  entry   a1, 48
        l32r    a4, .Ltwo
        callx8  a4
        j       ext_tail
        j       lay_b
        rsr     a5, excsave2
        wsr.intenable a5
        retw
        .align  8
        .byte   1
        .align  2
        .hword  2
        .align  16
lay_c:  ret
        .section .text.vec, "ax", @progbits
        nop
        .literal_position
        .literal .Lthree, lay_c
        .align  4
vec:    l32r    a2, .Lthree
        bnez    a2, vec
        j       lay_a
        .data
        .word   lay_a, ext_data, .Lone, 5
        .end    no-transform
