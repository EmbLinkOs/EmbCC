; dspic33-dsp.s -- the dsPIC33E core beyond the reference manual's examples,
; for tests/golden/embsim-dspic33.sh, each by its instruction's or register's
; description (DS70000157G; the dsPIC33F family reference manual's CPU and
; data memory sections, DS70204, DS70202). Generated (gen-dsp-own.py) and
; assembled by pic24-asm.awk; each .word is the instruction its comment
; names. A line a part, on UART1:
;
;   I  out of reset: W15 at the RAM's start (0x1000), CORCON.SATDW, DSRPAG
;      and DSWPAG 1;
;   F  the PIC24E's flow: CALL.L (the SFA bit in the return address's bit 0,
;      the routine running without it, RETURN restoring it, ULNK
;      clearing it), the computed BRA, GOTO and RCALL's encodings, GOTO.L,
;      CALL.L to 0x012000;
;      CPBEQ taken over two words, CPBNE not taken, CPBLT.B, CPSEQ (an
;      offset of 1) over a two-word GOTO; BRA OA and SB taken, OB and SA
;      not; REPEAT #0x4000 and of W1
;      = 0x8001 (15 and 16 bits); CP with the 8-bit literal 0xC8; MOVPAG
;      to DSWPAG and TBLPAG;
;   D  DO loops: two nested (12 passes, DL and DA back to 0); four nested
;      with a fifth DO in the innermost loop, the soft trap each time (16)
;      with INTCON3.DOOVR; CORCON.EDT and a DCOUNT written 0 ending a
;      loop at its pass's end; DOSTART read-only, DOEND written (bit 0
;      0);
;   S  the saturation logic: ACCA + 1 from 0x7FFFFFFFFF with saturation
;      disabled (0x8000000000: OA and SA, the sign lost), super (kept:
;      OA, SA), normal from 0x007FFFFFFF (kept; SA, no OA); SA sticky
;      through an operation that does not saturate, cleared with SAB; OA
;      written; CLR clearing OA and SA; -1.0 * -1.0 (0x0080000000
;      unsaturated with OA, 0x007FFFFFFF normal with SA);
;   R  rounding and stores: SAC.R of 0x0012348000 convergent (0x1234) and
;      conventional (0x1235), of 0x0012358000 convergent (0x1236); the
;      write-back rounded as RND says; SAC without SATDW (0x8918), with
;      it of 0x0090000000 (0x7FFF);
;   M  the multiplier: unsigned (CORCON.US 01: 0xFFFF * 0xFFFF), mixed
;      (10: W4 unsigned, W5 signed), MUL.SS into ACCA fractional; LAC
;      shifting left;
;   T  math error traps: SFTAC A, #17 (SFTACERR, ACCA unchanged), an
;      overflow with OVATE (OVAERR), a catastrophic one with COVTE
;      (COVAERR), the same overflow without them, and with them and the
;      accumulator saturating (no trap);
;   A  the address generators: the Y modulo buffer stepped by a
;      prefetch, an X prefetch from Y space and a Y one from X space and
;      from the DMA RAM (0), XMODSRT's bit 0 0 and XMODEND's 1, an X
;      modulo [W1+W2] read past the end, the X
;      prefetch's register in the X buffer, a byte buffer's last byte
;      and past it, bit-reversed addressing left
;      alone by a byte write, a post-decrement and a read.
        .equ U1MODE, 0x220
        .equ U1STA, 0x222
        .equ U1TXREG, 0x224
        .equ STACK, 0x7000
        .org 0
        goto start
        .fill 254, trap
        .org 0x0c
        .word math                      ; the math error trap
        .org 0x10
        .word soft                      ; the soft trap
        .org 0x200
start:  mov w15, w9                 ; out of reset: W15, CORCON, the pages
        mov 0x44, w10
        mov 0x32, w11
        mov 0x34, w12
        mov #STACK, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA
        mov #'I', w1
        rcall putc
        mov w9, w0
        rcall print
        mov w10, w0
        rcall print
        mov w11, w0
        rcall print
        mov w12, w0
        rcall print
        rcall newline
        mov #0, w0
        mov w0, 0x44
; ---- F: the PIC24E's flow
        mov #'F', w1
        rcall putc
        .word 0xfa0000        ; LNK #0: CORCON.SFA
        mov #lcall, w4
        mov #0, w5
        .word 0x01a804        ; CALL.L W4
        mov 0x44, w0                ; SFA back
        mov w0, w10
        .word 0xfa8000        ; ULNK
        mov 0x44, w0
        mov w0, w11
        mov w8, w0
        rcall print
        mov w9, w0
        rcall print
        mov w10, w0
        rcall print
        mov w11, w0
        rcall print
        mov #0, w5
        mov #2, w7
        .word 0x010607        ; BRA W7 (the PIC24E's)
        mov #1, w5
        mov #2, w5
        mov #lgoto, w7
        .word 0x010407        ; GOTO W7 (the PIC24E's)
        mov #3, w5
lgoto:  add #0x10, w5
        mov #lgotol, w2
        mov #0, w3
        .word 0x019c02        ; GOTO.L W2
        mov #0x0bad, w5
lgotol: mov w5, w0
        rcall print
        mov #0x2000, w4
        mov #1, w5
        mov #0, w12
        .word 0x01a804        ; CALL.L W4 (0x012000)
        mov w12, w0
        rcall print
        mov #lrcall, w6
        mov #lrcall_from, w7
        .word 0x530307        ; SUB W6, W7, W6
        lsr w6, #1, w6
        .word 0x010206        ; RCALL W6 (the PIC24E's)
lrcall_from:
        mov w12, w0                 ; set by the RCALL's routine
        rcall print
        mov #0x1234, w0
        mov #0x1234, w1
        mov #0, w5
        .word 0xe78021        ; CPBEQ W0, W1, +2
        mov #1, w5
        mov #2, w5
        .word 0xe70021        ; CPBNE W0, W1, +2
        add #4, w5
        mov #0x00ff, w2
        mov #0x0001, w3
        .word 0xe69423        ; CPBLT.B W2, W3, +2 (0xFF < 0x01, signed)
        add #8, w5
        add #0, w5
        .word 0xe78011        ; CPSEQ W0, W1 (the offset 1): over the GOTO's two words
        goto lbad
        add #0x10, w5
        mov w5, w0
        rcall print
        mov #0, w5
        mov #0x8000, w0
        mov w0, 0x42
        .word 0x0c0001        ; BRA OA, +1 (OA set: taken)
        add #1, w5
        .word 0x0d0001        ; BRA OB, +1 (not taken)
        add #2, w5
        .word 0x0e0001        ; BRA SA, +1 (not taken)
        add #4, w5
        mov #0x1000, w0
        mov w0, 0x42
        .word 0x0f0001        ; BRA SB, +1 (SB set: taken)
        add #8, w5
        mov #0x0000, w0
        mov w0, 0x42
        mov w5, w0
        rcall print
        mov #0, w0
        .word 0x094000        ; REPEAT #0x4000
        .word 0xe80000        ; INC W0, W0
        rcall print
        mov #0, w0
        mov #0x8001, w1
        .word 0x098001        ; REPEAT W1
        .word 0xe80000        ; INC W0, W0
        rcall print
        mov #0x00c8, w4
        mov #0, w0
        mov w0, 0x42
        .word 0xe12368        ; CP W4, #0xC8
        mov 0x42, w0
        rcall print
        .word 0xfec5ff        ; MOVPAG #0x1FF, DSWPAG
        .word 0xfec8ab        ; MOVPAG #0xAB, TBLPAG
        mov 0x34, w0
        rcall print
        mov 0x54, w0
        rcall print
        mov #1, w2
        .word 0xfed402        ; MOVPAG W2, DSWPAG
        mov 0x34, w0
        rcall print
        rcall newline
; ---- D: DO loops
        mov #'D', w1
        rcall putc
        mov #0, w0
        .word 0x080002        ; DO #2, outer (three passes)
        .word 0x000003        ; ... its end three words on
        .word 0x080003        ; DO #3, inner (four passes)
        .word 0x000000        ; ... its end the next word
        .word 0xe80000        ; INC W0, W0 (the inner loop's only instruction)
        nop                         ; the outer loop's end
        mov 0x44, w6
        mov 0x42, w7
        mov w0, w0
        rcall print
        mov w6, w0
        rcall print
        mov w7, w0
        rcall print
        mov #0, w0
        mov w0, 0x1f00
        mov w0, 0x1f04
        .word 0x080001        ; DO #1, level 1
        .word 0x00000b        ; ... its end
        .word 0x080001        ; DO #1, level 2
        .word 0x000008        ; ... its end
        .word 0x080001        ; DO #1, level 3
        .word 0x000005        ; ... its end
        .word 0x080001        ; DO #1, level 4
        .word 0x000002        ; ... its end
        .word 0x080000        ; DO #0: the fifth, past the DO stack
        .word 0x000000        ; ... (not taken: the soft trap)
        .word 0xe80000        ; INC W0, W0 (level 4's end)
        nop                         ; level 3's end
        nop                         ; level 2's end
        nop                         ; level 1's end
        rcall print
        mov 0x1f00, w0
        rcall print
        mov 0x1f04, w0
        rcall print
        mov 0x44, w0
        rcall print
        mov #0, w0
        .word 0x080009        ; DO #9
        .word 0x000004        ; ... its end four words on
        .word 0xe80000        ; INC W0, W0
        cp w0, #3
        bra nz, ledt
        bset 0x44, #11              ; CORCON.EDT
ledt:   nop                         ; the loop's end
        rcall print
        mov #0, w0
        .word 0x080009        ; DO #9
        .word 0x000005        ; ... its end five words on
        .word 0xe80000        ; INC W0, W0
        cp w0, #2
        bra nz, ldc
        mov #0, w1
        mov w1, 0x38                ; DCOUNT
ldc:    nop                         ; the loop's end
        mov 0x42, w7
        mov w0, w0
        rcall print
        mov w7, w0
        rcall print
        mov #0x5554, w0
        mov w0, 0x3a
        mov #0x1235, w0
        mov w0, 0x3e
        mov 0x3a, w0
        rcall print
        mov 0x3e, w0
        rcall print
        rcall newline
; ---- S: the saturation logic
        mov #'S', w1
        rcall putc
        mov #0x0000, w0
        mov w0, 0x44
        mov #0xffff, w0
        mov w0, 0x22
        mov #0xffff, w0
        mov w0, 0x24
        mov #0x007f, w0
        mov w0, 0x26
        mov #0x0001, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, 0x42
        .word 0xcb0000        ; ADD A
        mov 0x42, w0
        rcall print
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0090, w0
        mov w0, 0x44
        mov #0xffff, w0
        mov w0, 0x22
        mov #0xffff, w0
        mov w0, 0x24
        mov #0x007f, w0
        mov w0, 0x26
        mov #0x0001, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, 0x42
        .word 0xcb0000        ; ADD A
        mov 0x42, w0
        rcall print
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0080, w0
        mov w0, 0x44
        mov #0xffff, w0
        mov w0, 0x22
        mov #0x7fff, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0001, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, 0x42
        .word 0xcb0000        ; ADD A
        mov 0x42, w0
        rcall print
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        .word 0xcb0000        ; ADD A (no saturation, SA kept)
        mov 0x42, w7
        mov #0xfe00, w6
        .word 0x638386        ; AND W7, W6, W7
        mov w7, w0
        rcall print
        bclr 0x42, #10              ; SAB
        mov 0x42, w7
        mov #0xfe00, w6
        .word 0x638386        ; AND W7, W6, W7
        mov w7, w0
        rcall print
        mov #0x8000, w0
        mov w0, 0x42
        mov 0x42, w0
        rcall print
        mov #0xa000, w0
        mov w0, 0x42
        .word 0xc30112        ; CLR A (OA, SA cleared)
        mov 0x42, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x0000, w0
        mov w0, 0x42
        mov #0x8000, w4
        mov #0x8000, w5
        .word 0xc00113        ; MPY W4*W5, A
        mov 0x42, w0
        rcall print
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0080, w0
        mov w0, 0x44
        mov #0x0000, w0
        mov w0, 0x42
        .word 0xc00113        ; MPY W4*W5, A (normal saturation)
        mov 0x42, w0
        rcall print
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        rcall newline
; ---- R: rounding and stores
        mov #'R', w1
        rcall putc
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x8000, w0
        mov w0, 0x22
        mov #0x1234, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xcd0003        ; SAC.R A, W3
        mov w3, w0
        rcall print
        mov #0x0002, w0
        mov w0, 0x44
        mov #0x8000, w0
        mov w0, 0x22
        mov #0x1234, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xcd0003        ; SAC.R A, W3
        mov w3, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x8000, w0
        mov w0, 0x22
        mov #0x1235, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xcd0003        ; SAC.R A, W3
        mov w3, w0
        rcall print
        mov #0x0002, w0
        mov w0, 0x44
        mov #0x8000, w0
        mov w0, 0x22
        mov #0x1234, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xc78110        ; MOVSAC B, W13 (ACCA written back)
        mov w13, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x8000, w0
        mov w0, 0x22
        mov #0x1234, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xc78110        ; MOVSAC B, W13 (ACCA written back)
        mov w13, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0xc891, w0
        mov w0, 0x2a
        mov #0x00ff, w0
        mov w0, 0x2c
        .word 0xcc8603        ; SAC B, #-4, W3 (no SATDW)
        mov w3, w0
        rcall print
        mov #0x0020, w0
        mov w0, 0x44
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x9000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xcc0003        ; SAC A, W3 (SATDW: 0x0090000000 past 1.15)
        mov w3, w0
        rcall print
        rcall newline
; ---- M: the multiplier
        mov #'M', w1
        rcall putc
        mov #0xffff, w4
        mov #0xffff, w5
        mov #0x1001, w0
        mov w0, 0x44
        .word 0xc00113        ; MPY W4*W5, A (US 01, integer)
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x2001, w0
        mov w0, 0x44
        .word 0xc00113        ; MPY W4*W5, A (US 10: W4 unsigned, W5 signed)
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x4000, w0
        mov #0x4000, w1
        .word 0xb98701        ; MUL.SS W0, W1, A (fractional)
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0123, w0
        .word 0xca8600        ; LAC W0, #-4, B (shifted left)
        mov 0x2c, w0
        rcall print
        mov 0x2a, w0
        rcall print
        mov 0x28, w0
        rcall print
        rcall newline
; ---- T: math error traps
        mov #'T', w1
        rcall putc
        mov #0x0000, w0
        mov w0, 0x1f00
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x5678, w0
        mov w0, 0x22
        mov #0x1234, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xc80051        ; SFTAC A, #17
        mov 0x1f00, w0
        rcall print
        mov 0x1f02, w0
        rcall print
        mov 0x26, w0
        rcall print
        mov 0x24, w0
        rcall print
        mov 0x22, w0
        rcall print
        mov #0x0400, w0
        mov w0, 0x8c0
        mov #0xffff, w0
        mov w0, 0x22
        mov #0x7fff, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0001, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        .word 0xcb0000        ; ADD A (into the guard bits, OVATE)
        mov 0x1f00, w0
        rcall print
        mov 0x1f02, w0
        rcall print
        mov #0x0100, w0
        mov w0, 0x8c0
        mov #0xffff, w0
        mov w0, 0x22
        mov #0xffff, w0
        mov w0, 0x24
        mov #0x007f, w0
        mov w0, 0x26
        .word 0xcb0000        ; ADD A (past bit 39, COVTE)
        mov 0x1f00, w0
        rcall print
        mov 0x1f02, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x8c0
        mov #0xffff, w0
        mov w0, 0x22
        mov #0xffff, w0
        mov w0, 0x24
        mov #0x007f, w0
        mov w0, 0x26
        .word 0xcb0000        ; ADD A (past bit 39, no trap enabled)
        mov 0x1f00, w0
        rcall print
        mov #0x0100, w0
        mov w0, 0x8c0
        mov #0x0090, w0
        mov w0, 0x44
        mov #0xffff, w0
        mov w0, 0x22
        mov #0xffff, w0
        mov w0, 0x24
        mov #0x007f, w0
        mov w0, 0x26
        .word 0xcb0000        ; ADD A (saturated: no catastrophic overflow)
        mov #0x0400, w0
        mov w0, 0x8c0
        mov #0x0080, w0
        mov w0, 0x44
        mov #0xffff, w0
        mov w0, 0x22
        mov #0x7fff, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        .word 0xcb0000        ; ADD A (saturated: no overflow)
        mov #0x0000, w0
        mov w0, 0x8c0
        mov #0x0000, w0
        mov w0, 0x44
        mov #0x0000, w0
        mov w0, 0x42
        mov 0x1f00, w0
        rcall print
        rcall newline
; ---- A: the address generators
        mov #'A', w1
        rcall putc
        mov #0x1111, w0
        mov w0, 0x9000
        mov #0x2222, w0
        mov w0, 0x9002
        mov #0x3333, w0
        mov w0, 0x9004
        mov #0x4444, w0
        mov w0, 0x9006
        mov #0x9000, w0
        mov w0, 0x4c
        mov #0x9007, w0
        mov w0, 0x4e
        mov #0x4faf, w0
        mov w0, 0x46
        mov #0x9000, w10
        .word 0xc70106        ; MOVSAC A, [W10]+=2, W4
        .word 0xc70106        ; MOVSAC A, [W10]+=2, W4
        .word 0xc70106        ; MOVSAC A, [W10]+=2, W4
        .word 0xc70106        ; MOVSAC A, [W10]+=2, W4
        .word 0xc70106        ; MOVSAC A, [W10]+=2, W4
        mov w4, w0
        rcall print
        mov w10, w0
        rcall print
        mov #0x0fff, w0
        mov w0, 0x46
        mov #0x5555, w0
        mov w0, 0x2000
        mov #0x9000, w8
        mov #0x2000, w10
        .word 0xc71802        ; MOVSAC A, [W8], W5, [W10], W6 (each the other space)
        mov w5, w0
        rcall print
        mov w6, w0
        rcall print
        mov #0x6666, w0
        mov w0, 0xd000
        mov #0xd000, w10
        .word 0xc70902        ; MOVSAC A, [W10], W6 (the DMA RAM: X space)
        mov w6, w0
        rcall print
        mov #0x7777, w0
        mov w0, 0x1102
        mov #0x1101, w0
        mov w0, 0x48
        mov #0x110e, w0
        mov w0, 0x4a
        mov 0x48, w0
        rcall print
        mov 0x4a, w0
        rcall print
        mov #0x8ff1, w0
        mov w0, 0x46
        mov #0x110c, w1
        mov #6, w2
        .word 0x7901e1        ; MOV [W1+W2], W3 (0x1112: 0x1102)
        mov w3, w0
        rcall print
        mov #0x8ff8, w0
        mov w0, 0x46
        mov #0x110e, w8
        .word 0xc70052        ; MOVSAC A, [W8]+=2, W4
        mov w8, w0
        rcall print
        mov #0x1103, w0
        mov w0, 0x4a
        mov #0x8ff1, w0
        mov w0, 0x46
        mov #0x1102, w1
        .word 0x785880        ; MOV.B W0, [W1++] (to 0x1103, the last byte)
        mov w1, w0
        rcall print
        .word 0x785880        ; MOV.B W0, [W1++] (past it: 0x1100)
        mov w1, w0
        rcall print
        mov #0x01ff, w0
        mov w0, 0x46
        mov #0x8008, w0
        mov w0, 0x50
        mov #0x1300, w1
        .word 0x785880        ; MOV.B W0, [W1++] (a byte: not reversed)
        mov w1, w0
        rcall print
        .word 0x785080        ; MOV.B W0, [W1--]
        .word 0x781080        ; MOV W0, [W1--] (not reversed)
        mov w1, w0
        rcall print
        .word 0x781880        ; MOV W0, [W1++] (reversed: every bit under XB 8 set)
        mov w1, w0
        rcall print
        .word 0x780131        ; MOV [W1++], W2 (a read: not reversed)
        mov w1, w0
        rcall print
        mov #0x0000, w0
        mov w0, 0x46
        mov #0x0000, w0
        mov w0, 0x50
        rcall newline
        mov #'.', w1
        rcall putc
        rcall newline
done:   bra done
lbad:   mov #'X', w1
        rcall putc
        reset
lcall:  mov 0x44, w8
        mov w15, w9
        .word 0x5484e4        ; SUB W9, #4, W9
        .word 0x780499        ; MOV [W9], W9 (the return address's low word)
        mov #1, w0
        .word 0x648480        ; AND W9, W0, W9
        return
lrcall: mov #0x00aa, w12
        return
math:   push w0                     ; the math error trap: INTCON1, counted
        mov 0x8c0, w0
        mov w0, 0x1f02
        mov 0x1f00, w0
        add #1, w0
        mov w0, 0x1f00
        mov #0, w0
        mov w0, 0x8c0
        pop w0
        .word 0x064000        ; RETFIE
soft:   push w0                     ; the soft trap: INTCON3, counted
        mov 0x8c4, w0
        mov w0, 0x1f04
        mov 0x1f00, w0
        add #1, w0
        mov w0, 0x1f00
        mov #0, w0
        mov w0, 0x8c4
        pop w0
        .word 0x064000        ; RETFIE
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
trap:   mov #'T', w1           ; another trap: "T" and the end
        rcall putc
        reset
        .org 0x2000                 ; where CALL.L W4 would go without W5
        goto lbad
        .org 0x12000
        mov #0x00f0, w12            ; CALL.L's routine past 64K
        return
