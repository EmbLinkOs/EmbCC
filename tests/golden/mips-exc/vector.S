/* A MIPS32 general exception handler that RETURNS, written as a .S file
 * for EmbCC's assembler (tests/golden/mips-exc.sh).
 *
 * exc_stub..exc_stub_end are the words main() copies to the general
 * exception vector, EBase + 0x180 = 0x80000180: they jump to exc_entry
 * through k0, the register the ABI leaves to exception code. exc_entry
 * saves every register C may clobber (and HI/LO), calls
 * exc_c(cause, epc, frame) -- which may rewrite saved registers in the
 * frame and returns the address to resume at -- restores, and erets.
 *
 * The file is in `.set reorder` mode (GNU as's default) except where the
 * stub must be exactly four words: the assembler fills each delay slot
 * with a nop there. */
#define CP0_CAUSE $13
#define CP0_EPC   $14

#define FRAME   (16 + 4 * 24)       /* home area, then 24 saved words */
#define SAVE(r, k)  sw r, (16 + 4 * (k))($sp)
#define LOAD(r, k)  lw r, (16 + 4 * (k))($sp)

    .text
    .set noat                       /* $at is saved, by hand */

    .globl exc_stub
    .globl exc_stub_end
    .set push
    .set noreorder
exc_stub:
    lui     $k0, %hi(exc_entry)
    addiu   $k0, $k0, %lo(exc_entry)
    jr      $k0
    nop
exc_stub_end:
    .set pop

    .globl exc_entry
    .ent exc_entry
    .type exc_entry, @function
exc_entry:
    addiu   $sp, $sp, -FRAME
    SAVE($at, 0)
    SAVE($v0, 1)
    SAVE($v1, 2)
    SAVE($a0, 3)
    SAVE($a1, 4)
    SAVE($a2, 5)
    SAVE($a3, 6)
    SAVE($t0, 7)
    SAVE($t1, 8)
    SAVE($t2, 9)
    SAVE($t3, 10)
    SAVE($t4, 11)
    SAVE($t5, 12)
    SAVE($t6, 13)
    SAVE($t7, 14)
    SAVE($t8, 15)
    SAVE($t9, 16)
    SAVE($ra, 17)
    mfhi    $t0
    mflo    $t1
    SAVE($t0, 18)
    SAVE($t1, 19)

    mfc0    $a0, CP0_CAUSE
    mfc0    $a1, CP0_EPC
    addiu   $a2, $sp, 16            /* the saved words, for C to edit */
    jal     exc_c                   /* the resume address comes back */
    mtc0    $v0, CP0_EPC
    ehb

    LOAD($t0, 18)
    LOAD($t1, 19)
    mthi    $t0
    mtlo    $t1
    LOAD($at, 0)
    LOAD($v0, 1)
    LOAD($v1, 2)
    LOAD($a0, 3)
    LOAD($a1, 4)
    LOAD($a2, 5)
    LOAD($a3, 6)
    LOAD($t0, 7)
    LOAD($t1, 8)
    LOAD($t2, 9)
    LOAD($t3, 10)
    LOAD($t4, 11)
    LOAD($t5, 12)
    LOAD($t6, 13)
    LOAD($t7, 14)
    LOAD($t8, 15)
    LOAD($t9, 16)
    LOAD($ra, 17)
    addiu   $sp, $sp, FRAME
    eret
    .end exc_entry
    .size exc_entry, .-exc_entry
