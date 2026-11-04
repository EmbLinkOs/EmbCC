/* What embld's -Tstack entry stub does (src/link/link.c, add_entry_stub), for a link under a
 * script, where there is no stub: sp below a 32-byte bottom frame, PS for the windowed ABI
 * (WOE | UM, level 0), this window the only live one, then _start by callx8. */
    .section .text.reset, "ax"
    .global _reset
    .align 4
_reset:
    j       1f
    .align  4
.Lsp:
    .word   __stack_top - 32
.Lps:
    .word   0x00040020
.Lentry:
    .word   _start
1:
    l32r    a1, .Lsp
    addi    a2, a1, 32
    addi    a3, a1, -16
    s32i    a2, a3, 4
    l32r    a2, .Lps
    wsr     a2, ps
    movi    a2, 1
    wsr     a2, windowstart
    rsync
    l32r    a8, .Lentry
    callx8  a8
2:
    j       2b
