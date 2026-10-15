/* mcu-boards.c -- what the boards around EmbSim's microcontrollers wire to
 * their pins (struct board_io): LEDs and buttons, each board a table.
 *
 * The part's GPIO model tells the board each pin's level as it changes
 * (pin_out: port and bit as the model numbers them) and asks it whether
 * it drives a pin (pin_in). An LED that goes on or off is a line on
 * stderr, with the time: the part's clock model's (s->now_ns), or the
 * core's cycles at the board's clock. A button is pressed or released by
 * --set NAME=pressed|released; released, it is not driven (the board's
 * pull-up, if it has one, is the pull field: it then reads high).
 *
 * The boards:
 *   - pico: the Raspberry Pi Pico (its datasheet, "Raspberry Pi Pico
 *     Datasheet", 2.1 and 4.1): the LED on GP25, lit high; GP24 is VBUS
 *     sense, high (the board on USB); the BOOTSEL button pulls the QSPI
 *     chip select low (port 1, bit 1), which the program reads with the
 *     select let go (rp2040-io.c);
 *   - bluepill: the STM32F103C8 board sold as the "Blue Pill": its LED on
 *     PC13, lit low (to 3.3 V through a resistor); no user button;
 *   - nrf52840dk: Nordic's nRF52840 DK (its pins as Zephyr's board file
 *     has them): LED1-4 on P0.13-16, lit low; BUTTON1-4 on P0.11, P0.12,
 *     P0.24, P0.25, low when pressed, with no pull-up on the board (the
 *     program enables the pin's own);
 *   - esp32c3-devkitm-1: Espressif's ESP32-C3-DevKitM-1 (its schematic, V1):
 *     the BOOT button on GPIO9, low when pressed, its pull-up R14 not
 *     fitted (GPIO9's own is on out of reset); and the RGB LED, an
 *     SK68XXMINI-HS on GPIO8, an addressable LED and not one a level
 *     lights. It takes 24 bits, green, red, blue, most significant first,
 *     each a high pulse -- 0.3 us a 0, 0.6 us a 1, each +-0.15 us (the
 *     SK6812MINI's datasheet) -- and shows them after 80 us of low; later
 *     bits are its DOUT's, for the next LED, which this board does not
 *     have. A high under 0.45 us is read as a 0, a longer one a 1; one
 *     outside 0.15 to 0.75 us is warned of, once. The colour is reported
 *     as its 24th bit ends: "RGB LED #rrggbb", or "RGB LED off". */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"

struct mled { int port, bit; const char *name, *pin; int active_low; };
struct mbutton { int port, bit; const char *name; int pull; };
/* a pin the board drives, whatever the program does */
struct mfixed { int port, bit, level; };

/* an SK6812-style addressable LED on a pin */
struct mrgb { int port, bit; const char *name, *pin; };

struct mboard {
    const char *name;
    const struct mled *led;
    int nled;
    const struct mbutton *button;
    int nbutton;
    const struct mfixed *fixed;
    int nfixed;
    double hz;                      /* the core's clock, without s->now_ns */
    void (*changed)(struct sim *s); /* the part's pins, after an input */
    const struct mrgb *rgb;
};

static const struct mled pico_leds[] = { { 0, 25, "LED", "GP25", 0 } };
static const struct mbutton pico_buttons[] = { { 1, 1, "BOOTSEL", 1 } };
static const struct mfixed pico_fixed[] = { { 0, 24, 1 } };
static const struct mled bluepill_leds[] = { { 2, 13, "LED", "PC13", 1 } };
static const struct mled dk_leds[] = {
    { 0, 13, "LED1", "P0.13", 1 }, { 0, 14, "LED2", "P0.14", 1 },
    { 0, 15, "LED3", "P0.15", 1 }, { 0, 16, "LED4", "P0.16", 1 },
};
static const struct mbutton dk_buttons[] = {
    { 0, 11, "BUTTON1", 0 }, { 0, 12, "BUTTON2", 0 },
    { 0, 24, "BUTTON3", 0 }, { 0, 25, "BUTTON4", 0 },
};

static const struct mbutton devkitm_buttons[] = { { 0, 9, "BOOT", 0 } };
static const struct mrgb devkitm_rgb = { 0, 8, "RGB LED", "GPIO8" };

#define N(a) (int)(sizeof a / sizeof a[0])
static const struct mboard mboards[] = {
    { "pico", pico_leds, N(pico_leds), pico_buttons, N(pico_buttons),
      pico_fixed, N(pico_fixed), 0, rp2040_pins_changed, 0 },
    { "bluepill", bluepill_leds, N(bluepill_leds), 0, 0, 0, 0, 0, stm32f1_pins_changed, 0 },
    { "nrf52840dk", dk_leds, N(dk_leds), dk_buttons, N(dk_buttons), 0, 0, 0,
      nrf_pins_changed, 0 },
    { "esp32c3-devkitm-1", 0, 0, devkitm_buttons, N(devkitm_buttons), 0, 0, 0,
      esp32c3_pins_changed, &devkitm_rgb },
};

#define MAXLED 8
#define MAXBUTTON 8

struct mstate {
    struct sim *s;
    const struct mboard *b;
    int led[MAXLED];
    int known[MAXLED];
    int pressed[MAXBUTTON];
    /* the addressable LED: its line, since when, the bits of the frame
     * (24 at most), the colour shown */
    int rgb_level, rgb_n, rgb_warned;
    double rgb_edge;
    u32 rgb_bits, rgb_shown;
};

static double now_ms(const struct mstate *x)
{
    struct sim *s = x->s;
    if (s->now_ns)
        return s->now_ns(s) / 1e6;
    return x->b->hz ? (double)s->cycles / (x->b->hz / 1000.0) : 0;
}

/* the addressable LED's data line changed: a bit as each high ends */
static void rgb_line(struct mstate *x, int high)
{
    const struct mrgb *r = x->b->rgb;
    struct sim *s = x->s;
    double t = s->now_ns ? s->now_ns(s) : 0, d = t - x->rgb_edge;
    if (high == x->rgb_level)
        return;
    x->rgb_level = high;
    x->rgb_edge = t;
    if (high) {
        if (d >= 80000)             /* the reset: the next bits are its own */
            x->rgb_n = 0;
        return;
    }
    if ((d < 150 || d > 750) && !x->rgb_warned) {
        x->rgb_warned = 1;
        fflush(stdout);
        fprintf(stderr, "%s: %s (%s): a high of %.3f us is neither a 0 "
                "(0.3 +- 0.15 us) nor a 1 (0.6 +- 0.15 us); taken as a %d\n",
                x->b->name, r->name, r->pin, d / 1000, d >= 450);
    }
    if (x->rgb_n >= 24)
        return;                     /* DOUT's: the next LED's */
    x->rgb_bits = x->rgb_bits << 1 | (d >= 450);
    if (++x->rgb_n < 24)
        return;
    /* green, red, blue: as #rrggbb */
    u32 g = x->rgb_bits >> 16 & 0xff, red = x->rgb_bits >> 8 & 0xff, b = x->rgb_bits & 0xff;
    u32 rgb = red << 16 | g << 8 | b;
    if (rgb == x->rgb_shown)
        return;
    x->rgb_shown = rgb;
    fflush(stdout);
    if (rgb)
        fprintf(stderr, "%s: %s #%06x (%s) at %.3f ms\n", x->b->name, r->name,
                (unsigned)rgb, r->pin, t / 1e6);
    else
        fprintf(stderr, "%s: %s off (%s) at %.3f ms\n", x->b->name, r->name,
                r->pin, t / 1e6);
}

static void pin_out(void *ctx, int port, int bit, int level)
{
    struct mstate *x = ctx;
    if (x->b->rgb && x->b->rgb->port == port && x->b->rgb->bit == bit)
        rgb_line(x, level == 1);
    for (int i = 0; i < x->b->nled; i++) {
        const struct mled *l = &x->b->led[i];
        if (l->port != port || l->bit != bit)
            continue;
        int on = l->active_low ? level == 0 : level == 1;
        if (x->known[i] && x->led[i] == on)
            continue;
        int first = !x->known[i];
        x->known[i] = 1;
        x->led[i] = on;
        if (first && !on)
            continue;               /* off from the start: nothing to say */
        fflush(stdout);
        fprintf(stderr, "%s: %s %s (%s) at %.3f ms\n", x->b->name, l->name,
                on ? "on" : "off", l->pin, now_ms(x));
    }
}

static int pin_in(void *ctx, int port, int bit, int *level)
{
    struct mstate *x = ctx;
    for (int i = 0; i < x->b->nfixed; i++)
        if (x->b->fixed[i].port == port && x->b->fixed[i].bit == bit) {
            *level = x->b->fixed[i].level;
            return 1;
        }
    for (int i = 0; i < x->b->nbutton; i++) {
        const struct mbutton *b = &x->b->button[i];
        if (b->port == port && b->bit == bit && (x->pressed[i] || b->pull)) {
            *level = !x->pressed[i];
            return 1;
        }
    }
    return 0;
}

static int set(void *ctx, const char *name, const char *value)
{
    struct mstate *x = ctx;
    for (int i = 0; i < x->b->nbutton; i++)
        if (!strcmp(x->b->button[i].name, name)) {
            if (!strcmp(value, "pressed"))
                x->pressed[i] = 1;
            else if (!strcmp(value, "released"))
                x->pressed[i] = 0;
            else
                die("--set %s=%s: a button is pressed or released", name, value);
            if (x->b->changed)
                x->b->changed(x->s);
            return 0;
        }
    return -1;
}

static void board_reset(void *ctx)
{
    struct mstate *x = ctx;
    memset(x->known, 0, sizeof x->known);
    x->rgb_level = x->rgb_n = 0;
    x->rgb_edge = 0;
    x->rgb_bits = x->rgb_shown = 0;
}

const struct dev_ops mcu_board_ops = { "mcu-board", 0, 0, board_reset, 0, 0 };

void *mcu_board_create(struct sim *s, const struct dev_desc *d)
{
    const struct mboard *b = 0;
    for (int i = 0; i < N(mboards); i++)
        if (!strcmp(mboards[i].name, d->type))
            b = &mboards[i];
    if (!b)
        die("no board named %s", d->type);
    struct mstate *x = calloc(1, sizeof *x);
    if (!x)
        die("out of memory");
    x->s = s;
    x->b = b;
    s->io.ctx = x;
    s->io.pin_out = pin_out;
    s->io.pin_in = pin_in;
    s->io.set = set;
    return x;
}
