; dspic33-part.s -- a dsPIC33EP or PIC24EP part's own memory, by its EDC,
; for tests/golden/embsim-dspic33.sh: the same image on parts of the
; family, each line's values its own. Assembled by pic24-asm.awk. It
; prints on UART1 (at 0x220 on every part of the family):
;
;   P  W15 out of reset (the RAM's start, 0x1000); OSCCON.COSC, the
;      oscillator FOSCSEL's FNOSC chose at reset -- the image gives the
;      configuration memory's FOSCSEL (0xF80006, the larger parts') FRC
;      (000), and program memory's last page's (0xAFF8, a 64 KiB part's)
;      FRC with the PLL (001); a word written at 0x2FFE and read back (12
;      KiB of RAM and more: it is there);
;   Y  a Y prefetch (MOVSAC A, [W10], W4) from 0x1800 and from 0x2000,
;      each holding a word: the part's Y space has it, X space reads 0;
;      a PIC24EP has no DSP engine: the run ends at the MOVSAC.
        .equ U1MODE, 0x220
        .equ U1STA, 0x222
        .equ U1TXREG, 0x224
        .equ OSCCON, 0x742
        .org 0
        goto start
        .fill 254, trap
        .org 0x200
start:  mov w15, w9
        mov #0x1100, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA
        mov #'P', w1
        rcall putc
        mov w9, w0
        rcall print
        mov OSCCON, w0
        lsr w0, #12, w0
        mov #7, w1
        .word 0x600001                  ; AND W0, W1, W0
        rcall print
        mov #0x9abc, w0
        mov w0, 0x2ffe
        mov 0x2ffe, w0
        rcall print
        rcall newline
        mov #'Y', w1
        rcall putc
        mov #0x1234, w0
        mov w0, 0x1800
        mov #0x5678, w0
        mov w0, 0x2000
        mov #0x1800, w10
        .word 0xc70102                  ; MOVSAC A, [W10], W4
        mov w4, w0
        rcall print
        mov #0x2000, w10
        .word 0xc70102                  ; MOVSAC A, [W10], W4
        mov w4, w0
        rcall print
        rcall newline
        mov #'.', w1
        rcall putc
        rcall newline
done:   bra done
print:  push w1                  ; a space and W0 as four hex digits
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
trap:   mov #'T', w1            ; a trap: "T" and the end
        rcall putc
        reset
; ---- the configuration words FOSCSEL: FNOSC FRC with the PLL in a 64 KiB
; part's last page, FRC in the configuration memory
        .org 0xaff8
        .word 0xffff79
        .org 0xf80006
        .word 0xffff78
