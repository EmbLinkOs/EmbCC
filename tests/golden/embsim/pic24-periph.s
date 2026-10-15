; pic24-periph.s -- the PIC24FJ1024GB610's peripherals beyond the board's,
; for tests/golden/embsim-pic24.sh, by the data sheet (DS30010074).
; Assembled by pic24-asm.awk; the forms it has not are .words with their
; instruction. It prints on UART1, a line a peripheral:
;
;   I  interrupt-on-change on RA0, driven by LATA: a rising edge with
;      IOCPA (IOCFA, IOCSTAT, IOC's flag in IFS1), IOCFA cleared (IOCSTAT
;      follows), a write of ones that cannot set it, a falling edge with
;      IOCNA;
;   N  the flash controller: two words programmed from the write latches
;      (TBLWTL, TBLWTH at TBLPAG 0xFA) after NVMKEY's unlock, read back
;      by TBLRDL and TBLRDH, NVMCON and NVM's flag after it; WR without
;      the unlock (WRERR); a page erased (FFFF 00FF);
;   R  the RTCC on the system clock, set to 23:59:58 on 31 December 2099,
;      its alarm on the minute's second 01: when the alarm comes, the
;      time and date (00:00:01, Saturday 1 January 2000), RTCSTATL (ALMEVT)
;      and RTCCON1H (ALRMEN cleared: no repeat); WRLOCK set ignores a
;      write to TIMEH, and is not cleared without NVMKEY's unlock, then
;      is; a timestamp (TSAEVT, TSATIMEH);
;   D  the DMA controller: four words copied by channel 0 on CHREQ, in one
;      continuous transfer (the copy's first and last words, DMACNT0,
;      DMAINT0's DONEIF and HALFIF, DMACH0 with CHEN cleared); a transfer
;      to RAM above DMAH (HIGHIF, nothing moved: DMACNT0 kept); three bytes
;      by channel 1 one at each of Timer2's periods (DMACNT1 after the
;      first, the copy, DMACNT1, DMAINT1, DMACH1); DMA0's and DMA1's flags;
;   C  I2C1 as a master on a bus with no slave: a start (STAT.S, MI2C1's
;      flag), a byte sent (TBF and TRSTAT while it goes, a second byte
;      written then a collision, IWCOL; then ACKSTAT: no acknowledge),
;      IWCOL cleared, a byte received (RBF), a second with RBF still set
;      (I2COV), I2C1RCV (FF: the pull-ups) and RBF cleared by reading it,
;      a stop (STAT.P);
;   P  OC1's output on RP24 through the PPS, captured from it by IC1 and
;      IC2 (IC1R, IC2R: RP24), their timers on the instruction clock: a
;      single pulse from OC1R to OC1RS (every edge: the pulse's width, 10,
;      and no second one);
;      edge-aligned PWM, period OC1RS + 1, OC1R high, with IC1 and IC2
;      synchronized to OC1's sync output (the rising edge a count after
;      it, four periods summed; the falling one 25 later); IC1
;      free-running (the period, 100),
;      every fourth rising edge (400); ICI 01: no interrupt at the first
;      capture, one at the second; the FIFO overflowed (ICOV) and read
;      empty (ICOV, ICBNE cleared); OC1 driving RD1 (PORTD) high, low
;      with OCINV, letting it go to its pull-up with OCTRIS, off; IC1's
;      and OC1's flags;
;   Q  SCCP5's PWM (period CCP5PRL + 1 = 50, CCP5RA to CCP5RB) on RP24,
;      captured by SCCP4 from its ICM4 pin (RD1, which is RP24): the
;      pulse's width (10) and the period; CCP5's flag (the pulse's end) and CCT5's (its period); the
;      FIFO overflowed (ICOV) and discarded by clearing ICOV; CCP6's
;      triggered timer: held at 0, CCPTRIG set by TRSET, CCT6's flag at
;      its period, held again by TRCLR.
        .equ U1MODE, 0x398
        .equ U1STA, 0x39a
        .equ U1TXREG, 0x39c
        .equ TBLPAG, 0x54
        .equ IFS0, 0x88
        .equ IFS1, 0x8a
        .equ IFS3, 0x8e
        .equ RTCCON1L, 0x1cc
        .equ RTCCON1H, 0x1ce
        .equ RTCCON2L, 0x1d0
        .equ RTCCON2H, 0x1d2
        .equ RTCSTATL, 0x1d8
        .equ TIMEL, 0x1dc
        .equ TIMEH, 0x1de
        .equ DATEL, 0x1e0
        .equ DATEH, 0x1e2
        .equ ALMTIMEL, 0x1e4
        .equ ALMTIMEH, 0x1e6
        .equ TSATIMEH, 0x1ee
        .equ PADCON, 0x65e
        .equ IOCSTAT, 0x660
        .equ TRISA, 0x662
        .equ LATA, 0x666
        .equ ANSA, 0x66a
        .equ IOCPA, 0x66c
        .equ IOCNA, 0x66e
        .equ IOCFA, 0x670
        .equ NVMCON, 0x760
        .equ NVMADRL, 0x762
        .equ NVMADRH, 0x764
        .equ NVMKEY, 0x766
        .equ IEC0, 0x98
        .equ TMR2, 0x196
        .equ PR2, 0x19c
        .equ T2CON, 0x1a0
        .equ I2C1RCV, 0x494
        .equ I2C1TRN, 0x496
        .equ I2C1BRG, 0x498
        .equ I2C1CONL, 0x49a
        .equ I2C1STAT, 0x49e
        .equ DMACON, 0x4c4
        .equ DMAL, 0x4c8
        .equ DMAH, 0x4ca
        .equ DMACH0, 0x4cc
        .equ DMAINT0, 0x4ce
        .equ DMASRC0, 0x4d0
        .equ DMADST0, 0x4d2
        .equ DMACNT0, 0x4d4
        .equ DMACH1, 0x4d6
        .equ DMAINT1, 0x4d8
        .equ DMASRC1, 0x4da
        .equ DMADST1, 0x4dc
        .equ DMACNT1, 0x4de
        .equ IFS2, 0x8c
        .equ IC1CON1, 0x200
        .equ IC1CON2, 0x202
        .equ IC1BUF, 0x204
        .equ IC2CON1, 0x208
        .equ IC2CON2, 0x20a
        .equ IC2BUF, 0x20c
        .equ OC1CON1, 0x230
        .equ OC1CON2, 0x232
        .equ OC1RS, 0x234
        .equ OC1R, 0x236
        .equ CCP4CON1L, 0x300
        .equ CCP4STATL, 0x30c
        .equ CCP4BUFL, 0x320
        .equ CCP5CON1L, 0x324
        .equ CCP5PRL, 0x338
        .equ CCP5RAL, 0x33c
        .equ CCP5RBL, 0x340
        .equ CCP6CON1L, 0x348
        .equ CCP6CON1H, 0x34a
        .equ CCP6STATL, 0x354
        .equ CCP6TMRL, 0x358
        .equ CCP6PRL, 0x35c
        .equ PORTD, 0x6a0
        .equ IOCPUD, 0x6ae
        .equ RPINR7, 0x79e
        .equ RPOR12, 0x7ec
        .equ SRC, 0x1000
        .equ SRC2, 0x1002
        .equ SRC4, 0x1004
        .equ SRC6, 0x1006
        .equ DST, 0x1100
        .equ DST6, 0x1106
        .equ DST8, 0x1108
        .equ BDST, 0x1110
        .equ BDST2, 0x1112
        .equ STACK, 0x1800
        .org 0
        goto start
        .fill 126, trap
        .org 0x100
start:  mov #STACK, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA

; ---- interrupt-on-change ----------------------------------------------------
        mov #'I', w1
        rcall putc
        mov #0, w0
        mov w0, ANSA
        mov w0, LATA
        mov #0xfffe, w0
        mov w0, TRISA                   ; RA0 an output, at 0
        mov #1, w0
        mov w0, IOCPA                   ; its rising edge
        mov #0x8000, w0
        mov w0, PADCON                  ; IOCON
        bset LATA, #0                   ; up
        mov IOCFA, w0
        rcall print
        mov IOCSTAT, w0
        rcall print
        mov IFS1, w0
        .word 0xb20080                  ; and #8, w0: IOC's flag
        rcall print
        mov #0, w0
        mov w0, IOCFA                   ; cleared
        mov IOCSTAT, w0
        rcall print
        mov #0xffff, w0
        mov w0, IOCFA                   ; ones: not set by the program
        mov IOCFA, w0
        rcall print
        mov #0, w0
        mov w0, IOCPA
        mov #1, w0
        mov w0, IOCNA                   ; the falling edge
        bclr LATA, #0                   ; down
        mov IOCFA, w0
        rcall print
        rcall newline

; ---- the flash controller ----------------------------------------------------
        mov #'N', w1
        rcall putc
        mov #0xfa, w0
        mov w0, TBLPAG                  ; the write latches
        mov #0, w6
        mov #0x3456, w0
        mov #0x0012, w1
        .word 0xbb0b00                  ; tblwtl w0, [w6]
        .word 0xbb8b01                  ; tblwth w1, [w6]
        mov #2, w6
        mov #0xcdef, w0
        mov #0x00ab, w1
        .word 0xbb0b00                  ; tblwtl w0, [w6]
        .word 0xbb8b01                  ; tblwth w1, [w6]
        mov #0x8000, w0
        mov w0, NVMADRL
        mov #0, w0
        mov w0, NVMADRH
        mov #0x4001, w0
        mov w0, NVMCON                  ; WREN, two words
        mov #0x55, w0
        mov w0, NVMKEY
        mov #0xaa, w0
        mov w0, NVMKEY
        bset NVMCON, #15                ; WR
        nop
        nop
        rcall readback
        mov NVMCON, w0
        rcall print
        mov IFS0, w0
        lsr w0, #15, w0                 ; NVM's flag
        rcall print
        bset NVMCON, #15                ; WR, not unlocked: WRERR
        mov NVMCON, w0
        rcall print
        mov #0x4003, w0
        mov w0, NVMCON                  ; WREN, a page's erase
        mov #0x55, w0
        mov w0, NVMKEY
        mov #0xaa, w0
        mov w0, NVMKEY
        bset NVMCON, #15
        nop
        nop
        mov #0, w0
        mov w0, TBLPAG
        mov #0x8000, w2
        .word 0xba0012                  ; tblrdl [w2], w0
        rcall print
        .word 0xba8012                  ; tblrdh [w2], w0
        rcall print
        rcall newline

; ---- the RTCC ---------------------------------------------------------------
        mov #'R', w1
        rcall putc
        mov #0x0033, w0
        mov w0, RTCCON2L                ; the system clock, 1:256
        mov #15, w0
        mov w0, RTCCON2H                ; DIV: half a second is 2048 Tcy
        mov #0x2359, w0
        mov w0, TIMEH
        mov #0x5800, w0
        mov w0, TIMEL                   ; 23:59:58
        mov #0x3105, w0
        mov w0, DATEL                   ; the 31st, a Friday
        mov #0x9912, w0
        mov w0, DATEH                   ; December 2099
        mov #0x0100, w0
        mov w0, ALMTIMEL                ; the alarm at second 01
        mov #0x8300, w0
        mov w0, RTCCON1H                ; ALRMEN, each minute, no repeat
        mov #0x8000, w0
        mov w0, RTCCON1L                ; RTCEN
wait:   btss IFS3, #14                  ; RTCC's flag
        bra wait
        mov TIMEH, w0
        rcall print
        mov TIMEL, w0
        rcall print
        mov DATEL, w0
        rcall print
        mov DATEH, w0
        rcall print
        mov RTCSTATL, w0
        rcall print
        mov RTCCON1H, w0
        rcall print
        bset RTCCON1L, #11              ; WRLOCK
        mov #0x1234, w0
        mov w0, TIMEH                   ; ignored
        mov TIMEH, w0
        rcall print
        bclr RTCCON1L, #11              ; not without the unlock
        mov RTCCON1L, w0
        rcall print
        mov #0x55, w0
        mov w0, NVMKEY
        mov #0xaa, w0
        mov w0, NVMKEY
        bclr RTCCON1L, #11              ; with it
        mov RTCCON1L, w0
        rcall print
        mov #0x0008, w0
        mov w0, RTCSTATL                ; a timestamp; ALMEVT cleared
        mov RTCSTATL, w0
        .word 0xb20280                  ; and #0x28, w0: ALMEVT, TSAEVT
        rcall print
        mov TSATIMEH, w0
        rcall print
        rcall newline

; ---- the DMA controller ------------------------------------------------------
        mov #'D', w1
        rcall putc
        mov #0x1111, w0
        mov w0, SRC
        mov #0x2222, w0
        mov w0, SRC2
        mov #0x3333, w0
        mov w0, SRC4
        mov #0x4444, w0
        mov w0, SRC6
        mov #0x0800, w0
        mov w0, DMAL
        mov #0x17ff, w0
        mov w0, DMAH                    ; RAM to the stack's top
        mov #0x8000, w0
        mov w0, DMACON                  ; DMAEN
        mov #SRC, w0
        mov w0, DMASRC0
        mov #DST, w0
        mov w0, DMADST0
        mov #4, w0
        mov w0, DMACNT0
        mov #0x0059, w0
        mov w0, DMACH0                  ; words, both incremented, continuous, CHEN
        bset DMACH0, #8                 ; CHREQ
        mov DST, w0
        rcall print
        mov DST6, w0
        rcall print
        mov DMACNT0, w0
        rcall print
        mov DMAINT0, w0
        rcall print
        mov DMACH0, w0
        rcall print
        mov #0, w0
        mov w0, DMAINT0
        mov #0x10ff, w0
        mov w0, DMAH                    ; DST past it
        mov #SRC, w0
        mov w0, DMASRC0
        mov #DST8, w0
        mov w0, DMADST0
        mov #1, w0
        mov w0, DMACNT0
        mov #0x0059, w0
        mov w0, DMACH0
        bset DMACH0, #8
        mov DMAINT0, w0
        rcall print
        mov DMACNT0, w0
        rcall print
        mov DST8, w0
        rcall print
        mov #0x17ff, w0
        mov w0, DMAH
        mov #SRC, w0
        mov w0, DMASRC1
        mov #BDST, w0
        mov w0, DMADST1
        mov #3, w0
        mov w0, DMACNT1
        mov #0x6200, w0
        mov w0, DMAINT1                 ; CHSEL: Timer2
        mov #0x0053, w0
        mov w0, DMACH1                  ; bytes, both incremented, one-shot, CHEN
        bclr IFS0, #7
        mov #0, w0
        mov w0, TMR2
        mov #400, w0
        mov w0, PR2
        mov #0x8000, w0
        mov w0, T2CON                   ; Timer2 on, 1:1
t2wait: btss IFS0, #7                   ; its first period
        bra t2wait
        mov DMACNT1, w0                 ; one byte moved
        rcall print
dwait:  btss DMAINT1, #5                ; DONEIF
        bra dwait
        mov #0, w0
        mov w0, T2CON
        mov BDST, w0
        rcall print
        mov BDST2, w0
        rcall print
        mov DMACNT1, w0
        rcall print
        mov DMAINT1, w0
        rcall print
        mov DMACH1, w0
        rcall print
        mov #0x4010, w2
        mov IFS0, w0
        .word 0x600002                  ; and w0, w2, w0: DMA1's, DMA0's flags
        rcall print
        rcall newline

; ---- I2C1 --------------------------------------------------------------------
        mov #'C', w1
        rcall putc
        mov #10, w0
        mov w0, I2C1BRG                 ; a bit is 12 Tcy
        mov #0x8000, w0
        mov w0, I2C1CONL                ; I2CEN
        bset I2C1CONL, #0               ; SEN
swait:  btsc I2C1CONL, #0
        bra swait
        mov I2C1STAT, w0
        rcall print
        mov IFS1, w0
        .word 0xb20020                  ; and #2, w0: MI2C1's flag
        rcall print
        mov #0xa0, w0
        mov w0, I2C1TRN                 ; an address, to write
        mov I2C1STAT, w0
        rcall print
        mov #0x55, w0
        mov w0, I2C1TRN                 ; while it goes: a collision
txwait: btsc I2C1STAT, #14              ; TRSTAT
        bra txwait
        mov I2C1STAT, w0
        rcall print
        bclr I2C1STAT, #7               ; IWCOL cleared
        bset I2C1CONL, #3               ; RCEN
rxwait: btsc I2C1CONL, #3
        bra rxwait
        mov I2C1STAT, w0
        rcall print
        bset I2C1CONL, #3               ; again, RBF still set
rxwai2: btsc I2C1CONL, #3
        bra rxwai2
        mov I2C1STAT, w0
        rcall print
        mov I2C1RCV, w0
        rcall print
        mov I2C1STAT, w0
        rcall print
        bset I2C1CONL, #2               ; PEN
pwait:  btsc I2C1CONL, #2
        bra pwait
        mov I2C1STAT, w0
        rcall print
        rcall newline

; ---- output compare into input capture ---------------------------------------
        mov #'P', w1
        rcall putc
        mov #0x1818, w0
        mov w0, RPINR7                  ; IC1R, IC2R: RP24
        mov #13, w0
        mov w0, RPOR12                  ; RP24: OC1
        mov #0x1c01, w0
        mov w0, IC1CON1                 ; the instruction clock, every edge
        mov #10, w0
        mov w0, OC1R
        mov #20, w0
        mov w0, OC1RS
        mov #0x001f, w0
        mov w0, OC1CON2                 ; its own sync: period OC1RS + 1
        mov #0x1c04, w0
        mov w0, OC1CON1                 ; a single pulse, OC1R to OC1RS
        rcall cap1
        mov w0, w4
        rcall cap1
        .word 0x500004                  ; sub w0, w4, w0: the width
        rcall print
        repeat #200
        nop
        mov IC1CON1, w0                 ; no second pulse: ICBNE clear
        rcall print
        mov #0x0001, w0
        mov w0, IC1CON2                 ; synchronized to OC1's sync output
        mov w0, IC2CON2
        mov #0x1c03, w0
        mov w0, IC1CON1                 ; rising edges
        mov #0x1c02, w0
        mov w0, IC2CON1                 ; falling edges
        mov #25, w0
        mov w0, OC1R
        mov #99, w0
        mov w0, OC1RS
        bclr IFS0, #2                   ; the pulse's flag cleared
        mov #0x1c06, w0
        mov w0, OC1CON1                 ; edge-aligned PWM
        rcall cap2                      ; a period, then both drained
        rcall drain
        rcall cap1                      ; the rising edge: 1 after the sync,
        mov w0, w4                      ; each period (four of them summed)
        rcall cap1
        .word 0x400204                  ; add w0, w4, w4
        rcall cap1
        .word 0x400204
        rcall cap1
        .word 0x400204
        mov w4, w0
        rcall print
        rcall cap2
        rcall print                     ; the falling edge: 25 later
        mov #0, w0
        mov w0, IC1CON2                 ; free-running
        rcall drain
        rcall cap1
        mov w0, w4
        rcall cap1
        .word 0x500004                  ; sub w0, w4, w0: the period
        rcall print
        mov #0x1c04, w0
        mov w0, IC1CON1                 ; every fourth rising edge
        rcall cap1
        mov w0, w4
        rcall cap1
        .word 0x500004
        rcall print
        mov #0x1c23, w0
        mov w0, IC1CON1                 ; rising, interrupt every second
        bclr IFS0, #1
w1st:   btss IC1CON1, #3                ; the first capture
        bra w1st
        mov IFS0, w0
        .word 0xb20020                  ; and #2, w0: no interrupt yet
        rcall print
        repeat #100
        nop                             ; the second: the interrupt
        mov IFS0, w0
        .word 0xb20020
        rcall print
ovwait: btss IC1CON1, #4                ; ICOV
        bra ovwait
        mov IC1CON1, w0
        rcall print
        mov IC1BUF, w0
        mov IC1BUF, w0
        mov IC1BUF, w0
        mov IC1BUF, w0                  ; read empty
        mov IC1CON1, w0
        rcall print
        mov #0x8000, w0
        mov w0, OC1R                    ; past the period: no match
        bset IOCPUD, #1                 ; RD1's pull-up
        mov #0x1c02, w0
        mov w0, OC1CON1                 ; single compare, starting high
        mov PORTD, w0
        .word 0xb20020                  ; and #2, w0: RD1, which is RP24
        rcall print
        bset OC1CON2, #12               ; OCINV: low
        mov PORTD, w0
        .word 0xb20020
        rcall print
        bset OC1CON2, #5                ; OCTRIS: let go, the pull-up's
        mov PORTD, w0
        .word 0xb20020
        rcall print
        bclr OC1CON2, #5                ; driven low again
        mov #0, w0
        mov w0, OC1CON1                 ; off: let go too
        mov PORTD, w0
        .word 0xb20020
        rcall print
        bclr OC1CON2, #12
        bclr IOCPUD, #1
        mov #0, w0
        mov w0, OC1CON1
        mov w0, IC1CON1
        mov w0, IC2CON1
        mov IFS0, w0
        .word 0xb20060                  ; and #6, w0: OC1's, IC1's flags
        rcall print
        rcall newline

; ---- the CCPs -----------------------------------------------------------------
        mov #'Q', w1
        rcall putc
        mov #17, w0
        mov w0, RPOR12                  ; RP24: OCM5
        mov #0x8013, w0
        mov w0, CCP4CON1L               ; capture, every edge
        mov #49, w0
        mov w0, CCP5PRL
        mov #0, w0
        mov w0, CCP5RAL
        mov #10, w0
        mov w0, CCP5RBL
        mov #0x8005, w0
        mov w0, CCP5CON1L               ; buffered dual edge compare: PWM
        rcall cap4
        mov w0, w4
        rcall cap4
        .word 0x500004                  ; sub w0, w4, w0: the pulse's width
        rcall print
        rcall cap4
        .word 0x500004                  ; the period
        rcall print
        mov IFS1, w0
        .word 0xb20400                  ; and #0x40, w0: CCP5's flag
        rcall print
        mov #0x8000, w2
        mov IFS2, w0
        .word 0x600002                  ; and w0, w2, w0: CCT5's flag
        rcall print
ov4:    btss CCP4STATL, #1              ; ICOV
        bra ov4
        mov #0, w0
        mov w0, CCP5CON1L
        mov CCP4STATL, w0
        rcall print
        bclr CCP4STATL, #1              ; ICOV cleared: the FIFO discarded
        mov CCP4STATL, w0
        rcall print
        mov #20, w0
        mov w0, CCP6PRL
        mov #0x0080, w0
        mov w0, CCP6CON1H               ; TRIGEN
        mov #0x8000, w0
        mov w0, CCP6CON1L               ; a timer
        nop
        mov CCP6TMRL, w0                ; held
        rcall print
        bset CCP6STATL, #6              ; TRSET
        mov CCP6STATL, w0
        rcall print
ct6:    btss IFS3, #0                   ; CCT6's flag
        bra ct6
        mov IFS3, w0
        .word 0xb20010                  ; and #1, w0
        rcall print
        bset CCP6STATL, #5              ; TRCLR
        nop
        mov CCP6TMRL, w0
        rcall print
        mov #0, w0
        mov w0, CCP6CON1L
        mov w0, CCP4CON1L
        rcall newline
        mov #'.', w1
        rcall putc
        rcall newline
        reset

readback:
        mov #0, w0
        mov w0, TBLPAG
        mov #0x8000, w2
        .word 0xba0012                  ; tblrdl [w2], w0
        rcall print
        .word 0xba8012                  ; tblrdh [w2], w0
        rcall print
        mov #0x8002, w2
        .word 0xba0012                  ; tblrdl [w2], w0
        rcall print
        .word 0xba8012                  ; tblrdh [w2], w0
        rcall print
        return

cap1:   btss IC1CON1, #3                ; IC1's next capture, in W0
        bra cap1
        mov IC1BUF, w0
        return
cap2:   btss IC2CON1, #3                ; IC2's
        bra cap2
        mov IC2BUF, w0
        return
cap4:   btss CCP4STATL, #0              ; SCCP4's
        bra cap4
        mov CCP4BUFL, w0
        return
drain:  btss IC1CON1, #3                ; IC1's and IC2's FIFOs emptied
        bra drain2
        mov IC1BUF, w0
        bra drain
drain2: btss IC2CON1, #3
        return
        mov IC2BUF, w0
        bra drain2

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
