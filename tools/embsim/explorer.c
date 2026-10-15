/* explorer.c -- Microchip's Explorer 16/32 board (DM240001-3) with the
 * PIC32MZ EF plug-in module (MA320019): what is on the board around the
 * part, wired to its pins (struct board_io).
 *
 * The pins are the PIM's: its information sheet maps each pin of the
 * Explorer's 100-pin socket to the PIC32MZ2048EFH100's (DS50002218,
 * written for the EC PIM, whose part has the EF's pinout), and Harmony's
 * BSP for the PIM agrees on the LEDs:
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
 * What the run's user sees, on stderr: each LED that goes on or off, and
 * the LCD's two lines each time the program clears the display or sends
 * the cursor home -- the frame it had drawn -- and when the run ends.
 * What they set (--set NAME=VALUE): temperature=DEGREES (25 by default),
 * pot=VOLTS or pot=PERCENT% (half way by default), S4, S5 or S6 =pressed
 * or =released.
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

enum { PA = 0, PD = 3, PF = 5 };

static const struct { int port, bit; const char *name; } leds[] = {
    { PA, 0, "D3" }, { PA, 1, "D4" }, { PA, 2, "D5" }, { PA, 3, "D6" },
    { PF, 13, "D7" }, { PF, 12, "D8" }, { PA, 6, "D9" }, { PA, 7, "D10" },
};
#define NLED (int)(sizeof leds / sizeof leds[0])

static const struct { int port, bit; const char *name; } buttons[] = {
    { PF, 8, "S4" }, { PA, 7, "S5" }, { PD, 13, "S6" },
};
#define NBUTTON (int)(sizeof buttons / sizeof buttons[0])

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

struct explorer {
    struct sim *s;
    int led[NLED];
    int pressed[NBUTTON];
    double temperature, pot;
    struct lcd lcd;
};

static double now_ms(const struct explorer *x)
{
    return (double)x->s->cycles / (PIC32_SYSCLK / 1000.0);
}

static void say(const struct explorer *x, const char *fmt, const char *a,
                const char *b)
{
    fflush(stdout);
    fprintf(stderr, "explorer16-32: ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, " at %.3f ms\n", now_ms(x));
}

/* ---- the LEDs and buttons ------------------------------------------------ */

static void pin_out(void *ctx, int port, int bit, int level)
{
    struct explorer *x = ctx;
    char pin[8];
    for (int i = 0; i < NLED; i++)
        if (leds[i].port == port && leds[i].bit == bit && x->led[i] != level) {
            x->led[i] = level;
            snprintf(pin, sizeof pin, "R%c%d", 'A' + port, bit);
            say(x, level ? "LED %s on (%s)" : "LED %s off (%s)", leds[i].name,
                pin);
        }
}

static int pin_in(void *ctx, int port, int bit, int *level)
{
    struct explorer *x = ctx;
    for (int i = 0; i < NBUTTON; i++)
        if (buttons[i].port == port && buttons[i].bit == bit) {
            *level = !x->pressed[i];
            return 1;
        }
    return 0;
}

/* ---- the analog inputs ---------------------------------------------------- */

static double analog(void *ctx, int n)
{
    struct explorer *x = ctx;
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
static void show(struct explorer *x)
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
    fprintf(stderr, "explorer16-32: LCD |%s| at %.3f ms\n"
                    "explorer16-32:     |%s|\n", f[0], now_ms(x), f[1]);
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

static void lcd_command(struct explorer *x, u8 c)
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

static void lcd_data(struct explorer *x, u8 d)
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
    struct explorer *x = ctx;
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
    struct explorer *x = ctx;
    struct lcd *l = &x->lcd;
    if (!(addr & 1))
        return l->ac & 0x7f;                       /* BF 0 */
    u8 d = l->cgram ? 0 : l->ddram[l->ac & 0x7f];
    lcd_step(l);
    return d;
}

/* ---- the user's inputs, and the end --------------------------------------- */

static int set(void *ctx, const char *name, const char *value)
{
    struct explorer *x = ctx;
    char *e;
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
    for (int i = 0; i < NBUTTON; i++)
        if (!strcmp(name, buttons[i].name)) {
            if (!strcmp(value, "pressed") || !strcmp(value, "1"))
                x->pressed[i] = 1;
            else if (!strcmp(value, "released") || !strcmp(value, "0"))
                x->pressed[i] = 0;
            else
                die("--set %s=%s: a button is pressed or released", name, value);
            return 0;
        }
    if (!strcmp(name, "S3"))
        die("--set S3: S3 is not wired to the PIC32MZ EF plug-in module");
    return -1;
}

static void end(void *ctx)
{
    struct explorer *x = ctx;
    x->lcd.drawn = 1;
    show(x);
}

static void explorer_reset(void *ctx)
{
    struct explorer *x = ctx;
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

const struct dev_ops explorer_ops = {
    "explorer16-32", 0, 0, explorer_reset, 0, 0,
};

void *explorer_create(struct sim *s, const struct dev_desc *d)
{
    (void)d;
    struct explorer *x = calloc(1, sizeof *x);
    if (!x)
        die("out of memory");
    x->s = s;
    x->temperature = 25.0;
    x->pot = 1.65;
    explorer_reset(x);
    s->io.ctx = x;
    s->io.pin_out = pin_out;
    s->io.pin_in = pin_in;
    s->io.analog = analog;
    s->io.bus_write = pmp_write;
    s->io.bus_read = pmp_read;
    s->io.set = set;
    s->io.end = end;
    return x;
}
