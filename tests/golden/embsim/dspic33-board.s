; dspic33-board.s -- the dsPIC33EP512MU810's peripherals and the Explorer
; 16/32's dsPIC33EP512MU810 PIM, for tests/golden/embsim-dspic33.sh, run
; with the temperature at 30 degrees and S3 pressed. Assembled by
; pic24-asm.awk; the forms it has not are .words with their instruction.
; It prints on UART1, and the board reports its LEDs and LCD on stderr:
;
;   - the clock: FOSCSEL (below) starts the part on the FRC with the PLL,
;     PLLFBD and CLKDIV as reset leaves them (M 50, N1 2, N2 4): 46.06 MHz,
;     an instruction cycle at 23.03 MHz, which the board's reports' times
;     are in;
;   - Timer1's interrupt, three times: the controller, the vector, RETFIE;
;   - LATA's LEDs on and off, ANSELA cleared;
;   - S3 (RD6) pressed and S6 (RD7) released, pulled up by the board,
;     ANSELD cleared;
;   - the TC1047A on AN4 and the potentiometer on AN5, converted by ADC1
;     at 10 bits and at 12 (AD12B), the sample ended by the program
;     (SSRC 000); AN4 at 10 bits as a signed fraction (FORM 11);
;   - the LCD on the PMP in master mode 2, RS on PMA0 (PMAEN), each
;     cycle waited for on PMMODE.BUSY;
;   - the 25LC256 on SPI2 (its own pins, RG6-RG8), its CS on RD12:
;     SPI2STAT once enabled (SRMPT set out of reset), WREN,
;     a byte written at 0x0200, the write cycle waited for on RDSR, read
;     back with the erased byte after it; read again, a second byte
;     received before SPI2BUF is read (SPIROV; the first kept).
        .equ U1MODE, 0x220
        .equ U1STA, 0x222
        .equ U1TXREG, 0x224
        .equ IFS0, 0x800
        .equ IEC0, 0x820
        .equ TMR1, 0x100
        .equ PR1, 0x102
        .equ T1CON, 0x104
        .equ SPI2STAT, 0x260
        .equ SPI2CON1, 0x262
        .equ SPI2BUF, 0x268
        .equ ADC1BUF0, 0x300
        .equ AD1CON1, 0x320
        .equ AD1CON3, 0x324
        .equ AD1CHS0, 0x328
        .equ PMCON, 0x600
        .equ PMMODE, 0x602
        .equ PMADDR, 0x604
        .equ PMDIN1, 0x608
        .equ PMAEN, 0x60c
        .equ TRISA, 0xe00
        .equ LATA, 0xe04
        .equ ANSELA, 0xe0e
        .equ TRISD, 0xe30
        .equ PORTD, 0xe32
        .equ LATD, 0xe34
        .equ ANSELD, 0xe3e
        .equ COUNT, 0x1800
        .equ TMP, 0x1802
        .equ STACK, 0x2000
        .org 0
        goto start
        .fill 254, trap
        .org 0x1a
        .word t1isr                     ; Timer1's vector (interrupt 3)
        .org 0x200
start:  mov #STACK, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA
        mov #'B', w1
        rcall putc
        rcall newline

; ---- Timer1's interrupt, three times ---------------------------------------
        mov #0, w0
        mov w0, COUNT
        mov #200, w0
        mov w0, PR1
        bset IEC0, #3
        mov #0x8000, w0
        mov w0, T1CON
wait1:  mov COUNT, w0
        cp w0, #3
        bra lt, wait1
        mov #0, w0
        mov w0, T1CON
        bclr IEC0, #3
        mov COUNT, w0
        rcall print
        rcall newline

; ---- the LEDs ----------------------------------------------------------------
        mov #0, w0
        mov w0, ANSELA
        mov #0xff00, w0
        mov w0, TRISA
        mov #0x0055, w0
        mov w0, LATA                    ; D3, D5, D7, D9
        mov #0x0000, w0
        mov w0, LATA

; ---- the buttons: S3 (RD6) pressed, S6 (RD7) released --------------------------
        mov #0, w0
        mov w0, ANSELD
        mov PORTD, w0
        lsr w0, #6, w0
        .word 0x6000e3                  ; and w0, #3, w1
        mov w1, w0
        rcall print
        rcall newline

; ---- ADC1: AN4 and AN5 at 10 bits, AN4 at 12 ------------------------------------
        mov #0x0001, w0
        mov w0, AD1CON3                 ; TAD 2 Tcy
        mov #0x8000, w0
        mov w0, AD1CON1                 ; ADON, 10 bits, SSRC 000
        mov #4, w0
        rcall adc
        mov #5, w0
        rcall adc
        bset AD1CON1, #10               ; AD12B
        mov #4, w0
        rcall adc
        mov #0x8300, w0
        mov w0, AD1CON1                 ; 10 bits, FORM 11: signed fractional
        mov #4, w0
        rcall adc
        rcall newline

; ---- the LCD on the PMP --------------------------------------------------------------
        mov #0x0000, w0
        mov w0, PMCON
        mov #0x024d, w0
        mov w0, PMMODE                  ; master 2, WAITB 1, WAITM 3, WAITE 1
        mov #0x0001, w0
        mov w0, PMAEN                   ; PMA0: RS
        mov #0x8300, w0
        mov w0, PMCON                   ; PMPEN, PTWREN, PTRDEN
        mov #0, w1
        mov #0x38, w0
        rcall lcd
        mov #0x0c, w0
        rcall lcd
        mov #0x01, w0
        rcall lcd
        mov #0x06, w0
        rcall lcd
        mov #1, w1
        mov #'d', w0
        rcall lcd
        mov #'s', w0
        rcall lcd
        mov #'P', w0
        rcall lcd
        mov #'I', w0
        rcall lcd
        mov #'C', w0
        rcall lcd
        mov #'3', w0
        rcall lcd
        mov #'3', w0
        rcall lcd
        mov #0, w1
        mov #0x02, w0
        rcall lcd                       ; home: the frame is shown

; ---- the 25LC256 on SPI2, its CS on RD12 -----------------------------------------
        bset LATD, #12
        bclr TRISD, #12
        mov #0x0120, w0
        mov w0, SPI2CON1                ; MSTEN, CKE
        mov #0x8000, w0
        mov w0, SPI2STAT                ; SPIEN
        mov SPI2STAT, w0                ; SRMPT: the shift register empty
        rcall print
        bclr LATD, #12
        mov #6, w0                      ; WREN
        rcall xfer
        bset LATD, #12
        bclr LATD, #12
        mov #2, w0                      ; WRITE 0x0200: 0x3C
        rcall xfer
        mov #0x02, w0
        rcall xfer
        mov #0x00, w0
        rcall xfer
        mov #0x3c, w0
        rcall xfer
        bset LATD, #12
poll:   bclr LATD, #12
        mov #5, w0                      ; RDSR, until WIP clears
        rcall xfer
        mov #0, w0
        rcall xfer
        bset LATD, #12
        mov w0, TMP
        btsc TMP, #0
        bra poll
        bclr LATD, #12
        mov #3, w0                      ; READ 0x0200
        rcall xfer
        mov #0x02, w0
        rcall xfer
        mov #0x00, w0
        rcall xfer
        mov #0, w0
        rcall xfer
        rcall print
        mov #0, w0
        rcall xfer
        rcall print
        bset LATD, #12
        bclr LATD, #12
        mov #3, w0                      ; READ 0x0200 again
        rcall xfer
        mov #0x02, w0
        rcall xfer
        mov #0x00, w0
        rcall xfer
        mov #0, w0
        mov w0, SPI2BUF                 ; 0x3C received
        mov w0, SPI2BUF                 ; 0xFF too, SPIRBF still set: lost
        mov SPI2STAT, w0
        rcall print
        mov SPI2BUF, w0
        rcall print
        mov SPI2STAT, w0
        rcall print
        bset LATD, #12
        rcall newline

        mov #'.', w1
        rcall putc
        rcall newline
done:   bra done

; ---- the routines ---------------------------------------------------------------------
t1isr:  push w0                         ; Timer1: counted
        mov COUNT, w0
        add #1, w0
        mov w0, COUNT
        bclr IFS0, #3
        pop w0
        .word 0x064000                  ; retfie
adc:    mov w0, AD1CHS0                 ; W0's input: sampled, converted, printed
        bset AD1CON1, #1                ; SAMP
        repeat #20
        nop
        bclr AD1CON1, #1                ; the conversion starts
adcw:   btss AD1CON1, #0                ; DONE
        bra adcw
        mov ADC1BUF0, w0
        rcall print
        return
lcd:    mov w1, PMADDR                  ; W0 to the LCD, RS W1
        mov w0, PMDIN1
lcdw:   btsc PMMODE, #15                ; BUSY
        bra lcdw
        return
xfer:   mov w0, SPI2BUF                 ; W0's byte out, the byte in to W0
xferw:  btss SPI2STAT, #0               ; SPIRBF
        bra xferw
        mov SPI2BUF, w0
        return
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
; ---- the configuration word FOSCSEL: FNOSC 001, the FRC with the PLL
        .org 0xf80006
        .word 0xffff79
