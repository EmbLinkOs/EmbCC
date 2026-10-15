/* pic32-boards.c -- the boards around a PIC32: what is wired to the
 * part's pins (struct board_io), each board a table of LEDs and buttons,
 * and the Explorer 16/32's sensors and LCD.
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
 *   - the USB-UART is whichever UART the program uses: every UART's
 *     output is the run's stdout (pic32-uart.c).
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
#include "pic32.h"

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
};

static const struct led e16_leds[] = {
    { PA, 0, "D3", 0 }, { PA, 1, "D4", 0 }, { PA, 2, "D5", 0 },
    { PA, 3, "D6", 0 }, { PF, 13, "D7", 0 }, { PF, 12, "D8", 0 },
    { PA, 6, "D9", 0 }, { PA, 7, "D10", 0 },
};
static const struct button e16_buttons[] = {
    { PF, 8, "S4", 1 }, { PA, 7, "S5", 1 }, { PD, 13, "S6", 1 },
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
    { "explorer16-32", e16_leds, N(e16_leds), e16_buttons, N(e16_buttons), 1 },
    { "curiosity-pic32mz-ef-2", cur_leds, N(cur_leds), cur_buttons,
      N(cur_buttons), 0 },
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

struct pboard {
    struct sim *s;
    const struct board *b;
    int led[NLED];
    int pressed[NBUTTON];
    double temperature, pot;
    struct lcd lcd;
};

static double now_ms(const struct pboard *x)
{
    return (double)x->s->cycles / (PIC32_SYSCLK / 1000.0);
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

static void pin_out(void *ctx, int port, int bit, int level)
{
    struct pboard *x = ctx;
    char pin[8];
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
    if (n == 4)                     /* TC1047A */
        return 0.5 + 0.01 * x->temperature;
    if (n == 45)                    /* R6 */
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
    if (!strcmp(name, "S3"))
        die("--set S3: S3 is not wired to the PIC32MZ EF plug-in module");
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
}

const struct dev_ops pic32_board_ops = {
    "pic32-board", 0, 0, board_reset, 0, 0,
};

void *pic32_board_create(struct sim *s, const struct dev_desc *d)
{
    const struct board *b = 0;
    for (int i = 0; i < N(boards_); i++)
        if (!strcmp(boards_[i].name, d->type))
            b = &boards_[i];
    if (!b)
        die("no PIC32 board named %s", d->type);
    struct pboard *x = calloc(1, sizeof *x);
    if (!x)
        die("out of memory");
    x->s = s;
    x->b = b;
    x->temperature = 25.0;
    x->pot = 1.65;
    board_reset(x);
    s->io.ctx = x;
    s->io.pin_out = pin_out;
    s->io.pin_in = pin_in;
    s->io.analog = analog;
    if (b->explorer) {
        s->io.bus_write = pmp_write;
        s->io.bus_read = pmp_read;
    }
    s->io.set = set;
    s->io.end = end;
    return x;
}
