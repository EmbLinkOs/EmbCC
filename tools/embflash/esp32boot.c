/* embflash's ESP32 boot stub: maps a program's flash pieces, then starts it.
 *
 * The ROM boots the image at 0x1000 and loads RAM segments only. For an ELF
 * with pieces in flash-mapped memory (code at 0x400D0000-0x403FFFFF, data at
 * 0x3F400000-0x3F7FFFFF), embflash writes those pieces to flash, each region
 * on 64 KiB pages of its own, and puts this stub in the image at 0x400C0000
 * (RTC fast memory, apart from the program's IRAM and DRAM) as the entry.
 * The stub maps each region through the cache MMU with the ROM's own
 * functions, as ESP-IDF's second-stage bootloader does, and calls the
 * program's entry. Its parameters are a table embflash writes after it.
 *
 * Built by EmbCC; embflash carries the bytes (esp32boot.inc), and
 * tests/golden/embflash-esp.sh rebuilds them and compares:
 *
 *   embcc --target=xtensa-esp32-elf -Os -mtext-section-literals -c esp32boot.c
 *   embld -e esp32boot -Ttext 0x400C0000 esp32boot.o -o esp32boot.elf
 *
 * RTC fast memory is on the instruction bus: 32-bit loads only, so the
 * table is words and the message is read a word at a time. */
#define REG(a) (*(volatile unsigned *)(a))
#define UART0_FIFO      REG(0x3FF40000u)
#define UART0_STATUS    REG(0x3FF4001Cu)
#define PRO_CACHE_CTRL1 REG(0x3FF00044u)
#define MASK_IRAM0 (1u << 0)            /* 0x40070000-0x40400000: flash code */
#define MASK_DRAM1 (1u << 3)
#define MASK_DROM0 (1u << 4)            /* 0x3F400000-0x3F800000: flash data */

/* The ESP32 ROM's functions, as its linker script names them (each read off
 * a D0WD-V3 as a windowed `entry a1, 32`) */
typedef void (*rom_cpu_fn)(int cpu);
typedef int (*rom_mmu_set_fn)(int cpu, int pid, unsigned vaddr, unsigned paddr, int psize, int num);
#define Cache_Read_Disable  ((rom_cpu_fn)0x40009ab8u)
#define Cache_Flush         ((rom_cpu_fn)0x40009a14u)
#define mmu_init            ((rom_cpu_fn)0x400095a4u)
#define cache_flash_mmu_set ((rom_mmu_set_fn)0x400095e0u)
#define Cache_Read_Enable   ((rom_cpu_fn)0x40009a84u)

/* embflash's table: the entry, the count, and per region its first
 * virtual page, the flash page it is at, and the number of 64 KiB pages */
#define TABLE 0x400C0200u
#define T(i) (((volatile const unsigned *)TABLE)[i])

static const char failed[] __attribute__((aligned(4))) = "embflash boot: cache_flash_mmu_set refused a region\r\n";

void esp32boot(void)
{
    Cache_Read_Disable(0);
    Cache_Flush(0);
    mmu_init(0);
    for (unsigned i = 0; i < T(1); i++)
        if (cache_flash_mmu_set(0, 0, T(2 + 3 * i), T(3 + 3 * i), 64, (int)T(4 + 3 * i)) != 0) {
            const volatile unsigned *w = (const volatile unsigned *)failed;
            for (unsigned k = 0; k < sizeof failed - 1; k++) {
                while (((UART0_STATUS >> 16) & 0xff) >= 120)
                    ;
                UART0_FIFO = (w[k / 4] >> (8 * (k % 4))) & 0xff;
            }
            for (;;)
                ;
        }
    PRO_CACHE_CTRL1 &= ~(MASK_IRAM0 | MASK_DRAM1 | MASK_DROM0);
    Cache_Read_Enable(0);
    ((void (*)(void))T(0))();
}
