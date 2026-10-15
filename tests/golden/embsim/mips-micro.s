# mips-micro.s -- microMIPS32 edges for tests/golden/embsim-mips.sh, run
# on QEMU's malta with an M14Kc (MIPS32 and microMIPS) and on EmbSim's
# malta with the pic32mz core, a PIC32MZ's microAptiv. Each case prints
# its number and a result ("0123 deadbeef"), so EmbSim's output can be
# held to QEMU's (and to the record, mips-micro.txt).
#
# Assembled by llvm-mc (mipsel, mips32r2, microMIPS), linked by embld at
# malta's RAM (0x80100000, no entry stub: _start is the image's first
# byte). The microMIPS code carries no relocations -- embld links MIPS32
# ones only -- so its branches are local, its absolute jumps (j, jal,
# jals, which llvm-mc would relocate) are .hword pairs computed from
# BASE, and its data is at fixed addresses: the exception record at
# 0x80200000 (Cause, EPC, BadVAddr, taken, and where to resume), scratch
# memory at 0x80200100.
#
# _start (MIPS32) sets the stack and the general exception vector, then
# jalx's to umain (microMIPS), which returns to it by jr. The handler
# (MIPS32) records the exception and resumes at the address the case
# left at 0x80200010, or past the instruction (2 or 4 bytes, by its
# major opcode, when EPC's bit 0 says microMIPS).
#
# The cases: the 16-bit instructions (their 3-bit registers and encoded
# immediates: andi16, addiur2, addiusp, li16, lbu16's -1, movep's pairs),
# the 32-bit ALU, shifts, multiplies, bit fields, loads and stores (lwl
# to swr, ll and sc, lwp and swp, lwm and swm with each list, lwxs,
# addiupc), the branches (taken and not, the links of jalr, jalrs,
# jalr16, jalrs16, bgezal, bgezals, bltzal, bltzals, jal, jals, jalx with
# their delay slots' sizes, the compact beqzc, bnezc, jrc, jraddiusp),
# the ISA switches (jalx, jr to an even address and back), the
# exceptions (EPC's ISA bit, a delay slot's branch and Cause.BD, a slot
# of the wrong size, a branch in a slot, lwp into its base, lwm in a
# slot, overflow, traps, syscall, break, misaligned accesses, reserved
# encodings), di, ei, mfc0, mtc0, rdpgpr, wrpgpr.

    .equ    BASE, 0x80100000
    .equ    EXC, 0x80200000
    .equ    MEM, 0x80200100
    .equ    THR, 0xbf000900

    .text
    .set    noreorder
    .set    noat

# ---- MIPS32: the start, the exception handler ---------------------------

    .set    nomicromips
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
    jalx    umain
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
    lhu     $k1, -5($k0)                # microMIPS: its first halfword
    srl     $k1, $k1, 10
    andi    $k1, $k1, 7
    addiu   $k1, $k1, -1
    sltiu   $k1, $k1, 3                 # a 16-bit one: 2 bytes, not 4
    sll     $k1, $k1, 1
    subu    $k0, $k0, $k1
2:  mtc0    $k0, $14
    ehb
    eret
    nop

# a MIPS32 function jalx and jr reach: v0 = 0x32, back by jr
    .p2align 2
m32_func:
    li      $v0, 0x32
    jr      $ra
    nop

# reached by jr16 from microMIPS: back to the address in t9 by jr
    .p2align 2
m32_bounce:
    li      $v1, 0x3232
    jr      $t9
    nop

# reached by jalr from microMIPS with a 32-bit delay slot
    .p2align 2
m32_jalx_back:
    li      $v0, 0x3333
    jalx    umain_back                  # MIPS32 to microMIPS: link even
    nop
    jr      $s5
    nop

# ---- microMIPS ------------------------------------------------------------

    .set    micromips

# print a1 (4 digits), a space, a0 (8 digits): clobbers t0-t4, a0, a1
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

out:
    li      $t0, THR
    sll     $a1, $a1, 16
    DIGITS  $a1, 4
    PUTC    32
    DIGITS  $a0, 8
    PUTC    10
    jrc     $ra

# llvm-mc misplaces the fixups of beqz16 and bnez16 and of a forward
# label difference in an instruction's immediate (onto the other
# halfword), so those are built here: r3 a 3-bit register number (0-7:
# s0, s1, v0, v1, a0-a3), registers by number
    .macro  BEQZ16 r3, lbl
    .hword  0x8c00 | (\r3 << 7) | (((\lbl - . - 2) >> 1) & 0x7f)
    .endm
    .macro  BNEZ16 r3, lbl
    .hword  0xac00 | (\r3 << 7) | (((\lbl - . - 2) >> 1) & 0x7f)
    .endm
    .macro  LI_L r, expr                # lui and ori (llvm-mc's li drops the ori)
    .hword  0x41a0 | \r, ((\expr) >> 16) & 0xffff
    .hword  0x5000 | (\r << 5) | \r, (\expr) & 0xffff
    .endm
    .macro  ADDIU_L rt, rs, expr
    .hword  0x3000 | (\rt << 5) | \rs, (\expr) & 0xffff
    .endm

# OUT id, reg: print the case and a register (bals, a 16-bit slot)
    .macro  OUT id, reg
    move    $a0, \reg
    li      $a1, \id
    bgezals $zero, out
    move    $zero, $zero
    .endm

# the exception record: RESUME label (where the handler goes back to,
# microMIPS), EXCOUT id (the code, BD, and EPC from `ref`), cleared
    .macro  RESUME lbl
    bgezals $zero, 94f
    move    $zero, $zero
94: ADDIU_L 13, 31, \lbl - 94b
    sw      $t5, 16($s7)
    .endm

# mfc0 and mtc0 by number (llvm-mc's microMIPS has neither)
    .macro  MFC0 rt, rd
    .hword  (\rt << 5) | \rd, 0x00fc
    .endm
    .macro  MTC0 rt, rd
    .hword  (\rt << 5) | \rd, 0x02fc
    .endm

    .macro  EXCOUT id, ref
    lw      $t6, 0($s7)
    srl     $t7, $t6, 2
    andi    $t7, $t7, 31
    srl     $t6, $t6, 31
    sll     $t6, $t6, 8
    or      $t7, $t7, $t6
    lw      $t6, 12($s7)
    sll     $t6, $t6, 12
    or      $t7, $t7, $t6
    OUT     \id, $t7                    # 1xxx taken, x1xx BD, the code
    lw      $t6, 4($s7)
    beqz    $t6, 96f                    # none taken: 0
    nop
    subu    $t6, $t6, \ref
96: OUT     \id + 1, $t6                # EPC from ref (bit 0 the ISA)
    sw      $zero, 12($s7)
    sw      $zero, 0($s7)
    sw      $zero, 4($s7)
    .endm

# HERE: t8 = the address of its label 95 (the move), the ISA bit set
    .macro  HERE
    bgezals $zero, 95f
    move    $zero, $zero
95: move    $t8, $ra
    .endm

    .p2align 2
umain:
    li      $s7, EXC
    sw      $ra, 0x20($s7)              # back to _start (MIPS32: even)
    li      $s0, 0x12345678
    li      $s1, 0x0f0f00ff
    li      $v0, 0x80000001
    li      $v1, 3

# ---- the 16-bit ALU ----
    addu16  $a2, $s0, $s1
    OUT     0x0101, $a2
    subu16  $a2, $s0, $s1
    OUT     0x0102, $a2
    subu16  $a3, $v1, $v0
    OUT     0x0103, $a3
    move    $a2, $s0
    and16   $a2, $s1
    OUT     0x0104, $a2
    move    $a2, $s0
    or16    $a2, $s1
    OUT     0x0105, $a2
    move    $a2, $s0
    xor16   $a2, $s1
    OUT     0x0106, $a2
    not16   $a2, $s1
    OUT     0x0107, $a2
    sll16   $a2, $s0, 1
    OUT     0x0108, $a2
    sll16   $a2, $s0, 8
    OUT     0x0109, $a2
    srl16   $a2, $v0, 3
    OUT     0x010a, $a2
    srl16   $a2, $v0, 8
    OUT     0x010b, $a2
    li      $v1, 0xffff8f8f
    andi16  $a2, $v1, 128
    OUT     0x0110, $a2
    andi16  $a2, $v1, 1
    OUT     0x0111, $a2
    andi16  $a2, $v1, 2
    OUT     0x0112, $a2
    andi16  $a2, $v1, 3
    OUT     0x0113, $a2
    andi16  $a2, $v1, 4
    OUT     0x0114, $a2
    andi16  $a2, $v1, 7
    OUT     0x0115, $a2
    andi16  $a2, $v1, 8
    OUT     0x0116, $a2
    andi16  $a2, $v1, 15
    OUT     0x0117, $a2
    andi16  $a2, $v1, 16
    OUT     0x0118, $a2
    andi16  $a2, $v1, 31
    OUT     0x0119, $a2
    andi16  $a2, $v1, 32
    OUT     0x011a, $a2
    andi16  $a2, $v1, 63
    OUT     0x011b, $a2
    andi16  $a2, $v1, 64
    OUT     0x011c, $a2
    andi16  $a2, $v1, 255
    OUT     0x011d, $a2
    andi16  $a2, $v1, 32768
    OUT     0x011e, $a2
    andi16  $a2, $v1, 65535
    OUT     0x011f, $a2
    li16    $a2, 0
    OUT     0x0120, $a2
    li16    $a2, 1
    OUT     0x0121, $a2
    li16    $a2, 126
    OUT     0x0122, $a2
    li16    $a2, -1
    OUT     0x0123, $a2
    addiur2 $a2, $s0, 1
    OUT     0x0124, $a2
    addiur2 $a2, $s0, 4
    OUT     0x0125, $a2
    addiur2 $a2, $s0, 8
    OUT     0x0126, $a2
    addiur2 $a2, $s0, 12
    OUT     0x0127, $a2
    addiur2 $a2, $s0, 16
    OUT     0x0128, $a2
    addiur2 $a2, $s0, 20
    OUT     0x0129, $a2
    addiur2 $a2, $s0, 24
    OUT     0x012a, $a2
    addiur2 $a2, $s0, -1
    OUT     0x012b, $a2
    addiur1sp $a2, 0
    subu    $a2, $a2, $sp
    OUT     0x012c, $a2
    addiur1sp $a2, 4
    subu    $a2, $a2, $sp
    OUT     0x012d, $a2
    addiur1sp $a2, 252
    subu    $a2, $a2, $sp
    OUT     0x012e, $a2
    move    $t9, $s0
    addius5 $t9, -8
    OUT     0x0130, $t9
    addius5 $t9, 7
    OUT     0x0131, $t9
    move    $s2, $sp
    addiusp -16
    subu    $a2, $sp, $s2
    OUT     0x0132, $a2
    addiusp 1024
    subu    $a2, $sp, $s2
    OUT     0x0133, $a2
    addiusp 1028
    subu    $a2, $sp, $s2
    OUT     0x0134, $a2
    addiusp -1024
    subu    $a2, $sp, $s2
    OUT     0x0135, $a2
    addiusp -1028
    subu    $a2, $sp, $s2
    OUT     0x0136, $a2
    addiusp -1032
    subu    $a2, $sp, $s2
    OUT     0x0137, $a2
    addiusp 1020
    subu    $a2, $sp, $s2
    OUT     0x0138, $a2
    move    $sp, $s2
    move    $t9, $s0
    move    $a2, $t9
    OUT     0x0140, $a2
    li      $s2, 0x22222222
    li      $s3, 0x33333333
    li      $s4, 0x44444444
    movep   $a2, $a3, $s0, $s1
    OUT     0x0141, $a2
    OUT     0x0142, $a3
    movep   $a1, $a2, $v0, $v1
    move    $t9, $a1
    OUT     0x0143, $t9
    OUT     0x0144, $a2
    movep   $a0, $s5, $zero, $s4
    move    $t9, $a0
    OUT     0x0145, $t9
    OUT     0x0146, $s5
    movep   $a0, $s6, $s2, $s3
    move    $t9, $a0
    OUT     0x0147, $t9
    OUT     0x0148, $s6
    mult    $s0, $s1
    mfhi16  $a2
    OUT     0x0149, $a2
    mflo16  $a3
    OUT     0x014a, $a3

# ---- the 16-bit loads and stores ----
    li      $v1, MEM
    li      $t9, 0x11223344
    sw      $t9, 0($v1)
    li      $t9, 0x8899aabb
    sw      $t9, 4($v1)
    li      $t9, 0xccddeeff
    sw      $t9, 8($v1)
    li      $t9, 0x01020304
    sw      $t9, 60($v1)
    lbu16   $a2, 0($v1)
    OUT     0x0201, $a2
    lbu16   $a2, 7($v1)
    OUT     0x0202, $a2
    lbu16   $a2, 14($v1)
    OUT     0x0203, $a2
    addiur2 $a3, $v1, 4
    lbu16   $a2, -1($a3)
    OUT     0x0204, $a2
    lhu16   $a2, 0($v1)
    OUT     0x0205, $a2
    lhu16   $a2, 6($v1)
    OUT     0x0206, $a2
    lhu16   $a2, 30($v1)
    OUT     0x0207, $a2
    lw16    $a2, 4($v1)
    OUT     0x0208, $a2
    lw16    $a2, 60($v1)
    OUT     0x0209, $a2
    li      $s1, 0xa1b2c3d4
    sb16    $s1, 1($v1)
    sb16    $zero, 15($v1)
    sh16    $s1, 6($v1)
    sh16    $zero, 30($v1)
    sw16    $s1, 8($v1)
    sw16    $zero, 56($v1)
    lw      $a2, 0($v1)
    OUT     0x020a, $a2
    lw      $a2, 4($v1)
    OUT     0x020b, $a2
    lw      $a2, 8($v1)
    OUT     0x020c, $a2
    lw      $a2, 12($v1)
    OUT     0x020d, $a2
    lw      $a2, 28($v1)
    OUT     0x020e, $a2
    lw      $a2, 56($v1)
    OUT     0x020f, $a2
    addiusp -128
    li      $t9, 0x5a5a1234
    sw      $t9, 124($sp)
    lw      $t8, 124($sp)
    OUT     0x0210, $t8
    sw      $s1, 0($sp)
    lw      $t8, 0($sp)
    OUT     0x0211, $t8
    move    $s3, $gp
    li      $gp, MEM + 8
    lw      $a2, -4($gp)
    OUT     0x0212, $a2
    lw      $a2, 4($gp)
    OUT     0x0213, $a2
    move    $gp, $s3
    li      $s0, 0x50505050
    li      $s1, 0x51515151
    li      $s2, 0x52525252
    li      $s3, 0x53535353
    li      $ra, 0x7a7a7a7a
    swm16   $16, $17, $18, $19, $ra, 4($sp)
    swm16   $16, $ra, 60($sp)
    lw      $a2, 4($sp)
    OUT     0x0214, $a2
    lw      $a2, 16($sp)
    OUT     0x0215, $a2
    lw      $a2, 20($sp)
    OUT     0x0216, $a2
    lw      $a2, 60($sp)
    OUT     0x0217, $a2
    lw      $a2, 64($sp)
    OUT     0x0218, $a2
    li      $t9, 0x60606060
    sw      $t9, 32($sp)
    li      $t9, 0x61616161
    sw      $t9, 36($sp)
    li      $t9, 0x62626262
    sw      $t9, 40($sp)
    li      $t9, 0x6e6e6e6e
    sw      $t9, 44($sp)
    lwm16   $16, $17, $18, $ra, 32($sp)
    move    $t9, $ra
    OUT     0x0219, $s0
    OUT     0x021a, $s1
    OUT     0x021b, $s2
    OUT     0x021c, $t9
    lwm16   $16, $ra, 36($sp)
    move    $t9, $ra
    OUT     0x021d, $s0
    OUT     0x021e, $t9
    addiusp 128

# ---- the 32-bit ALU ----
    li      $s0, 0x12345678
    li      $s1, 0xfedcba98
    li      $s2, 0x7fffffff
    li      $s3, 5
    addu    $t9, $s0, $s1
    OUT     0x0301, $t9
    subu    $t9, $s0, $s1
    OUT     0x0302, $t9
    subu    $t9, $s1, $s0
    OUT     0x0303, $t9
    and     $t9, $s0, $s1
    OUT     0x0304, $t9
    or      $t9, $s0, $s1
    OUT     0x0305, $t9
    nor     $t9, $s0, $s1
    OUT     0x0306, $t9
    xor     $t9, $s0, $s1
    OUT     0x0307, $t9
    slt     $t9, $s0, $s1
    OUT     0x0308, $t9
    slt     $t9, $s1, $s0
    OUT     0x0309, $t9
    sltu    $t9, $s0, $s1
    OUT     0x030a, $t9
    sllv    $t9, $s0, $s3
    OUT     0x030b, $t9
    srlv    $t9, $s1, $s3
    OUT     0x030c, $t9
    srav    $t9, $s1, $s3
    OUT     0x030d, $t9
    rotrv   $t9, $s0, $s3
    OUT     0x030e, $t9
    sll     $t9, $s0, 31
    OUT     0x030f, $t9
    srl     $t9, $s1, 1
    OUT     0x0310, $t9
    sra     $t9, $s1, 4
    OUT     0x0311, $t9
    rotr    $t9, $s0, 8
    OUT     0x0312, $t9
    add     $t9, $s0, $s3
    OUT     0x0313, $t9
    sub     $t9, $s0, $s3
    OUT     0x0314, $t9
    addi    $t9, $s0, -32768
    OUT     0x0315, $t9
    addiu   $t9, $s0, 32767
    OUT     0x0316, $t9
    slti    $t9, $s1, -1
    OUT     0x0317, $t9
    sltiu   $t9, $s0, 0x7fff
    OUT     0x0318, $t9
    sltiu   $t9, $s3, -1
    OUT     0x0319, $t9
    andi    $t9, $s1, 0xf0f0
    OUT     0x031a, $t9
    ori     $t9, $s0, 0xf0f0
    OUT     0x031b, $t9
    xori    $t9, $s0, 0xffff
    OUT     0x031c, $t9
    lui     $t9, 0x8765
    OUT     0x031d, $t9
    mul     $t9, $s0, $s1
    OUT     0x031e, $t9
    mult    $s0, $s1
    mfhi    $t9
    OUT     0x031f, $t9
    mflo    $t9
    OUT     0x0320, $t9
    multu   $s0, $s1
    mfhi    $t9
    OUT     0x0321, $t9
    div     $zero, $s1, $s3
    mflo    $t9
    OUT     0x0322, $t9
    mfhi    $t9
    OUT     0x0323, $t9
    divu    $zero, $s1, $s3
    mflo    $t9
    OUT     0x0324, $t9
    mfhi    $t9
    OUT     0x0325, $t9
    mthi    $s0
    mtlo    $s1
    madd    $s0, $s3
    mfhi    $t9
    OUT     0x0326, $t9
    mflo    $t9
    OUT     0x0327, $t9
    maddu   $s1, $s3
    mflo    $t9
    OUT     0x0328, $t9
    msub    $s2, $s2
    mfhi    $t9
    OUT     0x0329, $t9
    msubu   $s1, $s1
    mflo    $t9
    OUT     0x032a, $t9
    clz     $t9, $s3
    OUT     0x032b, $t9
    clo     $t9, $s1
    OUT     0x032c, $t9
    seb     $t9, $s1
    OUT     0x032d, $t9
    seh     $t9, $s1
    OUT     0x032e, $t9
    wsbh    $t9, $s0
    OUT     0x032f, $t9
    ext     $t9, $s0, 4, 12
    OUT     0x0330, $t9
    ext     $t9, $s1, 0, 32
    OUT     0x0331, $t9
    move    $t9, $s1
    ins     $t9, $s0, 8, 8
    OUT     0x0332, $t9
    move    $t9, $s1
    ins     $t9, $s0, 31, 1
    OUT     0x0333, $t9
    li      $t9, 1
    movn    $t9, $s0, $s3
    OUT     0x0334, $t9
    movn    $t9, $s1, $zero
    OUT     0x0335, $t9
    movz    $t9, $s1, $zero
    OUT     0x0336, $t9
    li      $s4, MEM
    li      $s5, 2
    lwxs    $t9, $s5($s4)
    OUT     0x0337, $t9

# ---- the 32-bit loads and stores ----
    li      $s4, MEM
    li      $t9, 0x80ff7f01
    sw      $t9, 16($s4)
    li      $t9, 0x12345678
    sw      $t9, 20($s4)
    lb      $t9, 18($s4)
    OUT     0x0401, $t9
    lbu     $t9, 18($s4)
    OUT     0x0402, $t9
    lh      $t9, 18($s4)
    OUT     0x0403, $t9
    lhu     $t9, 18($s4)
    OUT     0x0404, $t9
    lw      $t9, 20($s4)
    OUT     0x0405, $t9
    sb      $s1, 21($s4)
    sh      $s1, 22($s4)
    lw      $t9, 20($s4)
    OUT     0x0406, $t9
    li      $t9, 0x11111111
    lwl     $t9, 18($s4)
    OUT     0x0407, $t9
    li      $t9, 0x11111111
    lwr     $t9, 18($s4)
    OUT     0x0408, $t9
    swl     $s0, 25($s4)
    swr     $s0, 30($s4)
    lw      $t9, 24($s4)
    OUT     0x0409, $t9
    lw      $t9, 28($s4)
    OUT     0x040a, $t9
    ll      $t9, 20($s4)
    addiu   $t9, $t9, 1
    sc      $t9, 20($s4)
    OUT     0x040b, $t9
    lw      $t9, 20($s4)
    OUT     0x040c, $t9
    swp     $s0, 32($s4)
    lwp     $s2, 32($s4)
    OUT     0x040d, $s2
    OUT     0x040e, $s3
    li      $s0, 0x70707070
    li      $s1, 0x71717171
    li      $s2, 0x72727272
    li      $s3, 0x73737373
    li      $t9, 0x75757575
    move    $s5, $t9
    li      $t9, 0x76767676
    move    $s6, $t9
    li      $t9, 0x77777777
    move    $s7, $t9
    move    $t9, $fp
    li      $fp, 0x7f7f7f7f
    li      $ra, 0x7e7e7e7e
    li      $at, MEM + 0x40
    swm32   $16, $17, $18, $19, $20, $21, $22, $23, $fp, $ra, 0($at)
    move    $fp, $t9
    li      $s7, EXC
    li      $s4, MEM + 0x40
    lw      $t9, 0($s4)
    OUT     0x0410, $t9
    lw      $t9, 28($s4)
    OUT     0x0411, $t9
    lw      $t9, 32($s4)
    OUT     0x0412, $t9
    lw      $t9, 36($s4)
    OUT     0x0413, $t9
    move    $t8, $fp
    lwm32   $16, $17, $18, $19, $20, $21, $22, $23, $fp, 4($s4)
    move    $t9, $fp
    move    $fp, $t8
    li      $s7, EXC
    li      $s4, MEM + 0x40
    OUT     0x0414, $s0
    OUT     0x0415, $s6
    OUT     0x0416, $t9
    lwm32   $ra, 0($s4)
    move    $t9, $ra
    OUT     0x0417, $t9
    lwm32   $16, $17, -32($s4)
    OUT     0x0418, $s1
    HERE                                # addiupc: from its address, word-aligned
    addiupc $a2, 8
    subu    $t9, $a2, $t8
    OUT     0x0419, $t9
    HERE
    move    $zero, $zero
    addiupc $a2, -4
    subu    $t9, $a2, $t8
    OUT     0x041a, $t9

# ---- branches ----
    li      $s0, 0
    li      $s1, 1
    li      $s2, -1
    li      $a2, 0
    b16     1f
    addiu   $a2, $a2, 1                 # slot
    addiu   $a2, $a2, 0x100             # skipped
1:  BEQZ16  0, 1f
    addiu   $a2, $a2, 2
    addiu   $a2, $a2, 0x200
1:  BEQZ16  1, 1f
    addiu   $a2, $a2, 4
    addiu   $a2, $a2, 0x400             # not taken: runs
1:  BNEZ16  1, 1f
    move    $zero, $zero
    addiu   $a2, $a2, 0x800
1:  BNEZ16  0, 1f
    move    $zero, $zero
    addiu   $a2, $a2, 8                 # not taken: runs
1:  OUT     0x0501, $a2
    li      $a2, 0
    beq     $s1, $s1, 1f
    addiu   $a2, $a2, 1
    addiu   $a2, $a2, 0x100
1:  bne     $s0, $s1, 1f
    addiu   $a2, $a2, 2
    addiu   $a2, $a2, 0x200
1:  beq     $s0, $s1, 1f
    addiu   $a2, $a2, 4
    addiu   $a2, $a2, 0x400             # runs
1:  bltz    $s2, 1f
    nop
    addiu   $a2, $a2, 0x800
1:  bltz    $s1, 1f
    nop
    addiu   $a2, $a2, 8                 # runs
1:  bgez    $s0, 1f
    nop
    addiu   $a2, $a2, 0x1000
1:  blez    $s0, 1f
    nop
    addiu   $a2, $a2, 0x2000
1:  bgtz    $s0, 1f
    nop
    addiu   $a2, $a2, 16                # runs
1:  bgtz    $s1, 1f
    nop
    addiu   $a2, $a2, 0x4000
1:  OUT     0x0502, $a2
    li      $a2, 0
    beqzc   $s0, 1f
    addiu   $a2, $a2, 0x100             # no slot: skipped
1:  beqzc   $s1, 1f
    addiu   $a2, $a2, 1                 # runs
1:  bnezc   $s1, 1f
    addiu   $a2, $a2, 0x200
1:  bnezc   $s0, 1f
    addiu   $a2, $a2, 2                 # runs
1:  OUT     0x0503, $a2
    # backward
    li      $a2, 0
    li      $a3, 3
1:  addiu   $a2, $a2, 5
    addiu   $a3, $a3, -1
    BNEZ16  7, 1b
    move    $zero, $zero
    OUT     0x0504, $a2
    # the links: ra from the label after the slot (bit 0: microMIPS)
    HERE
    bgezal  $zero, 2f                   # bal: a 32-bit slot
    nop
2:  subu    $t9, $ra, $t8
    OUT     0x0505, $t9
    HERE
    bgezals $zero, 2f
    move    $zero, $zero
2:  subu    $t9, $ra, $t8
    OUT     0x0506, $t9
    HERE
    bltzal  $s2, 2f
    nop
2:  subu    $t9, $ra, $t8
    OUT     0x0507, $t9
    HERE
    bltzals $s2, 2f
    move    $zero, $zero
2:  subu    $t9, $ra, $t8
    OUT     0x0508, $t9
    li      $ra, 0
    bltzal  $s0, 2f                     # not taken: links anyway
    nop
2:  sltu    $t9, $zero, $ra
    OUT     0x0509, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jalr    $s5, $t9
    nop
3:  subu    $t9, $s5, $t8
    OUT     0x050a, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jalrs   $s5, $t9
    move    $zero, $zero
3:  subu    $t9, $s5, $t8
    OUT     0x050b, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jalr    $t9                         # jalr16: a 32-bit slot
    nop
3:  subu    $t9, $ra, $t8
    OUT     0x050c, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jalrs16 $t9
    move    $zero, $zero
3:  subu    $t9, $ra, $t8
    OUT     0x050d, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jr16    $t9
    addiu   $t9, $t9, 0x40              # the slot runs
    addiu   $t9, $t9, 0x80
3:  subu    $t9, $t9, $t8
    OUT     0x050e, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jrc     $t9
    addiu   $t9, $t9, 0x40              # no slot
3:  subu    $t9, $t9, $t8
    OUT     0x050f, $t9
    HERE
    ADDIU_L 25, 24, 3f - 95b
    jr      $t9
    nop
    addiu   $t9, $t9, 0x40
3:  subu    $t9, $t9, $t8
    OUT     0x0510, $t9
    # jraddiusp: back to ra, sp up
    move    $s2, $sp
    HERE
    ADDIU_L 31, 24, 3f - 95b
    jraddiusp 16
    addiu   $s2, $s2, 0x40              # no slot
3:  subu    $t9, $sp, $s2
    OUT     0x0511, $t9
    move    $sp, $s2
    # j, jal, jals: their targets from BASE (llvm-mc would relocate them)
    li      $a2, 0
    .hword  0xd400 | (((BASE + (3f - _start)) >> 17) & 0x3ff)    # j 3f
    .hword  ((BASE + (3f - _start)) >> 1) & 0xffff
    addiu   $a2, $a2, 1                 # slot
    addiu   $a2, $a2, 0x100
3:  OUT     0x0512, $a2
    HERE
    .hword  0xf400 | (((BASE + (3f - _start)) >> 17) & 0x3ff)    # jal 3f
    .hword  ((BASE + (3f - _start)) >> 1) & 0xffff
    nop
3:  subu    $t9, $ra, $t8
    OUT     0x0513, $t9
    HERE
    .hword  0x7400 | (((BASE + (3f - _start)) >> 17) & 0x3ff)    # jals 3f
    .hword  ((BASE + (3f - _start)) >> 1) & 0xffff
    move    $zero, $zero
3:  subu    $t9, $ra, $t8
    OUT     0x0514, $t9

# ---- the ISA switches ----
    li      $v0, 0
    .hword  0xf000 | (((BASE + (m32_func - _start)) >> 18) & 0x3ff)   # jalx m32_func
    .hword  ((BASE + (m32_func - _start)) >> 2) & 0xffff
    nop
    OUT     0x0601, $v0
    HERE
    ADDIU_L 25, 24, 3f - 95b           # back here: odd
    li      $v1, 0
    LI_L    1, BASE + (m32_bounce - _start)
    jr16    $at                         # even: to MIPS32
    nop
3:  OUT     0x0602, $v1
    # MIPS32 code jalx-ing back into microMIPS
    HERE
    ADDIU_L 21, 24, 3f - 95b
    LI_L    1, BASE + (m32_jalx_back - _start)
    jalr    $at
    nop
    .p2align 2
umain_back:
    move    $s4, $ra
    move    $t9, $v0
    OUT     0x0603, $t9
    jr      $s4                         # even: back to MIPS32, which jr's to 3f
    nop
3:  OUT     0x0604, $v0

# ---- exceptions ----
    li      $s0, 0x7fffffff
    li      $s1, 1
    li      $s4, MEM
    HERE
    add     $t9, $s0, $s1
    EXCOUT  0x0701, $t8
    HERE
    addi    $t9, $s0, 1
    EXCOUT  0x0703, $t8
    HERE
    teq     $s1, $s1
    EXCOUT  0x0705, $t8
    HERE
    tlti    $s1, 5
    EXCOUT  0x0707, $t8
    HERE
    tgei    $s1, 5
    EXCOUT  0x0709, $t8
    HERE
    syscall
    EXCOUT  0x070b, $t8
    HERE
    break
    EXCOUT  0x070d, $t8
    HERE
    break16 7
    EXCOUT  0x070f, $t8
    HERE
    lw      $t9, 2($s4)
    EXCOUT  0x0711, $t8
    lw      $t9, 8($s7)
    subu    $t9, $t9, $s4
    OUT     0x0713, $t9                 # BadVAddr
    move    $v1, $s4
    HERE
    addiur2 $a2, $v1, 1
    lw16    $a3, 0($a2)
    EXCOUT  0x0714, $t8
    HERE
    sh      $s1, 3($s4)
    EXCOUT  0x0716, $t8
    HERE
    .hword  0xa400                      # major 0x29: none
    EXCOUT  0x0718, $t8
    HERE
    .hword  0x0000, 0x003f              # POOL32A 0x3f (Release 6's sigrie)
    EXCOUT  0x071a, $t8
    HERE
    .hword  0x0000, 0x0010 | (15 << 6)  # POOL32A 0x10, minor 15: none
    EXCOUT  0x071c, $t8
    # in a delay slot: EPC the branch (ISA bit set), Cause.BD
    RESUME  3f
    HERE
    b16     3f
    lw      $t9, 2($s4)
3:  EXCOUT  0x0720, $t8
    RESUME  3f
    addiur2 $a2, $v1, 1
    HERE
    beq     $zero, $zero, 3f
    lw16    $a3, 0($a2)
3:  EXCOUT  0x0722, $t8
    # a slot of the wrong size: jals wants 16 bits, bal 32
    RESUME  3f
    HERE
    bgezals $zero, 3f
    nop
3:  EXCOUT  0x0724, $t8
    RESUME  3f
    HERE
    bgezal  $zero, 3f
    move    $zero, $zero
3:  EXCOUT  0x0726, $t8
    # a branch in a slot
    RESUME  3f
    HERE
    beq     $zero, $zero, 3f
    b16     3f
    move    $zero, $zero
3:  EXCOUT  0x0728, $t8
    # lwm in a slot, lwp into its base
    RESUME  3f
    HERE
    beq     $zero, $zero, 3f
    lwm32   $16, 0($s4)
3:  EXCOUT  0x072a, $t8
    HERE
    move    $s2, $s4
    .hword  0x2252, 0x1000              # lwp $s2, 0($s2) (llvm-mc refuses it)
    EXCOUT  0x072c, $t8
    li      $s7, EXC

# ---- coprocessor 0 ----
    di      $t9
    MFC0    25, 12
    andi    $t9, $t9, 1
    OUT     0x0801, $t9
    ei      $t9
    andi    $t9, $t9, 1
    OUT     0x0802, $t9
    MFC0    25, 12
    andi    $t9, $t9, 1
    OUT     0x0803, $t9
    di
    li      $s0, 0x12345674
    MTC0    16, 14
    MFC0    25, 14
    OUT     0x0804, $t9
    MTC0    16, 30
    MFC0    25, 30
    OUT     0x0805, $t9
    li      $s1, 0x5555
    .hword  0x0271, 0xf17c              # wrpgpr $s3, $s1 (llvm-mc has none)
    .hword  0x0333, 0xe17c              # rdpgpr $t9, $s3
    OUT     0x0806, $t9
    OUT     0x0807, $s3
    di

    li      $t0, THR
    PUTC    100
    PUTC    111
    PUTC    110
    PUTC    101
    PUTC    10
    li      $s7, EXC
    lw      $ra, 0x20($s7)
    jr      $ra
    nop
