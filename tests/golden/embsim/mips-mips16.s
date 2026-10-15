# mips-mips16.s -- MIPS16e edges for tests/golden/embsim-mips.sh, run on
# QEMU's malta and on EmbSim's, both with the 24Kc (MIPS32 and MIPS16e).
# Each case prints its number and a result ("0123 deadbeef"), so
# EmbSim's output can be held to QEMU's (and to the record,
# mips-mips16.txt).
#
# llvm-mc has no MIPS16 assembler, so the MIPS16 code is .hwords the
# macros below build from the instruction formats, their registers the
# three-bit numbers (0-7: s0, s1, v0, v1, a0, a1, a2, a3), branches'
# offsets label differences; llvm-mc assembles the rest (MIPS32), and
# embld links it at malta's RAM (0x80100000, no entry stub: _start is
# the image's first byte), so jal's and jalx's absolute targets come
# from BASE. The exception record is at 0x80200000 (Cause, EPC,
# BadVAddr, taken, where to resume), scratch memory at 0x80200100.
#
# _start (MIPS32) sets the stack and the exception vector, and jalx's to
# m16main; each case prints through `out`, a MIPS32 routine MIPS16 jalx's
# to and jr returns from. The handler (MIPS32) records the exception and
# resumes where the case said, or past the instruction (4 bytes for an
# EXTENDed one or jal, else 2).

    .equ    BASE, 0x80100000
    .equ    EXC, 0x80200000
    .equ    MEM, 0x80200100
    .equ    THR, 0xbf000900

    .text
    .set    noreorder
    .set    noat

# ---- MIPS32 ---------------------------------------------------------------

    .globl  _start
_start:
    lui     $sp, 0x8080
    la      $t0, exc_handler            # the vector: j exc_handler; nop
    srl     $t0, $t0, 2
    lui     $t1, 0x03ff
    ori     $t1, $t1, 0xffff
    and     $t0, $t0, $t1
    lui     $t1, 0x0800
    or      $t0, $t0, $t1
    lui     $t2, 0x8000
    sw      $t0, 0x180($t2)
    sw      $zero, 0x184($t2)
    mfc0    $t0, $12                    # Status: BEV, IM, ERL, EXL, IE clear
    li      $t1, ~((1 << 22) | 0xff00 | 7)
    and     $t0, $t0, $t1
    mtc0    $t0, $12
    ehb
    lui     $s7, 0x8020
    sw      $zero, 12($s7)
    sw      $zero, 16($s7)
    jalx    m16main
    nop
    lui     $t0, 0xbf00                 # SOFTRES
    li      $t1, 0x42
    sw      $t1, 0x500($t0)
1:  b       1b
    nop

exc_handler:
    lui     $k1, 0x8020
    mfc0    $k0, $13
    sw      $k0, 0($k1)
    mfc0    $k0, $14
    sw      $k0, 4($k1)
    mfc0    $k0, $8
    sw      $k0, 8($k1)
    addiu   $k0, $zero, 1
    sw      $k0, 12($k1)
    lw      $k0, 16($k1)                # the case's resume address
    bnez    $k0, 2f
    sw      $zero, 16($k1)
    mfc0    $k0, $14                    # else past the instruction
    andi    $k1, $k0, 1
    beqz    $k1, 2f
    addiu   $k0, $k0, 4
    lhu     $k1, -5($k0)                # MIPS16: its first halfword
    srl     $k1, $k1, 11
    xori    $k1, $k1, 0x1e
    beqz    $k1, 2f                     # EXTEND: 4 bytes
    xori    $k1, $k1, 0x1e ^ 0x03
    beqz    $k1, 2f                     # jal, jalx: 4
    nop
    addiu   $k0, $k0, -2
2:  mtc0    $k0, $14
    ehb
    eret
    nop

# print a1 (4 digits), a space, a0 (8 digits): clobbers t0-t4, a0, a1;
# back by jr ra (to MIPS16: ra is odd)
    .macro  DIGITS val, count
    move    $t4, \val
    li      $t1, \count
91: srl     $t2, $t4, 28
    sltiu   $t3, $t2, 10
    bnez    $t3, 92f
    addiu   $t2, $t2, 48
    addiu   $t2, $t2, 39
92: lbu     $t3, 0x28($t0)
    andi    $t3, $t3, 0x20
    beqz    $t3, 92b
    nop
    sb      $t2, 0($t0)
    sll     $t4, $t4, 4
    addiu   $t1, $t1, -1
    bnez    $t1, 91b
    nop
    .endm

    .macro  PUTC c
93: lbu     $t3, 0x28($t0)
    andi    $t3, $t3, 0x20
    beqz    $t3, 93b
    nop
    li      $t2, \c
    sb      $t2, 0($t0)
    .endm

    .p2align 2
out:
    li      $t0, THR
    sll     $a1, $a1, 16
    DIGITS  $a1, 4
    PUTC    32
    DIGITS  $a0, 8
    PUTC    10
    jr      $ra
    nop

    .p2align 2
done:
    li      $t0, THR
    PUTC    100
    PUTC    111
    PUTC    110
    PUTC    101
    PUTC    10
    lw      $ra, 0x20($s7)
    jr      $ra
    nop

# a MIPS32 function MIPS16's jalx reaches: v0 = 0x32, back by jr
    .p2align 2
m32_func:
    li      $v0, 0x32
    jr      $ra
    nop

# ---- the MIPS16 encodings ------------------------------------------------

    .macro  EXT imm                     # EXTEND: imm[10:5], imm[15:11]
    .hword  0xf000 | ((((\imm) >> 5) & 0x3f) << 5) | (((\imm) >> 11) & 0x1f)
    .endm
    .macro  RRR rz, rx, ry, f           # f 1 addu, 3 subu (0, 2: 64-bit)
    .hword  0xe000 | (\rx << 8) | (\ry << 5) | (\rz << 2) | \f
    .endm
    .macro  RR rx, ry, f
    .hword  0xe800 | (\rx << 8) | (\ry << 5) | \f
    .endm
    .macro  RI op, rx, imm8             # rx and an 8-bit immediate
    .hword  (\op << 11) | (\rx << 8) | ((\imm8) & 0xff)
    .endm
    .macro  XRI op, rx, imm             # its extended form
    EXT     \imm
    .hword  (\op << 11) | (\rx << 8) | ((\imm) & 0x1f)
    .endm
    .macro  RRI op, rx, ry, imm5        # loads and stores
    .hword  (\op << 11) | (\rx << 8) | (\ry << 5) | ((\imm5) & 0x1f)
    .endm
    .macro  XRRI op, rx, ry, imm
    EXT     \imm
    .hword  (\op << 11) | (\rx << 8) | (\ry << 5) | ((\imm) & 0x1f)
    .endm
    .macro  SHIFT rx, ry, sa, f         # f 0 sll, 2 srl, 3 sra; sa 1-8
    .hword  0x3000 | (\rx << 8) | (\ry << 5) | (((\sa) & 7) << 2) | \f
    .endm
    .macro  XSHIFT rx, ry, sa, f        # sa 0-31
    .hword  0xf000 | ((\sa) << 6)
    .hword  0x3000 | (\rx << 8) | (\ry << 5) | \f
    .endm
    .macro  ADDIU3 ry, rx, imm4         # addiu ry, rx, imm4 (RRIA)
    .hword  0x4000 | (\rx << 8) | (\ry << 5) | ((\imm4) & 0xf)
    .endm
    .macro  XADDIU3 ry, rx, imm         # 15 bits
    .hword  0xf000 | ((((\imm) >> 4) & 0x7f) << 4) | (((\imm) >> 11) & 0xf)
    .hword  0x4000 | (\rx << 8) | (\ry << 5) | ((\imm) & 0xf)
    .endm
    .macro  I8 f, imm8
    .hword  0x6000 | (\f << 8) | ((\imm8) & 0xff)
    .endm
    .macro  XI8 f, imm
    EXT     \imm
    .hword  0x6000 | (\f << 8) | ((\imm) & 0x1f)
    .endm
    .macro  NOP16
    .hword  0x6500                      # move $0, $16
    .endm
    .macro  MOV32R r32, rz              # r32 = rz
    .hword  0x6500 | (((\r32) & 7) << 5) | ((((\r32) >> 3) & 3) << 3) | \rz
    .endm
    .macro  MOVR32 ry, r32              # ry = r32
    .hword  0x6700 | (\ry << 5) | (\r32)
    .endm
    .macro  B lbl
    .hword  0x1000 | ((((\lbl) - . - 2) >> 1) & 0x7ff)
    .endm
    .macro  XB lbl
    EXT     (((\lbl) - . - 4) >> 1)
    .hword  0x1000 | ((((\lbl) - . + 2 - 4) >> 1) & 0x1f)
    .endm
    .macro  BEQZ rx, lbl
    .hword  0x2000 | (\rx << 8) | ((((\lbl) - . - 2) >> 1) & 0xff)
    .endm
    .macro  BNEZ rx, lbl
    .hword  0x2800 | (\rx << 8) | ((((\lbl) - . - 2) >> 1) & 0xff)
    .endm
    .macro  XBEQZ rx, lbl
    EXT     (((\lbl) - . - 4) >> 1)
    .hword  0x2000 | (\rx << 8) | ((((\lbl) - . + 2 - 4) >> 1) & 0x1f)
    .endm
    .macro  BTEQZ lbl
    .hword  0x6000 | ((((\lbl) - . - 2) >> 1) & 0xff)
    .endm
    .macro  BTNEZ lbl
    .hword  0x6100 | ((((\lbl) - . - 2) >> 1) & 0xff)
    .endm
    .macro  JALX32 lbl                  # jalx: to MIPS32
    .hword  0x1c00 | ((((BASE + ((\lbl) - _start)) >> 2) >> 16) & 0x1f) << 5 | ((((BASE + ((\lbl) - _start)) >> 2) >> 21) & 0x1f)
    .hword  ((BASE + ((\lbl) - _start)) >> 2) & 0xffff
    .endm
    .macro  JAL16 lbl                   # jal: stays MIPS16
    .hword  0x1800 | ((((BASE + ((\lbl) - _start)) >> 2) >> 16) & 0x1f) << 5 | ((((BASE + ((\lbl) - _start)) >> 2) >> 21) & 0x1f)
    .hword  ((BASE + ((\lbl) - _start)) >> 2) & 0xffff
    .endm
    # addiupc and lwpc to a label: from this instruction's word (BASE is
    # word-aligned, so the word's offset is the image's)
    .macro  ADDIUPC rx, lbl
    .hword  (0x01 << 11) | (\rx << 8) | (((((\lbl) - _start) - ((. - _start) & ~3)) >> 2) & 0xff)
    .endm
    .macro  LWPC rx, lbl
    .hword  (0x16 << 11) | (\rx << 8) | (((((\lbl) - _start) - ((. - _start) & ~3)) >> 2) & 0xff)
    .endm
    .macro  XLWPC rx, lbl
99: EXT     ((\lbl) - _start) - ((99b - _start) & ~3)
    .hword  (0x16 << 11) | (\rx << 8) | ((((\lbl) - _start) - ((99b - _start) & ~3)) & 0x1f)
    .endm
    # li32 rx, value through tmp (both 3-bit numbers)
    .macro  LI32 rx, tmp, v
    XRI     0x0d, \rx, ((\v) >> 16) & 0xffff
    XSHIFT  \rx, \rx, 16, 0
    XRI     0x0d, \tmp, (\v) & 0xffff
    RR      \rx, \tmp, 0x0d             # or
    .endm
    # OUT id, r3: print the case and a 3-bit register (jalx out, a 16-bit slot)
    .macro  OUT id, r
    MOV32R  4, \r                       # a0
    XRI     0x0d, 5, \id                # li a1, id
    JALX32  out
    NOP16
    .endm
    # OUT32 id, reg32: a register by its 5-bit number
    .macro  OUT32 id, r32
    MOVR32  4, \r32
    XRI     0x0d, 5, \id
    JALX32  out
    NOP16
    .endm

    .equ    S0, 0
    .equ    S1, 1
    .equ    V0, 2
    .equ    V1, 3
    .equ    A0, 4
    .equ    A1, 5
    .equ    A2, 6
    .equ    A3, 7

# ---- MIPS16 ----------------------------------------------------------------

    .p2align 2
m16main:
    I8      2, 0x20 >> 2                # sw ra, 0x20(sp): (MIPS16 cannot reach EXC)
    MOVR32  A0, 29
    MOV32R  23, A0                      # s7 = sp: the way back is at 0x20(s7)
    LI32    S0, A3, 0x12345678
    LI32    S1, A3, 0x0f0f00ff
    LI32    V0, A3, 0x80000001
    RI      0x0d, V1, 3                 # li v1, 3

# ---- ALU ----
    RRR     A2, S0, S1, 1               # addu a2, s0, s1
    OUT     0x0101, A2
    RRR     A2, S0, S1, 3               # subu a2, s0, s1
    OUT     0x0102, A2
    RRR     A2, V1, V0, 3
    OUT     0x0103, A2
    MOV32R  0, S0                       # (a move to $0: nothing)
    RR      A2, S1, 0x0b                # neg a2, s1
    OUT     0x0104, A2
    MOVR32  A2, 16
    RR      A2, S1, 0x0c                # and
    OUT     0x0105, A2
    MOVR32  A2, 16
    RR      A2, S1, 0x0d                # or
    OUT     0x0106, A2
    MOVR32  A2, 16
    RR      A2, S1, 0x0e                # xor
    OUT     0x0107, A2
    RR      A2, S1, 0x0f                # not
    OUT     0x0108, A2
    RR      S0, S1, 0x02                # slt: t8
    OUT32   0x0109, 24
    RR      V0, S1, 0x02
    OUT32   0x010a, 24
    RR      V0, S1, 0x03                # sltu
    OUT32   0x010b, 24
    RR      S0, S1, 0x0a                # cmp: t8 = xor
    OUT32   0x010c, 24
    RI      0x0d, A3, 5
    MOVR32  A2, 16
    RR      A3, A2, 0x04                # sllv a2, a3
    OUT     0x010d, A2
    MOVR32  A2, 2
    RR      A3, A2, 0x06                # srlv
    OUT     0x010e, A2
    MOVR32  A2, 2
    RR      A3, A2, 0x07                # srav
    OUT     0x010f, A2
    SHIFT   A2, S0, 1, 0                # sll a2, s0, 1
    OUT     0x0110, A2
    SHIFT   A2, S0, 8, 0                # (sa 0 is 8)
    OUT     0x0111, A2
    SHIFT   A2, V0, 3, 2                # srl
    OUT     0x0112, A2
    SHIFT   A2, V0, 8, 3                # sra
    OUT     0x0113, A2
    XSHIFT  A2, S0, 0, 0
    OUT     0x0114, A2
    XSHIFT  A2, S0, 31, 0
    OUT     0x0115, A2
    XSHIFT  A2, V0, 17, 3
    OUT     0x0116, A2
    XSHIFT  A2, V0, 31, 2
    OUT     0x0117, A2
    MOVR32  A2, 16
    RI      0x09, A2, -128              # addiu a2, -128
    OUT     0x0118, A2
    RI      0x09, A2, 127
    OUT     0x0119, A2
    XRI     0x09, A2, -32768
    OUT     0x011a, A2
    ADDIU3  A2, S0, -8                  # addiu a2, s0, -8
    OUT     0x011b, A2
    ADDIU3  A2, S0, 7
    OUT     0x011c, A2
    XADDIU3 A2, S0, -16384
    OUT     0x011d, A2
    XADDIU3 A2, S0, 16383
    OUT     0x011e, A2
    RI      0x0a, V0, 0x80              # slti: the 8 bits zero-extended
    OUT32   0x011f, 24
    RI      0x0a, V1, 0x80
    OUT32   0x0120, 24
    RI      0x0b, V1, 0xff              # sltiu
    OUT32   0x0121, 24
    XRI     0x0a, V1, -1                # extended: sign-extended
    OUT32   0x0122, 24
    XRI     0x0b, V1, -1
    OUT32   0x0123, 24
    RI      0x0d, A2, 0xff              # li: zero-extended
    OUT     0x0124, A2
    XRI     0x0d, A2, 0xffff
    OUT     0x0125, A2
    RI      0x0e, S1, 0xff              # cmpi: t8 = s1 ^ 0xff
    OUT32   0x0126, 24
    XRI     0x0e, S1, 0x8000
    OUT32   0x0127, 24
    LI32    A2, A3, 0x8182f3f4
    MOVR32  A3, 6
    RR      A3, 0, 0x11                 # zeb
    OUT     0x0128, A3
    MOVR32  A3, 6
    RR      A3, 1, 0x11                 # zeh
    OUT     0x0129, A3
    MOVR32  A3, 6
    RR      A3, 4, 0x11                 # seb
    OUT     0x012a, A3
    MOVR32  A3, 6
    RR      A3, 5, 0x11                 # seh
    OUT     0x012b, A3
    RR      S0, S1, 0x18                # mult
    RR      A2, 0, 0x10                 # mfhi
    OUT     0x012c, A2
    RR      A2, 0, 0x12                 # mflo
    OUT     0x012d, A2
    RR      S0, S1, 0x19                # multu
    RR      A2, 0, 0x10
    OUT     0x012e, A2
    RR      V0, V1, 0x1a                # div
    RR      A2, 0, 0x12
    OUT     0x012f, A2
    RR      A2, 0, 0x10
    OUT     0x0130, A2
    RR      V0, V1, 0x1b                # divu
    RR      A2, 0, 0x12
    OUT     0x0131, A2
    MOV32R  25, S0                      # t9 = s0
    MOVR32  A2, 25
    OUT     0x0132, A2
    MOV32R  31, S1                      # ra = s1
    MOVR32  A2, 31
    OUT     0x0133, A2
    MOVR32  A2, 29
    RI      0x00, A3, 0xff              # addiu a3, sp, 1020
    RRR     A3, A3, A2, 3
    OUT     0x0134, A3
    XRI     0x00, A3, -4
    RRR     A3, A3, A2, 3
    OUT     0x0135, A3
    I8      3, -16                      # adjsp: sp -= 128
    MOVR32  A3, 29
    RRR     A3, A3, A2, 3
    OUT     0x0136, A3
    XI8     3, 128                      # sp += 128
    MOVR32  A3, 29
    RRR     A3, A3, A2, 3
    OUT     0x0137, A3
    # addiupc and lwpc: from the word-aligned address
    RI      0x01, A2, 4                 # addiupc a2, 16
    RI      0x01, A3, 0                 # addiupc a3, 0, two bytes on
    RRR     A2, A2, A3, 3
    OUT     0x0138, A2
    LWPC    A2, 9f
    B       7f
    .p2align 2
9:  .word   0xc0ffee11
7:  OUT     0x0139, A2
    XLWPC   A2, 9b
    OUT     0x013a, A2

# ---- loads and stores ----
    LI32    V1, A3, MEM
    LI32    A2, A3, 0x11223344
    RRI     0x1b, V1, A2, 0             # sw a2, 0(v1)
    LI32    A2, A3, 0x8899aabb
    RRI     0x1b, V1, A2, 1             # sw a2, 4(v1)
    LI32    A2, A3, 0xccddeeff
    RRI     0x1b, V1, A2, 31            # sw a2, 124(v1)
    RRI     0x10, V1, A2, 7             # lb a2, 7(v1)
    OUT     0x0201, A2
    RRI     0x14, V1, A2, 7             # lbu
    OUT     0x0202, A2
    RRI     0x11, V1, A2, 3             # lh a2, 6(v1)
    OUT     0x0203, A2
    RRI     0x15, V1, A2, 3             # lhu
    OUT     0x0204, A2
    RRI     0x13, V1, A2, 31            # lw a2, 124(v1)
    OUT     0x0205, A2
    XRRI    0x13, V1, A2, 4
    OUT     0x0206, A2
    XRRI    0x10, V1, A2, 126
    OUT     0x0207, A2
    LI32    S1, A3, 0xa1b2c3d4
    RRI     0x18, V1, S1, 1             # sb s1, 1(v1)
    RRI     0x19, V1, S1, 3             # sh s1, 6(v1)
    RRI     0x1b, V1, S1, 2             # sw s1, 8(v1)
    RRI     0x13, V1, A2, 0
    OUT     0x0208, A2
    RRI     0x13, V1, A2, 1
    OUT     0x0209, A2
    RRI     0x13, V1, A2, 2
    OUT     0x020a, A2
    RI      0x1a, S1, 2                 # sw s1, 8(sp)
    RI      0x12, A2, 2                 # lw a2, 8(sp)
    OUT     0x020b, A2
    XRI     0x1a, S0, -4
    XRI     0x12, A2, -4
    OUT     0x020c, A2
    MOV32R  31, S0
    I8      2, 3                        # sw ra, 12(sp)
    RI      0x12, A2, 3
    OUT     0x020d, A2
    MOV32R  31, S1
    XI8     2, 16
    RI      0x12, A2, 4
    OUT     0x020e, A2

# ---- save and restore ----
    LI32    S0, A3, 0x50505050
    LI32    S1, A3, 0x51515151
    MOVR32  A2, 29                      # a2 = sp before
    MOV32R  25, A2                      # (t9 keeps it)
    I8      4, 0x80 | 0x40 | 0x20 | 0x10 | 2    # save 16, ra, s0, s1
    MOVR32  A3, 29
    RRR     A3, A3, A2, 3
    OUT     0x0301, A3                  # sp moved by -16
    MOVR32  A2, 29
    RRI     0x13, A2, A3, 3             # lw a3, 12(sp): ra
    MOVR32  V0, 31
    RRR     A3, A3, V0, 3
    OUT     0x0302, A3                  # 0: ra saved at the top
    RRI     0x13, A2, A3, 2             # s1
    OUT     0x0303, A3
    RRI     0x13, A2, A3, 1             # s0
    OUT     0x0304, A3
    RI      0x0d, S0, 0
    RI      0x0d, S1, 0
    I8      4, 0x40 | 0x20 | 0x10 | 2   # restore 16, ra, s0, s1
    OUT     0x0305, S0
    OUT     0x0306, S1
    MOVR32  A2, 29
    MOVR32  A3, 25
    RRR     A3, A2, A3, 3
    OUT     0x0307, A3                  # sp back
    # extended: xsregs 2 (s2, s3), aregs 6 (one argument, two static: a3, a2)
    LI32    A0, A3, 0x0a0a0a0a
    LI32    A1, A3, 0x0b0b0b0b
    LI32    A2, A3, 0x0c0c0c0c
    LI32    A3, S0, 0x0d0d0d0d
    LI32    S0, V0, 0x02020202
    MOV32R  18, S0                      # s2
    LI32    S0, V0, 0x03030303
    MOV32R  19, S0                      # s3
    MOV32R  25, S0
    MOVR32  S0, 29
    MOV32R  25, S0                      # t9 = sp before
    .hword  0xf000 | (2 << 8) | (0 << 4) | 6    # extend: xsregs 2, frame hi 0, aregs 6
    .hword  0x6000 | (4 << 8) | 0x80 | 0x40 | 4 # save 32, ra
    MOVR32  V0, 29
    RRI     0x13, V0, S0, 7             # lw s0, 28(sp): ra
    MOVR32  S1, 31
    RRR     S0, S0, S1, 3
    OUT     0x0308, S0
    RRI     0x13, V0, S0, 6             # 24: s3
    OUT     0x0309, S0
    RRI     0x13, V0, S0, 5             # 20: s2
    OUT     0x030a, S0
    RRI     0x13, V0, S0, 4             # 16: a3 (static)
    OUT     0x030b, S0
    RRI     0x13, V0, S0, 3             # 12: a2 (static)
    OUT     0x030c, S0
    MOVR32  S1, 25
    RRI     0x13, S1, S0, 0             # 0 above the frame: a0 (the argument)
    OUT     0x030d, S0
    RRR     S0, V0, S1, 3
    OUT     0x030e, S0                  # -32
    RI      0x0d, S0, 0
    MOV32R  18, S0
    MOV32R  19, S0
    RI      0x0d, A2, 0
    RI      0x0d, A3, 0
    .hword  0xf000 | (2 << 8) | (0 << 4) | 6
    .hword  0x6000 | (4 << 8) | 0x40 | 4        # restore 32, ra
    MOVR32  S0, 18
    OUT     0x030f, S0
    MOVR32  S0, 19
    OUT     0x0310, S0
    OUT     0x0311, A3
    OUT     0x0312, A2
    MOVR32  S0, 29
    MOVR32  S1, 25
    RRR     S0, S0, S1, 3
    OUT     0x0313, S0                  # sp back

# ---- branches (b, beqz, bnez, bteqz, btnez: no delay slot) ----
    RI      0x0d, S0, 0
    RI      0x0d, S1, 1
    RI      0x0d, A2, 0
    B       1f
    RI      0x09, A2, 1                 # skipped
1:  BEQZ    S0, 1f
    RI      0x09, A2, 2                 # skipped
1:  BEQZ    S1, 1f
    RI      0x09, A2, 4                 # runs
1:  BNEZ    S1, 1f
    RI      0x09, A2, 8
1:  BNEZ    S0, 1f
    RI      0x09, A2, 16                # runs
1:  RI      0x0e, S1, 1                 # cmpi: t8 = 0
    BTEQZ   1f
    RI      0x09, A2, 32
1:  BTNEZ   1f
    RI      0x09, A2, 64                # runs
1:  XBEQZ   S0, 1f
    RI      0x09, A2, 1
1:  XB      1f
    RI      0x09, A2, 2
1:  OUT     0x0401, A2
    RI      0x0d, A2, 0                 # backward
    RI      0x0d, A3, 3
1:  RI      0x09, A2, 5
    RI      0x09, A3, -1
    BNEZ    A3, 1b
    OUT     0x0402, A2
    # jr: its delay slot runs; jrc: none
    ADDIUPC A3, 3f                      # (3f word-aligned below)
    RI      0x09, A3, 1                 # +1: MIPS16
    RI      0x0d, A2, 0
    RR      A3, 0, 0x00                 # jr a3
    RI      0x09, A2, 1                 # the slot
    RI      0x09, A2, 2
    .p2align 2
3:  OUT     0x0403, A2
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x0d, A2, 0
    RR      A3, 4, 0x00                 # jrc a3 (nd)
    RI      0x09, A2, 1
    .p2align 2
3:  OUT     0x0404, A2
    # jalr: ra past the slot; jalrc: past itself (bit 0 set)
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x01, V0, 0                 # v0 = this word
    RR      A3, 2, 0x00                 # jalr a3
    NOP16
    .p2align 2
3:  MOVR32  V1, 31
    RRR     V1, V1, V0, 3
    OUT     0x0405, V1
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x01, V0, 0
    RR      A3, 6, 0x00                 # jalrc a3
    .p2align 2
3:  MOVR32  V1, 31
    RRR     V1, V1, V0, 3
    OUT     0x0406, V1
    # jal: MIPS16 to MIPS16, a 16-bit slot
    RI      0x0d, A2, 0
    RI      0x01, V0, 0
    JAL16   3f
    RI      0x09, A2, 1                 # the slot
    RI      0x09, A2, 2
    .p2align 2
3:  MOVR32  V1, 31
    RRR     V1, V1, V0, 3
    OUT     0x0407, V1
    OUT     0x0408, A2
    # jalx to MIPS32 and back by jr ra
    RI      0x0d, V0, 0
    JALX32  m32_func
    NOP16
    OUT     0x0409, V0

# ---- exceptions ----
    LI32    V1, A3, MEM
    MOV32R  22, V1                      # s6 = MEM
    LI32    S1, A3, EXC
    .macro  EXCOUT id                   # the code, BD, taken; EPC from v0
    RRI     0x13, S1, A2, 0             # cause
    XSHIFT  A3, A2, 2, 2
    RI      0x0d, A0, 31
    RR      A3, A0, 0x0c
    XSHIFT  A2, A2, 31, 2
    XSHIFT  A2, A2, 8, 0
    RR      A3, A2, 0x0d
    RRI     0x13, S1, A2, 3             # taken
    XSHIFT  A2, A2, 12, 0
    RR      A3, A2, 0x0d
    OUT     \id, A3
    RRI     0x13, S1, A2, 1             # EPC
    BEQZ    A2, 96f
    RRR     A2, A2, V0, 3
96: OUT     \id + 1, A2
    RI      0x0d, A2, 0
    RRI     0x1b, S1, A2, 0
    RRI     0x1b, S1, A2, 1
    RRI     0x1b, S1, A2, 3
    .endm
    .macro  RESUME lbl                  # where the handler goes back to
    ADDIUPC A2, \lbl
    RI      0x09, A2, 1
    RRI     0x1b, S1, A2, 4
    .endm
    RI      0x01, V0, 0
    RR      0, 0, 0x05                  # break
    EXCOUT  0x0501
    RI      0x01, V0, 0
    RRI     0x13, V1, A2, 1             # lw a2, 4(v1)
    XRRI    0x13, V1, A2, 2             # misaligned
    EXCOUT  0x0503
    RRI     0x13, S1, A2, 2             # BadVAddr
    RRR     A2, A2, V1, 3
    OUT     0x0505, A2
    RI      0x01, V0, 0
    RRI     0x11, V1, A2, 0
    XRRI    0x19, V1, A2, 13            # sh: misaligned
    EXCOUT  0x0506
    RI      0x01, V0, 0
    RRR     A2, A2, A2, 0               # daddu
    EXCOUT  0x0508
    RI      0x01, V0, 0
    SHIFT   A2, A2, 1, 1                # dsll
    EXCOUT  0x050a
    RI      0x01, V0, 0
    .hword  0x4010                      # addiu ry, rx with bit 4: daddiu
    EXCOUT  0x050c
    RI      0x01, V0, 0
    EXT     0
    RR      A2, A3, 0x0c                # extend can't extend RR
    EXCOUT  0x050e
    RI      0x01, V0, 0
    EXT     0
    .hword  0x6500                      # nor MOV32R
    EXCOUT  0x0510
    RI      0x01, V0, 0
    .hword  0x3800                      # major 7: ld
    EXCOUT  0x0512
    # in jr's delay slot: EPC the jr (ISA bit set), Cause.BD
    RESUME  3f
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x01, V0, 0
    RR      A3, 0, 0x00                 # jr a3
    RRI     0x11, V1, A2, 13 >> 1       # (lh a2, 12: aligned) no fault
    .p2align 2
3:  EXCOUT  0x0514
    RESUME  3f
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x01, V0, 0
    RR      A3, 0, 0x00                 # jr a3
    RR      0, 0, 0x05                  # break in the slot
    .p2align 2
3:  EXCOUT  0x0516
    # in jal's delay slot
    RESUME  3f
    RI      0x01, V0, 0
    JAL16   3f
    RR      0, 0, 0x05
    .p2align 2
3:  EXCOUT  0x0518
    # a branch in a delay slot
    RESUME  3f
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x01, V0, 0
    RR      A3, 0, 0x00
    B       3f
    .p2align 2
3:  EXCOUT  0x051a
    # an extended addiupc in a delay slot
    RESUME  3f
    ADDIUPC A3, 3f
    RI      0x09, A3, 1
    RI      0x01, V0, 0
    RR      A3, 0, 0x00
    XRI     0x01, A2, 0
    .p2align 2
3:  EXCOUT  0x051c

    JALX32  done
    NOP16
