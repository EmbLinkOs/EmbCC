/* embsvd -- a microcontroller's register database, from its CMSIS-SVD file.
 *
 * Every Cortex-M vendor publishes an SVD: an XML description of the
 * device's peripherals, their registers and bit fields, and its
 * interrupts. From it this tool writes what a project starts with:
 *
 *   --header FILE    the device header, in the shape CMSIS's svdconv and
 *                    the vendors' own headers have: IRQn_Type, one struct
 *                    per peripheral layout and per cluster, base
 *                    addresses, instance pointers, and _Pos/_Msk for
 *                    every field
 *   --startup FILE   a startup file in C: the vector table, with a weak
 *                    handler per interrupt, and a Reset_Handler that
 *                    copies .data, zeroes .bss, runs the constructors and
 *                    calls main
 *   --ld FILE        a linker script for it, in STM32CubeMX's shape, given
 *                    --flash ORIGIN:LENGTH and --ram ORIGIN:LENGTH (an SVD
 *                    describes peripherals, not memories)
 *   --json FILE      the hardware for tools: the cpu, the memories (from
 *                    --flash and --ram), and every peripheral with its
 *                    interrupts and every register, arrays and clusters
 *                    expanded, at its absolute address, with its fields
 *                    and their enumerated values
 *   --nvic-prio-bits N, --fpu-present 0|1
 *                    what the header tells CMSIS about the core, where the
 *                    vendor's SVD is wrong (ST's STM32F405.svd 1.2 says 3
 *                    priority bits and no FPU; the part has 4 and one)
 *   --list           the peripherals, their addresses and interrupts
 *   --show NAME      one peripheral's registers and fields
 *
 * Clusters, nested to any depth, and arrays of registers, clusters,
 * fields and peripherals are laid out as svdconv lays them out; a
 * register, cluster, field or enumeratedValues derivedFrom another is
 * resolved. What an SVD can say that this does not handle is refused by
 * name rather than written approximately: a header whose offsets are
 * wrong compiles and then drives the wrong register.
 *
 * The reading is svd.c's, which embsim shares; this file is what is
 * written from it.
 *
 * ISO C and standalone, like embar: no part of the compiler is needed.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svd.h"

/* ---- a part's boot stage: the RP2040's boot2 ---------------------------- */

/* The RP2040 boots from flash through boot2, the flash's first 256 bytes:
 * its bootrom copies them to SRAM if the CRC-32 of the first 252 is the
 * last word, and runs them; boot2 sets up the XIP and goes to the vector
 * table at flash + 0x100 (the datasheet's 2.8.1). So for the RP2040 the
 * startup holds a boot2, in .boot2, and the linker script puts it first.
 * This one sets the XIP up for 03h serial reads, which any SPI flash
 * answers -- the settings of the Pico SDK's boot2_generic_03h --
 * assembled (llvm-mc --triple=thumbv6m) from:
 *
 *         push {lr}
 *         ldr r3, =0x18000000          @ XIP_SSI_BASE
 *         movs r1, #0
 *         str r1, [r3, #8]             @ SSIENR: off, to set it up
 *         movs r1, #4
 *         str r1, [r3, #0x14]          @ BAUDR: SCK at clk_sys / 4
 *         ldr r1, =0x001f0300          @ CTRLR0: 32-bit frames, EEPROM read
 *         str r1, [r3, #0]
 *         ldr r1, =0x03000218          @ SPI_CTRLR0: 03h, 8-bit instruction,
 *         ldr r0, =0x180000f4          @   24-bit address
 *         str r1, [r0]
 *         movs r1, #0
 *         str r1, [r3, #4]             @ CTRLR1: one frame
 *         movs r1, #1
 *         str r1, [r3, #8]             @ SSIENR: on
 *         pop {r0}                     @ the bootrom's lr is 0; a caller's
 *         cmp r0, #0                   @   returns there
 *         beq 1f
 *         bx r0
 *     1:  ldr r0, =0x10000100          @ the vector table, after boot2
 *         ldr r1, =0xe000ed08          @ VTOR
 *         str r0, [r1]
 *         ldmia r0, {r0, r1}
 *         msr msp, r0
 *         bx r1
 *
 * and padded with zeros; embsvd adds the CRC (tests/golden/embsim-pico.sh
 * holds these bytes to the assembler's). */
static const unsigned char rp2040_boot2[] = {
    0x00, 0xb5, 0x0c, 0x4b, 0x00, 0x21, 0x99, 0x60, 0x04, 0x21, 0x59, 0x61,
    0x0a, 0x49, 0x19, 0x60, 0x0a, 0x49, 0x0b, 0x48, 0x01, 0x60, 0x00, 0x21,
    0x59, 0x60, 0x01, 0x21, 0x99, 0x60, 0x01, 0xbc, 0x00, 0x28, 0x00, 0xd0,
    0x00, 0x47, 0x07, 0x48, 0x07, 0x49, 0x08, 0x60, 0x03, 0xc8, 0x80, 0xf3,
    0x08, 0x88, 0x08, 0x47, 0x00, 0x00, 0x00, 0x18, 0x00, 0x03, 0x1f, 0x00,
    0x18, 0x02, 0x00, 0x03, 0xf4, 0x00, 0x00, 0x18, 0x00, 0x01, 0x00, 0x10,
    0x08, 0xed, 0x00, 0xe0,
};

static int is_rp2040(const struct svd_device *d)
{
    return d->name && !strcmp(d->name, "RP2040");
}

/* a RISC-V part: its SVD's cpu is RV32IMC or the like, and it has none of
 * the Cortex-M's exceptions */
static int cpu_riscv(const struct svd_device *d)
{
    return d->cpu && !strncmp(d->cpu, "RV", 2);
}

static int is_esp32c3(const struct svd_device *d)
{
    return d->name && !strcmp(d->name, "ESP32-C3");
}

/* the bootrom's checksum: CRC-32 with polynomial 0x04c11db7, seed
 * 0xffffffff, not reflected, no final XOR */
static unsigned long crc32_mpeg2(const unsigned char *p, int n)
{
    unsigned long c = 0xffffffffUL;
    for (int i = 0; i < n; i++) {
        c ^= (unsigned long)p[i] << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000UL ? c << 1 ^ 0x04c11db7UL : c << 1) & 0xffffffffUL;
    }
    return c;
}

static void write_boot2(FILE *f)
{
    unsigned char b[256] = { 0 };
    memcpy(b, rp2040_boot2, sizeof rp2040_boot2);
    unsigned long crc = crc32_mpeg2(b, 252);
    for (int i = 0; i < 4; i++)
        b[252 + i] = (unsigned char)(crc >> (8 * i));
    fprintf(f, "/* boot2: the flash's first 256 bytes, which the RP2040's bootrom\n"
               " * checks (the CRC-32 of the first 252 is the last word), copies to\n"
               " * SRAM and runs: it sets the XIP up for 03h reads, as the Pico\n"
               " * SDK's boot2_generic_03h does, and goes to the vector table\n"
               " * after it (embsvd's tools/embsvd/embsvd.c has its source). */\n"
               "__attribute__((section(\".boot2\"), used))\n"
               "const unsigned char __boot2[256] = {");
    for (int i = 0; i < 256; i++)
        fprintf(f, "%s0x%02x,", i % 12 ? " " : "\n    ", b[i]);
    fprintf(f, "\n};\n\n");
}

/* ---- the header -------------------------------------------------------- */

static void comment(FILE *f, const char *s)
{
    if (!s)
        return;
    fputs("  /*!< ", f);
    for (; *s; s++)
        if (!(s[0] == '*' && s[1] == '/'))
            fputc(*s, f);
    fputs(" */", f);
}

struct core_exc { int n; const char *irq, *handler; int m0; };
static const struct core_exc core_excs[] = {
    { -14, "NonMaskableInt", "NMI_Handler", 1 },
    { -13, "HardFault", "HardFault_Handler", 1 },
    { -12, "MemoryManagement", "MemManage_Handler", 0 },
    { -11, "BusFault", "BusFault_Handler", 0 },
    { -10, "UsageFault", "UsageFault_Handler", 0 },
    { -5, "SVCall", "SVC_Handler", 1 },
    { -4, "DebugMonitor", "DebugMon_Handler", 0 },
    { -2, "PendSV", "PendSV_Handler", 1 },
    { -1, "SysTick", "SysTick_Handler", 1 },
};

/* ARMv6-M and ARMv8-M Baseline have no MemManage, BusFault, UsageFault
 * or DebugMonitor exception */
static int cpu_baseline(const struct svd_device *d)
{
    return d->cpu && (!strncmp(d->cpu, "CM0", 3) || !strcmp(d->cpu, "CM1") ||
                      !strcmp(d->cpu, "CM23"));
}

/* CMSIS's core header for the SVD's cpu name */
static const char *core_header(const struct svd_device *d)
{
    static const struct { const char *svd, *h, *rev; } m[] = {
        { "CM0", "core_cm0.h", "__CM0_REV" },
        { "CM0PLUS", "core_cm0plus.h", "__CM0PLUS_REV" },
        { "CM0+", "core_cm0plus.h", "__CM0PLUS_REV" },
        { "CM1", "core_cm1.h", "__CM1_REV" },
        { "CM3", "core_cm3.h", "__CM3_REV" },
        { "CM4", "core_cm4.h", "__CM4_REV" },
        { "CM7", "core_cm7.h", "__CM7_REV" },
        { "CM23", "core_cm23.h", "__CM23_REV" },
        { "CM33", "core_cm33.h", "__CM33_REV" },
        { "CM55", "core_cm55.h", "__CM55_REV" },
        { "CM85", "core_cm85.h", "__CM85_REV" },
    };
    for (size_t i = 0; d->cpu && i < sizeof m / sizeof m[0]; i++)
        if (!strcmp(d->cpu, m[i].svd))
            return m[i].h;
    return NULL;
}

static const char *core_rev_macro(const struct svd_device *d)
{
    static char buf[32];
    const char *h = core_header(d);
    if (!h)
        return NULL;
    /* core_cm4.h -> __CM4_REV */
    snprintf(buf, sizeof buf, "__%.*s_REV", (int)(strlen(h) - 7), h + 5);
    for (char *q = buf; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    return buf;
}

static unsigned cpu_rev_value(const char *rev)
{
    unsigned r = 0, p = 0;
    if (rev && sscanf(rev, "r%up%u", &r, &p) == 2)
        return r << 8 | p;
    return 0;
}

/* The ARM-defined part of the private peripheral bus -- ITM, DWT, FPB,
 * the System Control Space (NVIC, SCB, SysTick, MPU, FPU), TPIU, ETM --
 * which CMSIS-Core describes. With it included, a vendor's SVD copy of
 * them would define NVIC_Type and SCB_HFSR_FORCED_Msk a second time, so
 * they are left out, as svdconv leaves them out. 0xE0042000 and up is the
 * vendor's own (ST's DBGMCU) and stays. */
static int cmsis_core_periph(const struct svd_periph *p)
{
    return p->base >= 0xE0000000ULL && p->base < 0xE0042000ULL;
}

static int irq_cmp(const void *a, const void *b)
{
    return ((const struct svd_irq *)a)->value - ((const struct svd_irq *)b)->value;
}


/* every cluster struct under kids, innermost first, each once */
static void write_types(FILE *f, const struct svd_node *kids, int nkid)
{
    for (int i = 0; i < nkid; i++) {
        struct svd_node *t = kids[i].type;
        if (!kids[i].cluster || t->emitted)
            continue;
        t->emitted = 1;
        write_types(f, t->kid, t->nkid);
        char *pl = svd_plain_name(t->name);
        fprintf(f, "/* %s%s", pl, t->desc ? ": " : "");
        if (t->desc)
            svd_put_desc(f, t->desc);
        fprintf(f, " */\ntypedef struct {\n");
        free(pl);
        unsigned al;
        int nres = 0;
        unsigned long long end = svd_lay(t->kid, t->nkid, f, t->tname, &al, &nres);
        if (t->tsize > end)
            fprintf(f, "  uint8_t RESERVED%d[%llu];\n", nres, t->tsize - end);
        fprintf(f, "} %s_Type;  /* %llu bytes */\n\n", t->tname, t->tsize);
    }
}

static void write_struct(FILE *f, const struct svd_device *d, const struct svd_periph *p)
{
    write_types(f, p->kid, p->nkid);
    fprintf(f, "/* %s%s%s */\ntypedef struct {\n", p->name,
            p->desc ? ": " : "", p->desc ? p->desc : "");
    unsigned al;
    int nres = 0;
    svd_lay(p->kid, p->nkid, f, p->name, &al, &nres);
    fprintf(f, "} %s%s_Type;\n\n", d->prefix ? d->prefix : "", p->tname);
}

static void field_macros(FILE *f, const char *base, const char *reg,
                         const struct svd_node *r)
{
    for (int m = 0; m < r->nf; m++) {
        const struct svd_field *fd = &r->f[m];
        char pre[768];
        snprintf(pre, sizeof pre, "%s_%s_", base, svd_ident(reg));
        snprintf(pre + strlen(pre), sizeof pre - strlen(pre), "%s",
                 svd_ident(fd->name));
        unsigned long long mask = fd->width >= 64 ? ~0ULL
            : ((1ULL << fd->width) - 1);
        fprintf(f, "#define %s_Pos %dU\n", pre, fd->lsb);
        fprintf(f, "#define %s_Msk (0x%llxUL << %s_Pos)\n", pre, mask, pre);
    }
}

/* every field of a struct: STRUCT_REG_FIELD_Pos and _Msk; then the
 * clusters' structs' own, each once */
static void write_macros(FILE *f, const char *base, const struct svd_node *kids,
                         int nkid)
{
    for (int i = 0; i < nkid; i++) {
        const struct svd_node *r = &kids[i];
        if (r->cluster)
            continue;
        if (r->dim && !r->bracket) {
            for (int e = 0; e < r->dim; e++) {
                char *nm = svd_subst(r->name, r->idx[e], 0);
                field_macros(f, base, nm, r);
                free(nm);
            }
        } else {
            char *nm = svd_plain_name(r->name);
            field_macros(f, base, nm, r);
            free(nm);
        }
    }
    for (int i = 0; i < nkid; i++) {
        struct svd_node *t = kids[i].type;
        if (!kids[i].cluster || t->emitted == 2)
            continue;
        t->emitted = 2;
        write_macros(f, t->tname, t->kid, t->nkid);
    }
}

static void write_header(FILE *f, const struct svd_device *d, int cmsis)
{
    const char *pfx = d->prefix ? d->prefix : "";
    char guard[256];
    snprintf(guard, sizeof guard, "%s_H", svd_ident(d->name));
    for (char *q = guard; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    fprintf(f, "/* %s: generated by embsvd from %s. Do not edit.\n", d->name,
            svd_file);
    if (d->desc)
        fprintf(f, " * %s\n", d->desc);
    fprintf(f, " */\n#ifndef %s\n#define %s\n\n#include <stdint.h>\n\n",
            guard, guard);
    fprintf(f, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    /* the interrupt numbers */
    struct svd_irq *q = svd_alloc((size_t)(d->nirq + 1) * sizeof *q);
    memcpy(q, d->irq, (size_t)d->nirq * sizeof *q);
    qsort(q, (size_t)d->nirq, sizeof *q, irq_cmp);
    fprintf(f, "typedef enum {\n");
    int base = cpu_baseline(d);
    for (size_t i = 0; i < sizeof core_excs / sizeof core_excs[0] && !cpu_riscv(d); i++)
        if (!base || core_excs[i].m0)
            fprintf(f, "  %s_IRQn = %d,\n", core_excs[i].irq, core_excs[i].n);
    for (int i = 0; i < d->nirq; i++) {
        fprintf(f, "  %s_IRQn = %d,", svd_ident(q[i].name), q[i].value);
        comment(f, q[i].desc);
        fputc('\n', f);
    }
    fprintf(f, "} IRQn_Type;\n\n");
    free(q);

    if (cmsis) {
        const char *h = core_header(d);
        if (!h)
            svd_die("cpu '%s' has no CMSIS core header that embsvd knows; "
                "--no-cmsis writes the header without one",
                d->cpu ? d->cpu : "(none)");
        fprintf(f, "#define %s 0x%04xU\n", core_rev_macro(d),
                cpu_rev_value(d->cpurev));
        fprintf(f, "#define __MPU_PRESENT %d\n", d->mpu);
        fprintf(f, "#define __FPU_PRESENT %d\n", d->fpu);
        fprintf(f, "#define __NVIC_PRIO_BITS %d\n", d->prio_bits);
        fprintf(f, "#define __Vendor_SysTickConfig %d\n\n", d->vendor_systick);
        fprintf(f, "#include \"%s\"\n\n", h);
    } else {
        fprintf(f, "#ifndef __I\n#define __I volatile const\n#endif\n"
                   "#ifndef __O\n#define __O volatile\n#endif\n"
                   "#ifndef __IO\n#define __IO volatile\n#endif\n\n");
    }

    if (cmsis) {
        int any = 0;
        for (int i = 0; i < d->np; i++)
            if (cmsis_core_periph(&d->p[i]))
                fprintf(f, "%s%s", any++ ? ", " : "/* CMSIS-Core describes "
                        "these, so the SVD's copies are left out: ",
                        d->p[i].name);
        if (any)
            fprintf(f, " */\n\n");
    }
#define SKIP(p) (cmsis && cmsis_core_periph(p))
    /* one struct per layout */
    for (int i = 0; i < d->np; i++)
        if (d->p[i].layout == &d->p[i] && d->p[i].nkid && !SKIP(&d->p[i]))
            write_struct(f, d, &d->p[i]);

    /* where each one is */
    for (int i = 0; i < d->np; i++)
        if (!SKIP(&d->p[i]))
            fprintf(f, "#define %s%s_BASE 0x%08llxUL\n", pfx,
                    svd_ident(d->p[i].name), d->p[i].base);
    fputc('\n', f);
    for (int i = 0; i < d->np; i++)
        if (d->p[i].layout->nkid && !SKIP(&d->p[i]) && !SKIP(d->p[i].layout))
            fprintf(f, "#define %s%s ((%s%s_Type *)%s%s_BASE)\n", pfx,
                    svd_ident(d->p[i].name), pfx, d->p[i].layout->tname, pfx,
                    svd_ident(d->p[i].name));
    fputc('\n', f);

    /* every field: LAYOUT_REG_FIELD_Pos and _Msk */
    for (int i = 0; i < d->np; i++) {
        const struct svd_periph *p = &d->p[i];
        if (p->layout == p && !SKIP(p))
            write_macros(f, p->tname, p->kid, p->nkid);
    }
#undef SKIP
    fprintf(f, "\n#ifdef __cplusplus\n}\n#endif\n\n#endif /* %s */\n", guard);
}

/* ---- the startup --------------------------------------------------------- */

/* The ESP32-C3's: its ROM's loader has put the image's segments where they
 * are linked (IRAM and DRAM, write_ld_esp32c3) and jumps to the entry. Its
 * core takes every trap in vectored mode, its only mode: mtvec + 0 for an
 * exception, mtvec + 4N for CPU interrupt N (the TRM, 1.5.2), mtvec
 * 256-aligned -- a jump in each entry, four bytes even with the C
 * extension. The interrupt matrix, not the vector table, says which
 * peripheral source raises which CPU interrupt. */
static void write_startup_esp32c3(FILE *f, const struct svd_device *d)
{
    fprintf(f, "/* %s startup: generated by embsvd from %s.\n"
            " *\n"
            " * The ROM's loader copies the image's segments to the RAM they are\n"
            " * linked for and jumps to its entry, _start, which sets sp to\n"
            " * _estack and calls Reset_Handler: it points mtvec at the vector\n"
            " * table, zeroes .bss, calls SystemInit if the program has one, runs\n"
            " * the constructors and calls main. .data needs no copy: the loader\n"
            " * put it where it is linked.\n"
            " *\n"
            " * The vector table is the core's, in vectored mode (its only one):\n"
            " * entry 0 jumps to Exception_Handler, entry N to CPU_INTN_Handler,\n"
            " * N = 1..31, each weak. Define the ones you use, with\n"
            " * __attribute__((interrupt)) so that they save what they use and\n"
            " * return with mret. A peripheral's interrupt (its source, the IRQn\n"
            " * values of the header) reaches CPU interrupt N when N is written to\n"
            " * the source's map register in INTERRUPT_CORE0 and N has a priority\n"
            " * (CPU_INT_PRI_N, 1-15, at least CPU_INT_THRESH) and is enabled\n"
            " * (CPU_INT_ENABLE); then mstatus.MIE. The ROM leaves the watchdogs\n"
            " * running: disable or feed them first (RTC_CNTL's RWDT and super\n"
            " * watchdog, TIMG0's MWDT). */\n", d->name, svd_file);
    fprintf(f, "extern unsigned long _sbss[], _ebss[], _estack[];\n"
               "extern void (*__init_array_start[])(void);\n"
               "extern void (*__init_array_end[])(void);\n"
               "extern char _vector_table[];\n"
               "int main(void);\n"
               "void Reset_Handler(void);\n\n"
               "void Default_Handler(void)\n{\n    for (;;)\n        ;\n}\n\n"
               "__attribute__((weak)) void SystemInit(void)\n{\n}\n\n"
               "void Exception_Handler(void) __attribute__((weak, alias(\"Default_Handler\")));\n");
    for (int n = 1; n < 32; n++)
        fprintf(f, "void CPU_INT%d_Handler(void) __attribute__((weak, alias(\"Default_Handler\")));\n", n);
    fprintf(f, "\n__asm__(\".balign 256\\n\"\n"
               "        \".globl _vector_table\\n\"\n"
               "        \"_vector_table:\\n\"\n"
               "        \".option push\\n\"\n"
               "        \".option norvc\\n\"\n"
               "        \"    j Exception_Handler\\n\"\n");
    for (int n = 1; n < 32; n++)
        fprintf(f, "        \"    j CPU_INT%d_Handler\\n\"\n", n);
    fprintf(f, "        \".option pop\\n\"\n"
               "        \".globl _start\\n\"\n"
               "        \".type _start, @function\\n\"\n"
               "        \"_start:\\n\"\n"
               "        \"    la sp, _estack\\n\"\n"
               "        \"    j Reset_Handler\\n\");\n\n"
               "void Reset_Handler(void)\n{\n"
               "    __asm__ volatile(\"csrw mtvec, %%0\" : : \"r\"((unsigned long)_vector_table | 1));\n"
               "    for (unsigned long *dst = _sbss; dst < _ebss; )\n        *dst++ = 0;\n"
               "    SystemInit();\n"
               "    for (void (**p)(void) = __init_array_start; p < __init_array_end; p++)\n"
               "        (*p)();\n"
               "    main();\n"
               "    for (;;)\n        ;\n}\n");
}

static void write_startup(FILE *f, const struct svd_device *d)
{
    if (is_esp32c3(d)) {
        write_startup_esp32c3(f, d);
        return;
    }
    if (cpu_riscv(d))
        svd_die("--startup: of the RISC-V parts embsvd writes the ESP32-C3's "
            "startup only: a RISC-V part's interrupts are its vendor's own "
            "(the ESP32-C3's interrupt matrix, a CLIC, a PLIC), which its SVD "
            "does not describe");
    int base = cpu_baseline(d), max = -1;
    for (int i = 0; i < d->nirq; i++)
        if (d->irq[i].value > max)
            max = d->irq[i].value;
    fprintf(f, "/* %s startup: generated by embsvd from %s.\n"
            " *\n"
            " * The vector table, with a weak handler for every exception and\n"
            " * interrupt (define one with the same name to take it), and a\n"
            " * Reset_Handler that copies .data from flash, zeroes .bss, calls\n"
            " * SystemInit if the program has one, runs the constructors and\n"
            " * calls main -- by the symbols the generated linker script\n"
            " * defines. */\n", d->name, svd_file);
    fprintf(f, "extern unsigned long _sidata[], _sdata[], _edata[], _sbss[], "
               "_ebss[], _estack[];\n"
               "extern void (*__init_array_start[])(void);\n"
               "extern void (*__init_array_end[])(void);\n"
               "int main(void);\n"
               "void Reset_Handler(void);\n\n"
               "void Default_Handler(void)\n{\n    for (;;)\n        ;\n}\n\n"
               "__attribute__((weak)) void SystemInit(void)\n{\n}\n\n");
    if (is_rp2040(d))
        write_boot2(f);
    for (size_t i = 0; i < sizeof core_excs / sizeof core_excs[0]; i++)
        if (!base || core_excs[i].m0)
            fprintf(f, "void %s(void) __attribute__((weak, alias(\"Default_Handler\")));\n",
                    core_excs[i].handler);
    for (int i = 0; i < d->nirq; i++)
        fprintf(f, "void %s_IRQHandler(void) __attribute__((weak, alias(\"Default_Handler\")));\n",
                svd_ident(d->irq[i].name));
    fprintf(f, "\n__attribute__((section(\".isr_vector\"), used))\n"
               "void (*const g_pfnVectors[%d])(void) = {\n"
               "    (void (*)(void))_estack,\n    Reset_Handler,\n", 16 + max + 1);
    static const char *const slots[14] = {
        "NMI_Handler", "HardFault_Handler", "MemManage_Handler",
        "BusFault_Handler", "UsageFault_Handler", 0, 0, 0, 0, "SVC_Handler",
        "DebugMon_Handler", 0, "PendSV_Handler", "SysTick_Handler" };
    for (int i = 0; i < 14; i++) {
        const char *s = slots[i];
        if (s && base && (i == 2 || i == 3 || i == 4 || i == 10))
            s = NULL;
        fprintf(f, "    %s,\n", s ? s : "0");
    }
    for (int v = 0; v <= max; v++) {
        const char *s = NULL;
        for (int i = 0; i < d->nirq; i++)
            if (d->irq[i].value == v)
                s = d->irq[i].name;
        if (s)
            fprintf(f, "    %s_IRQHandler,         /* %d */\n", svd_ident(s), v);
        else
            fprintf(f, "    0,                     /* %d */\n", v);
    }
    fprintf(f, "};\n\n"
               "void Reset_Handler(void)\n{\n"
               "    unsigned long *src = _sidata, *dst = _sdata;\n"
               "    while (dst < _edata)\n        *dst++ = *src++;\n"
               "    for (dst = _sbss; dst < _ebss; )\n        *dst++ = 0;\n"
               "    SystemInit();\n"
               "    for (void (**p)(void) = __init_array_start; p < __init_array_end; p++)\n"
               "        (*p)();\n"
               "    main();\n"
               "    for (;;)\n        ;\n}\n");
}

/* ---- the linker script ------------------------------------------------------ */

static void parse_region(const char *s, unsigned long long *org,
                         unsigned long long *len, const char *what)
{
    const char *c = strchr(s, ':');
    if (!c)
        svd_die("%s wants ORIGIN:LENGTH, as in 0x08000000:1M", what);
    char a[64];
    snprintf(a, sizeof a, "%.*s", (int)(c - s), s);
    *org = svd_num(a, what, 0);
    *len = svd_num(c + 1, what, 0);
}

/* The ESP32-C3's image runs from its SRAM1, which the core reaches twice:
 * as IRAM at 0x40380000 (fetches) and as DRAM at 0x3FC80000 (loads and
 * stores), the same bytes 0x700000 apart. The code takes IRAM's first
 * 128 KiB, the data DRAM after them -- the same SRAM twice would be
 * overwritten -- up to 0x3FCDC000, below the ROM loader's own data and
 * stack (0x3FCDC710 up, ESP-IDF's bootloader.ld), which must survive the
 * loading. The stack is at the top of it. */
static void write_ld_esp32c3(FILE *f, const struct svd_device *d)
{
    fprintf(f, "/* %s: generated by embsvd. The ROM's loader copies each loadable\n"
               " * segment to the address it is linked at -- the code to IRAM, the\n"
               " * rest to DRAM: one SRAM, seen 0x700000 apart, split here so the\n"
               " * two do not overlap -- and jumps to the entry. DRAM stops below\n"
               " * the loader's own RAM (0x3FCDC710). */\n"
               "ENTRY(_start)\n\n"
               "MEMORY\n{\n"
               "  IRAM (rx)  : ORIGIN = 0x40380000, LENGTH = 128K\n"
               "  DRAM (rw)  : ORIGIN = 0x3FCA0000, LENGTH = 0x3C000\n}\n\n"
               "_estack = ORIGIN(DRAM) + LENGTH(DRAM);\n\n"
               "SECTIONS\n{\n"
               "  .text : { . = ALIGN(256); *(.text) *(.text*) . = ALIGN(4); _etext = .; } >IRAM\n"
               "  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) *(.srodata*) . = ALIGN(4); } >DRAM\n"
               "  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .);\n"
               "                     KEEP(*(.preinit_array*))\n"
               "                     PROVIDE_HIDDEN(__preinit_array_end = .); } >DRAM\n"
               "  .init_array : { PROVIDE_HIDDEN(__init_array_start = .);\n"
               "                  KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))\n"
               "                  PROVIDE_HIDDEN(__init_array_end = .); } >DRAM\n"
               "  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .);\n"
               "                  KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*))\n"
               "                  PROVIDE_HIDDEN(__fini_array_end = .); } >DRAM\n"
               "  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) *(.sdata*) . = ALIGN(4);\n"
               "            _edata = .; } >DRAM\n"
               "  .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; __bss_start__ = _sbss; *(.bss) *(.bss*)\n"
               "           *(.sbss*) *(COMMON) . = ALIGN(4); _ebss = .; __bss_end__ = _ebss; } >DRAM\n"
               "  PROVIDE(end = _ebss);\n"
               "  PROVIDE(_end = _ebss);\n"
               "}\n", d->name);
}

static void write_ld(FILE *f, const struct svd_device *d, const char *flash,
                     const char *ram)
{
    if (is_esp32c3(d)) {
        if (flash || ram)
            svd_die("--ld: the ESP32-C3 takes no --flash or --ram: its ROM loads "
                "the image into SRAM, which the script splits into IRAM and "
                "DRAM; edit its MEMORY to split it otherwise");
        write_ld_esp32c3(f, d);
        return;
    }
    if (!flash || !ram)
        svd_die("--ld needs --flash ORIGIN:LENGTH and --ram ORIGIN:LENGTH: an "
            "SVD describes the peripherals, not the memories");
    unsigned long long fo, fl, ro, rl;
    parse_region(flash, &fo, &fl, "--flash");
    parse_region(ram, &ro, &rl, "--ram");
    fprintf(f, "/* %s: generated by embsvd, in STM32CubeMX's shape. */\n"
               "ENTRY(Reset_Handler)\n"
               "_estack = ORIGIN(RAM) + LENGTH(RAM);\n\n"
               "MEMORY\n{\n"
               "  FLASH (rx)  : ORIGIN = 0x%08llx, LENGTH = 0x%llx\n"
               "  RAM   (xrw) : ORIGIN = 0x%08llx, LENGTH = 0x%llx\n}\n\n",
            d->name, fo, fl, ro, rl);
    fputs("SECTIONS\n{\n", f);
    if (is_rp2040(d)) {
        if (fo != 0x10000000ULL)
            svd_die("the RP2040 boots from the flash at 0x10000000, not from "
                    "--flash 0x%08llx", fo);
        /* boot2, then the vector table at 0x10000100, where boot2 goes */
        fputs("  .boot2 : { __boot2_start__ = .; KEEP(*(.boot2)) "
              "__boot2_end__ = .; } >FLASH\n", f);
    }
    fputs("  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH\n"
          "  .text : { . = ALIGN(4); *(.text) *(.text*) KEEP(*(.init)) KEEP(*(.fini))\n"
          "            . = ALIGN(4); _etext = .; } >FLASH\n"
          "  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH\n"
          "  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH\n"
          "  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .);\n"
          "                     KEEP(*(.preinit_array*))\n"
          "                     PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH\n"
          "  .init_array : { PROVIDE_HIDDEN(__init_array_start = .);\n"
          "                  KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))\n"
          "                  PROVIDE_HIDDEN(__init_array_end = .); } >FLASH\n"
          "  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .);\n"
          "                  KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*))\n"
          "                  PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH\n"
          "  _sidata = LOADADDR(.data);\n"
          "  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) . = ALIGN(4);\n"
          "            _edata = .; } >RAM AT> FLASH\n"
          "  .bss : { . = ALIGN(4); _sbss = .; __bss_start__ = _sbss; *(.bss) *(.bss*)\n"
          "           *(COMMON) . = ALIGN(4); _ebss = .; __bss_end__ = _ebss; } >RAM\n"
          "  PROVIDE(end = _ebss);\n"
          "  PROVIDE(_end = _ebss);\n"
          "}\n", f);
}

/* ---- listing ----------------------------------------------------------------- */

static void list_device(const struct svd_device *d)
{
    printf("%s: %s, %d peripherals, %d interrupts\n", d->name,
           d->cpu ? d->cpu : "(no cpu)", d->np, d->nirq);
    for (int i = 0; i < d->np; i++) {
        const struct svd_periph *p = &d->p[i];
        printf("  %-12s 0x%08llx", p->name, p->base);
        if (p->layout != p) {
            printf("  like %s", p->layout->name);
        } else {
            struct svd_flats fl = svd_flat_regs(p, 0);
            printf("  %d registers", fl.n);
            svd_free_flats(&fl);
        }
        if (p->desc)
            printf("  %s", p->desc);
        putchar('\n');
    }
}

static void show_periph(const struct svd_device *d, const char *name)
{
    const struct svd_periph *p = NULL;
    for (int i = 0; i < d->np; i++)
        if (!strcmp(d->p[i].name, name))
            p = &d->p[i];
    if (!p)
        svd_die("there is no peripheral %s (--list names them)", name);
    printf("%s at 0x%08llx%s%s\n", p->name, p->base, p->desc ? ": " : "",
           p->desc ? p->desc : "");
    struct svd_flats fl = svd_flat_regs(p, 1);
    for (int k = 0; k < fl.n; k++) {
        const struct svd_flat *x = &fl.v[k];
        const struct svd_node *r = x->r;
        printf("  0x%08llx  +0x%03llx  %-14s %2d bits  %s  reset 0x%llx%s%s\n",
               p->base + x->off, x->off, x->path, r->size,
               r->access == 1 ? "ro" : r->access == 2 ? "wo" : "rw",
               r->reset, r->desc ? "  " : "", r->desc ? r->desc : "");
        for (int m = 0; m < r->nf; m++) {
            const struct svd_field *fd = &r->f[m];
            if (fd->width == 1)
                printf("      [%d]     %s\n", fd->lsb, fd->name);
            else
                printf("      [%d:%d]%*s%s\n", fd->lsb + fd->width - 1,
                       fd->lsb, fd->lsb + fd->width - 1 >= 10 ? 1 : 2, "",
                       fd->name);
        }
    }
    svd_free_flats(&fl);
}

/* ---- the hardware, as JSON --------------------------------------------------
 *
 * --json FILE describes the device for tools rather than for a compiler:
 * every register of every peripheral, each array element and cluster
 * expanded, at its absolute address. The schema is in the manual
 * (docs/manual/tools/embsvd.md); "schema" is its version, and changes
 * only when a key changes meaning or goes away. */

static void js(FILE *f, const char *s)
{
    if (!s) {
        fputs("null", f);
        return;
    }
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", f);
        else if (c == '\t')
            fputs("\\t", f);
        else if (c < 0x20 || c == 0x7f)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static const char *jb(int b)
{
    return b ? "true" : "false";
}

static void json_irqs(FILE *f, const struct svd_irq *q, int n, const char *ind)
{
    fputc('[', f);
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n%s{\"name\": ", i ? "," : "", ind);
        js(f, q[i].name);
        fprintf(f, ", \"value\": %d, \"description\": ", q[i].value);
        js(f, q[i].desc);
        fputc('}', f);
    }
    if (n)
        fprintf(f, "\n%.*s", (int)strlen(ind) - 2, ind);
    fputc(']', f);
}

static void json_field(FILE *f, const struct svd_field *fd)
{
    fputs("{\"name\": ", f);
    js(f, fd->name);
    fprintf(f, ", \"bitOffset\": %d, \"bitWidth\": %d, \"access\": ", fd->lsb,
            fd->width);
    js(f, fd->acc);
    fputs(", \"description\": ", f);
    js(f, fd->desc);
    fputs(", \"enumeratedValues\": [", f);
    for (int i = 0; i < fd->nev; i++) {
        const struct svd_enumval *e = &fd->ev[i];
        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        js(f, e->name);
        /* "care": the bits of the field a #1x0 value fixes */
        unsigned long long fm = fd->width >= 64 ? ~0ULL
                              : (1ULL << fd->width) - 1;
        if (e->isdef && !e->care)
            fputs(", \"value\": null", f);
        else
            fprintf(f, ", \"value\": %llu", e->value & fm);
        if (e->care && (e->care & fm) != fm)
            fprintf(f, ", \"care\": %llu", e->care & fm);
        fprintf(f, ", \"isDefault\": %s, \"usage\": ", jb(e->isdef));
        js(f, e->usage);
        fputs(", \"description\": ", f);
        js(f, e->desc);
        fputc('}', f);
    }
    fputs("]}", f);
}

static void write_json(FILE *f, const struct svd_device *d, const char *flash,
                       const char *ram);

static void parse_region(const char *s, unsigned long long *org,
                         unsigned long long *len, const char *what);

static void json_memory(FILE *f, const char *name, const char *spec,
                        const char *acc, const char *what, int first)
{
    unsigned long long o, l;
    parse_region(spec, &o, &l, what);
    fprintf(f, "%s\n    {\"name\": \"%s\", \"origin\": %llu, \"length\": %llu, "
            "\"access\": \"%s\"}", first ? "" : ",", name, o, l, acc);
}

static void write_json(FILE *f, const struct svd_device *d, const char *flash,
                       const char *ram)
{
    fprintf(f, "{\n  \"schema\": 1,\n  \"generator\": \"embsvd\",\n  \"source\": ");
    js(f, svd_file);
    fputs(",\n  \"device\": {\"name\": ", f);
    js(f, d->name);
    fputs(", \"vendor\": ", f);
    js(f, d->vendor);
    fputs(", \"version\": ", f);
    js(f, d->version);
    fputs(", \"description\": ", f);
    js(f, d->desc);
    fprintf(f, ", \"addressUnitBits\": %d, \"width\": %d, "
            "\"headerDefinitionsPrefix\": ", d->aub, d->width);
    js(f, d->prefix);
    fputs("},\n  \"cpu\": ", f);
    if (!d->has_cpu) {
        fputs("null", f);
    } else {
        fputs("{\"name\": ", f);
        js(f, d->cpu);
        fputs(", \"revision\": ", f);
        js(f, d->cpurev);
        fputs(", \"endian\": ", f);
        js(f, d->endian);
        fprintf(f, ", \"mpuPresent\": %s, \"fpuPresent\": %s, \"fpuDP\": %s, "
                "\"nvicPrioBits\": %d, \"vendorSystickConfig\": %s, "
                "\"deviceNumInterrupts\": ", jb(d->mpu), jb(d->fpu),
                jb(d->fpu_dp), d->prio_bits, jb(d->vendor_systick));
        if (d->num_irq < 0)
            fputs("null}", f);
        else
            fprintf(f, "%d}", d->num_irq);
    }
    fputs(",\n  \"memories\": [", f);
    if (flash)
        json_memory(f, "FLASH", flash, "rx", "--flash", 1);
    if (ram)
        json_memory(f, "RAM", ram, "rwx", "--ram", !flash);
    fputs(flash || ram ? "\n  ],\n" : "],\n", f);

    struct svd_irq *q = svd_alloc((size_t)(d->nirq + 1) * sizeof *q);
    memcpy(q, d->irq, (size_t)d->nirq * sizeof *q);
    qsort(q, (size_t)d->nirq, sizeof *q, irq_cmp);
    fputs("  \"interrupts\": ", f);
    json_irqs(f, q, d->nirq, "    ");
    free(q);

    fputs(",\n  \"peripherals\": [", f);
    for (int i = 0; i < d->np; i++) {
        const struct svd_periph *p = &d->p[i];
        fprintf(f, "%s\n    {\"name\": ", i ? "," : "");
        js(f, p->name);
        fputs(", \"description\": ", f);
        js(f, p->desc);
        fputs(", \"groupName\": ", f);
        js(f, p->group);
        fprintf(f, ", \"baseAddress\": %llu, \"derivedFrom\": ", p->base);
        js(f, p->layout != p ? p->layout->name : NULL);
        fprintf(f, ", \"typeName\": \"%s%s_Type\",\n      \"addressBlocks\": [",
                d->prefix ? d->prefix : "", p->layout->tname);
        for (int k = 0; k < p->nab; k++) {
            fprintf(f, "%s{\"offset\": %llu, \"address\": %llu, \"size\": %llu, "
                    "\"usage\": ", k ? ", " : "", p->ab[k].off,
                    p->base + p->ab[k].off, p->ab[k].size);
            js(f, p->ab[k].usage);
            fputc('}', f);
        }
        fputs("],\n      \"interrupts\": ", f);
        json_irqs(f, p->irq, p->nirq, "        ");
        fputs(",\n      \"registers\": [", f);
        struct svd_flats fl = svd_flat_regs(p, 1);
        for (int k = 0; k < fl.n; k++) {
            const struct svd_flat *x = &fl.v[k];
            const struct svd_node *r = x->r;
            fprintf(f, "%s\n        {\"name\": ", k ? "," : "");
            js(f, x->name);
            fputs(", \"path\": ", f);
            js(f, x->path);
            fputs(", \"index\": [", f);
            for (int m = 0; m < x->nidx; m++)
                fprintf(f, "%s%d", m ? ", " : "", x->idx[m]);
            fprintf(f, "], \"address\": %llu, \"offset\": %llu, \"size\": %d, "
                    "\"access\": ", p->base + x->off, x->off, r->size);
            js(f, r->acc);
            /* a device's 32-bit resetMask, on a 16-bit register */
            unsigned long long rm = r->size == 64 ? ~0ULL
                                  : (1ULL << r->size) - 1;
            fprintf(f, ", \"resetValue\": %llu, \"resetMask\": %llu, "
                    "\"alternate\": ", r->reset & rm, r->rmask & rm);
            js(f, r->alt);
            fputs(", \"description\": ", f);
            js(f, r->desc);
            fputs(",\n         \"fields\": [", f);
            for (int m = 0; m < r->nf; m++) {
                fputs(m ? ",\n           " : "\n           ", f);
                json_field(f, &r->f[m]);
            }
            fputs("]}", f);
        }
        svd_free_flats(&fl);
        fputs(fl.n ? "\n      ]}" : "]}", f);
    }
    fputs("\n  ]\n}\n", f);
}

/* ---- main ---------------------------------------------------------------- */

static FILE *open_out(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "embsvd: cannot write '%s'\n", path);
        exit(1);
    }
    return f;
}

static void close_out(FILE *f, const char *path)
{
    if (fclose(f) != 0) {
        fprintf(stderr, "embsvd: cannot write '%s'\n", path);
        exit(1);
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: embsvd DEVICE.svd [--header FILE] [--no-cmsis]\n"
        "              [--nvic-prio-bits N] [--fpu-present 0|1]\n"
        "              [--startup FILE]\n"
        "              [--ld FILE --flash ORIGIN:LENGTH --ram ORIGIN:LENGTH]\n"
        "              [--json FILE [--flash ORIGIN:LENGTH] [--ram ORIGIN:LENGTH]]\n"
        "       embsvd DEVICE.svd --list | --show PERIPHERAL\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *hdr = NULL, *st = NULL, *ld = NULL, *json = NULL;
    const char *flash = NULL, *ram = NULL, *show = NULL;
    const char *prio = NULL, *fpu = NULL;
    int cmsis = 1, list = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **slot = !strcmp(a, "--header") ? &hdr
                          : !strcmp(a, "--startup") ? &st
                          : !strcmp(a, "--ld") ? &ld
                          : !strcmp(a, "--json") ? &json
                          : !strcmp(a, "--flash") ? &flash
                          : !strcmp(a, "--ram") ? &ram
                          : !strcmp(a, "--show") ? &show
                          : !strcmp(a, "--nvic-prio-bits") ? &prio
                          : !strcmp(a, "--fpu-present") ? &fpu : NULL;
        if (slot) {
            if (++i == argc) {
                fprintf(stderr, "embsvd: %s needs a value\n", a);
                return 2;
            }
            *slot = argv[i];
        } else if (!strcmp(a, "--no-cmsis")) {
            cmsis = 0;
        } else if (!strcmp(a, "--list")) {
            list = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "embsvd: unknown option '%s'\n", a);
            usage();
        } else if (in) {
            fprintf(stderr, "embsvd: one SVD file at a time\n");
            return 2;
        } else {
            in = a;
        }
    }
    if (!in || (!hdr && !st && !ld && !json && !list && !show))
        usage();
    struct svd_device *d = svd_load(in);
    if (prio) {
        int n = atoi(prio);
        if (n < 2 || n > 8) {
            fprintf(stderr, "embsvd: --nvic-prio-bits is 2 to 8\n");
            return 2;
        }
        d->prio_bits = n;
    }
    if (fpu) {
        if (strcmp(fpu, "0") && strcmp(fpu, "1")) {
            fprintf(stderr, "embsvd: --fpu-present is 0 or 1\n");
            return 2;
        }
        d->fpu = fpu[0] == '1';
    }
    if (list)
        list_device(d);
    if (show)
        show_periph(d, show);
    if (hdr) {
        FILE *f = open_out(hdr);
        write_header(f, d, cmsis);
        close_out(f, hdr);
    }
    if (st) {
        FILE *f = open_out(st);
        write_startup(f, d);
        close_out(f, st);
    }
    if (ld) {
        FILE *f = open_out(ld);
        write_ld(f, d, flash, ram);
        close_out(f, ld);
    }
    if (json) {
        FILE *f = open_out(json);
        write_json(f, d, flash, ram);
        close_out(f, json);
    }
    return 0;
}
