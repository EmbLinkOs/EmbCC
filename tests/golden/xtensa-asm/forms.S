/* Xtensa assembly the way an ESP32 port writes it, run by
 * tests/golden/xtensa-asm.sh on QEMU's de212 with main.c -- assembled by
 * EmbCC and, for the reference, by Espressif's GNU as
 * (--text-section-literals). Windowed functions (entry/retw), calls both
 * ways between C and assembly with call4, call8, call12 and callx8,
 * literals from movi, .literal and .literal_position, numeric labels and
 * the zero-overhead loop, the branch forms, the special registers (PS
 * through rsil, CCOUNT, EXCSAVE1 through xsr), data words naming
 * symbols, and a function in a section of its own. Every value is fixed;
 * main.c prints them. */
#define FRAME   32
#define LEVEL   5

        .text
        .literal_position
        .literal .Lcnt, counter
        .literal .Lk, 1000

/* int asm_add(int a, int b): a + b + counter. A literal shared by two
 * movi, and one cancelled out. */
        .align  4
        .global asm_add
        .type   asm_add, @function
asm_add:
        entry   a1, FRAME
        add     a2, a2, a3
        movi    a4, counter
        l32i    a4, a4, 0
        add     a2, a2, a4
        movi    a5, 0x12345678
        sub     a2, a2, a5
        movi    a6, 0x12345678
        add     a2, a2, a6
        retw
        .size   asm_add, . - asm_add

/* int asm_call(int x): c_twice(x) + 1, calling C with call8 -- the
 * callee's a2 is this window's a10. */
        .align  4
        .global asm_call
        .type   asm_call, @function
asm_call:
        entry   a1, FRAME
        mov     a10, a2
        call8   c_twice
        addi    a2, a10, 1
        retw
        .size   asm_call, . - asm_call

/* int asm_sum(int n): 1 + ... + n twice over, once with a branch loop and
 * once with the zero-overhead loop. */
        .align  4
        .global asm_sum
        .type   asm_sum, @function
asm_sum:
        entry   a1, FRAME
        movi    a3, 0
        mov     a4, a2
        beqz    a4, 2f
1:      add     a3, a3, a4
        addi    a4, a4, -1
        bnez    a4, 1b
2:      movi    a5, 0
        movi    a6, 0
        loopgtz a2, 3f
        addi    a6, a6, 1
        add     a5, a5, a6
3:      add     a2, a3, a5
        retw
        .size   asm_sum, . - asm_sum

/* int asm_classify(int x): a bit for each test x passes */
        .align  4
        .global asm_classify
        .type   asm_classify, @function
asm_classify:
        entry   a1, FRAME
        movi    a3, 0
        bnei    a2, 10, 1f
        addi    a3, a3, 1
1:      bbci    a2, 3, 2f
        addi    a3, a3, 2
2:      bltui   a2, 16, 3f
        addi    a3, a3, 4
3:      bltz    a2, 4f
        addi    a3, a3, 8
4:      movi    a4, 7
        bge     a4, a2, 5f
        addi    a3, a3, 16
5:      bbsi.l  a2, 0, 6f
        addi    a3, a3, 32
6:      mov     a2, a3
        retw
        .size   asm_classify, . - asm_classify

/* int asm_ps(void): raise the interrupt level with rsil, read it back
 * from PS, restore PS and read it again: level * 16 + restored level */
        .align  4
        .global asm_ps
        .type   asm_ps, @function
asm_ps:
        entry   a1, FRAME
        rsil    a3, LEVEL
        rsr     a4, ps
        extui   a4, a4, 0, 4
        wsr     a3, ps
        rsync
        rsr.ps  a5
        extui   a5, a5, 0, 4
        slli    a4, a4, 4
        add     a2, a4, a5
        retw
        .size   asm_ps, . - asm_ps

/* unsigned asm_ccount(void): how far CCOUNT moved over a short loop */
        .align  4
        .global asm_ccount
        .type   asm_ccount, @function
asm_ccount:
        entry   a1, FRAME
        rsr     a3, ccount
        movi    a4, 200
1:      addi    a4, a4, -1
        bnez    a4, 1b
        rsr     a5, ccount
        sub     a2, a5, a3
        retw
        .size   asm_ccount, . - asm_ccount

/* int asm_xsr(void): EXCSAVE1 = 77, exchanged for 5: old * 10 + new */
        .align  4
        .global asm_xsr
        .type   asm_xsr, @function
asm_xsr:
        entry   a1, FRAME
        movi    a3, 77
        wsr     a3, excsave1
        movi    a4, 5
        xsr     a4, excsave1
        rsr     a5, 209
        movi    a6, 10
        mull    a4, a4, a6
        add     a2, a4, a5
        retw
        .size   asm_xsr, . - asm_xsr

/* int asm_calls(int x): asm_add(x, 1) by call4 + asm_add(x, 2) by call12
 * + c_twice(x) by callx8. a2 and a3 are below every callee's window. */
        .align  4
        .global asm_calls
        .type   asm_calls, @function
asm_calls:
        entry   a1, 48
        mov     a6, a2
        movi    a7, 1
        call4   asm_add
        mov     a3, a6
        mov     a14, a2
        movi    a15, 2
        call12  asm_add
        add     a3, a3, a14
        movi    a8, c_twice
        mov     a10, a2
        callx8  a8
        add     a2, a3, a10
        retw
        .size   asm_calls, . - asm_calls

/* int asm_lit(void): counter + 1000, through the .literal words above */
        .align  4
        .global asm_lit
        .type   asm_lit, @function
asm_lit:
        entry   a1, FRAME
        l32r    a3, .Lcnt
        l32i    a3, a3, 0
        l32r    a4, .Lk
        add     a2, a3, a4
        .begin  no-transform
        _movi   a5, 0
        add     a2, a2, a5
        .end    no-transform
        retw
        .size   asm_lit, . - asm_lit

        .section .text.other, "ax", @progbits
        .literal_position
/* int asm_other(int x): x * 0x10001, in a section of its own */
        .align  4
        .global asm_other
        .type   asm_other, @function
asm_other:
        entry   a1, FRAME
        movi    a3, 0x10001
        mull    a2, a2, a3
        retw
        .size   asm_other, . - asm_other

        .data
        .align  4
        .global table
table:  .word   counter, 1234, asm_add
