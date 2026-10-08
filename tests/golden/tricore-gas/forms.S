/* TriCore assembly in a file, run by tests/golden/tricore-gas.sh with
 * main.c on QEMU's tricore_testboard: calls both ways between C and
 * assembly (CALL saves the upper context, RET restores it), an address
 * built from hi:/lo: and %hi/%lo halves, loads through [aB]lo:sym, a
 * jump-and-link to a local routine, numeric labels, the conditional
 * branches against registers and constants, LOOP, a tail jump to C, and
 * data words naming symbols. Arguments arrive in d4/d5 (a4 for a
 * pointer), a result leaves in d2. */
#define ONE 1

        .text
        .align  2
/* int tc_add(int a, int b): a + b + counter */
        .global tc_add
        .type   tc_add, @function
tc_add:
        movh.a  a2, hi:counter
        ld.w    d2, [a2]lo:counter
        add     d2, d2, d4
        add     d2, d2, d5
        ret
        .size   tc_add, . - tc_add

/* int tc_call(int x): c_twice(x) + 1 -- C called with call */
        .global tc_call
        .type   tc_call, @function
tc_call:
        call    c_twice
        addi    d2, d2, ONE
        ret

/* int tc_sum(int n): 1 + ... + n twice over: a branch loop on numeric
 * labels and the LOOP instruction */
        .global tc_sum
        .type   tc_sum, @function
tc_sum:
        mov     d2, 0
        mov     d3, d4
        jz      d3, 2f
1:      add     d2, d2, d3
        addi    d3, d3, -1
        jnz     d3, 1b
2:      mov     d5, 0
        jlt     d4, 1, 4f
        addi    d6, d4, -1
        mov.a   a3, d6
        mov     d7, 0
3:      addi    d7, d7, 1
        add     d5, d5, d7
        loop    a3, 3b
4:      add     d2, d2, d5
        ret

/* int tc_classify(int x): a bit for each test x passes */
        .global tc_classify
        .type   tc_classify, @function
tc_classify:
        mov     d2, 0
        jne     d4, 7, 1f
        addi    d2, d2, 1
1:      jge.u   d4, 15, 2f
        addi    d2, d2, 2
2:      jlt     d4, 0, 3f
        addi    d2, d2, 4
3:      mov     d3, 7
        jge     d3, d4, 4f
        addi    d2, d2, 8
4:      mov     d3, 10
        jeq     d4, d3, 5f
        addi    d2, d2, 16
5:      mov.a   a3, d4
        jz.a    a3, 6f
        addi    d2, d2, 32
6:      ret

/* int *tc_table(void): &table, through %hi and %lo */
        .global tc_table
        .type   tc_table, @function
tc_table:
        movh    d2, %hi(table)
        addi    d2, d2, %lo(table)
        mov.a   a2, d2
        ret

/* int tc_jl(void): a jump-and-link to a local routine and back through
 * a11 (saved first: RET returns through it) */
        .global tc_jl
        .type   tc_jl, @function
tc_jl:
        mov.aa  a4, a11
        mov     d2, 5
        jl      1f
        addi    d2, d2, 100
        mov.aa  a11, a4
        ret
1:      addi    d2, d2, 1
        ji      a11

/* int tc_tail(int x): c_twice(x + 1), by a jump into C */
        .global tc_tail
        .type   tc_tail, @function
tc_tail:
        addi    d4, d4, 1
        j       c_twice

        .data
        .align  2
        .global table
table:  .word   counter, 1234, tc_add
