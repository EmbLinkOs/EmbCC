; pic24-part.s -- for tests/golden/embsim-pic24.sh: a configuration word
; where a PIC24FJ128GA606's EDC puts its FOSCSEL, program address 0x15F18
; (its last page; the PIC24FJ1024GB610's is 0xABF18), choosing the FRC
; with the PLL (FNOSC 001). It prints OSCCON.COSC on UART1: 1 on the
; PIC24FJ128GA606, 7 (FRCDIV: an erased FOSCSEL's choice) on the
; PIC24FJ1024GB610, where the word is no configuration word; and the last
; word of that page, program address 0x15FFE, erased (FFFF 00FF): the
; part's program memory ends with the page.
        .equ U1MODE, 0x398
        .equ U1STA, 0x39a
        .equ U1TXREG, 0x39c
        .equ OSCCON, 0x100
        .equ TBLPAG, 0x54
        .org 0
        goto start
        .fill 126, trap
        .org 0x100
start:  mov #0x1800, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA
        mov #'O', w1
        rcall putc
        mov OSCCON, w0
        lsr w0, #12, w0                 ; COSC
        rcall print
        mov #1, w0
        mov w0, TBLPAG
        mov #0x5ffe, w2                 ; 0x15FFE: the last page's last word
        .word 0xba0012                  ; tblrdl [w2], w0
        rcall print
        .word 0xba8012                  ; tblrdh [w2], w0
        rcall print
        rcall newline
        mov #'.', w1
        rcall putc
        rcall newline
        reset

print:  push w1                         ; a space and W0 as four hex digits
        push w2
        push w3
        mov #' ', w1
        rcall putc
        mov w0, w3
        mov #4, w2
digit:  lsr w3, #12, w1
        sl w3, #4, w3
        cp w1, #10
        bra lt, num
        add #7, w1
num:    add #0x30, w1
        rcall putc
        dec w2, w2
        bra nz, digit
        pop w3
        pop w2
        pop w1
        return
newline: mov #10, w1
putc:   btsc U1STA, #9
        bra putc
        mov w1, U1TXREG
        return
trap:   mov #'T', w1
        rcall putc
        reset

        .org 0x15f18
        .word 0xfffff9                  ; FOSCSEL: FNOSC 001, FRCPLL
