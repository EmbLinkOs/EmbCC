# Boards

`--board=NAME` compiles and links a program for a development board. The
board's target, part and clock are set, its startup and runtime are linked,
and `<board.h>` gives the same small API on every board. `embflash` puts the
program on the board.

```sh
embcc --board=uno blink.c -o blink.elf
embflash blink.elf --console 5
```

[The board list](board-list.md) has every board `--board=` takes.

## What a board sets

| | `--board=uno`, `--board=nano` | `--board=mega2560` | `--board=esp32-devkit` |
|---|---|---|---|
| Target | `avr`, `--mcu=atmega328p` | `avr`, `--mcu=atmega2560` | `xtensa-esp32-elf` |
| Clock | `F_CPU=16000000UL` | `F_CPU=16000000UL` | the ROM's: CPU and APB at the crystal's 40 MHz |
| Startup | the part's (`lib/avr/crt.S`) | the part's | `lib/esp32/crt0.S` and `start.c` |
| Runtime | `lib/boards/uno/board.c` | the same, with the Mega's pin map | `lib/boards/esp32-devkit/board.c` |
| Linker script | the part's (`avr5.ld`) | the part's (`avr6.ld`) | `lib/esp32/esp32.ld` |
| Console | USART0 at 115200 | USART0 at 115200 | UART0 at 115200 |
| Clock for `board_millis` | Timer1, an interrupt each millisecond | Timer1, the same | timer group 0's 64-bit counter at 1 MHz, no interrupt |
| Programmed by | Optiboot (`embflash --stk500`) | its STK500v2 bootloader (`embflash --stk500v2`) | the ESP32's ROM (`embflash --esp`) |

The runtimes are built with the target's library (`make rt-embedded`). Each
board's runtime is linked as an object, ahead of the program, so its
console and its constructor are the program's.

## A first program

```c
#include <board.h>
#include <stdio.h>

int main(void)
{
    for (unsigned n = 0;; n++) {
        board_led(n & 1);
        printf("%s: tick %u at %lu ms\n", BOARD_NAME, n, board_millis());
        board_delay_ms(500);
    }
}
```

The same source builds for each board:

```sh
embcc --board=uno -Os blink.c -o blink-uno.elf
embcc --board=esp32-devkit -O2 blink.c -o blink-esp32.elf
embflash blink-uno.elf --verify --console 3
embflash blink-esp32.elf --verify --console 3
```

`embflash` needs no programmer and no port for these. An AVR program goes
through Optiboot and an Xtensa program through the ESP32's ROM. The port is
the one USB serial port in `/dev` the board's bridge makes (`serial:auto`,
[embflash](tools/embflash.md#the-port)). `--board=NAME` says it explicitly.

## `<board.h>`

| Name | What it does |
|---|---|
| `BOARD_NAME` | the board's name, a string |
| `BOARD_LED` | the pin of the board's LED |
| `BOARD_CONSOLE_BAUD` | 115200 |
| `board_led(on)` | the LED on or off |
| `board_pin_mode(pin, mode)` | `BOARD_INPUT`, `BOARD_OUTPUT` or `BOARD_INPUT_PULLUP` |
| `board_pin_write(pin, high)`, `board_pin_read(pin)` | a pin's level |
| `board_delay_ms(ms)`, `board_delay_us(us)` | busy waits |
| `board_millis()`, `board_micros()` | the time since the program started |
| `board_putc(c)`, `board_getc()` | the console, raw: `board_getc` is -1 when no byte waits |

`printf`, `puts` and `putchar` write to the console, with `\n` sent as
`\r\n` for a serial terminal. `getchar` (the Uno) and `read` on stdin (the
ESP32) read from it. The board's set-up runs before `main`, as the first
constructor.

## The Arduino Uno and Nano

- **Pins** are the board's numbers: 0 to 13 on the digital header, 14 to
  19 for A0 to A5. `BOARD_LED` is 13, the "L" LED.
- **Timer1** is the board's clock, so interrupts are on when `main`
  starts. Timer0 and Timer2 are free. (Arduino's core uses Timer0, but
  QEMU's ATmega328P has only the 16-bit timers, and the runtime is tested
  there too.)
- **The console** is set as Arduino's core sets it: U2X with UBRR 16, which
  is 117647 baud. The board's USB bridge reads it as 115200.
- **The Nano** has the same part and pins. A Nano with the old bootloader
  takes `serial:auto@57600`.

## The Arduino Mega 2560

The Uno's runtime with the Mega's pin map: 0 to 53 on the digital header
and 54 to 69 for A0 to A15, as the Arduino Mega numbers them (`BOARD_LED`
is 13, PB7). Timer1 is the clock; Timer0, Timer2 and Timer3 to Timer5 are
free. The program is built for the ATmega2560's 3-byte PC and runs on
QEMU's `mega2560`; `embflash` programs it through `--stk500v2`, which has
been tested against a model of the bootloader, not yet against a board.

## The ESP32 DevKit

The ESP32-WROOM-32 boards: DevKitC, DOIT DevKit V1 and their kin. The ROM
boots the program from flash at 0x1000, with no second-stage bootloader.

- **Memory.** Code is in flash, read through the cache (IROM, from
  0x400D0000), and so is read-only data (DROM, from 0x3F400000).
  `embflash` writes both to flash and maps them with its boot stub
  ([embflash](tools/embflash.md#the-image)). `.data`, `.bss`, the heap and
  the stack are in DRAM, 192 KiB from 0x3FFB0000. IRAM (128 KiB from
  0x40080000) holds the startup and what is marked `IRAM_ATTR`.
- **Clocks.** The CPU and APB run at the crystal's 40 MHz, as the ROM leaves
  them. The runtime reads APB off UART0's divider, so `board_micros` counts
  true microseconds on a 26 MHz board as well.
- **Watchdogs.** The RTC watchdog and timer group 0's, which a flash boot
  leaves running, are turned off before `main`.
- **Pins** are GPIO numbers. 34 to 39 are inputs only, and 6 to 11 are the
  flash's. `BOARD_LED` is GPIO 2, the blue LED of the DevKit V1 boards; a
  DevKitC has no LED there. A pull-up on an RTC pin (0, 2, 4, 12 to 15, 25
  to 27, 32 to 39) is IO_MUX's, which the RTC domain can override.
- **The startup** turns interrupts off, makes the current register window
  the only live one, moves the stack to the top of DRAM, sets PS for the
  windowed ABI, then zeroes `.bss` and runs the constructors and `main`.
  `exit` stops in a loop.

## Limits

- No interrupt API: an AVR program uses `ISR()` as with avr-libc. On the
  ESP32, interrupts stay off.
- The ESP32 runs at 40 MHz: the PLL (160 or 240 MHz) is not set up. There
  is no Wi-Fi, Bluetooth or PSRAM, which need Espressif's libraries.
- `--board=esp32-devkit` programs are C. C++ needs a C++ library built for
  `xtensa-none-elf`, which is not built yet.
- Tested on an Arduino Uno R3, and on an ESP32-D0WD-V3 DevKit (V3.1 silicon,
  4 MiB of flash, a CP2102 bridge). The Mega 2560 on QEMU and against a
  model of its bootloader.
