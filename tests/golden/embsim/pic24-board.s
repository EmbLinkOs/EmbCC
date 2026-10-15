; pic24-board.s -- the PIC24FJ1024GB610's peripherals and the Explorer
; 16/32's PIC24 PIM, for tests/golden/embsim-pic24.sh, run with the
; temperature at 30 degrees and S3 pressed. Assembled by pic24-asm.awk;
; the forms it has not are .words with their instruction. It prints on
; UART1, and the board reports its LEDs and LCD on stderr:
;
;   - Timer1's interrupt, three times: the controller, the vector, RETFIE;
;   - LATA's LEDs on and off;
;   - S3 (RD6) pressed and S6 (RD7) released, pulled up by the board;
;   - the TC1047A on AN4 and the potentiometer on AN5, converted at 10
;     and 12 bits, the sample ended by the program (SSRC 0);
;   - the LCD on the EPMP: its chip select 1 at EDS 0x20000, the command
;     register there and the data register two bytes up (RS on PMA0);
;   - the 25LC256 on SPI1, the Peripheral Pin Select putting SCK1OUT on
;     RP21, SDO1 on RP19 and SDI1 on RP26 behind OSCCON's unlock
;     sequence, its CS on RG0: a byte written, the write cycle waited for
;     on RDSR, read back; with IOLOCK set, a PPS write that would unmap it
;     ignored.
        .equ SR, 0x42
        .equ DSWPAG, 0x34
        .equ IFS0, 0x88
        .equ IEC0, 0x98
        .equ OSCCON, 0x100
        .equ PMCON1, 0x128
        .equ PMCON4, 0x12e
        .equ PMCS1CF, 0x130
        .equ PMCS1BS, 0x132
        .equ TMR1, 0x190
        .equ PR1, 0x192
        .equ T1CON, 0x194
        .equ U1MODE, 0x398
        .equ U1STA, 0x39a
        .equ U1TXREG, 0x39c
        .equ SPI1CON1L, 0x3f4
        .equ SPI1STATL, 0x3fc
        .equ SPI1BUFL, 0x400
        .equ TRISA, 0x662
        .equ LATA, 0x666
        .equ ANSA, 0x66a
        .equ PORTD, 0x6a0
        .equ ANSD, 0x6a6
        .equ TRISG, 0x6da
        .equ LATG, 0x6de
        .equ ANSG, 0x6e2
        .equ ADC1BUF0, 0x712
        .equ AD1CON1, 0x746
        .equ AD1CON3, 0x74a
        .equ AD1CHS, 0x74c
        .equ RPINR20, 0x7b8
        .equ RPOR9, 0x7e6
        .equ RPOR10, 0x7e8
        .equ COUNT, 0x1000
        .equ STACK, 0x1800
        .org 0
        goto start
        .fill 126, trap
        .org 0x1a
        .word t1isr                     ; Timer1's vector (interrupt 3)
        .org 0x100
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
        mov w0, ANSA
        mov #0xff00, w0
        mov w0, TRISA
        mov #0x0055, w0
        mov w0, LATA                    ; D3, D5, D7, D9
        mov #0x0000, w0
        mov w0, LATA

; ---- the buttons: S3 (RD6) pressed, S6 (RD7) released --------------------------
        mov #0, w0
        mov w0, ANSD
        mov PORTD, w0
        lsr w0, #6, w0
        .word 0x6000e3                  ; and w0, #3, w1 -> W1
        mov w1, w0
        rcall print
        rcall newline

; ---- the ADC: AN4 and AN5 at 10 bits, AN4 at 12 ------------------------------------
        mov #0x0001, w0
        mov w0, AD1CON3                 ; TAD 2 Tcy
        mov #0x8000, w0
        mov w0, AD1CON1                 ; on, 10 bits, SSRC 0
        mov #4, w0
        rcall adc
        mov #5, w0
        rcall adc
        bset AD1CON1, #10               ; MODE12
        mov #4, w0
        rcall adc
        rcall newline

; ---- the LCD on the EPMP -------------------------------------------------------------
        mov #0x0300, w0
        mov w0, PMCON1                  ; master mode
        mov #0x0200, w0
        mov w0, PMCS1BS                 ; CS1 at EDS 0x20000
        mov #0x0000, w0
        mov w0, PMCS1CF                 ; enabled, 8 bits
        mov #0x0001, w0
        mov w0, PMCON4                  ; PMA0
        bset PMCON1, #15                ; PMPEN
        mov #4, w0
        mov w0, DSWPAG                  ; EDS page 4: 0x20000
        mov #0x8000, w1                 ; the command register
        mov #0x8002, w2                 ; the data register
        mov #0x38, w0
        .word 0x780880                  ; mov w0, [w1]
        mov #0x0c, w0
        .word 0x780880
        mov #0x01, w0
        .word 0x780880
        mov #'P', w0
        .word 0x780900                  ; mov w0, [w2]
        mov #'I', w0
        .word 0x780900
        mov #'C', w0
        .word 0x780900
        mov #'2', w0
        .word 0x780900
        mov #'4', w0
        .word 0x780900
        mov #0x02, w0
        .word 0x780880                  ; home: the frame is shown

; ---- the EEPROM on SPI1 -------------------------------------------------------------
        mov #0x00, w3
        rcall osccon                    ; IOLOCK clear
        mov #0x0800, w0
        mov w0, RPOR10                  ; RP21: SCK1OUT (8)
        mov #0x0700, w0
        mov w0, RPOR9                   ; RP19: SDO1 (7)
        mov #0x3f1a, w0
        mov w0, RPINR20                 ; SDI1: RP26
        mov #0x40, w3
        rcall osccon                    ; IOLOCK set
        mov #0, w0
        mov w0, RPOR9                   ; locked: ignored
        mov #0, w0
        mov w0, ANSG
        bset LATG, #0                   ; CS high
        bclr TRISG, #0
        mov #0x8120, w0
        mov w0, SPI1CON1L               ; on, master
        bclr LATG, #0
        mov #0x06, w0                   ; WREN
        rcall xfer
        bset LATG, #0
        bclr LATG, #0
        mov #0x02, w0                   ; WRITE 0x0010
        rcall xfer
        mov #0x00, w0
        rcall xfer
        mov #0x10, w0
        rcall xfer
        mov #0x5a, w0
        rcall xfer
        bset LATG, #0                   ; the write cycle begins
        mov #0, w4                      ; status reads while busy
busy:   bclr LATG, #0
        mov #0x05, w0                   ; RDSR
        rcall xfer
        mov #0xff, w0
        rcall xfer
        bset LATG, #0
        .word 0xe80204                  ; inc w4, w4
        .word 0xa70000                  ; btsc w0, #0
        bra busy
        cp w4, #2
        bra lt, short
        mov #'W', w1                    ; it was busy for a while
        rcall putc
short:  bclr LATG, #0
        mov #0x03, w0                   ; READ 0x0010
        rcall xfer
        mov #0x00, w0
        rcall xfer
        mov #0x10, w0
        rcall xfer
        mov #0xff, w0
        rcall xfer
        bset LATG, #0
        rcall print
        rcall newline
        mov #'.', w1
        rcall putc
        rcall newline
done:   bra done

; ---- routines -------------------------------------------------------------------------
; an ADC conversion of input W0, printed
adc:    mov w0, AD1CHS
        bset AD1CON1, #1                ; SAMP
        nop
        nop
        bclr AD1CON1, #1                ; convert
wait2:  btss AD1CON1, #0                ; DONE
        bra wait2
        mov ADC1BUF0, w0
        bra print

; OSCCONL = W3, through its unlock sequence
osccon: mov #OSCCON, w2
        mov #0x46, w0
        mov #0x57, w1
        .word 0x784900                  ; mov.b w0, [w2]
        .word 0x784901                  ; mov.b w1, [w2]
        .word 0x784903                  ; mov.b w3, [w2]
        return

; SPI1: W0 out, W0 in
xfer:   mov w0, SPI1BUFL
wait3:  btsc SPI1STATL, #5              ; SPIRBE
        bra wait3
        mov SPI1BUFL, w0
        return

t1isr:  push w0
        mov COUNT, w0
        .word 0xe80000                  ; inc w0, w0
        mov w0, COUNT
        bclr IFS0, #3
        pop w0
        .word 0x064000                  ; retfie

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
