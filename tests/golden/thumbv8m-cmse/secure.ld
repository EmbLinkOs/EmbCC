/* The Secure image of tests/golden/thumbv8m-cmse.sh, in the Secure alias of
 * the mps2-an505's ZBT SSRAM1 (0x10000000): code, then the secure gateway
 * veneers in a 32-byte aligned region of their own, which the boot marks
 * Non-secure Callable in the SAU (and the IDAU, through NSCCFG), then the
 * data. QEMU loads every segment where it is linked, so .data needs no
 * copy; .bss is zeroed by the boot. CMSIS's Secure linker scripts place
 * .gnu.sgstubs the same way. */
MEMORY
{
    S_CODE (rx)  : ORIGIN = 0x10000000, LENGTH = 0x000F0000
    S_NSC  (rx)  : ORIGIN = 0x100F0000, LENGTH = 0x00010000
    S_RAM  (rwx) : ORIGIN = 0x10100000, LENGTH = 0x00100000
}
ENTRY(reset)
SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    } > S_CODE
    .gnu.sgstubs : ALIGN(32) {
        __sg_start = .;
        KEEP(*(.gnu.sgstubs*))
        . = ALIGN(32);
        __sg_end = .;
    } > S_NSC
    .data : { *(.data*) } > S_RAM
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > S_RAM
}
