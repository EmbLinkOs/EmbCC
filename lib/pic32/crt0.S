/* lib/pic32/crt0.S -- EmbCC's startup for a MIPS PIC32 (--mcu=32MZ...),
 * the same for every part: what differs between parts is in the linker
 * script the driver makes from the device pack (src/driver/pic32.c), and
 * this reads it through these symbols:
 *
 *   _stack, _gp                     the stack's top, the small-data base
 *   _ebase_address, _vector_spacing EBASE and IntCtl.VS
 *   __vector_offset_table/_regs/_count
 *                                   each interrupt's offset from EBASE,
 *                                   the first OFFn register, how many
 *                                   (0 on a part with fixed spacing)
 *   _data_load, _data_begin, _data_end, _bss_begin, _bss_end
 *   __init_array_start, __init_array_end
 *
 * In order, as XC32's startup does: an NMI goes to _nmi_handler; sp and
 * gp, gp in each shadow register set (a handler declared IPLnSRS runs in
 * one); the program's _on_reset; the L1 caches, initialised line by line
 * from Config1's geometry and then made the kseg0 default; EBASE, the
 * vector spacing and each OFFn; .data copied and .bss cleared; exceptions
 * moved off the boot vectors (Status.BEV) and interrupts to the special
 * vector (Cause.IV), the DSP ASE on (Status.MX) where the core has it;
 * _on_bootstrap; the constructors; main(0, 0). Every
 * hook and handler is weak: a program's own takes its place.
 */
    .set    noreorder
    .set    noat

/* ---- the reset vector, 0xBFC00000 (kseg1: uncached) ------------------- */
    .section .reset, "ax", @progbits
    .globl  _reset
_reset:
    la      $26, _startup               /* k0 */
    jr      $26
    nop

/* ---- the startup, in boot flash; caches not on yet ---------------------- */
    .section .reset.startup, "ax", @progbits
    .globl  _startup
    .weak   _on_reset
    .weak   _on_bootstrap
_startup:
    /* an NMI comes through reset with Status.NMI set */
    mfc0    $26, $12, 0
    ext     $26, $26, 19, 1
    beqz    $26, 1f
    nop
    la      $26, _nmi_handler
    jr      $26
    nop
1:
    la      $29, _stack                 /* sp */
    la      $28, _gp                    /* gp */

    /* gp in shadow register sets 1..SRSCtl.HSS */
    mfc0    $8, $12, 2                  /* SRSCtl */
    ext     $9, $8, 26, 4               /* HSS */
    beqz    $9, 3f
    nop
2:  ins     $8, $9, 6, 4                /* PSS = the set */
    mtc0    $8, $12, 2
    ehb
    wrpgpr  $28, $28
    addiu   $9, $9, -1
    bnez    $9, 2b
    nop
    ins     $8, $0, 6, 4                /* PSS = 0, as at reset */
    mtc0    $8, $12, 2
    ehb
3:
    la      $8, _on_reset
    beqz    $8, 4f
    nop
    jalr    $8
    nop
4:
    /* The L1 caches: every line's tag cleared (Index Store Tag, TagLo 0),
     * the instruction cache's then the data cache's, each sized from
     * Config1: line 2 << L bytes (none when L is 0), 64 << S sets of
     * A + 1 ways. Then kseg0 is cacheable, write-back (Config.K0 = 3). */
    mfc0    $8, $16, 1                  /* Config1 */
    ext     $9, $8, 19, 3               /* IL */
    beqz    $9, 6f
    nop
    li      $10, 2
    sllv    $10, $10, $9                /* line bytes */
    ext     $11, $8, 22, 3              /* IS */
    li      $12, 64
    sllv    $12, $12, $11               /* sets */
    ext     $11, $8, 16, 3              /* IA */
    addiu   $11, $11, 1                 /* ways */
    mul     $12, $12, $11               /* lines */
    mtc0    $0, $28, 0                  /* ITagLo */
    ehb
    lui     $13, 0x8000
5:  cache   0x08, 0($13)                /* Index Store Tag, I */
    addiu   $12, $12, -1
    bnez    $12, 5b
    addu    $13, $13, $10
6:
    ext     $9, $8, 10, 3               /* DL */
    beqz    $9, 8f
    nop
    li      $10, 2
    sllv    $10, $10, $9
    ext     $11, $8, 13, 3              /* DS */
    li      $12, 64
    sllv    $12, $12, $11
    ext     $11, $8, 7, 3               /* DA */
    addiu   $11, $11, 1
    mul     $12, $12, $11
    mtc0    $0, $28, 2                  /* DTagLo */
    ehb
    lui     $13, 0x8000
7:  cache   0x09, 0($13)                /* Index Store Tag, D */
    addiu   $12, $12, -1
    bnez    $12, 7b
    addu    $13, $13, $10
    mfc0    $8, $16, 0                  /* Config */
    li      $9, 3
    ins     $8, $9, 0, 3                /* K0: cacheable, write-back */
    mtc0    $8, $16, 0
    ehb
8:
    /* EBASE (written with Status.BEV still set, as at reset), the vector
     * spacing, and each interrupt's offset into OFFn */
    la      $8, _ebase_address
    mtc0    $8, $15, 1
    ehb
    la      $8, _vector_spacing
    sll     $8, $8, 5                   /* IntCtl.VS */
    mtc0    $8, $12, 1
    ehb
    la      $8, __vector_offset_table
    la      $9, __vector_offset_regs
    la      $10, __vector_offset_count
    beqz    $10, 10f
    nop
9:  lw      $11, 0($8)
    sw      $11, 0($9)
    addiu   $8, $8, 4
    addiu   $10, $10, -1
    bnez    $10, 9b
    addiu   $9, $9, 4
10:
    /* .data from flash, .bss cleared (both word-aligned by the script) */
    la      $8, _data_load
    la      $9, _data_begin
    la      $10, _data_end
    b       12f
    nop
11: lw      $11, 0($8)
    sw      $11, 0($9)
    addiu   $8, $8, 4
    addiu   $9, $9, 4
12: bne     $9, $10, 11b
    nop
    la      $9, _bss_begin
    la      $10, _bss_end
    b       14f
    nop
13: sw      $0, 0($9)
    addiu   $9, $9, 4
14: bne     $9, $10, 13b
    nop

    /* interrupts to the special vector (Cause.IV), exceptions off the boot
     * vectors and out of error level (Status.BEV, Status.ERL); and the
     * DSP ASE's registers usable (Status.MX) on a core that has it
     * (Config3.DSPP), as XC32's startup makes them: an RTOS saves and
     * restores $ac1-$ac3 and DSPControl in every context switch, which
     * without MX is a DSP State Disabled exception */
    mfc0    $8, $13, 0
    li      $9, 1
    ins     $8, $9, 23, 1
    mtc0    $8, $13, 0
    mfc0    $8, $12, 0
    ins     $8, $0, 22, 1
    ins     $8, $0, 2, 1
    mfc0    $9, $16, 3                  /* Config3 */
    ext     $9, $9, 10, 1               /* DSPP */
    ins     $8, $9, 24, 1               /* Status.MX */
    mtc0    $8, $12, 0
    ehb

    la      $8, _on_bootstrap
    beqz    $8, 15f
    nop
    jalr    $8
    nop
15:
    la      $16, __init_array_start
    la      $17, __init_array_end
    b       17f
    nop
16: lw      $8, 0($16)
    jalr    $8
    addiu   $16, $16, 4
17: bne     $16, $17, 16b
    nop

    move    $4, $0
    move    $5, $0
    la      $8, main
    jalr    $8
    nop
18: wait                                /* main returned */
    b       18b
    nop

/* ---- exceptions: the general one saves what a C function may change,
 * calls _general_exception_handler, restores it and returns; the others
 * stop, and a debugger finds them stopped ----------------------------- */
    .section .gen_handler, "ax", @progbits
    .globl  _gen_exception
_gen_exception:                         /* EBASE + 0x180: 0x80 bytes */
    la      $26, _general_exception_context
    jr      $26
    nop

    .text
    .globl  _general_exception_context
_general_exception_context:
    addiu   $29, $29, -88
    sw      $1, 4($29)
    sw      $2, 8($29)
    sw      $3, 12($29)
    sw      $4, 16($29)
    sw      $5, 20($29)
    sw      $6, 24($29)
    sw      $7, 28($29)
    sw      $8, 32($29)
    sw      $9, 36($29)
    sw      $10, 40($29)
    sw      $11, 44($29)
    sw      $12, 48($29)
    sw      $13, 52($29)
    sw      $14, 56($29)
    sw      $15, 60($29)
    sw      $24, 64($29)
    sw      $25, 68($29)
    sw      $31, 72($29)
    mfhi    $8
    sw      $8, 76($29)
    mflo    $8
    sw      $8, 80($29)
    la      $8, _general_exception_handler
    jalr    $8
    nop
    lw      $8, 80($29)
    mtlo    $8
    lw      $8, 76($29)
    mthi    $8
    lw      $1, 4($29)
    lw      $2, 8($29)
    lw      $3, 12($29)
    lw      $4, 16($29)
    lw      $5, 20($29)
    lw      $6, 24($29)
    lw      $7, 28($29)
    lw      $8, 32($29)
    lw      $9, 36($29)
    lw      $10, 40($29)
    lw      $11, 44($29)
    lw      $12, 48($29)
    lw      $13, 52($29)
    lw      $14, 56($29)
    lw      $15, 60($29)
    lw      $24, 64($29)
    lw      $25, 68($29)
    lw      $31, 72($29)
    addiu   $29, $29, 88
    eret
    nop

    .section .bev_handler, "ax", @progbits
    .weak   _bootstrap_exception_handler
_bootstrap_exception_handler:
1:  sdbbp   0
    b       1b
    nop

    .section .simple_tlb_refill_vector, "ax", @progbits
    .weak   _simple_tlb_refill_exception_handler
_simple_tlb_refill_exception_handler:
1:  sdbbp   0
    b       1b
    nop

    .section .cache_err_vector, "ax", @progbits
    .weak   _cache_err_exception_handler
_cache_err_exception_handler:
1:  sdbbp   0
    b       1b
    nop

/* the defaults a program replaces: an interrupt with no handler of its own
 * (.vector_default, every OFFn not set), an exception, an NMI */
    .section .vector_default, "ax", @progbits
    .weak   _DefaultInterrupt
_DefaultInterrupt:
1:  sdbbp   0
    b       1b
    nop

    .text
    .weak   _general_exception_handler
_general_exception_handler:
1:  sdbbp   0
    b       1b
    nop

    .weak   _nmi_handler
_nmi_handler:
    eret
    nop
