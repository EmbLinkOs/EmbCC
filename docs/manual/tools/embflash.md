# embflash — an image put into a target

`embflash` programs a firmware image into a target through the GDB remote
serial protocol. That one protocol reaches nearly every way of programming
a part: QEMU's gdbstub, OpenOCD, pyOCD, SEGGER's J-Link GDB server and the
Black Magic Probe all speak it. The server runs the part's flash algorithm.
`embflash` reads the image, erases and writes the flash, verifies it, and
can start the program. A part with an STK500 serial bootloader, such as an
Arduino Uno with Optiboot, needs no server: `--stk500` programs it
directly. Neither does an ESP32, through its ROM: `--esp`. This page is the
command reference.

## Synopsis

```text
embflash IMAGE --gdb TARGET [--base ADDR] [--verify] [--run]
               [--entry ADDR] [--vector-table ADDR] [--monitor CMD]...
               [--dry-run] [--timeout SECONDS] [-v]
embflash IMAGE --stk500 serial:DEV[@BAUD] [--base ADDR] [--verify]
               [--no-reset] [--console SECONDS] [--timeout SECONDS]
embflash IMAGE --esp serial:DEV[@BAUD] [--base ADDR] [--verify]
               [--no-reset] [--ram] [--console SECONDS] [--timeout SECONDS]
embflash ELF --esp-image FILE
embflash --esp serial:DEV[@BAUD] --read ADDR SIZE|all FILE
```

`TARGET` is where the server listens:

| Target | Reaches |
|---|---|
| `HOST:PORT` | a TCP port: OpenOCD's `3333`, a J-Link GDB server's `2331`, pyOCD's `3333` |
| `unix:PATH` | a Unix-domain socket, such as QEMU's `-gdb chardev:` |
| `serial:DEV[@BAUD]` | a serial line at `BAUD` (default 115200; a rate the host cannot set is refused), such as the Black Magic Probe's GDB port |

## Images

| Image | Read as |
|---|---|
| ELF | its stored bytes at their **load** addresses, as `embpack` packs them; the entry point comes from the header |
| `.hex`, `.ihex` | Intel HEX (record types 0 to 5) |
| `.srec`, `.s19`, `.s28`, `.s37`, `.mot` | Motorola S-records (S1–S3 data, S7–S9 entry) |
| anything else | raw bytes, placed at `--base ADDR` |

Every checksum is checked. A HEX file must end with its end-of-file
record, so a truncated download is refused. Two pieces that overlap are
refused, and pieces that touch are merged.

## How each byte gets there

`embflash` asks the server for its memory map (`qXfer:memory-map:read`):

- **Flash.** Every block the image touches is erased, in runs of
  blocks, with `vFlashErase`; blocks the image does not touch keep their
  contents. The bytes are written with `vFlashWrite` and committed with
  `vFlashDone`. The block size is the server's.
- **RAM, and anything the server does not map.** Written as memory with
  `X` (binary). If the server does not take `X`, `embflash` falls back to
  `M` (hex). QEMU's gdbstub has no memory map and writes its flash this
  way.

Packets are as large as the server's `PacketSize`. A NAKed packet is sent
again. No-ack mode is used when the server offers it.

- `--verify` reads every byte back with `m` and names the first one that
  differs.
- `--monitor CMD` sends `CMD` to the server first (`qRcmd`). For example,
  `--monitor "reset halt"` for OpenOCD. Repeat the flag for more commands,
  up to 16. The server's console output is copied to stderr.

## Starting the program

`--run` sets the registers and continues. The registers are found **by
name** in the server's target description (`qXfer:features:read`), so
`embflash` keeps no table of register numbers.

- **Cortex-M.** If the description has `org.gnu.gdb.arm.m-profile`, the
  core starts the way it does at reset:
  - `sp` is the vector table's first word;
  - `pc` is the reset handler, with its Thumb bit cleared;
  - the Thumb bit is set in `xpsr`.

  The vector table is at the image's lowest address, or at
  `--vector-table ADDR`.
- **Other cores.** `pc` is the ELF entry, or `--entry ADDR`. HEX and
  S-records carry an entry in their start records.

Without `--run`, `embflash` detaches (`D`) and leaves the target halted.

## A serial bootloader: --stk500

`--stk500 serial:DEV[@BAUD]` takes `--gdb`'s place for an AVR part with a
bootloader that speaks STK500 version 1. Optiboot, the Arduino Uno's, does.
The baud rate is 115200 when left out; older Arduino bootloaders take 57600.

1. **Reset.** DTR and RTS go down for 250 ms, then up: through the board's
   capacitor that resets the part into its bootloader. `--no-reset` leaves
   the lines alone, for a board reset by hand.
2. **Sync.** `embflash` sends the sync request until the bootloader answers,
   up to ten times within three seconds.
3. **The part.** The signature names it, and so its flash, page and
   bootloader sizes:

   | Part | Signature | Flash | Page | Bootloader |
   |---|---|---|---|---|
   | ATmega328P | `1E 95 0F` | 32 KiB | 128 B | 512 B |
   | ATmega328 | `1E 95 14` | 32 KiB | 128 B | 512 B |
   | ATmega328PB | `1E 95 16` | 32 KiB | 128 B | 512 B |

   Any other signature is refused. An image that reaches the bootloader
   (0x7E00 on these parts) is refused before anything is written.
4. **Pages.** Every page the image touches is written whole. The bytes of
   the page the image does not give are the erased `0xff`, and pages it
   does not touch keep their contents. `--verify` reads every written page
   back and names the first byte that differs.
5. **Start.** Leaving programming mode starts the program. `--console
   SECONDS` then prints what it writes on the serial line, through the same
   open port: reopening the port resets an Uno and starts the program over.
   `--run`, `--monitor`, `--entry` and `--vector-table` belong to `--gdb`
   and are refused here.

## The Espressif ROM: --esp

`--esp serial:DEV[@BAUD]` programs an ESP32 through the serial download
mode in its ROM, the protocol esptool speaks. It reaches the ROM at the
ROM's 115200 baud, then asks it for `BAUD`: 460800 when not given, as
esptool's default. `@921600` is faster where the USB adapter carries it;
`@115200` keeps the ROM's rate.

- **An ELF** is made into the image the ROM boots (as esptool's `elf2image`
  makes it) and written to flash at `--base`, 0x1000 by default, which is
  where the ESP32's ROM boots from. Its code and read-only data may be in
  flash-mapped memory, as a program larger than the RAM needs: see [The
  image](#the-image). `--esp-image FILE` writes the flash from `--base` to a
  file instead, with no board.
- **A raw `.bin` or a HEX file** is written as it is: its bytes go to flash
  at their addresses, a `.bin` at `--base`. The ROM erases whole 4 KiB
  sectors, so a piece is written from the start of its sector, and the
  bytes of that sector the image does not give read `0xff`.
- **`--ram`** loads an ELF's pieces into RAM and starts it, as esptool's
  `load-ram` does. Nothing is written to flash.
- **`--console SECONDS`** prints what the program writes on the serial line
  for that long after it starts, through the same open port. Reopening the
  port would pulse the board's auto-reset and start the chip over.

1. **Into the ROM.** A development board's auto-reset circuit ties DTR to
   IO0 and RTS to EN, each through an inverting transistor. `embflash`
   holds EN low, pulls IO0 low, and releases EN, as esptool's classic reset
   does: the chip starts in download mode. On a board without the circuit,
   hold BOOT while pressing EN and pass `--no-reset`.
2. **Sync.** The sync request is sent up to ten times, and the reset and
   the syncs up to seven times over, as esptool's. The ROM answers it
   several times over, and the extra answers are drained.
3. **The chip.** The chip-detect register (0x40001000) names it. The ESP32,
   `0x00f01d83`, is the one known, with 4 MiB of flash assumed. Any other
   value is refused, and so is an image beyond the flash. Then the rate:
   `CHANGE_BAUDRATE`, answered at the old rate, then the new one on both
   ends. A ROM silent at the new rate is reported as an adapter that cannot
   carry it.
4. **Writing.** The flash is attached and its parameters given. Then each
   piece of the image is erased (`FLASH_BEGIN`) and written in blocks of
   1024 bytes, each with its checksum. `--verify` asks the ROM for the MD5
   of each piece as written and compares it with the image's.
5. **Start.** An EN pulse with IO0 high starts the program from flash, at
   115200 again, and `--console` reads it there. No `FLASH_END` is sent:
   the ROM answers it with an error. `--no-reset` leaves the chip in the
   ROM. A `--ram` load goes back to 115200 before `MEM_END`, so the program
   starts at the rate a cold boot gives it.

On an ESP32-D0WD-V3 board with a CP2102, 256 KiB went in 28.9 s at
115200 (9 KiB/s), 9.4 s at 460800 (27 KiB/s) and 6.5 s at 921600
(39 KiB/s), each verified. Past 460800 the time is mostly the ROM writing
the flash, one 1024-byte block per answer.

### Reading the flash: --read

`--read ADDR SIZE FILE` reads the ESP32's flash into `FILE` instead of
writing it, as esptool's `read-flash` does. `SIZE` is a number of bytes,
with `K` or `M` for KiB and MiB, or `all` for the rest of the flash from
`ADDR`.

- **The reads.** `READ_FLASH_SLOW`, the ROM's own read, 64 bytes a request.
  A reply the line loses is asked again, after half a second, and the
  count of those is printed.
- **The check.** The ROM's MD5 of the whole range is compared with the
  bytes received. A difference fails the run and no file is written.
- **The speed.** On the board, 25 KiB/s at 460800 and 45 KiB/s at 921600:
  all 4 MiB in 91 s. The 64-byte limit is the ROM's, and a flasher stub in
  RAM would read faster.

```sh
# the whole flash, as a backup before the first write
embflash --esp serial:/dev/cu.usbserial-0001@921600 --read 0 all backup.bin
```

### The image

For an ELF in RAM only, the image is esptool's format, byte for byte
(`tests/golden/embflash-esp.sh` compares them when esptool is installed):

- **Header.** `0xE9`, the segment count, the flash's mode, size and
  frequency (DIO, 4 MiB, 40 MHz), and the ELF's entry point.
- **Extended header.** No write-protect pin, chip 0 (the ESP32), any
  revision, and a SHA-256 appended.
- **Segments.** Each ELF piece at its load address, padded to a word.
- **Trailer.** The checksum byte, 0xEF xored with every segment byte, one
  byte short of a 16-byte boundary, then the SHA-256 of everything before it.

The ROM loads RAM segments only, into IRAM and DRAM. Pieces in
flash-mapped memory, code at 0x400D0000-0x403FFFFF and read-only data at
0x3F400000-0x3F7FFFFF, are read through the cache once its MMU maps them,
which ESP-IDF's second-stage bootloader does. `embflash` needs no second
stage for that:

- **The regions.** Each region (code, data) is written to flash after the
  image, from the 64 KiB page its first piece is in, on pages of its own:
  a piece at 0x400D0020 goes 0x20 into its flash page.
- **The boot stub.** A stub of about 400 bytes
  (`tools/embflash/esp32boot.c`, built by EmbCC) joins the image in RTC fast memory at 0x400C0000 as its
  entry, with a table of the program's entry and each region's virtual
  page, flash page and page count. It maps the regions with the ROM's own
  `cache_flash_mmu_set`, enables the cache, and calls the program's entry.
- **What it asks of the program.** Its entry is in IRAM, since the cache is
  not mapped until the stub has run. RTC fast memory is free for the
  program once it runs, but nothing may be loaded there: such a piece is
  refused.

On the ESP32-D0WD-V3 board a program with 291 KiB of code in flash (4000
functions, more than twice the IRAM) ran from it, 302 KiB written in about
7.5 s at 921600.

A program the ROM boots this way runs on the ROM's stack, with UART0 at
115200. The flash boot leaves the RTC and timer-group watchdogs running, so
it turns them off or feeds them.

## Examples

```sh
# QEMU: a board started with no program, its gdbstub on a socket
qemu-system-arm -M lm3s6965evb -nographic -S \
    -chardev socket,path=g.sock,server=on,wait=off,id=g -gdb chardev:g &
embflash fw.elf --gdb unix:g.sock --verify --run

# OpenOCD with an ST-Link
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg &
embflash fw.hex --gdb localhost:3333 --monitor "reset halt" --verify

# a Black Magic Probe
embflash fw.elf --gdb serial:/dev/ttyACM0 --verify --run

# an Arduino Uno through Optiboot, on the ATmega16U2's USB serial port,
# then what the program prints
embcc --target=avr -mmcu=atmega328p -Os blink.c -o blink.elf
embflash blink.elf --stk500 serial:/dev/cu.usbmodem1101 --verify --console 5

# an ESP32 development board: the ELF, as the image the ROM boots, with its output
embflash app.elf --esp serial:/dev/cu.usbserial-0001 --verify --console 5

# the same program run from RAM, the flash untouched
embflash app.elf --esp serial:/dev/cu.usbserial-0001 --ram --console 5

# the image alone, as esptool elf2image writes it
embflash app.elf --esp-image app.bin

# what would be written, with no target
embflash fw.bin --base 0x08000000 --dry-run
```

`--dry-run` prints the image's pieces and its entry. With `--gdb`, it
also prints the server's memory map, and writes nothing.

## Exit status

`embflash` exits with:

- `0` when the image was written, and verified if `--verify` was given.
- `2` when:
  - the image cannot be read;
  - an option is malformed;
  - the server refuses a request or does not answer within `--timeout`
    (10 s by default);
  - a byte reads back wrong.

Each failure names its reason. A server that answers an erase or a write
with an error is reported with that error.

## Limits

- `embflash` has been tested against QEMU and against a server that models
  a part's flash. It has **not yet** been tested against real probes. The
  `--stk500` path has been tested on an Arduino Uno R3 (an ATmega328P
  behind an ATmega16U2): 1.7 KiB written and read back in 1.2 s. The
  `--esp` path has been tested on an ESP32-D0WD-V3 development
  board (revision v3.1, 4 MiB of flash, a CP2102 bridge): an ELF made into
  the image and booted from 0x1000, an ELF run from RAM, and 28 KiB written
  and verified in 7 s.
- `--stk500` knows the ATmega328 family only. STK500 version 2, which the
  Arduino Mega's bootloader speaks, is not implemented.
- `--esp` knows the ESP32 only and assumes 4 MiB of flash and DIO at
  40 MHz. The flasher stub, compression, ESP-IDF's
  app images and partition table (an IDF program needs IDF's bootloader),
  PSRAM, and other chips (S2, S3, C3...) are not implemented.
- Option bytes, mass erase and read-out protection are server-specific.
  Reach them with `--monitor`.
- The transports need POSIX sockets and termios. Elsewhere `embflash`
  builds, but it can only `--dry-run`.

## Checked by

`tests/golden/embflash.sh` runs against `tests/golden/embflash/fakegdb.py`.
That server models a part's flash and reports any rule a programmer breaks:

- an erase that is not whole, aligned blocks;
- a write to bytes that were not erased;
- `X` or `M` into flash;
- a packet over `PacketSize`;
- a raw `$` or `*` in binary data.

The checks:

- **Every format:** one firmware as ELF, HEX, S-records and raw binary
  (all from `embpack`) lands byte for byte. Exactly the blocks it touches
  are erased (`check.py`), and the memory map is read in both orders
  (flash first, as OpenOCD sends it, and RAM first, as pyOCD does).
- **Escapes:** every byte value survives the binary escapes, in
  `vFlashWrite` and in `X`, split over 256-byte packets.
- **Protocol:** NAKed packets are sent again, and no-ack mode is taken.
  Without a memory map the image is written as memory, with `X` or, when
  the server has no `X`, with `M`.
- **Run and verify:** `--run` sets `sp`, `pc` and `xpsr` from the vector
  table. `--verify` catches a byte the server reads back wrong.
- **Refusals:** a truncated HEX file, a bad checksum, overlapping
  records, and a raw binary without `--base` are each refused for their
  own reason.
- **QEMU:** a program put into QEMU's `lm3s6965evb`, started with no
  image, runs and prints what it should.

Each of these checks has been seen to fail against a mutant of
`embflash` (18 of them).

`tests/golden/embflash-stk500.sh` runs against
`tests/golden/embflash/fakeoptiboot.py`. That model of Optiboot runs on a
pseudo-terminal and reports a page that is not whole or aligned, a write
with no address loaded, and a command Optiboot does not know. The checks:

- a HEX image with pieces in three pages, one partial, lands byte for byte,
  and is verified;
- a raw binary at `--base 0` lands the same way;
- a bootloader that misses the first three syncs is still synced;
- an image over the bootloader, an unknown signature, a bootloader that
  never answers, a byte that reads back wrong, and `--run` are each
  refused with exit status 2.

`tests/golden/embflash-esp.sh` runs against
`tests/golden/embflash/fakeesprom.py`. That model of the ESP32 ROM runs on a
pseudo-terminal and reports:

- a block with a wrong checksum, out of sequence, or outside the erased
  range;
- an unescaped SLIP frame;
- a write before the flash is attached;
- a RAM block out of sequence or beyond its load.

FLASH_END is answered with error 0x06, as the chip answers it, so a
programmer that sends it fails here as it fails on the board.

The checks:

- a binary holding every byte value lands byte for byte at 0x1000, and the
  ROM's MD5 matches;
- a HEX image in two pieces lands the same way;
- a ROM that misses three syncs is still synced;
- an unknown chip, a ROM that never answers, a wrong MD5 and an image beyond
  the flash are each refused with exit status 2.
