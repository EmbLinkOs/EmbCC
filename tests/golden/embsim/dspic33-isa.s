; dspic33-isa.s -- the dsPIC33E core of EmbSim's PIC24 against the examples
; of Microchip's "16-Bit MCU and DSC Programmer's Reference Manual"
; (DS70000157G) and of the data memory section of the dsPIC33F family
; reference manual (DS70202B), for tests/golden/embsim-dspic33.sh, run on
; the dsPIC33EP512MU810. Generated: each case sets the example's "Before"
; registers, memory, SR, CORCON and accumulators, executes the instruction
; (a .word: its encoding by the manual's tables), and prints what the
; example's "After" lists, which dspic33-isa.txt holds as the manual gives
; it (an accumulator as ACCxU, sign-extended, ACCxH and ACCxL). Data the
; manual places below this part's RAM (0x1000) is moved up 0x1000, and the
; Y prefetches' data into Y space (0x9000-0xCFFF) up 0x8000, as each case
; says; the products the manual gives as integers are run with CORCON.IF;
; DC is not compared. A DO is placed at the manual's address; its loop's
; first instruction calls the printing, which ends the loop with
; CORCON.EDT. Left out, as they contradict their own instruction's
; description: LAC [W4++],#-3,B (ACCBU FF of a positive word), MUL.UU
; W4,W0,B (FF of a product zero-extended), DIVF W0,W1 (Wn W1, outside its
; operands), DO W7,ENDA (DCOUNT 14 bits: the dsPIC33E's is 16), the CPBxx
; examples (SR changed by instructions that change none), GOTO.L (W15
; changed), ULNK of 0x0800 (below this part's RAM), CP.B W4,#0x12 with
; an 8-bit literal (C of 0x11 - 0x12). MUL.US Wb,Ws,Acc is
; encoded as MUL.US Wb,Ws,Wnd is (bit 15 set; the manual gives it
; MUL.UU's). Assembled by pic24-asm.awk into an Intel HEX image; UART1
; prints.
        .equ U1MODE, 0x220
        .equ U1STA, 0x222
        .equ U1TXREG, 0x224
        .equ SR, 0x42
        .equ CORCON, 0x44
        .equ SAVE, 0x1e00
        .equ STACK, 0x7000
        .equ NODC, 1
        .org 0
        goto start
        .fill 254, trap
        .org 0x4000                ; past the DO cases' addresses
start:  mov #STACK, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA
; ---- 1: MAC W4*W5, A, [W8]+=6, W4, [W10]+=2, W5 (data at 0x1A00 for 0x0A00, Y at 0x9800 for 0x1800)
        .case 1
        mov #0x2567, w0
        mov w0, 0x1a00
        mov #0x909c, w0
        mov w0, 0x9800
        mov #0x00c0, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x1200, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xa022, w4
        mov #0xb900, w5
        mov #0x1a00, w8
        mov #0x9800, w10
        .word 0xc004c6        ; MAC W4*W5, A, [W8]+=6, W4, [W10]+=2, W5
        .dump SAVE
        .show SAVE, sr, w4, w5, w8, w10, 0x26, 0x24, 0x22, 0x1a00, 0x9800, 0x44
; ---- 2: MAC W4*W5, A, [W8]-=2, W4, [W10]+=2, W5, W13 (as above; ACCB not compared)
        .case 2
        mov #0x5bbe, w0
        mov w0, 0x1a00
        mov #0xc967, w0
        mov w0, 0x9800
        mov #0x00d0, w0
        mov w0, CORCON
        mov #0x2000, w0
        mov w0, 0x22
        mov #0x5000, w0
        mov w0, 0x24
        mov #0x0023, w0
        mov w0, 0x26
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x1000, w4
        mov #0x3000, w5
        mov #0x1a00, w8
        mov #0x9800, w10
        mov #0x2000, w13
        .word 0xc005c4        ; MAC W4*W5, A, [W8]-=2, W4, [W10]+=2, W5, W13
        .dump SAVE
        .show SAVE, sr, w4, w5, w8, w10, w13, 0x26, 0x24, 0x22, 0x1a00, 0x9800, 0x44
; ---- 3: MAC W4*W4, B, [W9+W12], W4, [W10]-=2, W5 (X at 0x1C00 for 0x0C00, Y at 0x9900 for 0x1900)
        .case 3
        mov #0xa230, w0
        mov w0, 0x1c20
        mov #0x650b, w0
        mov w0, 0x9900
        mov #0x00c0, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x28
        mov #0x2000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0xa022, w4
        mov #0xb200, w5
        mov #0x1c00, w9
        mov #0x9900, w10
        mov #0x0020, w12
        .word 0xf0871c        ; MAC W4*W4, B, [W9+W12], W4, [W10]-=2, W5
        .dump SAVE
        .show SAVE, sr, w4, w5, w9, w10, w12, 0x2c, 0x2a, 0x28, 0x1c20, 0x9900, 0x44
; ---- 4: MAC W7*W7, A, [W11]-=2, W7 (Y at 0xA000 for 0x2000)
        .case 4
        mov #0x23ff, w0
        mov w0, 0xa000
        mov #0x00d0, w0
        mov w0, CORCON
        mov #0x4500, w0
        mov w0, 0x22
        mov #0x9834, w0
        mov w0, 0x24
        mov #0x00fe, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x76ae, w7
        mov #0xa000, w11
        .word 0xf30d3c        ; MAC W7*W7, A, [W11]-=2, W7
        .dump SAVE
        .show SAVE, sr, w7, w11, 0x26, 0x24, 0x22, 0xa000, 0x44
; ---- 5: MOVSAC B, [W9], W6, [W11]+=4, W7, W13 (X at 0x1800 for 0x0800, Y at 0x9900 for 0x1900)
        .case 5
        mov #0x7811, w0
        mov w0, 0x1800
        mov #0xb2af, w0
        mov w0, 0x9900
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x5968, w0
        mov w0, 0x22
        mov #0x3290, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xa022, w6
        mov #0xb200, w7
        mov #0x1800, w9
        mov #0x9900, w11
        mov #0x0020, w13
        .word 0xc7ae28        ; MOVSAC B, [W9], W6, [W11]+=4, W7, W13
        .dump SAVE
        .show SAVE, sr, w6, w7, w9, w11, w13, 0x26, 0x24, 0x22, 0x1800, 0x9900
; ---- 6: MOVSAC A, [W9]-=2, W4, [W11+W12], W6, [W13]+=2 (Y at 0xA000 for 0x2000)
        .case 6
        mov #0xbb00, w0
        mov w0, 0x1200
        mov #0x52ce, w0
        mov w0, 0xa024
        mov #0x23ff, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x4500, w0
        mov w0, 0x28
        mov #0x9834, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x76ae, w4
        mov #0x2000, w6
        mov #0x1200, w9
        mov #0xa000, w11
        mov #0x0024, w12
        mov #0x2300, w13
        .word 0xc70bf1        ; MOVSAC A, [W9]-=2, W4, [W11+W12], W6, [W13]+=2
        .dump SAVE
        .show SAVE, sr, w4, w6, w9, w11, w12, w13, 0x2c, 0x2a, 0x28, 0x1200, 0xa024, 0x2300
; ---- 7: MPY W4*W5, A, [W8]+=2, W6, [W10]-=2, W7 (Y at 0xA400 for 0x2400)
        .case 7
        mov #0x671f, w0
        mov w0, 0x1780
        mov #0xe3dc, w0
        mov w0, 0xa400
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x2087, w0
        mov w0, 0x22
        mov #0xf780, w0
        mov w0, 0x24
        mov #0x00ff, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xc000, w4
        mov #0x9000, w5
        mov #0x0800, w6
        mov #0xb200, w7
        mov #0x1780, w8
        mov #0xa400, w10
        .word 0xc02c5f        ; MPY W4*W5, A, [W8]+=2, W6, [W10]-=2, W7
        .dump SAVE
        .show SAVE, sr, w4, w5, w6, w7, w8, w10, 0x26, 0x24, 0x22, 0x1780, 0xa400, 0x44
; ---- 8: MPY W6*W7, B, [W8]+=2, W4, [W10]-=2, W5 (Y at 0xA3FE for 0x23FE)
        .case 8
        mov #0x8fdc, w0
        mov w0, 0x1782
        mov #0x0078, w0
        mov w0, 0xa3fe
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x4500, w0
        mov w0, 0x28
        mov #0x9834, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0xc000, w4
        mov #0x9000, w5
        mov #0x671f, w6
        mov #0xe3dc, w7
        mov #0x1782, w8
        mov #0xa3fe, w10
        .word 0xc6845f        ; MPY W6*W7, B, [W8]+=2, W4, [W10]-=2, W5
        .dump SAVE
        .show SAVE, sr, w4, w5, w6, w7, w8, w10, 0x2c, 0x2a, 0x28, 0x1782, 0xa3fe, 0x44
; ---- 9: MPY W6*W6, A, [W9]+=2, W6 (X at 0x1900 for 0x0900)
        .case 9
        mov #0xb865, w0
        mov w0, 0x1900
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0908, w0
        mov w0, 0x22
        mov #0x7c80, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x6500, w6
        mov #0x1900, w9
        .word 0xf22251        ; MPY W6*W6, A, [W9]+=2, W6
        .dump SAVE
        .show SAVE, sr, w6, w9, 0x26, 0x24, 0x22, 0x1900, 0x44
; ---- 10: MPY W4*W4, B, [W9+W12], W4, [W10]+=2, W5 (Y at 0x9B00 for 0x1B00)
        .case 10
        mov #0x8911, w0
        mov w0, 0x1600
        mov #0xf678, w0
        mov w0, 0x9b00
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x4500, w0
        mov w0, 0x28
        mov #0x9834, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0xe228, w4
        mov #0x9000, w5
        mov #0x1700, w9
        mov #0x9b00, w10
        mov #0xff00, w12
        .word 0xf08705        ; MPY W4*W4, B, [W9+W12], W4, [W10]+=2, W5
        .dump SAVE
        .show SAVE, sr, w4, w5, w9, w10, w12, 0x2c, 0x2a, 0x28, 0x1600, 0x9b00, 0x44
; ---- 11: MPY.N W4*W5, A, [W8]+=2, W4, [W10]+=2, W5 (integer; X at 0x1B00 for 0x0B00, Y at 0xA000 for 0x2000)
        .case 11
        mov #0x0054, w0
        mov w0, 0x1b00
        mov #0x660a, w0
        mov w0, 0xa000
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x2387, w0
        mov w0, 0x22
        mov #0x0000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x3023, w4
        mov #0x1290, w5
        mov #0x1b00, w8
        mov #0xa000, w10
        .word 0xc04447        ; MPY.N W4*W5, A, [W8]+=2, W4, [W10]+=2, W5
        .dump SAVE
        .show SAVE, sr, w4, w5, w8, w10, 0x26, 0x24, 0x22, 0x1b00, 0xa000, 0x44
; ---- 12: MPY.N W4*W5, A, [W8]+=2, W4, [W10]+=2, W5 (fractional; as above)
        .case 12
        mov #0x0054, w0
        mov w0, 0x1b00
        mov #0x660a, w0
        mov w0, 0xa000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x2387, w0
        mov w0, 0x22
        mov #0x0000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x3023, w4
        mov #0x1290, w5
        mov #0x1b00, w8
        mov #0xa000, w10
        .word 0xc04447        ; MPY.N W4*W5, A, [W8]+=2, W4, [W10]+=2, W5
        .dump SAVE
        .show SAVE, sr, w4, w5, w8, w10, 0x26, 0x24, 0x22, 0x1b00, 0xa000, 0x44
; ---- 13: MSC W6*W7, A, [W8]-=4, W6, [W10]-=4, W7 (X at 0x1C00 for 0x0C00, Y at 0x9C00 for 0x1C00)
        .case 13
        mov #0xd309, w0
        mov w0, 0x1c00
        mov #0x100b, w0
        mov w0, 0x9c00
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x8000, w0
        mov w0, 0x22
        mov #0x0567, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x9051, w6
        mov #0x7230, w7
        mov #0x1c00, w8
        mov #0x9c00, w10
        .word 0xc66d9a        ; MSC W6*W7, A, [W8]-=4, W6, [W10]-=4, W7
        .dump SAVE
        .show SAVE, sr, w6, w7, w8, w10, 0x26, 0x24, 0x22, 0x1c00, 0x9c00, 0x44
; ---- 14: MSC W4*W5, B, [W11+W12], W5, W13 (Y at 0x9800 for 0x1800)
        .case 14
        mov #0x3579, w0
        mov w0, 0xa000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x5ed0, w0
        mov w0, 0x22
        mov #0x3738, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, 0x28
        mov #0x1000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x0500, w4
        mov #0x2000, w5
        mov #0x9800, w11
        mov #0x0800, w12
        mov #0x6233, w13
        .word 0xc0c530        ; MSC W4*W5, B, [W11+W12], W5, W13
        .dump SAVE
        .show SAVE, sr, w4, w5, w11, w12, w13, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28, 0xa000, 0x44
; ---- 15: CLR A, [W8]+=2, W4, W13
        .case 15
        mov #0x1221, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x2345, w0
        mov w0, 0x22
        mov #0x0067, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x3bdd, w0
        mov w0, 0x28
        mov #0x5420, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0xf001, w4
        mov #0x2000, w8
        mov #0xc623, w13
        .word 0xc30050        ; CLR A, [W8]+=2, W4, W13
        .dump SAVE
        .show SAVE, sr, w4, w8, w13, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28, 0x2000
; ---- 16: CLR B, [W8]+=2, W6, [W10]+=2, W7, [W13]+=2 (Y at 0xB000 for 0x3000)
        .case 16
        mov #0x1221, w0
        mov w0, 0x2000
        mov #0xff80, w0
        mov w0, 0xb000
        mov #0xffc3, w0
        mov w0, 0x4000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x2345, w0
        mov w0, 0x22
        mov #0x0067, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0xabdd, w0
        mov w0, 0x28
        mov #0x5420, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0xf001, w6
        mov #0xc783, w7
        mov #0x2000, w8
        mov #0xb000, w10
        mov #0x4000, w13
        .word 0xc3ac45        ; CLR B, [W8]+=2, W6, [W10]+=2, W7, [W13]+=2
        .dump SAVE
        .show SAVE, sr, w6, w7, w8, w10, w13, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28, 0x2000, 0xb000, 0x4000
; ---- 17: ED W4*W4, A, [W8]+=2, [W10]-=2, W4 (integer; Y at 0xA300 for 0x2300)
        .case 17
        mov #0x007f, w0
        mov w0, 0x1100
        mov #0x0028, w0
        mov w0, 0xa300
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x3d0a, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x009a, w4
        mov #0x1100, w8
        mov #0xa300, w10
        .word 0xf0405f        ; ED W4*W4, A, [W8]+=2, [W10]-=2, W4
        .dump SAVE
        .show SAVE, sr, w4, w8, w10, 0x26, 0x24, 0x22, 0x1100, 0xa300
; ---- 18: ED W5*W5, B, [W9]+=2, [W11+W12], W5 (integer; Y at 0xA500 for 0x2500)
        .case 18
        mov #0x6a7c, w0
        mov w0, 0x1200
        mov #0x2b3d, w0
        mov w0, 0xa508
        mov #0x0001, w0
        mov w0, CORCON
        mov #0xf14c, w0
        mov w0, 0x28
        mov #0x28e3, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x43c2, w5
        mov #0x1200, w9
        mov #0xa500, w11
        mov #0x0008, w12
        .word 0xf1d273        ; ED W5*W5, B, [W9]+=2, [W11+W12], W5
        .dump SAVE
        .show SAVE, sr, w5, w9, w11, w12, 0x2c, 0x2a, 0x28, 0x1200, 0xa508
; ---- 19: EDAC W4*W4, A, [W8]+=2, [W10]-=2, W4 (integer; Y at 0xA300 for 0x2300)
        .case 19
        mov #0x007f, w0
        mov w0, 0x1100
        mov #0x0028, w0
        mov w0, 0xa300
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x3d0a, w0
        mov w0, 0x22
        mov #0x3d0a, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x009a, w4
        mov #0x1100, w8
        mov #0xa300, w10
        .word 0xf0405e        ; EDAC W4*W4, A, [W8]+=2, [W10]-=2, W4
        .dump SAVE
        .show SAVE, sr, w4, w8, w10, 0x26, 0x24, 0x22, 0x1100, 0xa300
; ---- 20: EDAC W5*W5, B, [W9]+=2, [W11+W12], W5 (integer; Y at 0xA500 for 0x2500)
        .case 20
        mov #0x6a7c, w0
        mov w0, 0x1200
        mov #0x2b3d, w0
        mov w0, 0xa508
        mov #0x0001, w0
        mov w0, CORCON
        mov #0xf14c, w0
        mov w0, 0x28
        mov #0x28e3, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x43c2, w5
        mov #0x1200, w9
        mov #0xa500, w11
        mov #0x0008, w12
        .word 0xf1d272        ; EDAC W5*W5, B, [W9]+=2, [W11+W12], W5
        .dump SAVE
        .show SAVE, sr, w5, w9, w11, w12, 0x2c, 0x2a, 0x28, 0x1200, 0xa508
; ---- 21: ADD A
        .case 21
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x3300, w0
        mov w0, 0x22
        mov #0x0022, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x4558, w0
        mov w0, 0x28
        mov #0x1833, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        .word 0xcb0000        ; ADD A
        .dump SAVE
        .show SAVE, sr, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28
; ---- 22: ADD B (super saturation)
        .case 22
        mov #0x00d0, w0
        mov w0, CORCON
        mov #0x2222, w0
        mov w0, 0x22
        mov #0xe111, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x3210, w0
        mov w0, 0x28
        mov #0x7654, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        .word 0xcb8000        ; ADD B
        .dump SAVE
        .show SAVE, sr, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28
; ---- 23: ADD W0, #2, A
        .case 23
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x7000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x8000, w0
        .word 0xc90100        ; ADD W0, #2, A
        .dump SAVE
        .show SAVE, sr, w0, 0x26, 0x24, 0x22
; ---- 24: ADD [W5++], A
        .case 24
        mov #0x5000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x2345, w0
        mov w0, 0x22
        mov #0x0067, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w5
        .word 0xc90035        ; ADD [W5++], A
        .dump SAVE
        .show SAVE, sr, w5, 0x26, 0x24, 0x22, 0x2000
; ---- 25: LAC [--W2], #7, A (saturation disabled)
        .case 25
        mov #0x9108, w0
        mov w0, 0x4000
        mov #0x1221, w0
        mov w0, 0x4002
        mov #0x0000, w0
        mov w0, CORCON
        mov #0xabcd, w0
        mov w0, 0x22
        mov #0x5125, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x4002, w2
        .word 0xca03c2        ; LAC [--W2], #7, A
        .dump SAVE
        .show SAVE, sr, w2, 0x26, 0x24, 0x22, 0x4000, 0x4002
; ---- 26: NEG A (no saturation)
        .case 26
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x59c8, w0
        mov w0, 0x22
        mov #0x3290, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        .word 0xcb1000        ; NEG A
        .dump SAVE
        .show SAVE, sr, 0x26, 0x24, 0x22, 0x44
; ---- 27: NEG B (normal saturation)
        .case 27
        mov #0x00c0, w0
        mov w0, CORCON
        mov #0x10dc, w0
        mov w0, 0x28
        mov #0xf230, w0
        mov w0, 0x2a
        mov #0x00ff, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        .word 0xcb9000        ; NEG B
        .dump SAVE
        .show SAVE, sr, 0x2c, 0x2a, 0x28, 0x44
; ---- 28: SUB A (no saturation; SR not compared: 1100 is neither flag it names)
        .case 28
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x098a, w0
        mov w0, 0x22
        mov #0x120f, w0
        mov w0, 0x24
        mov #0x0076, w0
        mov w0, 0x26
        mov #0xbc17, w0
        mov w0, 0x28
        mov #0xf312, w0
        mov w0, 0x2a
        mov #0x0023, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        .word 0xcb3000        ; SUB A
        .dump SAVE
        .show SAVE, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28
; ---- 29: SUB B (normal saturation)
        .case 29
        mov #0x0040, w0
        mov w0, CORCON
        mov #0x2ee1, w0
        mov w0, 0x22
        mov #0x9022, w0
        mov w0, 0x24
        mov #0x00ff, w0
        mov w0, 0x26
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0x2456, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        .word 0xcbb000        ; SUB B
        .dump SAVE
        .show SAVE, sr, 0x26, 0x24, 0x22, 0x2c, 0x2a, 0x28, 0x44
; ---- 30: SAC A, #4, W5 (SATDW: CORCON 0x0020, the manual's 0x0010 being ACCSAT)
        .case 30
        mov #0x0020, w0
        mov w0, CORCON
        mov #0xff00, w0
        mov w0, 0x22
        mov #0x120f, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xb900, w5
        .word 0xcc0205        ; SAC A, #4, W5
        .dump SAVE
        .show SAVE, sr, w5, 0x26, 0x24, 0x22
; ---- 31: SAC B, #-4, [W5++] (SATDW; ACCB not compared: the manual changes it)
        .case 31
        mov #0x5bbe, w0
        mov w0, 0x2000
        mov #0x0020, w0
        mov w0, CORCON
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0xc891, w0
        mov w0, 0x2a
        mov #0x00ff, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w5
        .word 0xcc8635        ; SAC B, #-4, [W5++]
        .dump SAVE
        .show SAVE, sr, w5, 0x2000
; ---- 32: SAC.R A, #4, W5 (SATDW)
        .case 32
        mov #0x0020, w0
        mov w0, CORCON
        mov #0xff00, w0
        mov w0, 0x22
        mov #0x120f, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xb900, w5
        .word 0xcd0205        ; SAC.R A, #4, W5
        .dump SAVE
        .show SAVE, sr, w5, 0x26, 0x24, 0x22
; ---- 33: SAC.R B, #-4, [W5++] (SATDW)
        .case 33
        mov #0x5bbe, w0
        mov w0, 0x2000
        mov #0x0020, w0
        mov w0, CORCON
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0xf891, w0
        mov w0, 0x2a
        mov #0x00ff, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w5
        .word 0xcd8635        ; SAC.R B, #-4, [W5++]
        .dump SAVE
        .show SAVE, sr, w5, 0x2c, 0x2a, 0x28, 0x2000
; ---- 34: SFTAC A, #12 (SATA)
        .case 34
        mov #0x0080, w0
        mov w0, CORCON
        mov #0xff00, w0
        mov w0, 0x22
        mov #0x120f, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        .word 0xc8004c        ; SFTAC A, #12
        .dump SAVE
        .show SAVE, sr, 0x26, 0x24, 0x22, 0x44
; ---- 35: SFTAC B, #-10 (SATB)
        .case 35
        mov #0x0040, w0
        mov w0, CORCON
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0xfff1, w0
        mov w0, 0x2a
        mov #0x00ff, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        .word 0xc88076        ; SFTAC B, #-10
        .dump SAVE
        .show SAVE, sr, 0x2c, 0x2a, 0x28, 0x44
; ---- 36: SFTAC A, W0 (no saturation)
        .case 36
        mov #0x0000, w0
        mov w0, CORCON
        mov #0xab09, w0
        mov w0, 0x22
        mov #0x320f, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xfffc, w0
        .word 0xc80000        ; SFTAC A, W0
        .dump SAVE
        .show SAVE, sr, w0, 0x26, 0x24, 0x22, 0x44
; ---- 37: SFTAC B, W12 (SATB)
        .case 37
        mov #0x0040, w0
        mov w0, CORCON
        mov #0x8f4c, w0
        mov w0, 0x28
        mov #0xfff1, w0
        mov w0, 0x2a
        mov #0x00ff, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0x000f, w12
        .word 0xc8800c        ; SFTAC B, W12
        .dump SAVE
        .show SAVE, sr, w12, 0x2c, 0x2a, 0x28, 0x44
; ---- 38: MUL.SS W0, W1, A (integer: CORCON.IF)
        .case 38
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x0000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x9823, w0
        mov #0x67dc, w1
        .word 0xb98701        ; MUL.SS W0, W1, A
        .dump SAVE
        .show SAVE, sr, w0, w1, 0x26, 0x24, 0x22
; ---- 39: MUL.SU W8, W9, A (integer)
        .case 39
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x0000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0xf000, w8
        mov #0xf000, w9
        .word 0xb94709        ; MUL.SU W8, W9, A
        .dump SAVE
        .show SAVE, sr, w8, w9, 0x26, 0x24, 0x22
; ---- 40: MUL.SU W8, #0x02, A (integer)
        .case 40
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x0000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x0042, w8
        .word 0xb94762        ; MUL.SU W8, #0x02, A
        .dump SAVE
        .show SAVE, sr, w8, 0x26, 0x24, 0x22
; ---- 41: MUL.US W0, W1, B (integer; W0 not compared: the manual clears it)
        .case 41
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x28
        mov #0x0000, w0
        mov w0, 0x2a
        mov #0x0000, w0
        mov w0, 0x2c
        mov #0x0000, w0
        mov w0, SR
        mov #0xc000, w0
        mov #0xf000, w1
        .word 0xb88781        ; MUL.US W0, W1, B
        .dump SAVE
        .show SAVE, sr, w1, 0x2c, 0x2a, 0x28
; ---- 42: MUL.UU W8, #0x02, A (integer)
        .case 42
        mov #0x0001, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, 0x22
        mov #0x0000, w0
        mov w0, 0x24
        mov #0x0000, w0
        mov w0, 0x26
        mov #0x0000, w0
        mov w0, SR
        mov #0x0042, w8
        .word 0xb84762        ; MUL.UU W8, #0x02, A
        .dump SAVE
        .show SAVE, sr, w8, 0x26, 0x24, 0x22
; ---- 43: MULW.SS W0, W1, W12
        .case 43
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x9823, w0
        mov #0x67dc, w1
        mov #0xffff, w12
        .word 0xb98681        ; MULW.SS W0, W1, W12
        .dump SAVE
        .show SAVE, sr, w0, w1, w12
; ---- 44: MULW.SS W2, [--W4], W0
        .case 44
        mov #0x0098, w0
        mov w0, 0x27fc
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0xffff, w0
        mov #0x0045, w2
        mov #0x27fe, w4
        .word 0xb990c4        ; MULW.SS W2, [--W4], W0
        .dump SAVE
        .show SAVE, sr, w0, w2, w4, 0x27fc
; ---- 45: MULW.SU W8, [W9], W0
        .case 45
        mov #0xf000, w0
        mov w0, 0x178c
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x68dc, w0
        mov #0xf000, w8
        mov #0x178c, w9
        .word 0xb94099        ; MULW.SU W8, [W9], W0
        .dump SAVE
        .show SAVE, sr, w0, w8, w9, 0x178c
; ---- 46: MULW.SU W2, [++W3], W4 (data at 0x1282 for 0x0282)
        .case 46
        mov #0x0068, w0
        mov w0, 0x1282
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x0040, w2
        mov #0x1280, w3
        mov #0x1819, w4
        .word 0xb912d3        ; MULW.SU W2, [++W3], W4
        .dump SAVE
        .show SAVE, sr, w2, w3, w4, 0x1282
; ---- 47: MULW.SU W8, #0x04, W0
        .case 47
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x68dc, w0
        mov #0x1000, w8
        .word 0xb940e4        ; MULW.SU W8, #0x04, W0
        .dump SAVE
        .show SAVE, sr, w0, w8
; ---- 48: MULW.US W0, [W1], W2
        .case 48
        mov #0xf000, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0xc000, w0
        mov #0x2300, w1
        mov #0x00da, w2
        .word 0xb88191        ; MULW.US W0, [W1], W2
        .dump SAVE
        .show SAVE, sr, w0, w1, w2, 0x2300
; ---- 49: MULW.US W6, [W5++], W10 (data at 0x1C00 for 0x0C00)
        .case 49
        mov #0x7fff, w0
        mov w0, 0x1c00
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x1c00, w5
        mov #0xffff, w6
        mov #0x0908, w10
        .word 0xb8b5b5        ; MULW.US W6, [W5++], W10
        .dump SAVE
        .show SAVE, sr, w5, w6, w10, 0x1c00
; ---- 50: MULW.UU W4, W0, W2
        .case 50
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0xffff, w0
        mov #0x2300, w2
        mov #0xffff, w4
        .word 0xb82180        ; MULW.UU W4, W0, W2
        .dump SAVE
        .show SAVE, sr, w0, w2, w4
; ---- 51: MULW.UU W4, #0x04, W2
        .case 51
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x2300, w2
        mov #0x1000, w4
        .word 0xb821e4        ; MULW.UU W4, #0x04, W2
        .dump SAVE
        .show SAVE, sr, w2, w4
; ---- 52: DIVF W8, W9 (after REPEAT #17)
        .case 52
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x8000, w0
        mov #0x1234, w1
        mov #0x1000, w8
        mov #0x4000, w9
        repeat #17
        .word 0xd94009        ; DIVF W8, W9
        .dump SAVE
        .show SAVE, sr, w0, w1, w8, w9
; ---- 53: DIVF W8, W9 (after REPEAT #17; by -1.0)
        .case 53
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x8000, w0
        mov #0x1234, w1
        mov #0x1000, w8
        mov #0x8000, w9
        repeat #17
        .word 0xd94009        ; DIVF W8, W9
        .dump SAVE
        .show SAVE, sr, w0, w1, w8, w9
; ---- 54: DO #5, END6 at 0x002000 (C set)
        .case 54
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0001, w0
        mov w0, SR
        mov #0x6000, w15       ; the DO's calls' stack
        call do54
        mov #STACK, w15
; ---- 55: DO #0x160, END12 at 0x01C000 (N set)
        .case 55
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0008, w0
        mov w0, SR
        mov #0x6000, w15       ; the DO's calls' stack
        call do55
        mov #STACK, w15
; ---- 56: DO W0, END6 at 0x003000 for 0x002000 (SR not compared: 0080 is not DA)
        .case 56
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x0012, w0
        mov #0x6000, w15       ; the DO's calls' stack
        call do56
        mov #STACK, w15
; ---- 57: CP W4, #0x12
        .case 57
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x7713, w4
        .word 0xe12072        ; CP W4, #0x12
        .dump SAVE
        .show SAVE, sr, w4
; ---- 58: CPB.B W4, #0x12 (C set)
        .case 58
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0001, w0
        mov w0, SR
        mov #0x7711, w4
        .word 0xe1a472        ; CPB.B W4, #0x12
        .dump SAVE
        .show SAVE, sr, w4
; ---- 59: CPB.B W4, #0x12
        .case 59
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x7711, w4
        .word 0xe1a472        ; CPB.B W4, #0x12
        .dump SAVE
        .show SAVE, sr, w4
; ---- 60: CPB W12, #0x1F (Z set)
        .case 60
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0002, w0
        mov w0, SR
        mov #0x0020, w12
        .word 0xe1e07f        ; CPB W12, #0x1F
        .dump SAVE
        .show SAVE, sr, w12
; ---- 61: CPB W12, #0x1F (Z, C set)
        .case 61
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0003, w0
        mov w0, SR
        mov #0x0020, w12
        .word 0xe1e07f        ; CPB W12, #0x1F
        .dump SAVE
        .show SAVE, sr, w12
; ---- 62: LNK #0xA0
        .case 62
        mov #0x0000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w14
        mov #0x2000, w15
        .word 0xfa00a0        ; LNK #0xA0
        .dump SAVE
        .show SAVE, sr, w14, w15, 0x2000, 0x44
; ---- 63: ULNK
        .case 63
        mov #0x2000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x2002, w14
        mov #0x20a2, w15
        .word 0xfa8000        ; ULNK
        .dump SAVE
        .show SAVE, sr, w14, w15, 0x2000, 0x44
; ---- 64: MOVPAG #0x02, DSRPAG
        .case 64
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0, w0
        mov w0, 0x32
        mov #0x0000, w0
        mov w0, SR
        .word 0xfec002        ; MOVPAG #0x02, DSRPAG
        .dump SAVE
        .show SAVE, 0x32
        mov #0, w0
        mov w0, 0x46
        mov w0, 0x50
        mov #1, w0
        mov w0, 0x32
; ---- 65: MOVPAG W2, DSRPAG
        .case 65
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0, w0
        mov w0, 0x32
        mov #0x0000, w0
        mov w0, SR
        mov #0x0002, w2
        .word 0xfed002        ; MOVPAG W2, DSRPAG
        .dump SAVE
        .show SAVE, 0x32, w2
        mov #0, w0
        mov w0, 0x46
        mov w0, 0x50
        mov #1, w0
        mov w0, 0x32
; ---- 66: incrementing modulo buffer: 50 words at 0x1100, DO #49 of MOV W0, [W1++] (Figure 3-5)
        .case 66
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x5a5a, w0
        mov w0, 0x1100
        mov w0, 0x1162
        mov w0, 0x1164
        mov #0x1100, w0
        mov w0, 0x48
        mov #0x1163, w0
        mov w0, 0x4a
        mov #0x8001, w0
        mov w0, 0x46
        mov #0x0000, w0
        mov #0x1100, w1
        .word 0x080031
        .word 0x000000
        .word 0x781880
        .dump SAVE
        .show SAVE, w1, 0x1100, 0x1162, 0x1164
        mov #0, w0
        mov w0, 0x46
        mov w0, 0x50
        mov #1, w0
        mov w0, 0x32
; ---- 67: decrementing modulo buffer: 16 words to 0x11FF, DO #15 of MOV W0, [W1--] and DEC W0, W0 (Figure 3-6)
        .case 67
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x5a5a, w0
        mov w0, 0x11de
        mov w0, 0x11e0
        mov #0x11e0, w0
        mov w0, 0x48
        mov #0x11ff, w0
        mov w0, 0x4a
        mov #0x8001, w0
        mov w0, 0x46
        mov #0x000f, w0
        mov #0x11fe, w1
        .word 0x08000f
        .word 0x000001
        .word 0x781080
        dec w0, w0
        .dump SAVE
        .show SAVE, w0, w1, 0x11fe, 0x11fc, 0x11e0, 0x11de
        mov #0, w0
        mov w0, 0x46
        mov w0, 0x50
        mov #1, w0
        mov w0, 0x32
; ---- 68: bit-reversed addressing: REPEAT #15 of MOV [W0++], [W1++], XB 8, from 0x1200 to 0x1300 (Example 3-6)
        .case 68
        mov #0x0000, w0
        mov w0, CORCON
        mov #0x0000, w0
        mov w0, SR
        mov #0x8008, w0
        mov w0, 0x50
        mov #0x01ff, w0
        mov w0, 0x46
        mov #0x1200, w0
        mov #0x1300, w1
        mov #0, w2
        mov #16, w3
fill68:
        .word 0x781802
        .word 0xe80102
        dec w3, w3
        bra nz, fill68
        mov #0x1200, w0
        repeat #15
        .word 0x7818b0
        .dump SAVE
        .show SAVE, w0, w1, 0x1300, 0x1302, 0x1304, 0x1306, 0x1308, 0x131e
        mov #0, w0
        mov w0, 0x46
        mov w0, 0x50
        mov #1, w0
        mov w0, 0x32
        mov #'.', w1
        rcall putc
        mov #10, w1
        rcall putc
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
trap:   mov #'T', w1           ; a trap: "T" and the end
        rcall putc
        reset
; ---- the DO cases, each at its example's address: the loop's first
; instruction calls the printing (DCOUNT as the DO left it), which sets
; CORCON.EDT so this pass is the last
show54: mov w15, 0x7f00
        .dump SAVE
        .show SAVE, sr, 0x38, 0x3c, 0x3a, 0x40, 0x3e, 0x44
        mov 0x7f00, w15
        bset CORCON, #11
        return
show55: mov w15, 0x7f00
        .dump SAVE
        .show SAVE, sr, 0x38, 0x3c, 0x3a, 0x40, 0x3e, 0x44
        mov 0x7f00, w15
        bset CORCON, #11
        return
show56: mov w15, 0x7f00
        .dump SAVE
        .show SAVE, w0, 0x38, 0x3c, 0x3a, 0x40, 0x3e, 0x44
        mov 0x7f00, w15
        bset CORCON, #11
        return
        .org 0x002000
do54:   .word 0x080005        ; DO #5, END6
        .word 0x000003
        call show54
        nop
        nop
        return
        .org 0x01c000
do55:   .word 0x080160        ; DO #0x160, END12
        .word 0x000008
        call show55
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        return
        .org 0x003000
do56:   .word 0x088000        ; DO W0, END6
        .word 0x000006
        call show56
        nop
        nop
        nop
        nop
        nop
        return
