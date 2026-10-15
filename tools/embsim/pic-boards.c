/* pic-boards.c -- the boards around a PIC32 or a PIC24: what is wired
 * to the part's pins (struct board_io), each board a table of LEDs and
 * buttons, and the Explorer 16/32's sensors, LCD and serial EEPROM.
 *
 * Microchip's Explorer 16/32 (DM240001-3) with the PIC32MZ EF plug-in
 * module (MA320019). The pins are the PIM's: its information sheet maps
 * each pin of the Explorer's 100-pin socket to the PIC32MZ2048EFH100's
 * (DS50002218, written for the EC PIM, whose part has the EF's pinout),
 * and Harmony's BSP for the PIM agrees on the LEDs:
 *   - LEDs D3-D10 (lit when the pin drives high): D3 RA0, D4 RA1, D5 RA2,
 *     D6 RA3, D7 RF13, D8 RF12, D9 RA6, D10 RA7;
 *   - push buttons, low when pressed and pulled up by the board: S4 RF8,
 *     S5 RA7 (with D10), S6 RD13. S3 (RD6 on a 16-bit PIM) is not wired
 *     to the PIC32MZ, by the sheet;
 *   - the potentiometer R6 on AN45 (RB5, ADC0's alternate input) and the
 *     TC1047A temperature sensor U4 on AN4 (RB4): the sensor gives 500 mV
 *     + 10 mV per degree C;
 *   - the 16x2 character LCD, an HD44780 on the parallel master port:
 *     data on PMD0-7, RS on PMA0, E on PMWR, R/W on PMRD.
 *   - the 25LC256 serial EEPROM on socket pins 10 (SCK), 12 (SI), 11 (SO)
 *     and 79 (CS), which the PIM gives RA5, RG8, RG7 and RG15: RA5 has
 *     no SPI clock, so a program clocks the EEPROM itself on the pins
 *     (SPI mode 0 or 3: SI read as SCK rises, SO changed as it falls);
 *   - the USB-UART is whichever UART the program uses: every UART's
 *     output is the run's stdout (pic32-uart.c).
 *
 * The Explorer 16/32 with the PIC24FJ1024GB610 plug-in module (MA240023):
 * the board's schematic (03-01872, "Explorer 16/32 Development Board",
 * revision 6.3) names the socket pin of each part, and the PIM connects
 * the PIC24FJ1024GB610's pins to the socket one to one but for the
 * exceptions its information sheet lists (DS50002423), with its jumpers
 * in their USB position (J1-J4 1-2):
 *   - LEDs D3-D10 on socket pins 17, 38, 58-61, 91, 92: RA0-RA7;
 *   - S3 (pin 83) RD6, S4 (80) RD13, S5 (92) RA7 (with D10), S6 (84) RD7,
 *     pulled up by the board;
 *   - the potentiometer on pin 20, AN5 (RB5); the TC1047A on pin 21, AN4
 *     (RB4);
 *   - the LCD's RS on pin 44, PMA0 (RB15), E on 81 PMWR (RD4), R/W on 82
 *     PMRD (RD5), data on PMD0-7 (RE0-RE7): the EPMP;
 *   - the 25LC256 serial EEPROM: SCK on pin 10 (RG6, RP21), SI on pin 12
 *     (RG8, RP19), SO on pin 11 (RG7, RP26), CS on pin 79, which J1 gives
 *     the part's RG0. It answers the SPI unit whose SCKxOUT and SDOx the
 *     Peripheral Pin Select puts on RP21 and RP19 while RG0 drives low;
 *     the part reads it on the unit whose SDIx input is RP26.
 *
 * The Explorer 16/32 with the PIC32MX795F512L PIM (MA320003): the part's
 * 100-pin package puts the board's signals on the socket's pins one to
 * one (its own pin list says so: RA0-RA7 on pins 17, 38, 58-61, 91, 92,
 * RD6/RD13/RD7 on 83/80/84, AN4 and AN5 on 21 and 20, PMA0, PMWR, PMRD
 * and PMD0-7 where the LCD is, SCK2/SDI2/SDO2 on 10/11/12, RD12 on 79):
 *   - LEDs D3-D10 on RA0-RA7; S3 RD6, S4 RD13, S5 RA7, S6 RD7, pulled up;
 *   - the potentiometer on AN5, the TC1047A on AN4;
 *   - the LCD on the PMP;
 *   - the 25LC256 on SPI2 (no Peripheral Pin Select on this part), its
 *     CS on RD12;
 *   - the core at 80 MHz (its usual PLL setting), for the times reported.
 *
 * Microchip's Curiosity PIC32MZ EF 2.0 (DM320209, a PIC32MZ2048EFM144),
 * by Harmony 3's BSP for it (bsp/boards/pic32mz_ef_curiosity_v2):
 *   - LED1 RJ7, LED2 RK7, LED3 RJ3, and the RGB LED's red RB7, green RB8
 *     and blue RB9, each lit when the pin drives low;
 *   - switches SW1 RJ4, SW2 RJ5, SW3 RJ6, SW4 RC14, low when pressed,
 *     with no pull-up on the board: released, a pin reads its own
 *     pull-up (CNPUx), else 0.
 *
 * What the run's user sees, on stderr: each LED that goes on or off, and
 * the LCD's two lines each time the program clears the display or sends
 * the cursor home -- the frame it had drawn -- and when the run ends.
 * What they set (--set NAME=VALUE): a button =pressed or =released, and
 * on the Explorer temperature=DEGREES (25 by default) and pot=VOLTS or
 * pot=PERCENT% (half way by default).
 *
 * The serial EEPROM, a 25LC256 (32 KiB, erased to 0xFF at power-on):
 * READ, WRITE (a 64-byte page, written when CS rises, then 5 ms busy:
 * STATUS.WIP), WREN, WRDI, RDSR, WRSR (BP1, BP0: the upper quarter, half
 * or all protected), as its data sheet (DS21822) has them.
 *
 * The LCD controller: the HD44780's instruction set -- clear, home,
 * entry mode (increment or decrement, display shift), display on/off,
 * cursor and display shift, function set (8 or 4-bit interface, one or
 * two lines), CGRAM and DDRAM addresses -- and its 80-byte display RAM,
 * line 2 at 0x40. It is never busy (a read of its status gives BF 0 and
 * the address counter). Characters outside ASCII show as '?', the eight
 * CGRAM ones as '#'. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "pic32.h"
#include "svd-map.h"

enum { PA = 0, PB, PC, PD, PE, PF, PG, PH, PJ, PK };

/* an LED: lit when its pin drives high, or (active_low) low */
struct led { int port, bit; const char *name; int active_low; };
/* a button: low when pressed; released, the board pulls it up (pull),
 * or nothing drives it */
struct button { int port, bit; const char *name; int pull; };

struct board {
    const char *name;
    const struct led *led;
    int nled;
    const struct button *button;
    int nbutton;
    int explorer;                   /* the TC1047A, the potentiometer, the LCD */
    int an_temp, an_pot;            /* their analog inputs */
    int eeprom;                     /* the 25LC256: EE_SPI, EE_SPI2 or EE_PINS */
    const char *no_s3;              /* why S3 is not there, or 0 */
    double hz;                      /* a PIC32's core clock, or 0: 200 MHz */
};

/* the 25LC256's wiring: an SPI unit the PPS routes (the PIC24 PIM's),
 * SPI2 with CS on RD12 (the PIC32MX PIM's), or the PIC32MZ PIM's pins,
 * which the program clocks itself */
enum { EE_NONE, EE_SPI, EE_SPI2, EE_PINS };

static const struct led e16_leds[] = {
    { PA, 0, "D3", 0 }, { PA, 1, "D4", 0 }, { PA, 2, "D5", 0 },
    { PA, 3, "D6", 0 }, { PF, 13, "D7", 0 }, { PF, 12, "D8", 0 },
    { PA, 6, "D9", 0 }, { PA, 7, "D10", 0 },
};
static const struct button e16_buttons[] = {
    { PF, 8, "S4", 1 }, { PA, 7, "S5", 1 }, { PD, 13, "S6", 1 },
};
static const struct led e16p24_leds[] = {
    { PA, 0, "D3", 0 }, { PA, 1, "D4", 0 }, { PA, 2, "D5", 0 },
    { PA, 3, "D6", 0 }, { PA, 4, "D7", 0 }, { PA, 5, "D8", 0 },
    { PA, 6, "D9", 0 }, { PA, 7, "D10", 0 },
};
static const struct button e16p24_buttons[] = {
    { PD, 6, "S3", 1 }, { PD, 13, "S4", 1 }, { PA, 7, "S5", 1 },
    { PD, 7, "S6", 1 },
};
static const struct led cur_leds[] = {
    { PJ, 7, "LED1", 1 }, { PK, 7, "LED2", 1 }, { PJ, 3, "LED3", 1 },
    { PB, 7, "RGB red", 1 }, { PB, 8, "RGB green", 1 }, { PB, 9, "RGB blue", 1 },
};
static const struct button cur_buttons[] = {
    { PJ, 4, "SW1", 0 }, { PJ, 5, "SW2", 0 }, { PJ, 6, "SW3", 0 },
    { PC, 14, "SW4", 0 },
};

#define N(a) (int)(sizeof a / sizeof a[0])
static const struct board boards_[] = {
    { "explorer16-32", e16_leds, N(e16_leds), e16_buttons, N(e16_buttons),
      1, 4, 45, EE_PINS, "S3 is not wired to the PIC32MZ EF plug-in module", 0 },
    { "explorer16-32-pic24", e16p24_leds, N(e16p24_leds), e16p24_buttons,
      N(e16p24_buttons), 1, 4, 5, EE_SPI, 0, 0 },
    { "explorer16-32-pic32mx", e16p24_leds, N(e16p24_leds), e16p24_buttons,
      N(e16p24_buttons), 1, 4, 5, EE_SPI2, 0, 80e6 },
    { "explorer16-32-dspic33", e16p24_leds, N(e16p24_leds), e16p24_buttons,
      N(e16p24_buttons), 1, 4, 5, EE_SPI2, 0, 0 },
    { "curiosity-pic32mz-ef-2", cur_leds, N(cur_leds), cur_buttons,
      N(cur_buttons), 0, 0, 0, 0, 0, 0 },
};

#define NLED 8
#define NBUTTON 4

struct lcd {
    u8 ddram[0x80];
    u8 ac;                          /* the address counter */
    int cgram;                      /* the counter addresses CGRAM */
    int inc, shift_on, display, lines2, bits8;
    int shift;                      /* the display's shift */
    int nibble;                     /* 4-bit mode: the high half is in */
    u8 hi;
    char shown[2][17];              /* the last frame printed */
    int drawn;                      /* written since then */
};

/* the 25LC256 */
#define EE_SIZE 0x8000
#define EE_PAGE 64
struct eeprom {
    u8 mem[EE_SIZE];
    int selected;                   /* CS low */
    int nbytes;                     /* bytes since it was selected */
    u8 cmd;
    u16 addr;
    int wel;                        /* write enabled */
    u8 bp;                          /* STATUS's BP1, BP0 and WPEN */
    u8 page[EE_PAGE];
    u64 page_set;                   /* the page's bytes written */
    u8 wrsr;                        /* WRSR's byte */
    u64 busy_until;                 /* the write cycle's end (cycles) */
    /* clocked on pins (EE_PINS) */
    int sck, si, so;                /* the pins' levels */
    int bits;                       /* of the byte coming in */
    u8 in, out;                     /* the byte coming in, the one going out */
};

struct pboard {
    struct sim *s;
    const struct board *b;
    int led[NLED];
    int pressed[NBUTTON];
    double temperature, pot;
    struct lcd lcd;
    struct eeprom *ee;
};

/* the core's clock: the time the board reports */
static double hz(const struct pboard *x)
{
    return pic24_is(x->s->cpu) ? (double)pic24_fcy(x->s) : x->b->hz ? x->b->hz : PIC32_SYSCLK;
}

static double now_ms(const struct pboard *x)
{
    return (double)x->s->cycles / (hz(x) / 1000.0);
}

static void say(const struct pboard *x, const char *fmt, const char *a,
                const char *b)
{
    fflush(stdout);
    fprintf(stderr, "%s: ", x->b->name);
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, " at %.3f ms\n", now_ms(x));
}

/* ---- the LEDs and buttons ------------------------------------------------ */

/* port K is the eleventh: there is no port I */
static char port_letter(int port)
{
    return (char)('A' + port + (port >= 8));
}

static void ee_select(struct pboard *x, int low);
static void ee_pins(struct pboard *x, int port, int bit, int level);

static void pin_out(void *ctx, int port, int bit, int level)
{
    struct pboard *x = ctx;
    char pin[8];
    if (x->b->eeprom == EE_SPI && port == PG && bit == 0)  /* CS, by J1 */
        ee_select(x, level == 0);
    if (x->b->eeprom == EE_SPI2 && port == PD && bit == 12)
        ee_select(x, level == 0);
    if (x->b->eeprom == EE_PINS)
        ee_pins(x, port, bit, level);
    for (int i = 0; i < x->b->nled; i++) {
        const struct led *l = &x->b->led[i];
        int on = l->active_low ? level == 0 : level == 1;
        if (l->port == port && l->bit == bit && x->led[i] != on) {
            x->led[i] = on;
            snprintf(pin, sizeof pin, "R%c%d", port_letter(port), bit);
            say(x, on ? "LED %s on (%s)" : "LED %s off (%s)", l->name, pin);
        }
    }
}

static int pin_in(void *ctx, int port, int bit, int *level)
{
    struct pboard *x = ctx;
    if (x->b->eeprom == EE_PINS && port == PG && bit == 7 && x->ee->selected) {
        *level = x->ee->so;         /* SO, driven while CS is low */
        return 1;
    }
    for (int i = 0; i < x->b->nbutton; i++) {
        const struct button *b = &x->b->button[i];
        if (b->port == port && b->bit == bit && (x->pressed[i] || b->pull)) {
            *level = !x->pressed[i];
            return 1;
        }
    }
    return 0;
}

/* ---- the analog inputs ---------------------------------------------------- */

static double analog(void *ctx, int n)
{
    struct pboard *x = ctx;
    if (!x->b->explorer)
        return -1;
    if (n == x->b->an_temp)         /* TC1047A */
        return 0.5 + 0.01 * x->temperature;
    if (n == x->b->an_pot)          /* R6 */
        return x->pot;
    return -1;
}

/* ---- the LCD ---------------------------------------------------------------- */

static int ddram_index(const struct lcd *l, int line, int col)
{
    int w = l->lines2 ? 40 : 80;
    return (line ? 0x40 : 0) + ((col + l->shift) % w + w) % w;
}

static void frame(struct lcd *l, char out[2][17])
{
    for (int line = 0; line < 2; line++) {
        for (int c = 0; c < 16; c++) {
            u8 ch = l->ddram[ddram_index(l, line, c)];
            out[line][c] = !l->display || (line && !l->lines2) ? ' '
                         : ch < 8 ? '#' : ch < 0x20 || ch > 0x7e ? '?'
                         : (char)ch;
        }
        out[line][16] = 0;
    }
}

/* the frame drawn since the last one shown, if it is new */
static void show(struct pboard *x)
{
    struct lcd *l = &x->lcd;
    char f[2][17];
    if (!l->drawn)
        return;
    l->drawn = 0;
    frame(l, f);
    if (!memcmp(f, l->shown, sizeof f))
        return;
    memcpy(l->shown, f, sizeof f);
    fflush(stdout);
    fprintf(stderr, "%s: LCD |%s| at %.3f ms\n%s:     |%s|\n", x->b->name,
            f[0], now_ms(x), x->b->name, f[1]);
}

/* the address counter after a character: in two-line mode 0x27 goes on
 * to 0x40 and 0x67 back to 0, as the HD44780's does */
static void lcd_step(struct lcd *l)
{
    if (l->cgram) {
        l->ac = (u8)((l->ac + (l->inc ? 1 : 0x3f)) & 0x3f);
        return;
    }
    if (l->lines2) {
        int line = l->ac >= 0x40, pos = (l->ac & 0x7f) - (line ? 0x40 : 0);
        pos += l->inc ? 1 : -1;
        if (pos >= 40 || pos < 0) {
            pos = pos < 0 ? 39 : 0;
            line = !line;
        }
        l->ac = (u8)(pos + (line ? 0x40 : 0));
    } else {
        l->ac = (u8)((l->ac + (l->inc ? 1 : 79)) % 80);
    }
    if (l->shift_on)
        l->shift += l->inc ? 1 : -1;
}

static void lcd_command(struct pboard *x, u8 c)
{
    struct lcd *l = &x->lcd;
    if (c & 0x80) {                                 /* DDRAM address */
        l->cgram = 0;
        l->ac = c & 0x7f;
    } else if (c & 0x40) {                          /* CGRAM address */
        l->cgram = 1;
        l->ac = c & 0x3f;
    } else if (c & 0x20) {                          /* function set */
        l->bits8 = (c >> 4) & 1;
        l->lines2 = (c >> 3) & 1;
        l->nibble = 0;
    } else if (c & 0x10) {                          /* cursor or display shift */
        int right = (c >> 2) & 1;
        if (c & 0x08)
            l->shift += right ? -1 : 1;
        else
            l->ac = (u8)((l->ac + (right ? 1 : 0x7f)) & 0x7f);
    } else if (c & 0x08) {                          /* display on/off */
        l->display = (c >> 2) & 1;
        l->drawn = 1;
    } else if (c & 0x04) {                          /* entry mode */
        l->inc = (c >> 1) & 1;
        l->shift_on = c & 1;
    } else if (c & 0x02) {                          /* home */
        show(x);
        l->ac = 0;
        l->cgram = 0;
        l->shift = 0;
    } else if (c & 0x01) {                          /* clear */
        show(x);
        memset(l->ddram, ' ', sizeof l->ddram);
        l->ac = 0;
        l->cgram = 0;
        l->shift = 0;
        l->inc = 1;
    }
}

static void lcd_data(struct pboard *x, u8 d)
{
    struct lcd *l = &x->lcd;
    if (!l->cgram) {
        l->ddram[l->ac & 0x7f] = d;
        l->drawn = 1;
    }
    lcd_step(l);
}

/* RS (PMA0) picks the register; after a function set to a 4-bit
 * interface, a byte comes in two writes, its high half first, on DB7-4 */
static void pmp_write(void *ctx, u32 addr, u32 data)
{
    struct pboard *x = ctx;
    struct lcd *l = &x->lcd;
    u8 b = (u8)data;
    if (!l->bits8) {
        if (!l->nibble) {
            l->hi = b & 0xf0;
            l->nibble = 1;
            return;
        }
        b = (u8)(l->hi | (b >> 4));
        l->nibble = 0;
    }
    if (addr & 1)
        lcd_data(x, b);
    else
        lcd_command(x, b);
}

static u32 pmp_read(void *ctx, u32 addr)
{
    struct pboard *x = ctx;
    struct lcd *l = &x->lcd;
    if (!(addr & 1))
        return l->ac & 0x7f;                       /* BF 0 */
    u8 d = l->cgram ? 0 : l->ddram[l->ac & 0x7f];
    lcd_step(l);
    return d;
}

/* ---- the serial EEPROM ------------------------------------------------------ */

enum { EE_WRSR = 1, EE_WRITE = 2, EE_READ = 3, EE_WRDI = 4, EE_RDSR = 5,
       EE_WREN = 6 };

static int ee_busy(const struct pboard *x)
{
    return x->s->cycles < x->ee->busy_until;
}

static u8 ee_status(const struct pboard *x)
{
    const struct eeprom *e = x->ee;
    return (u8)(e->bp | (e->wel ? 2 : 0) | (ee_busy(x) ? 1 : 0));
}

/* the protected addresses BP1:BP0 give: none, the upper quarter, half,
 * all */
static int ee_protected(const struct eeprom *e, u32 a)
{
    switch (e->bp >> 2 & 3) {
    case 1: return a >= 0x6000;
    case 2: return a >= 0x4000;
    case 3: return 1;
    }
    return 0;
}

/* CS falls (low 1) or rises: a write's page goes in as it rises, and the
 * write cycle begins */
static void ee_select(struct pboard *x, int low)
{
    struct eeprom *e = x->ee;
    if (low == e->selected)
        return;
    e->selected = low;
    if (low) {
        e->nbytes = 0;
        e->page_set = 0;
        return;
    }
    if (e->nbytes < 1 || ee_busy(x))
        return;
    if (e->cmd == EE_WRITE && e->wel && e->nbytes > 3 && e->page_set) {
        u32 base = e->addr & ~(u32)(EE_PAGE - 1);
        for (int k = 0; k < EE_PAGE; k++)
            if ((e->page_set >> k & 1) && !ee_protected(e, base + (u32)k))
                e->mem[base + (u32)k] = e->page[k];
        e->wel = 0;
        e->busy_until = x->s->cycles + (u64)(hz(x) * 0.005);
    } else if (e->cmd == EE_WRSR && e->wel && e->nbytes > 1) {
        e->bp = e->wrsr & 0x8c;
        e->wel = 0;
        e->busy_until = x->s->cycles + (u64)(hz(x) * 0.005);
    }
}

/* a byte in on SI; what goes out on SO (0xff while it drives nothing) */
static int ee_byte(struct pboard *x, int in)
{
    struct eeprom *e = x->ee;
    int k = e->nbytes++, out = 0xff;
    if (k == 0) {
        e->cmd = (u8)in;
        if (ee_busy(x) && in != EE_RDSR)
            e->cmd = 0;                         /* only RDSR while busy */
        if (e->cmd == EE_WREN)
            e->wel = 1;
        else if (e->cmd == EE_WRDI)
            e->wel = 0;
        return out;
    }
    switch (e->cmd) {
    case EE_RDSR:
        return ee_status(x);
    case EE_WRSR:
        if (k == 1)
            e->wrsr = (u8)in;
        return out;
    case EE_READ:
    case EE_WRITE:
        if (k == 1) {
            e->addr = (u16)((in & 0x7f) << 8);
            return out;
        }
        if (k == 2) {
            e->addr = (u16)(e->addr | (u8)in);
            return out;
        }
        if (e->cmd == EE_READ) {
            out = e->mem[e->addr];
            e->addr = (u16)((e->addr + 1) & (EE_SIZE - 1));
            return out;
        }
        /* within the page, wrapping */
        {
            u32 off = (e->addr + (u32)(k - 3)) & (EE_PAGE - 1);
            e->page[off] = (u8)in;
            e->page_set |= 1ull << off;
        }
        return out;
    }
    return out;
}

/* what goes out on SO through the next byte: STATUS after RDSR, the
 * data after READ's address (ee_byte's answer, before its byte is in) */
static u8 ee_next(const struct pboard *x)
{
    const struct eeprom *e = x->ee;
    if (e->nbytes && e->cmd == EE_RDSR)
        return ee_status(x);
    if (e->nbytes >= 3 && e->cmd == EE_READ)
        return e->mem[e->addr];
    return 0xff;
}

/* the PIC32 PIM's pins: CS RG15, SCK RA5, SI RG8 (SO RG7: pin_in). A
 * bit in as SCK rises, the next out as it falls; a byte's first bit is
 * out as CS falls, or as the byte before it ends */
static void ee_pins(struct pboard *x, int port, int bit, int level)
{
    struct eeprom *e = x->ee;
    if (level < 0)
        return;                     /* let go: the line keeps its level */
    if (port == PG && bit == 15) {
        ee_select(x, level == 0);
        e->bits = 0;
        e->out = ee_next(x);
        e->so = e->out >> 7 & 1;
    } else if (port == PG && bit == 8) {
        e->si = level;
    } else if (port == PA && bit == 5 && level != e->sck) {
        e->sck = level;
        if (!e->selected)
            return;
        if (level) {
            e->in = (u8)(e->in << 1 | e->si);
            if (++e->bits == 8) {
                ee_byte(x, e->in);
                e->bits = 0;
                e->out = ee_next(x);
            }
        } else {
            e->so = e->out >> (7 - e->bits) & 1;
        }
    }
}

/* the SPI unit whose clock and data the PPS puts on the EEPROM's pins:
 * SCKxOUT on RP21, SDOx on RP19 (RPOR10's and RPOR9's high fields); and
 * the unit reading RP26 (SDIxR) */
static int pps_out(struct pboard *x, int rp)
{
    struct rf_periph *p = x->s->svd ? rf_periph(x->s->svd, "PPS") : 0;
    char nm[16];
    snprintf(nm, sizeof nm, "RPOR%d", rp / 2);
    struct rf_reg *r = p ? rf_reg(p, nm) : 0;
    return r ? (int)(r->value >> (rp & 1 ? 8 : 0) & 0x3f) : 0;
}

static int pps_in(struct pboard *x, const char *reg)
{
    struct rf_periph *p = x->s->svd ? rf_periph(x->s->svd, "PPS") : 0;
    struct rf_reg *r = p ? rf_reg(p, reg) : 0;
    return r ? (int)(r->value & 0x3f) : 0x3f;
}

static int spi(void *ctx, int unit, int out)
{
    struct pboard *x = ctx;
    static const int sdo[4] = { 0, 7, 10, 23 }, sck[4] = { 0, 8, 11, 24 };
    static const char *const sdi[4] = { 0, "RPINR20", "RPINR22", "RPINR28" };
    if (x->b->eeprom == EE_SPI2)
        return x->ee && unit == 2 && x->ee->selected ? ee_byte(x, out) : -1;
    if (!x->ee || unit < 1 || unit > 3)
        return -1;
    if (pps_out(x, 21) != sck[unit] || pps_out(x, 19) != sdo[unit] ||
        !x->ee->selected)
        return -1;
    int b = ee_byte(x, out);
    return pps_in(x, sdi[unit]) == 26 ? b : -1;
}

/* ---- the user's inputs, and the end --------------------------------------- */

/* an input changed from outside: a port's next read sees it */
static void s_changed(struct pboard *x)
{
    x->s->bus.touched = 1;
}

static int set(void *ctx, const char *name, const char *value)
{
    struct pboard *x = ctx;
    char *e;
    for (int i = 0; i < x->b->nbutton; i++)
        if (!strcmp(name, x->b->button[i].name)) {
            if (!strcmp(value, "pressed") || !strcmp(value, "1"))
                x->pressed[i] = 1;
            else if (!strcmp(value, "released") || !strcmp(value, "0"))
                x->pressed[i] = 0;
            else
                die("--set %s=%s: a button is pressed or released", name, value);
            s_changed(x);
            return 0;
        }
    if (!x->b->explorer)
        return -1;
    if (!strcmp(name, "temperature")) {
        double t = strtod(value, &e);
        if (e == value || *e)
            die("--set temperature=%s: give degrees C, as 25 or -3.5", value);
        x->temperature = t;
        return 0;
    }
    if (!strcmp(name, "pot")) {
        double v = strtod(value, &e);
        if (e == value || (*e && strcmp(e, "%") && strcmp(e, "V")))
            die("--set pot=%s: give volts (1.65) or a percentage (50%%)", value);
        x->pot = *e == '%' ? 3.3 * v / 100.0 : v;
        if (x->pot < 0 || x->pot > 3.3)
            die("--set pot=%s: the potentiometer gives 0 to 3.3 V", value);
        return 0;
    }
    if (!strcmp(name, "S3") && x->b->no_s3)
        die("--set S3: %s", x->b->no_s3);
    return -1;
}

static void end(void *ctx)
{
    struct pboard *x = ctx;
    if (!x->b->explorer)
        return;
    x->lcd.drawn = 1;
    show(x);
}

static void board_reset(void *ctx)
{
    struct pboard *x = ctx;
    struct lcd *l = &x->lcd;
    memset(l->ddram, ' ', sizeof l->ddram);
    l->ac = 0;
    l->cgram = 0;
    l->inc = 1;
    l->shift_on = 0;
    l->display = 0;
    l->lines2 = 0;
    l->bits8 = 1;
    l->shift = 0;
    l->nibble = 0;
    l->drawn = 0;
    memset(l->shown, ' ', sizeof l->shown);
    l->shown[0][16] = l->shown[1][16] = 0;
    for (int i = 0; i < NLED; i++)
        x->led[i] = 0;
    if (x->ee) {                    /* its memory keeps what was written */
        x->ee->selected = 0;
        x->ee->wel = 0;
        x->ee->busy_until = 0;
        x->ee->sck = x->ee->si = x->ee->bits = 0;
        x->ee->so = 1;
    }
}

const struct dev_ops pic_board_ops = {
    "pic32-board", 0, 0, board_reset, 0, 0,
};

void *pic_board_create(struct sim *s, const struct dev_desc *d)
{
    const struct board *b = 0;
    for (int i = 0; i < N(boards_); i++)
        if (!strcmp(boards_[i].name, d->type))
            b = &boards_[i];
    if (!b)
        die("no PIC board named %s", d->type);
    struct pboard *x = calloc(1, sizeof *x);
    if (!x)
        die("out of memory");
    x->s = s;
    x->b = b;
    x->temperature = 25.0;
    x->pot = 1.65;
    if (b->eeprom) {
        x->ee = calloc(1, sizeof *x->ee);
        if (!x->ee)
            die("out of memory");
        memset(x->ee->mem, 0xff, sizeof x->ee->mem);
    }
    board_reset(x);
    s->io.ctx = x;
    s->io.pin_out = pin_out;
    s->io.pin_in = pin_in;
    s->io.analog = analog;
    if (b->explorer) {
        s->io.bus_write = pmp_write;
        s->io.bus_read = pmp_read;
    }
    if (b->eeprom == EE_SPI || b->eeprom == EE_SPI2)
        s->io.spi = spi;
    s->io.set = set;
    s->io.end = end;
    return x;
}
