# Board reference

Every board `--board=` accepts, generated from EmbCC's board table (`src/driver/options.c`) by `embcc --reference=boards`. [Boards](boards.md) is the guide: what a board's program gets, `<board.h>`, and putting it on the board.

| Board | What it is | Target | Programmed with |
|---|---|---|---|
| `uno` | Arduino Uno (ATmega328P, 16 MHz, Optiboot) | `avr` `--mcu=atmega328p` | `embflash --stk500 serial:PORT (115200)` |
| `nano` | Arduino Nano (ATmega328P, 16 MHz, Optiboot) | `avr` `--mcu=atmega328p` | `embflash --stk500 serial:PORT (115200; serial:PORT@57600 for the old bootloader)` |
| `mega2560` | Arduino Mega 2560 (ATmega2560, 16 MHz, the STK500v2 bootloader) | `avr` `--mcu=atmega2560` | `embflash --stk500v2 serial:PORT (115200)` |
| `esp32-devkit` | ESP32 DevKit (ESP32-WROOM-32, 40 MHz from the ROM, 4 MiB flash) | `xtensa-esp32-elf` | `embflash --esp serial:PORT (460800)` |
