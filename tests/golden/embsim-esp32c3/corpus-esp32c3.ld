/* The exec corpus on EmbSim's ESP32-C3-DevKitM-1 (embsim-esp32c3.sh), as
 * embsvd's script lays an image out -- the code in IRAM, the rest in
 * DRAM after the same SRAM's first 256 KiB -- but with DRAM grown to 7 MiB
 * (--ram-size 7M), so that the programs have QEMU's room: DRAM then ends
 * where IRAM begins. */
ENTRY(_start)
MEMORY
{
  IRAM (rx) : ORIGIN = 0x40380000, LENGTH = 256K
  DRAM (rw) : ORIGIN = 0x3FCC0000, LENGTH = 0x6C0000
}
_estack = ORIGIN(DRAM) + LENGTH(DRAM);
SECTIONS
{
  .text : { . = ALIGN(256); *(.text) *(.text*) } >IRAM
  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) *(.srodata*) } >DRAM
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .);
                  KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*))
                  PROVIDE_HIDDEN(__init_array_end = .); } >DRAM
  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) *(.sdata*) . = ALIGN(4);
            _edata = .; } >DRAM
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss) *(.bss*) *(.sbss*) *(COMMON)
                    . = ALIGN(4); _ebss = .; } >DRAM
  PROVIDE(end = _ebss);
  PROVIDE(_end = _ebss);
}
