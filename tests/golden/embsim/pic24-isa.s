; pic24-isa.s -- the instruction set of EmbSim's PIC24F core against the
; examples of Microchip's "16-Bit MCU and DSC Programmer's Reference Manual"
; (DS70000157G), for tests/golden/embsim-pic24.sh. Generated: each case sets
; the example's "Before" registers, memory and SR, executes the instruction
; (a .word: its encoding by the manual's tables), and prints SR and what
; the example's "After" lists, which pic24-isa.txt holds as the manual gives
; it. Data the manual places at this part's SFR addresses (0x200, 0x400,
; 0x600, 0x800) is moved to RAM, as each case says; DC is not compared
; (the examples leave it out where the definition sets it). Left out, as
; they contradict their own instruction's description: ADD RAM200,WREG (N
; of 0xCC40), ASR W0,W5,W6 (N of 0xF80F), CP0.B [W4--] (Z of a 0 byte),
; DEC and DEC2 of positive words (C), INC.B of 0xFF (Z), RLC.B W0,W3 and
; RLC [W2++],[W8] (C and the result), RLNC f,WREG (C changed), MOV.B WREG,f
; (SR changed), MUL.SU W2,#0x10,W0 (unsigned), BTSTS.Z W0,#7 (F2BC),
; SL.B 0x909 (0839), SUB.B and SUBB.B 0x1FFF (N), SUBBR W0,#8 (SR 0020),
; XOR.B W4,#0x14,W5 (1234). Assembled by pic24-asm.awk into an Intel HEX
; image; UART1 prints.
        .equ U1MODE, 0x398
        .equ U1STA, 0x39a
        .equ U1TXREG, 0x39c
        .equ SR, 0x42
        .equ TBLPAG, 0x54
        .equ SAVE, 0x1e00
        .equ STACK, 0x7800
        .equ NODC, 1
        .org 0
        goto start
        .fill 126, trap
        .org 0x100
start:  mov #STACK, w15
        mov #0x8000, w0
        mov w0, U1MODE
        mov #0x0400, w0
        mov w0, U1STA
; ---- 1: ADD.B RAM100 (RAM100 at 0x1100)
        .case 1
        mov #0xffc0, w0
        mov w0, 0x1100
        mov #0x0000, w0
        mov w0, SR
        mov #0xcc80, w0
        .word 0xb47100        ; ADD.B RAM100
        .dump SAVE
        .show SAVE, sr, w0, 0x1100
; ---- 2: ADD.B #0xFF, W7
        .case 2
        mov #0x0000, w0
        mov w0, SR
        mov #0x12c0, w7
        .word 0xb04ff7        ; ADD.B #0xFF, W7
        .dump SAVE
        .show SAVE, sr, w7
; ---- 3: ADD #0xFF, W1
        .case 3
        mov #0x0000, w0
        mov w0, SR
        mov #0x12c0, w1
        .word 0xb00ff1        ; ADD #0xFF, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 4: ADD W3, #0x6, [--W4]
        .case 4
        mov #0xddee, w0
        mov w0, 0x0ffe
        mov #0xddee, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0x6006, w3
        mov #0x1000, w4
        .word 0x41a266        ; ADD W3, #0x6, [--W4]
        .dump SAVE
        .show SAVE, sr, w3, w4, 0x0ffe, 0x1000
; ---- 5: ADD.B W5, W6, W7
        .case 5
        mov #0x0000, w0
        mov w0, SR
        mov #0xab00, w5
        mov #0x0030, w6
        mov #0xffff, w7
        .word 0x42c386        ; ADD.B W5, W6, W7
        .dump SAVE
        .show SAVE, sr, w5, w6, w7
; ---- 6: ADD W5, W6, W7
        .case 6
        mov #0x0000, w0
        mov w0, SR
        mov #0xab00, w5
        mov #0x0030, w6
        mov #0xffff, w7
        .word 0x428386        ; ADD W5, W6, W7
        .dump SAVE
        .show SAVE, sr, w7
; ---- 7: ADDC.B RAM100
        .case 7
        mov #0x8006, w0
        mov w0, 0x1100
        mov #0x0001, w0
        mov w0, SR
        mov #0xcc60, w0
        .word 0xb4f100        ; ADDC.B RAM100
        .dump SAVE
        .show SAVE, sr, w0, 0x1100
; ---- 8: ADDC RAM200, WREG
        .case 8
        mov #0x3400, w0
        mov w0, 0x1200
        mov #0x0001, w0
        mov w0, SR
        mov #0x5600, w0
        .word 0xb49200        ; ADDC RAM200, WREG
        .dump SAVE
        .show SAVE, sr, w0, 0x1200
; ---- 9: ADDC.B #0xFF, W7
        .case 9
        mov #0x0000, w0
        mov w0, SR
        mov #0x12c0, w7
        .word 0xb0cff7        ; ADDC.B #0xFF, W7
        .dump SAVE
        .show SAVE, sr, w7
; ---- 10: ADDC #0xFF, W1
        .case 10
        mov #0x0001, w0
        mov w0, SR
        mov #0x12c0, w1
        .word 0xb08ff1        ; ADDC #0xFF, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 11: ADDC.B W0, #0x1F, [W7]
        .case 11
        mov #0xb000, w0
        mov w0, 0x12c0
        mov #0x0000, w0
        mov w0, SR
        mov #0xcc80, w0
        mov #0x12c0, w7
        .word 0x484bff        ; ADDC.B W0, #0x1F, [W7]
        .dump SAVE
        .show SAVE, sr, w7, 0x12c0
; ---- 12: ADDC W3, #0x6, [--W4]
        .case 12
        mov #0xddee, w0
        mov w0, 0x0ffe
        mov #0xddee, w0
        mov w0, 0x1000
        mov #0x0001, w0
        mov w0, SR
        mov #0x6006, w3
        mov #0x1000, w4
        .word 0x49a266        ; ADDC W3, #0x6, [--W4]
        .dump SAVE
        .show SAVE, sr, w4, 0x0ffe, 0x1000
; ---- 13: ADDC.B W0, [W1++], [W2++]
        .case 13
        mov #0xab25, w0
        mov w0, 0x0800
        mov #0xffff, w0
        mov w0, 0x1000
        mov #0x0001, w0
        mov w0, SR
        mov #0xcc20, w0
        mov #0x0800, w1
        mov #0x1000, w2
        .word 0x485931        ; ADDC.B W0, [W1++], [W2++]
        .dump SAVE
        .show SAVE, sr, w1, w2, 0x0800, 0x1000
; ---- 14: ADDC W3, [W2++], [W1++]
        .case 14
        mov #0x8000, w0
        mov w0, 0x1000
        mov #0x2500, w0
        mov w0, 0x2000
        mov #0x0001, w0
        mov w0, SR
        mov #0x1000, w1
        mov #0x2000, w2
        mov #0x0180, w3
        .word 0x4998b2        ; ADDC W3, [W2++], [W1++]
        .dump SAVE
        .show SAVE, sr, w1, w2, 0x1000, 0x2000
; ---- 15: AND.B RAM100
        .case 15
        mov #0xffc0, w0
        mov w0, 0x1100
        mov #0x0000, w0
        mov w0, SR
        mov #0xcc80, w0
        .word 0xb67100        ; AND.B RAM100
        .dump SAVE
        .show SAVE, sr, 0x1100
; ---- 16: AND RAM200, WREG
        .case 16
        mov #0x12c0, w0
        mov w0, 0x1200
        mov #0x0008, w0
        mov w0, SR
        mov #0xcc80, w0
        .word 0xb61200        ; AND RAM200, WREG
        .dump SAVE
        .show SAVE, sr, w0, 0x1200
; ---- 17: AND.B #0x83, W7
        .case 17
        mov #0x0000, w0
        mov w0, SR
        mov #0x12c0, w7
        .word 0xb24837        ; AND.B #0x83, W7
        .dump SAVE
        .show SAVE, sr, w7
; ---- 18: AND #0x333, W1
        .case 18
        mov #0x0000, w0
        mov w0, SR
        mov #0x12d0, w1
        .word 0xb23331        ; AND #0x333, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 19: AND W0, #0x1F, W1
        .case 19
        mov #0x0000, w0
        mov w0, SR
        mov #0x6723, w0
        mov #0x7643, w1
        .word 0x6000ff        ; AND W0, #0x1F, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 20: AND.B W0, W1, [W2++]
        .case 20
        mov #0xffff, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0xaa55, w0
        mov #0x2211, w1
        mov #0x1001, w2
        .word 0x605901        ; AND.B W0, W1, [W2++]
        .dump SAVE
        .show SAVE, sr, w2, 0x1000
; ---- 21: AND W0, [W1++], W2
        .case 21
        mov #0x2634, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0xaa55, w0
        mov #0x1000, w1
        mov #0x55aa, w2
        .word 0x600131        ; AND W0, [W1++], W2
        .dump SAVE
        .show SAVE, sr, w1, w2
; ---- 22: ASR.B RAM400, WREG (RAM400 at 0x1400)
        .case 22
        mov #0x0823, w0
        mov w0, 0x1400
        mov #0x0000, w0
        mov w0, SR
        mov #0x0600, w0
        .word 0xd5d400        ; ASR.B RAM400, WREG
        .dump SAVE
        .show SAVE, sr, w0, 0x1400
; ---- 23: ASR RAM200
        .case 23
        mov #0x8009, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xd5b200        ; ASR RAM200
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 24: ASR.B [W0++], [W1++] (data at 0x1600, 0x1800 for 0x600, 0x800)
        .case 24
        mov #0x2366, w0
        mov w0, 0x1600
        mov #0xffc0, w0
        mov w0, 0x1800
        mov #0x0000, w0
        mov w0, SR
        mov #0x1600, w0
        mov #0x1801, w1
        .word 0xd1d8b0        ; ASR.B [W0++], [W1++]
        .dump SAVE
        .show SAVE, sr, w0, w1, 0x1800
; ---- 25: ASR W12, W13
        .case 25
        mov #0x0000, w0
        mov w0, SR
        mov #0xab01, w12
        mov #0x0322, w13
        .word 0xd1868c        ; ASR W12, W13
        .dump SAVE
        .show SAVE, sr, w13
; ---- 26: ASR W0, #0x4, W1
        .case 26
        mov #0x0000, w0
        mov w0, SR
        mov #0x060f, w0
        mov #0x1234, w1
        .word 0xde80c4        ; ASR W0, #0x4, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 27: ASR W0, #0x6, W1
        .case 27
        mov #0x0000, w0
        mov w0, SR
        mov #0x80ff, w0
        mov #0x0060, w1
        .word 0xde80c6        ; ASR W0, #0x6, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 28: ASR W0, #0xF, W1
        .case 28
        mov #0x0000, w0
        mov w0, SR
        mov #0x70ff, w0
        mov #0xcc26, w1
        .word 0xde80cf        ; ASR W0, #0xF, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 29: ASR W0, W5, W6
        .case 29
        mov #0x0000, w0
        mov w0, SR
        mov #0x6688, w0
        mov #0x000a, w5
        mov #0xff00, w6
        .word 0xde8305        ; ASR W0, W5, W6
        .dump SAVE
        .show SAVE, sr, w6
; ---- 30: ASR W11, W12, W13
        .case 30
        mov #0x0000, w0
        mov w0, SR
        mov #0x8765, w11
        mov #0x88e4, w12
        mov #0xa5a5, w13
        .word 0xdede8c        ; ASR W11, W12, W13
        .dump SAVE
        .show SAVE, sr, w13
; ---- 31: BCLR.B 0x1800, #0x7 (0x800 in the manual)
        .case 31
        mov #0x66ef, w0
        mov w0, 0x1800
        mov #0x0000, w0
        mov w0, SR
        .word 0xa9f800        ; BCLR.B 0x1800, #0x7
        .dump SAVE
        .show SAVE, sr, 0x1800
; ---- 32: BCLR 0x1400, #0x9 (0x400)
        .case 32
        mov #0xaa55, w0
        mov w0, 0x1400
        mov #0x0000, w0
        mov w0, SR
        .word 0xa93401        ; BCLR 0x1400, #0x9
        .dump SAVE
        .show SAVE, sr, 0x1400
; ---- 33: BCLR.B W2, #0x2
        .case 33
        mov #0x0000, w0
        mov w0, SR
        mov #0xf234, w2
        .word 0xa12402        ; BCLR.B W2, #0x2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 34: BCLR [W0++], #0x0
        .case 34
        mov #0x5607, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, SR
        mov #0x2300, w0
        .word 0xa10030        ; BCLR [W0++], #0x0
        .dump SAVE
        .show SAVE, sr, w0, 0x2300
; ---- 35: BSET.B 0x1601, #0x3 (0x601)
        .case 35
        mov #0xf234, w0
        mov w0, 0x1600
        mov #0x0000, w0
        mov w0, SR
        .word 0xa87601        ; BSET.B 0x1601, #0x3
        .dump SAVE
        .show SAVE, sr, 0x1600
; ---- 36: BSET 0x1444, #0xF (0x444)
        .case 36
        mov #0x5604, w0
        mov w0, 0x1444
        mov #0x0000, w0
        mov w0, SR
        .word 0xa8f445        ; BSET 0x1444, #0xF
        .dump SAVE
        .show SAVE, sr, 0x1444
; ---- 37: BSET.B W3, #0x7
        .case 37
        mov #0x0000, w0
        mov w0, SR
        mov #0x0026, w3
        .word 0xa07403        ; BSET.B W3, #0x7
        .dump SAVE
        .show SAVE, sr, w3
; ---- 38: BSET [W4++], #0x0
        .case 38
        mov #0x1734, w0
        mov w0, 0x6700
        mov #0x0000, w0
        mov w0, SR
        mov #0x6700, w4
        .word 0xa00034        ; BSET [W4++], #0x0
        .dump SAVE
        .show SAVE, sr, w4, 0x6700
; ---- 39: BSW.C W2, W3
        .case 39
        mov #0x0002, w0
        mov w0, SR
        mov #0xf234, w2
        mov #0x111f, w3
        .word 0xad1802        ; BSW.C W2, W3
        .dump SAVE
        .show SAVE, sr, w2
; ---- 40: BSW.Z W2, W3
        .case 40
        mov #0x0002, w0
        mov w0, SR
        mov #0xe235, w2
        mov #0x0550, w3
        .word 0xad9802        ; BSW.Z W2, W3
        .dump SAVE
        .show SAVE, sr, w2
; ---- 41: BSW.C [++W0], W6
        .case 41
        mov #0x2380, w0
        mov w0, 0x1002
        mov #0x0001, w0
        mov w0, SR
        mov #0x1000, w0
        mov #0x34a3, w6
        .word 0xad3050        ; BSW.C [++W0], W6
        .dump SAVE
        .show SAVE, sr, w0, 0x1002
; ---- 42: BSW.Z [W1--], W5
        .case 42
        mov #0xc4dd, w0
        mov w0, 0x1000
        mov #0x0001, w0
        mov w0, SR
        mov #0x1000, w1
        mov #0x888b, w5
        .word 0xada821        ; BSW.Z [W1--], W5
        .dump SAVE
        .show SAVE, sr, w1, 0x1000
; ---- 43: BTG.B 0x1001, #0x4
        .case 43
        mov #0xf234, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        .word 0xaa9001        ; BTG.B 0x1001, #0x4
        .dump SAVE
        .show SAVE, sr, 0x1000
; ---- 44: BTG 0x1660, #0x8
        .case 44
        mov #0x5606, w0
        mov w0, 0x1660
        mov #0x0000, w0
        mov w0, SR
        .word 0xaa1661        ; BTG 0x1660, #0x8
        .dump SAVE
        .show SAVE, sr, 0x1660
; ---- 45: BTG W2, #0x0
        .case 45
        mov #0x0000, w0
        mov w0, SR
        mov #0xf234, w2
        .word 0xa20002        ; BTG W2, #0x0
        .dump SAVE
        .show SAVE, sr, w2
; ---- 46: BTG [W0++], #0x0
        .case 46
        mov #0x5606, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, SR
        mov #0x2300, w0
        .word 0xa20030        ; BTG [W0++], #0x0
        .dump SAVE
        .show SAVE, sr, w0, 0x2300
; ---- 47: BTSC.B 0x1201, #2: not skipped
        .case 47
        mov #0x264f, w0
        mov w0, 0x1200
        mov #0, w5
        mov #0x0000, w0
        mov w0, SR
        .word 0xaf5201        ; BTSC.B 0x1201, #2
        goto over47            ; skipped: W5 = 1
        mov #1, w5
over47:
        .dump SAVE
        .show SAVE, sr, w5
; ---- 48: BTSC 0x1804, #14 (0x804): skipped
        .case 48
        mov #0x2647, w0
        mov w0, 0x1804
        mov #0, w5
        mov #0x0000, w0
        mov w0, SR
        .word 0xafd805        ; BTSC 0x1804, #14
        goto over48            ; skipped: W5 = 1
        mov #1, w5
over48:
        .dump SAVE
        .show SAVE, sr, w5
; ---- 49: BTSS [W6++], #0xC: skipped
        .case 49
        mov #0x1000, w0
        mov w0, 0x1800
        mov #0, w5
        mov #0x0000, w0
        mov w0, SR
        mov #0x1800, w6
        .word 0xa6c036        ; BTSS [W6++], #0xC
        goto over49            ; skipped: W5 = 1
        mov #1, w5
over49:
        .dump SAVE
        .show SAVE, sr, w6, w5
; ---- 50: BTST.B 0x1201, #0x3
        .case 50
        mov #0xf7ff, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xab7201        ; BTST.B 0x1201, #0x3
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 51: BTST 0x1302, #0x7
        .case 51
        mov #0xf7ff, w0
        mov w0, 0x1302
        mov #0x0002, w0
        mov w0, SR
        .word 0xabf302        ; BTST 0x1302, #0x7
        .dump SAVE
        .show SAVE, sr, 0x1302
; ---- 52: BTST.C [W0++], #0x3
        .case 52
        mov #0xfff7, w0
        mov w0, 0x1200
        mov #0x0001, w0
        mov w0, SR
        mov #0x1200, w0
        .word 0xa33030        ; BTST.C [W0++], #0x3
        .dump SAVE
        .show SAVE, sr, w0
; ---- 53: BTST.Z W0, #0x7
        .case 53
        mov #0x0000, w0
        mov w0, SR
        mov #0xf234, w0
        .word 0xa37800        ; BTST.Z W0, #0x7
        .dump SAVE
        .show SAVE, sr, w0
; ---- 54: BTST.C W2, W3
        .case 54
        mov #0x0001, w0
        mov w0, SR
        mov #0xf234, w2
        mov #0x2368, w3
        .word 0xa51802        ; BTST.C W2, W3
        .dump SAVE
        .show SAVE, sr
; ---- 55: BTST.Z [W0++], W1
        .case 55
        mov #0x6243, w0
        mov w0, 0x1200
        mov #0x0002, w0
        mov w0, SR
        mov #0x1200, w0
        mov #0xccc0, w1
        .word 0xa58830        ; BTST.Z [W0++], W1
        .dump SAVE
        .show SAVE, sr, w0
; ---- 56: BTSTS.C [W0++], #0x3
        .case 56
        mov #0xfff7, w0
        mov w0, 0x1200
        mov #0x0001, w0
        mov w0, SR
        mov #0x1200, w0
        .word 0xa43030        ; BTSTS.C [W0++], #0x3
        .dump SAVE
        .show SAVE, sr, w0, 0x1200
; ---- 57: CLR.B RAM200
        .case 57
        mov #0x8009, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xef7200        ; CLR.B RAM200
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 58: CLR WREG
        .case 58
        mov #0x0000, w0
        mov w0, SR
        mov #0x0600, w0
        .word 0xef0000        ; CLR WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 59: CLR.B W2
        .case 59
        mov #0x0000, w0
        mov w0, SR
        mov #0x3333, w2
        .word 0xeb4100        ; CLR.B W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 60: CLR [W0++]
        .case 60
        mov #0x5607, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, SR
        mov #0x2300, w0
        .word 0xeb1800        ; CLR [W0++]
        .dump SAVE
        .show SAVE, sr, w0, 0x2300
; ---- 61: COM.B RAM200
        .case 61
        mov #0x80ff, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xeef200        ; COM.B RAM200
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 62: COM RAM400, WREG
        .case 62
        mov #0x0823, w0
        mov w0, 0x1400
        mov #0x0000, w0
        mov w0, SR
        mov #0x1211, w0
        .word 0xee9400        ; COM RAM400, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 63: COM.B [W0++], [W1++]
        .case 63
        mov #0x5607, w0
        mov w0, 0x2300
        mov #0xabcd, w0
        mov w0, 0x2400
        mov #0x0000, w0
        mov w0, SR
        mov #0x2301, w0
        mov #0x2400, w1
        .word 0xead8b0        ; COM.B [W0++], [W1++]
        .dump SAVE
        .show SAVE, sr, w0, w1, 0x2400
; ---- 64: COM W0, [W1++]
        .case 64
        mov #0xaba9, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0xd004, w0
        mov #0x1000, w1
        .word 0xea9880        ; COM W0, [W1++]
        .dump SAVE
        .show SAVE, sr, w1, 0x1000
; ---- 65: CP.B RAM400
        .case 65
        mov #0x0823, w0
        mov w0, 0x1400
        mov #0x0000, w0
        mov w0, SR
        mov #0x8823, w0
        .word 0xe35400        ; CP.B RAM400
        .dump SAVE
        .show SAVE, sr
; ---- 66: CP 0x1200
        .case 66
        mov #0x2277, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        mov #0x2377, w0
        .word 0xe31200        ; CP 0x1200
        .dump SAVE
        .show SAVE, sr
; ---- 67: CP.B W4, #0x12
        .case 67
        mov #0x0000, w0
        mov w0, SR
        mov #0x7711, w4
        .word 0xe12472        ; CP.B W4, #0x12
        .dump SAVE
        .show SAVE, sr
; ---- 68: CP W4, #0x12
        .case 68
        mov #0x0000, w0
        mov w0, SR
        mov #0x7713, w4
        .word 0xe12072        ; CP W4, #0x12
        .dump SAVE
        .show SAVE, sr
; ---- 69: CP.B W0, [W1++]
        .case 69
        mov #0xd004, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0xaba9, w0
        mov #0x2000, w1
        .word 0xe10431        ; CP.B W0, [W1++]
        .dump SAVE
        .show SAVE, sr, w1
; ---- 70: CP W5, W6
        .case 70
        mov #0x0000, w0
        mov w0, SR
        mov #0x2334, w5
        mov #0x8001, w6
        .word 0xe12806        ; CP W5, W6
        .dump SAVE
        .show SAVE, sr
; ---- 71: CP0.B RAM100
        .case 71
        mov #0x44c3, w0
        mov w0, 0x1100
        mov #0x0000, w0
        mov w0, SR
        .word 0xe25100        ; CP0.B RAM100
        .dump SAVE
        .show SAVE, sr
; ---- 72: CP0 0x1FFE
        .case 72
        mov #0x0001, w0
        mov w0, 0x1ffe
        mov #0x0000, w0
        mov w0, SR
        .word 0xe21ffe        ; CP0 0x1FFE
        .dump SAVE
        .show SAVE, sr
; ---- 73: CP0 [--W5]
        .case 73
        mov #0x9000, w0
        mov w0, 0x23fe
        mov #0x0000, w0
        mov w0, SR
        mov #0x2400, w5
        .word 0xe00045        ; CP0 [--W5]
        .dump SAVE
        .show SAVE, sr, w5
; ---- 74: CPB.B RAM400
        .case 74
        mov #0x0823, w0
        mov w0, 0x1400
        mov #0x0000, w0
        mov w0, SR
        mov #0x8823, w0
        .word 0xe3d400        ; CPB.B RAM400
        .dump SAVE
        .show SAVE, sr
; ---- 75: CPB 0x1200
        .case 75
        mov #0x2377, w0
        mov w0, 0x1200
        mov #0x0001, w0
        mov w0, SR
        mov #0x2377, w0
        .word 0xe39200        ; CPB 0x1200
        .dump SAVE
        .show SAVE, sr
; ---- 76: CPB.B W0, [W1++] (Z cleared)
        .case 76
        mov #0xd0a9, w0
        mov w0, 0x1000
        mov #0x0002, w0
        mov w0, SR
        mov #0xaba9, w0
        mov #0x1000, w1
        .word 0xe18431        ; CPB.B W0, [W1++]
        .dump SAVE
        .show SAVE, sr, w1
; ---- 77: CPB.B W0, [W1++] (C set)
        .case 77
        mov #0xd0a9, w0
        mov w0, 0x1000
        mov #0x0001, w0
        mov w0, SR
        mov #0xaba9, w0
        mov #0x1000, w1
        .word 0xe18431        ; CPB.B W0, [W1++]
        .dump SAVE
        .show SAVE, sr, w1
; ---- 78: CPB W4, W5
        .case 78
        mov #0x0001, w0
        mov w0, SR
        mov #0x4000, w4
        mov #0x3000, w5
        .word 0xe1a005        ; CPB W4, W5
        .dump SAVE
        .show SAVE, sr
; ---- 79: CPSEQ W4, W8: skipped
        .case 79
        mov #0, w5
        mov #0x0002, w0
        mov w0, SR
        mov #0x3344, w4
        mov #0x3344, w8
        .word 0xe7a008        ; CPSEQ W4, W8
        goto over79            ; skipped: W5 = 1
        mov #1, w5
over79:
        .dump SAVE
        .show SAVE, sr, w5
; ---- 80: CPSGT.B W0, W1: skipped
        .case 80
        mov #0, w5
        mov #0x0009, w0
        mov w0, SR
        mov #0x00ff, w0
        mov #0x26fe, w1
        .word 0xe60401        ; CPSGT.B W0, W1
        goto over80            ; skipped: W5 = 1
        mov #1, w5
over80:
        .dump SAVE
        .show SAVE, sr, w5
; ---- 81: CPSGT W4, W5: not skipped
        .case 81
        mov #0x0004, w0
        mov w0, SR
        mov #0x2600, w4
        mov #0x2600, w5
        .word 0xe62005        ; CPSGT W4, W5
        goto over81            ; skipped: W5 = 1
        mov #1, w5
over81:
        .dump SAVE
        .show SAVE, sr, w5
; ---- 82: DAW.B W3
        .case 82
        mov #0x0000, w0
        mov w0, SR
        mov #0x77aa, w3
        .word 0xfd4003        ; DAW.B W3
        .dump SAVE
        .show SAVE, sr, w3
; ---- 83: DEC.B 0x1200 (0x200)
        .case 83
        mov #0x80ff, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xed7200        ; DEC.B 0x1200
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 84: DEC W5, [W6++]
        .case 84
        mov #0xaba9, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0xd004, w5
        mov #0x2000, w6
        .word 0xe91b05        ; DEC W5, [W6++]
        .dump SAVE
        .show SAVE, sr, w6, 0x2000
; ---- 85: DEC2.B 0x1200 (0x200)
        .case 85
        mov #0x80ff, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xedf200        ; DEC2.B 0x1200
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 86: DEC2.B [W7--], [W8--]
        .case 86
        mov #0x0107, w0
        mov w0, 0x2300
        mov #0xabcd, w0
        mov w0, 0x2400
        mov #0x0000, w0
        mov w0, SR
        mov #0x2301, w7
        mov #0x2400, w8
        .word 0xe9d427        ; DEC2.B [W7--], [W8--]
        .dump SAVE
        .show SAVE, sr, w7, w8, 0x2400
; ---- 87: DEC2 W5, [W6++]
        .case 87
        mov #0xaba9, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0xd004, w5
        mov #0x1000, w6
        .word 0xe99b05        ; DEC2 W5, [W6++]
        .dump SAVE
        .show SAVE, sr, w6, 0x1000
; ---- 88: REPEAT #17; DIV.S W3, W4
        .case 88
        mov #0x0000, w0
        mov w0, SR
        mov #0x5555, w0
        mov #0x1234, w1
        mov #0x3000, w3
        mov #0x0027, w4
        repeat #17
        .word 0xd80184        ; REPEAT #17; DIV.S W3, W4
        .dump SAVE
        .show SAVE, sr, w0, w1, w3, w4
; ---- 89: REPEAT #17; DIV.SD W0, W12
        .case 89
        mov #0x0000, w0
        mov w0, SR
        mov #0x2500, w0
        mov #0xff42, w1
        mov #0x2200, w12
        repeat #17
        .word 0xd8084c        ; REPEAT #17; DIV.SD W0, W12
        .dump SAVE
        .show SAVE, sr, w0, w1
; ---- 90: EXCH W1, W9
        .case 90
        mov #0x0000, w0
        mov w0, SR
        mov #0x55ff, w1
        mov #0xa3a3, w9
        .word 0xfd0481        ; EXCH W1, W9
        .dump SAVE
        .show SAVE, sr, w1, w9
; ---- 91: FBCL W1, W9 (55FF)
        .case 91
        mov #0x0000, w0
        mov w0, SR
        mov #0x55ff, w1
        mov #0xffff, w9
        .word 0xdf0481        ; FBCL W1, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 92: FBCL W1, W9 (FFFF)
        .case 92
        mov #0x0000, w0
        mov w0, SR
        mov #0xffff, w1
        mov #0xbbbb, w9
        .word 0xdf0481        ; FBCL W1, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 93: FBCL [W1++], W9
        .case 93
        mov #0xff0a, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w1
        mov #0xbbbb, w9
        .word 0xdf04b1        ; FBCL [W1++], W9
        .dump SAVE
        .show SAVE, sr, w1, w9
; ---- 94: FF1L W2, W5
        .case 94
        mov #0x0000, w0
        mov w0, SR
        mov #0x000a, w2
        mov #0xbbbb, w5
        .word 0xcf8282        ; FF1L W2, W5
        .dump SAVE
        .show SAVE, sr, w5
; ---- 95: FF1L [W2++], W5
        .case 95
        mov #0x0000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w2
        mov #0xbbbb, w5
        .word 0xcf82b2        ; FF1L [W2++], W5
        .dump SAVE
        .show SAVE, sr, w2, w5
; ---- 96: FF1R W1, W9
        .case 96
        mov #0x0000, w0
        mov w0, SR
        mov #0x000a, w1
        mov #0xbbbb, w9
        .word 0xcf0481        ; FF1R W1, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 97: FF1R [W1++], W9
        .case 97
        mov #0x8000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w1
        mov #0xbbbb, w9
        .word 0xcf04b1        ; FF1R [W1++], W9
        .dump SAVE
        .show SAVE, sr, w1, w9
; ---- 98: INC 0x1000, WREG
        .case 98
        mov #0x8fff, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0xabcd, w0
        .word 0xec1000        ; INC 0x1000, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 99: INC.B W1, [++W2]
        .case 99
        mov #0xabcd, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0xff7f, w1
        mov #0x2000, w2
        .word 0xe86901        ; INC.B W1, [++W2]
        .dump SAVE
        .show SAVE, sr, w2, 0x2000
; ---- 100: INC W1, W2
        .case 100
        mov #0x0000, w0
        mov w0, SR
        mov #0xff7f, w1
        mov #0x2000, w2
        .word 0xe80101        ; INC W1, W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 101: INC2.B W1, [++W2]
        .case 101
        mov #0xabcd, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0xff7f, w1
        mov #0x2000, w2
        .word 0xe8e901        ; INC2.B W1, [++W2]
        .dump SAVE
        .show SAVE, sr, w2, 0x2000
; ---- 102: INC2 W1, W2
        .case 102
        mov #0x0000, w0
        mov w0, SR
        mov #0xff7f, w1
        mov #0x2000, w2
        .word 0xe88101        ; INC2 W1, W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 103: IOR.B 0x1000
        .case 103
        mov #0xff00, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0x1234, w0
        .word 0xb77000        ; IOR.B 0x1000
        .dump SAVE
        .show SAVE, sr, 0x1000
; ---- 104: IOR 0x1000, WREG
        .case 104
        mov #0x0fab, w0
        mov w0, 0x1000
        mov #0x0008, w0
        mov w0, SR
        mov #0x1234, w0
        .word 0xb71000        ; IOR 0x1000, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 105: IOR.B #0xAA, W9
        .case 105
        mov #0x0000, w0
        mov w0, SR
        mov #0x1234, w9
        .word 0xb34aa9        ; IOR.B #0xAA, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 106: IOR #0x2AA, W4
        .case 106
        mov #0x0000, w0
        mov w0, SR
        mov #0xa34d, w4
        .word 0xb32aa4        ; IOR #0x2AA, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 107: IOR.B W1, #0x5, [W9++]
        .case 107
        mov #0x0000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0xaaaa, w1
        mov #0x2000, w9
        .word 0x70dce5        ; IOR.B W1, #0x5, [W9++]
        .dump SAVE
        .show SAVE, sr, w9, 0x2000
; ---- 108: IOR W1, #0x0, W9
        .case 108
        mov #0x0000, w0
        mov w0, SR
        mov #0x0000, w1
        mov #0xa34d, w9
        .word 0x7084e0        ; IOR W1, #0x0, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 109: IOR.B W1, [W5++], [W9++]
        .case 109
        mov #0x1155, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, 0x2400
        mov #0x0000, w0
        mov w0, SR
        mov #0xaaaa, w1
        mov #0x2000, w5
        mov #0x2400, w9
        .word 0x70dcb5        ; IOR.B W1, [W5++], [W9++]
        .dump SAVE
        .show SAVE, sr, w5, w9, 0x2400
; ---- 110: IOR W1, W5, W9
        .case 110
        mov #0x0000, w0
        mov w0, SR
        mov #0xaaaa, w1
        mov #0x5555, w5
        mov #0xa34d, w9
        .word 0x708485        ; IOR W1, W5, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 111: LNK #0xA0
        .case 111
        mov #0x0000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0x2000, w14
        mov #0x2000, w15
        .word 0xfa00a0        ; LNK #0xA0
        .dump SAVE
        .show SAVE, sr, w14, w15, 0x2000
; ---- 112: LSR.B 0x1600 (0x600)
        .case 112
        mov #0x55ff, w0
        mov w0, 0x1600
        mov #0x0000, w0
        mov w0, SR
        .word 0xd57600        ; LSR.B 0x1600
        .dump SAVE
        .show SAVE, sr, 0x1600
; ---- 113: LSR 0x1600, WREG
        .case 113
        mov #0x55ff, w0
        mov w0, 0x1600
        mov #0x0000, w0
        mov w0, SR
        mov #0x0000, w0
        .word 0xd51600        ; LSR 0x1600, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 114: LSR.B W0, W1
        .case 114
        mov #0x0000, w0
        mov w0, SR
        mov #0xff03, w0
        mov #0x2378, w1
        .word 0xd14080        ; LSR.B W0, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 115: LSR W0, W1
        .case 115
        mov #0x0000, w0
        mov w0, SR
        mov #0x8000, w0
        mov #0x2378, w1
        .word 0xd10080        ; LSR W0, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 116: LSR W4, #14, W5
        .case 116
        mov #0x0000, w0
        mov w0, SR
        mov #0xc800, w4
        mov #0x1200, w5
        .word 0xde22ce        ; LSR W4, #14, W5
        .dump SAVE
        .show SAVE, sr, w5
; ---- 117: LSR W4, #1, W5
        .case 117
        mov #0x0000, w0
        mov w0, SR
        mov #0x0505, w4
        mov #0xf000, w5
        .word 0xde22c1        ; LSR W4, #1, W5
        .dump SAVE
        .show SAVE, sr, w5
; ---- 118: LSR W0, W1, W2
        .case 118
        mov #0x0000, w0
        mov w0, SR
        mov #0xc00c, w0
        mov #0x0001, w1
        mov #0x2390, w2
        .word 0xde0101        ; LSR W0, W1, W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 119: LSR W5, W4, W3
        .case 119
        mov #0x0000, w0
        mov w0, SR
        mov #0xdd43, w3
        mov #0x000c, w4
        mov #0x0800, w5
        .word 0xde2984        ; LSR W5, W4, W3
        .dump SAVE
        .show SAVE, sr, w3
; ---- 120: MOV 0x1800 (SR from it)
        .case 120
        mov #0xb29f, w0
        mov w0, 0x1800
        mov #0x0000, w0
        mov w0, SR
        .word 0xbfb800        ; MOV 0x1800
        .dump SAVE
        .show SAVE, sr, 0x1800
; ---- 121: MOV.B WREG, 0x1801
        .case 121
        mov #0x4509, w0
        mov w0, 0x1800
        mov #0x0000, w0
        mov w0, SR
        mov #0x98f3, w0
        .word 0xb7f801        ; MOV.B WREG, 0x1801
        .dump SAVE
        .show SAVE, 0x1800
; ---- 122: MOV 0x27FE, W3
        .case 122
        mov #0xabcd, w0
        mov w0, 0x27fe
        mov #0x0000, w0
        mov w0, SR
        mov #0x0035, w3
        .word 0x813ff3        ; MOV 0x27FE, W3
        .dump SAVE
        .show SAVE, sr, w3
; ---- 123: MOV W8, 0x1222
        .case 123
        mov #0xfd88, w0
        mov w0, 0x1222
        mov #0x0000, w0
        mov w0, SR
        mov #0xf200, w8
        .word 0x889118        ; MOV W8, 0x1222
        .dump SAVE
        .show SAVE, sr, 0x1222
; ---- 124: MOV.B #0x17, W5
        .case 124
        mov #0x0000, w0
        mov w0, SR
        mov #0x7899, w5
        .word 0xb3c175        ; MOV.B #0x17, W5
        .dump SAVE
        .show SAVE, sr, w5
; ---- 125: MOV.B #0xFE, W9
        .case 125
        mov #0x0000, w0
        mov w0, SR
        mov #0xab23, w9
        .word 0xb3cfe9        ; MOV.B #0xFE, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 126: MOV #-1000, W8
        .case 126
        mov #0x0000, w0
        mov w0, SR
        mov #0x23ff, w8
        .word 0x2fc188        ; MOV #-1000, W8
        .dump SAVE
        .show SAVE, sr, w8
; ---- 127: MOV.B [W8+0x13], W10
        .case 127
        mov #0x3312, w0
        mov w0, 0x101a
        mov #0x0000, w0
        mov w0, SR
        mov #0x1008, w8
        mov #0x4009, w10
        .word 0x905538        ; MOV.B [W8+0x13], W10
        .dump SAVE
        .show SAVE, sr, w10
; ---- 128: MOV [W4+0x3E8], W2
        .case 128
        mov #0x5634, w0
        mov w0, 0x0be8
        mov #0x0000, w0
        mov w0, SR
        mov #0x9088, w2
        mov #0x0800, w4
        .word 0x93b144        ; MOV [W4+0x3E8], W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 129: MOV.B W0, [W1+0x7]
        .case 129
        mov #0x2345, w0
        mov w0, 0x1806
        mov #0x0000, w0
        mov w0, SR
        mov #0x9015, w0
        mov #0x1800, w1
        .word 0x9840f0        ; MOV.B W0, [W1+0x7]
        .dump SAVE
        .show SAVE, sr, 0x1806
; ---- 130: MOV W11, [W1-0x400]
        .case 130
        mov #0xffea, w0
        mov w0, 0x0c00
        mov #0x0000, w0
        mov w0, SR
        mov #0x1000, w1
        mov #0x8813, w11
        .word 0x9c008b        ; MOV W11, [W1-0x400]
        .dump SAVE
        .show SAVE, sr, 0x0c00
; ---- 131: MOV.B [W0--], W4
        .case 131
        mov #0x8988, w0
        mov w0, 0x0a00
        mov #0x0000, w0
        mov w0, SR
        mov #0x0a01, w0
        mov #0x2976, w4
        .word 0x784220        ; MOV.B [W0--], W4
        .dump SAVE
        .show SAVE, sr, w0, w4
; ---- 132: MOV [W6++], [W2+W3]
        .case 132
        mov #0x9870, w0
        mov w0, 0x0840
        mov #0x0690, w0
        mov w0, 0x1228
        mov #0x0000, w0
        mov w0, SR
        mov #0x0800, w2
        mov #0x0040, w3
        mov #0x1228, w6
        .word 0x79b136        ; MOV [W6++], [W2+W3]
        .dump SAVE
        .show SAVE, sr, w6, 0x0840
; ---- 133: MOV.D W2, W6
        .case 133
        mov #0x0000, w0
        mov w0, SR
        mov #0x12fb, w2
        mov #0x9877, w3
        mov #0x9833, w6
        mov #0xfcc6, w7
        .word 0xbe0302        ; MOV.D W2, W6
        .dump SAVE
        .show SAVE, sr, w6, w7
; ---- 134: MOV.D [W7--], W4
        .case 134
        mov #0xa319, w0
        mov w0, 0x0900
        mov #0x9927, w0
        mov w0, 0x0902
        mov #0x0000, w0
        mov w0, SR
        mov #0xb012, w4
        mov #0xfd89, w5
        mov #0x0900, w7
        .word 0xbe0227        ; MOV.D [W7--], W4
        .dump SAVE
        .show SAVE, sr, w4, w5, w7
; ---- 135: MUL.B 0x1800 (0x800)
        .case 135
        mov #0x2690, w0
        mov w0, 0x1800
        mov #0x0000, w0
        mov w0, SR
        mov #0x9823, w0
        mov #0xffff, w2
        mov #0xffff, w3
        .word 0xbc5800        ; MUL.B 0x1800
        .dump SAVE
        .show SAVE, sr, w2, w3
; ---- 136: MUL 0x1802 (TMR1 in the manual)
        .case 136
        mov #0x3287, w0
        mov w0, 0x1802
        mov #0x0000, w0
        mov w0, SR
        mov #0xf001, w0
        mov #0x0000, w2
        mov #0x0000, w3
        .word 0xbc1802        ; MUL 0x1802
        .dump SAVE
        .show SAVE, sr, w2, w3
; ---- 137: MUL.SS W0, W1, W12
        .case 137
        mov #0x0000, w0
        mov w0, SR
        mov #0x9823, w0
        mov #0x67dc, w1
        mov #0xffff, w12
        mov #0xffff, w13
        .word 0xb98601        ; MUL.SS W0, W1, W12
        .dump SAVE
        .show SAVE, sr, w12, w13
; ---- 138: MUL.SS W2, [--W4], W0
        .case 138
        mov #0x0098, w0
        mov w0, 0x27fc
        mov #0x0000, w0
        mov w0, SR
        mov #0xffff, w0
        mov #0xffff, w1
        mov #0x0045, w2
        mov #0x27fe, w4
        .word 0xb99044        ; MUL.SS W2, [--W4], W0
        .dump SAVE
        .show SAVE, sr, w0, w1, w4
; ---- 139: MUL.SU W0, #0x1F, W2
        .case 139
        mov #0x0000, w0
        mov w0, SR
        mov #0xc000, w0
        mov #0x1234, w2
        mov #0xc9ba, w3
        .word 0xb9017f        ; MUL.SU W0, #0x1F, W2
        .dump SAVE
        .show SAVE, sr, w2, w3
; ---- 140: MUL.SU W8, [W9], W0
        .case 140
        mov #0xf000, w0
        mov w0, 0x178c
        mov #0x0000, w0
        mov w0, SR
        mov #0x68dc, w0
        mov #0xaa40, w1
        mov #0xf000, w8
        mov #0x178c, w9
        .word 0xb94019        ; MUL.SU W8, [W9], W0
        .dump SAVE
        .show SAVE, sr, w0, w1
; ---- 141: MUL.SU W2, [++W3], W4
        .case 141
        mov #0x0068, w0
        mov w0, 0x0282
        mov #0x0000, w0
        mov w0, SR
        mov #0x0040, w2
        mov #0x0280, w3
        mov #0x1819, w4
        mov #0x2021, w5
        .word 0xb91253        ; MUL.SU W2, [++W3], W4
        .dump SAVE
        .show SAVE, sr, w3, w4, w5
; ---- 142: MUL.US W0, [W1], W2
        .case 142
        mov #0xf000, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, SR
        mov #0xc000, w0
        mov #0x2300, w1
        mov #0x00da, w2
        mov #0xcc25, w3
        .word 0xb88111        ; MUL.US W0, [W1], W2
        .dump SAVE
        .show SAVE, sr, w2, w3
; ---- 143: MUL.US W6, [W5++], W10
        .case 143
        mov #0x7fff, w0
        mov w0, 0x0c00
        mov #0x0000, w0
        mov w0, SR
        mov #0x0c00, w5
        mov #0xffff, w6
        mov #0x0908, w10
        mov #0x6eeb, w11
        .word 0xb8b535        ; MUL.US W6, [W5++], W10
        .dump SAVE
        .show SAVE, sr, w5, w10, w11
; ---- 144: MUL.UU W0, #0xF, W12
        .case 144
        mov #0x0000, w0
        mov w0, SR
        mov #0x2323, w0
        mov #0x4512, w12
        mov #0x7821, w13
        .word 0xb8066f        ; MUL.UU W0, #0xF, W12
        .dump SAVE
        .show SAVE, sr, w12, w13
; ---- 145: MUL.UU W7, #0x1F, W0
        .case 145
        mov #0x0000, w0
        mov w0, SR
        mov #0x780b, w0
        mov #0x3805, w1
        mov #0xf240, w7
        .word 0xb8387f        ; MUL.UU W7, #0x1F, W0
        .dump SAVE
        .show SAVE, sr, w0, w1
; ---- 146: MUL.UU W4, W0, W2
        .case 146
        mov #0x0000, w0
        mov w0, SR
        mov #0xffff, w0
        mov #0x2300, w2
        mov #0x00da, w3
        mov #0xffff, w4
        .word 0xb82100        ; MUL.UU W4, W0, W2
        .dump SAVE
        .show SAVE, sr, w2, w3
; ---- 147: MUL.UU W0, [W1++], W4
        .case 147
        mov #0xd625, w0
        mov w0, 0x2300
        mov #0x0000, w0
        mov w0, SR
        mov #0x1024, w0
        mov #0x2300, w1
        mov #0x9654, w4
        mov #0xbdbc, w5
        .word 0xb80231        ; MUL.UU W0, [W1++], W4
        .dump SAVE
        .show SAVE, sr, w1, w4, w5
; ---- 148: NEG 0x1200
        .case 148
        mov #0x8923, w0
        mov w0, 0x1200
        mov #0x0000, w0
        mov w0, SR
        .word 0xee3200        ; NEG 0x1200
        .dump SAVE
        .show SAVE, sr, 0x1200
; ---- 149: NEG.B W3, [W4++]
        .case 149
        mov #0x2355, w0
        mov w0, 0x1004
        mov #0x0000, w0
        mov w0, SR
        mov #0x7839, w3
        mov #0x1005, w4
        .word 0xea5a03        ; NEG.B W3, [W4++]
        .dump SAVE
        .show SAVE, sr, w4, 0x1004
; ---- 150: NEG [W2++], [--W4]
        .case 150
        mov #0x870f, w0
        mov w0, 0x0900
        mov #0x5105, w0
        mov w0, 0x1000
        mov #0x0000, w0
        mov w0, SR
        mov #0x0900, w2
        mov #0x1002, w4
        .word 0xea2232        ; NEG [W2++], [--W4]
        .dump SAVE
        .show SAVE, sr, w2, w4, 0x1000
; ---- 151: POP 0x1230
        .case 151
        mov #0xa401, w0
        mov w0, 0x1004
        mov #0x2355, w0
        mov w0, 0x1230
        mov #0x0000, w0
        mov w0, SR
        mov #0x1006, w15
        .word 0xf91230        ; POP 0x1230
        .dump SAVE
        .show SAVE, sr, w15, 0x1230
; ---- 152: POP W4
        .case 152
        mov #0xc45a, w0
        mov w0, 0x1006
        mov #0x0000, w0
        mov w0, SR
        mov #0x0000, w4
        mov #0x1008, w15
        .word 0x78024f        ; POP W4
        .dump SAVE
        .show SAVE, sr, w4, w15
; ---- 153: POP [++W10]
        .case 153
        mov #0xe3e1, w0
        mov w0, 0x0e04
        mov #0xc7b5, w0
        mov w0, 0x1764
        mov #0x0000, w0
        mov w0, SR
        mov #0x0e02, w10
        mov #0x1766, w15
        .word 0x782d4f        ; POP [++W10]
        .dump SAVE
        .show SAVE, sr, w10, w15, 0x0e04
; ---- 154: POP.D W6
        .case 154
        mov #0x3210, w0
        mov w0, 0x084c
        mov #0x7654, w0
        mov w0, 0x084e
        mov #0x0000, w0
        mov w0, SR
        mov #0x07bb, w6
        mov #0x89ae, w7
        mov #0x0850, w15
        .word 0xbe034f        ; POP.D W6
        .dump SAVE
        .show SAVE, sr, w6, w7, w15
; ---- 155: PUSH 0xC0E
        .case 155
        mov #0x0000, w0
        mov w0, 0x0920
        mov #0x67aa, w0
        mov w0, 0x0c0e
        mov #0x0000, w0
        mov w0, SR
        mov #0x0920, w15
        .word 0xf80c0e        ; PUSH 0xC0E
        .dump SAVE
        .show SAVE, sr, w15, 0x0920
; ---- 156: PUSH W2
        .case 156
        mov #0x0000, w0
        mov w0, 0x1566
        mov #0x0000, w0
        mov w0, SR
        mov #0x6889, w2
        mov #0x1566, w15
        .word 0x781f82        ; PUSH W2
        .dump SAVE
        .show SAVE, sr, w15, 0x1566
; ---- 157: PUSH [W5+W10]
        .case 157
        mov #0x216f, w0
        mov w0, 0x0806
        mov #0xb20a, w0
        mov w0, 0x1244
        mov #0x0000, w0
        mov w0, SR
        mov #0x1200, w5
        mov #0x0044, w10
        mov #0x0806, w15
        .word 0x7d1fe5        ; PUSH [W5+W10]
        .dump SAVE
        .show SAVE, sr, w15, 0x0806
; ---- 158: PUSH.D W10
        .case 158
        mov #0x79b5, w0
        mov w0, 0x0c08
        mov #0x008e, w0
        mov w0, 0x0c0a
        mov #0x0000, w0
        mov w0, SR
        mov #0x80d3, w10
        mov #0x4550, w11
        mov #0x0c08, w15
        .word 0xbe9f8a        ; PUSH.D W10
        .dump SAVE
        .show SAVE, sr, w15, 0x0c08, 0x0c0a
; ---- 159: PUSH.S; other W0-W3 and SR; POP.S
        .case 159
        mov #0x0001, w0
        mov w0, SR
        mov #0x0000, w0
        mov #0x1000, w1
        mov #0x2000, w2
        mov #0x3000, w3
        .word 0xfea000         ; PUSH.S
        mov #0x00e0, w0
        mov w0, SR
        mov #0x07bb, w0
        mov #0x03fd, w1
        mov #0x9610, w2
        mov #0x7249, w3
        .word 0xfe8000         ; POP.S
        .dump SAVE
        .show SAVE, sr, w0, w1, w2, w3
; ---- 160: RETLW.B #0xA, W0 (from an RCALL)
        .case 160
        mov #0x0000, w0
        mov w0, SR
        mov #0x9846, w0
        rcall sub160
        .dump SAVE
        .show SAVE, sr, w0
; ---- 161: RETLW #0x230, W2 (from an RCALL)
        .case 161
        mov #0x0000, w0
        mov w0, SR
        mov #0x0993, w2
        rcall sub161
        .dump SAVE
        .show SAVE, sr, w2
; ---- 162: RLC.B 0x1233
        .case 162
        mov #0xe807, w0
        mov w0, 0x1232
        mov #0x0000, w0
        mov w0, SR
        .word 0xd6f233        ; RLC.B 0x1233
        .dump SAVE
        .show SAVE, sr, 0x1232
; ---- 163: RLC 0x1820, WREG (0x820)
        .case 163
        mov #0x216e, w0
        mov w0, 0x1820
        mov #0x0001, w0
        mov w0, SR
        mov #0x5601, w0
        .word 0xd69820        ; RLC 0x1820, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 164: RLNC.B 0x1233
        .case 164
        mov #0xe807, w0
        mov w0, 0x1232
        mov #0x0000, w0
        mov w0, SR
        .word 0xd67233        ; RLNC.B 0x1233
        .dump SAVE
        .show SAVE, sr, 0x1232
; ---- 165: RLNC.B W0, W3
        .case 165
        mov #0x0001, w0
        mov w0, SR
        mov #0x9976, w0
        mov #0x5879, w3
        .word 0xd24180        ; RLNC.B W0, W3
        .dump SAVE
        .show SAVE, sr, w3
; ---- 166: RLNC [W2++], [W8]
        .case 166
        mov #0x3689, w0
        mov w0, 0x094e
        mov #0xc041, w0
        mov w0, 0x2008
        mov #0x0001, w0
        mov w0, SR
        mov #0x2008, w2
        mov #0x094e, w8
        .word 0xd20c32        ; RLNC [W2++], [W8]
        .dump SAVE
        .show SAVE, sr, w2, 0x094e
; ---- 167: RRC.B 0x1233
        .case 167
        mov #0xe807, w0
        mov w0, 0x1232
        mov #0x0000, w0
        mov w0, SR
        .word 0xd7f233        ; RRC.B 0x1233
        .dump SAVE
        .show SAVE, sr, 0x1232
; ---- 168: RRC 0x1820, WREG
        .case 168
        mov #0x216e, w0
        mov w0, 0x1820
        mov #0x0001, w0
        mov w0, SR
        mov #0x5601, w0
        .word 0xd79820        ; RRC 0x1820, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 169: RRC.B W0, W3
        .case 169
        mov #0x0001, w0
        mov w0, SR
        mov #0x9976, w0
        mov #0x5879, w3
        .word 0xd3c180        ; RRC.B W0, W3
        .dump SAVE
        .show SAVE, sr, w3
; ---- 170: RRC [W2++], [W8]
        .case 170
        mov #0x3689, w0
        mov w0, 0x094e
        mov #0xc041, w0
        mov w0, 0x2008
        mov #0x0001, w0
        mov w0, SR
        mov #0x2008, w2
        mov #0x094e, w8
        .word 0xd38c32        ; RRC [W2++], [W8]
        .dump SAVE
        .show SAVE, sr, w2, 0x094e
; ---- 171: RRNC.B 0x1233
        .case 171
        mov #0xe807, w0
        mov w0, 0x1232
        mov #0x0000, w0
        mov w0, SR
        .word 0xd77233        ; RRNC.B 0x1233
        .dump SAVE
        .show SAVE, sr, 0x1232
; ---- 172: RRNC 0x1820, WREG
        .case 172
        mov #0x216e, w0
        mov w0, 0x1820
        mov #0x0001, w0
        mov w0, SR
        mov #0x5601, w0
        .word 0xd71820        ; RRNC 0x1820, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 173: RRNC.B W0, W3
        .case 173
        mov #0x0001, w0
        mov w0, SR
        mov #0x9976, w0
        mov #0x5879, w3
        .word 0xd34180        ; RRNC.B W0, W3
        .dump SAVE
        .show SAVE, sr, w3
; ---- 174: RRNC [W2++], [W8]
        .case 174
        mov #0x3689, w0
        mov w0, 0x094e
        mov #0xc041, w0
        mov w0, 0x2008
        mov #0x0000, w0
        mov w0, SR
        mov #0x2008, w2
        mov #0x094e, w8
        .word 0xd30c32        ; RRNC [W2++], [W8]
        .dump SAVE
        .show SAVE, sr, w2, 0x094e
; ---- 175: SE W3, W4
        .case 175
        mov #0x0000, w0
        mov w0, SR
        mov #0x7839, w3
        mov #0x1005, w4
        .word 0xfb0203        ; SE W3, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 176: SE [W2++], W12
        .case 176
        mov #0x008f, w0
        mov w0, 0x0900
        mov #0x0000, w0
        mov w0, SR
        mov #0x0900, w2
        mov #0x1002, w12
        .word 0xfb0632        ; SE [W2++], W12
        .dump SAVE
        .show SAVE, sr, w2, w12
; ---- 177: SETM.B 0x891
        .case 177
        mov #0x2739, w0
        mov w0, 0x0890
        mov #0x0000, w0
        mov w0, SR
        .word 0xefe891        ; SETM.B 0x891
        .dump SAVE
        .show SAVE, sr, 0x0890
; ---- 178: SETM WREG
        .case 178
        mov #0x0000, w0
        mov w0, SR
        mov #0x0900, w0
        .word 0xef8000        ; SETM WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 179: SETM.B W13
        .case 179
        mov #0x0000, w0
        mov w0, SR
        mov #0x2739, w13
        .word 0xebc680        ; SETM.B W13
        .dump SAVE
        .show SAVE, sr, w13
; ---- 180: SETM [--W6]
        .case 180
        mov #0x3cd9, w0
        mov w0, 0x124e
        mov #0x0000, w0
        mov w0, SR
        mov #0x1250, w6
        .word 0xeba300        ; SETM [--W6]
        .dump SAVE
        .show SAVE, sr, w6, 0x124e
; ---- 181: SL 0x1650, WREG
        .case 181
        mov #0x4065, w0
        mov w0, 0x1650
        mov #0x0000, w0
        mov w0, SR
        mov #0x0900, w0
        .word 0xd41650        ; SL 0x1650, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 182: SL.B W3, W4
        .case 182
        mov #0x0000, w0
        mov w0, SR
        mov #0x78a9, w3
        mov #0x1005, w4
        .word 0xd04203        ; SL.B W3, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 183: SL [W2++], [W12]
        .case 183
        mov #0x800f, w0
        mov w0, 0x0900
        mov #0x6722, w0
        mov w0, 0x1002
        mov #0x0000, w0
        mov w0, SR
        mov #0x0900, w2
        mov #0x1002, w12
        .word 0xd00e32        ; SL [W2++], [W12]
        .dump SAVE
        .show SAVE, sr, w2, 0x1002
; ---- 184: SL W2, #4, W2
        .case 184
        mov #0x0000, w0
        mov w0, SR
        mov #0x78a9, w2
        .word 0xdd1144        ; SL W2, #4, W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 185: SL W3, #12, W8
        .case 185
        mov #0x0000, w0
        mov w0, SR
        mov #0x0912, w3
        mov #0x1002, w8
        .word 0xdd1c4c        ; SL W3, #12, W8
        .dump SAVE
        .show SAVE, sr, w8
; ---- 186: SL W0, W1, W2
        .case 186
        mov #0x0000, w0
        mov w0, SR
        mov #0x09a4, w0
        mov #0x8903, w1
        mov #0x78a9, w2
        .word 0xdd0101        ; SL W0, W1, W2
        .dump SAVE
        .show SAVE, sr, w2
; ---- 187: SL W4, W5, W6
        .case 187
        mov #0x0000, w0
        mov w0, SR
        mov #0xa409, w4
        mov #0xff01, w5
        mov #0x0883, w6
        .word 0xdd2305        ; SL W4, W5, W6
        .dump SAVE
        .show SAVE, sr, w6
; ---- 188: SUB 0xA04, WREG
        .case 188
        mov #0x4523, w0
        mov w0, 0x0a04
        mov #0x0000, w0
        mov w0, SR
        mov #0x6234, w0
        .word 0xb50a04        ; SUB 0xA04, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 189: SUB.B #0x23, W0
        .case 189
        mov #0x0000, w0
        mov w0, SR
        mov #0x7804, w0
        .word 0xb14230        ; SUB.B #0x23, W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 190: SUB #0x108, W4
        .case 190
        mov #0x0000, w0
        mov w0, SR
        mov #0x6234, w4
        .word 0xb11084        ; SUB #0x108, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 191: SUB.B W4, #0x10, W5
        .case 191
        mov #0x0000, w0
        mov w0, SR
        mov #0x1782, w4
        mov #0x7804, w5
        .word 0x5242f0        ; SUB.B W4, #0x10, W5
        .dump SAVE
        .show SAVE, sr, w5
; ---- 192: SUB W0, #0x8, [W2++]
        .case 192
        mov #0xa557, w0
        mov w0, 0x2004
        mov #0x0000, w0
        mov w0, SR
        mov #0xf230, w0
        mov #0x2004, w2
        .word 0x501968        ; SUB W0, #0x8, [W2++]
        .dump SAVE
        .show SAVE, sr, w2, 0x2004
; ---- 193: SUB.B W0, W1, W0
        .case 193
        mov #0x0000, w0
        mov w0, SR
        mov #0x1732, w0
        mov #0x7844, w1
        .word 0x504001        ; SUB.B W0, W1, W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 194: SUB W7, [W8++], [W9++]
        .case 194
        mov #0x92e4, w0
        mov w0, 0x1808
        mov #0xa557, w0
        mov w0, 0x2020
        mov #0x0000, w0
        mov w0, SR
        mov #0x2450, w7
        mov #0x1808, w8
        mov #0x2020, w9
        .word 0x539cb8        ; SUB W7, [W8++], [W9++]
        .dump SAVE
        .show SAVE, sr, w8, w9, 0x2020
; ---- 195: SUBB 0xA04, WREG
        .case 195
        mov #0x6235, w0
        mov w0, 0x0a04
        mov #0x0000, w0
        mov w0, SR
        mov #0x6234, w0
        .word 0xb58a04        ; SUBB 0xA04, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 196: SUBB.B #0x23, W0
        .case 196
        mov #0x0000, w0
        mov w0, SR
        mov #0x7804, w0
        .word 0xb1c230        ; SUBB.B #0x23, W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 197: SUBB #0x108, W4
        .case 197
        mov #0x0001, w0
        mov w0, SR
        mov #0x6234, w4
        .word 0xb19084        ; SUBB #0x108, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 198: SUBB.B W4, #0x10, W5
        .case 198
        mov #0x0000, w0
        mov w0, SR
        mov #0x1782, w4
        mov #0x7804, w5
        .word 0x5a42f0        ; SUBB.B W4, #0x10, W5
        .dump SAVE
        .show SAVE, sr, w5
; ---- 199: SUBB W0, #0x8, [W2++]
        .case 199
        mov #0xa557, w0
        mov w0, 0x2004
        mov #0x0002, w0
        mov w0, SR
        mov #0x0009, w0
        mov #0x2004, w2
        .word 0x581968        ; SUBB W0, #0x8, [W2++]
        .dump SAVE
        .show SAVE, sr, w2, 0x2004
; ---- 200: SUBB.B W0, W1, W0
        .case 200
        mov #0x0000, w0
        mov w0, SR
        mov #0x1732, w0
        mov #0x7844, w1
        .word 0x584001        ; SUBB.B W0, W1, W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 201: SUBB W7, [W8++], [W9++]
        .case 201
        mov #0x92e4, w0
        mov w0, 0x1808
        mov #0xa557, w0
        mov w0, 0x2022
        mov #0x0000, w0
        mov w0, SR
        mov #0x2450, w7
        mov #0x1808, w8
        mov #0x2022, w9
        .word 0x5b9cb8        ; SUBB W7, [W8++], [W9++]
        .dump SAVE
        .show SAVE, sr, w8, w9, 0x2022
; ---- 202: SUBBR.B 0x803
        .case 202
        mov #0x9439, w0
        mov w0, 0x0802
        mov #0x0002, w0
        mov w0, SR
        mov #0x7804, w0
        .word 0xbde803        ; SUBBR.B 0x803
        .dump SAVE
        .show SAVE, sr, 0x0802
; ---- 203: SUBBR 0xA04, WREG
        .case 203
        mov #0x6235, w0
        mov w0, 0x0a04
        mov #0x0000, w0
        mov w0, SR
        mov #0x6234, w0
        .word 0xbd8a04        ; SUBBR 0xA04, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 204: SUBBR.B W0, #0x10, W1
        .case 204
        mov #0x0003, w0
        mov w0, SR
        mov #0xf310, w0
        mov #0x786a, w1
        .word 0x1840f0        ; SUBBR.B W0, #0x10, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 205: SUBBR.B W0, W1, W0
        .case 205
        mov #0x0000, w0
        mov w0, SR
        mov #0x1732, w0
        mov #0x7844, w1
        .word 0x184001        ; SUBBR.B W0, W1, W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 206: SUBBR W7, [W8++], [W9++]
        .case 206
        mov #0x92e4, w0
        mov w0, 0x1808
        mov #0xa557, w0
        mov w0, 0x2022
        mov #0x0000, w0
        mov w0, SR
        mov #0x2450, w7
        mov #0x1808, w8
        mov #0x2022, w9
        .word 0x1b9cb8        ; SUBBR W7, [W8++], [W9++]
        .dump SAVE
        .show SAVE, sr, w8, w9, 0x2022
; ---- 207: SUBR.B 0x1FFF
        .case 207
        mov #0x9439, w0
        mov w0, 0x1ffe
        mov #0x0000, w0
        mov w0, SR
        mov #0x7804, w0
        .word 0xbd7fff        ; SUBR.B 0x1FFF
        .dump SAVE
        .show SAVE, sr, 0x1ffe
; ---- 208: SUBR 0xA04, WREG
        .case 208
        mov #0x6235, w0
        mov w0, 0x0a04
        mov #0x0000, w0
        mov w0, SR
        mov #0x6234, w0
        .word 0xbd0a04        ; SUBR 0xA04, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 209: SUBR.B W0, #0x10, W1
        .case 209
        mov #0x0000, w0
        mov w0, SR
        mov #0xf310, w0
        mov #0x786a, w1
        .word 0x1040f0        ; SUBR.B W0, #0x10, W1
        .dump SAVE
        .show SAVE, sr, w1
; ---- 210: SUBR W0, #0x8, [W2++]
        .case 210
        mov #0xa557, w0
        mov w0, 0x2004
        mov #0x0000, w0
        mov w0, SR
        mov #0x0009, w0
        mov #0x2004, w2
        .word 0x101968        ; SUBR W0, #0x8, [W2++]
        .dump SAVE
        .show SAVE, sr, w2, 0x2004
; ---- 211: SUBR.B W0, W1, W0
        .case 211
        mov #0x0000, w0
        mov w0, SR
        mov #0x1732, w0
        mov #0x7844, w1
        .word 0x104001        ; SUBR.B W0, W1, W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 212: SUBR W7, [W8++], [W9++]
        .case 212
        mov #0x92e4, w0
        mov w0, 0x1808
        mov #0xa557, w0
        mov w0, 0x2022
        mov #0x0000, w0
        mov w0, SR
        mov #0x2450, w7
        mov #0x1808, w8
        mov #0x2022, w9
        .word 0x139cb8        ; SUBR W7, [W8++], [W9++]
        .dump SAVE
        .show SAVE, sr, w8, w9, 0x2022
; ---- 213: SWAP.B W0
        .case 213
        mov #0x0000, w0
        mov w0, SR
        mov #0xab78, w0
        .word 0xfdc000        ; SWAP.B W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 214: SWAP W0
        .case 214
        mov #0x0000, w0
        mov w0, SR
        mov #0x8095, w0
        .word 0xfd8000        ; SWAP W0
        .dump SAVE
        .show SAVE, sr, w0
; ---- 215: TBLRDL [W6], [W8++] (0x003406 = 0x292E40)
        .case 215
        mov #0x658b, w0
        mov w0, 0x1202
        mov #0x0, w0
        mov w0, TBLPAG
        mov #0x0000, w0
        mov w0, SR
        mov #0x3406, w6
        mov #0x1202, w8
        .word 0xba1c16        ; TBLRDL [W6], [W8++]
        .dump SAVE
        .show SAVE, sr, w8, 0x1202
; ---- 216: TBLRDH [W6++], W8
        .case 216
        mov #0x0, w0
        mov w0, TBLPAG
        mov #0x0000, w0
        mov w0, SR
        mov #0x3406, w6
        mov #0x65b1, w8
        .word 0xba8436        ; TBLRDH [W6++], W8
        .dump SAVE
        .show SAVE, sr, w6, w8
; ---- 217: TBLRDL.B [W0++], W1 (0x010812 = 0xEF2042)
        .case 217
        mov #0x1, w0
        mov w0, TBLPAG
        mov #0x0000, w0
        mov w0, SR
        mov #0x0813, w0
        mov #0x0f71, w1
        .word 0xba40b0        ; TBLRDL.B [W0++], W1
        .dump SAVE
        .show SAVE, sr, w0, w1
; ---- 218: TBLRDH.B [W0], [W1++]
        .case 218
        mov #0x0944, w0
        mov w0, 0x0f70
        mov #0x1, w0
        mov w0, TBLPAG
        mov #0x0000, w0
        mov w0, SR
        mov #0x0812, w0
        mov #0x0f71, w1
        .word 0xbad890        ; TBLRDH.B [W0], [W1++]
        .dump SAVE
        .show SAVE, sr, w1, 0x0f70
; ---- 219: ULNK
        .case 219
        mov #0x2000, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, SR
        mov #0x2002, w14
        mov #0x20a2, w15
        .word 0xfa8000        ; ULNK
        .dump SAVE
        .show SAVE, sr, w14, w15
; ---- 220: XOR.B 0x1FFF
        .case 220
        mov #0x9439, w0
        mov w0, 0x1ffe
        mov #0x0000, w0
        mov w0, SR
        mov #0x7804, w0
        .word 0xb6ffff        ; XOR.B 0x1FFF
        .dump SAVE
        .show SAVE, sr, 0x1ffe
; ---- 221: XOR 0xA04, WREG
        .case 221
        mov #0xa053, w0
        mov w0, 0x0a04
        mov #0x0000, w0
        mov w0, SR
        mov #0x6234, w0
        .word 0xb68a04        ; XOR 0xA04, WREG
        .dump SAVE
        .show SAVE, sr, w0
; ---- 222: XOR #0x108, W4
        .case 222
        mov #0x0000, w0
        mov w0, SR
        mov #0x6134, w4
        .word 0xb29084        ; XOR #0x108, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 223: XOR W2, #0x1F, [W8++]
        .case 223
        mov #0x6628, w0
        mov w0, 0x1004
        mov #0x0000, w0
        mov w0, SR
        mov #0x8505, w2
        mov #0x1004, w8
        .word 0x691c7f        ; XOR W2, #0x1F, [W8++]
        .dump SAVE
        .show SAVE, sr, w8, 0x1004
; ---- 224: XOR.B W1, [W5++], [W9++]
        .case 224
        mov #0x115a, w0
        mov w0, 0x2000
        mov #0x0000, w0
        mov w0, 0x2600
        mov #0x0000, w0
        mov w0, SR
        mov #0xaaaa, w1
        mov #0x2000, w5
        mov #0x2600, w9
        .word 0x68dcb5        ; XOR.B W1, [W5++], [W9++]
        .dump SAVE
        .show SAVE, sr, w5, w9, 0x2600
; ---- 225: XOR W1, W5, W9
        .case 225
        mov #0x0000, w0
        mov w0, SR
        mov #0xfedc, w1
        mov #0x1234, w5
        mov #0xa34d, w9
        .word 0x688485        ; XOR W1, W5, W9
        .dump SAVE
        .show SAVE, sr, w9
; ---- 226: ZE W3, W4
        .case 226
        mov #0x0000, w0
        mov w0, SR
        mov #0x7839, w3
        mov #0x1005, w4
        .word 0xfb8203        ; ZE W3, W4
        .dump SAVE
        .show SAVE, sr, w4
; ---- 227: ZE [W2++], W12
        .case 227
        mov #0x268f, w0
        mov w0, 0x0900
        mov #0x0000, w0
        mov w0, SR
        mov #0x0900, w2
        mov #0x1002, w12
        .word 0xfb8632        ; ZE [W2++], W12
        .dump SAVE
        .show SAVE, sr, w2, w12
        mov #'.', w1
        rcall putc
        mov #10, w1
        rcall putc
done:   bra done
; ---- the routines the cases call
sub160:  .word 0x0540a0        ; RETLW.B #0xA, W0
sub161:  .word 0x052302        ; RETLW #0x230, W2
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
; ---- program memory the table reads read
        .org 0x3406
        .word 0x292e40
        .org 0x10812
        .word 0xef2042
