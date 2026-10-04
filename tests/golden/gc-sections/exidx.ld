/* For an object with ARM EHABI unwind tables (clang's, -funwind-tables):
 * each .ARM.exidx.text.F belongs to .text.F by SHF_LINK_ORDER. */
ENTRY(_start)
MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 64K }
SECTIONS
{
  .text : { *(.text .text.*) } > FLASH
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
}
