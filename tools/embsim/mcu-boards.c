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
 *     select let go (rp2040-io.c). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"

struct mled { int port, bit; const char *name, *pin; int active_low; };
struct mbutton { int port, bit; const char *name; int pull; };
/* a pin the board drives, whatever the program does */
struct mfixed { int port, bit, level; };

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
};

static const struct mled pico_leds[] = { { 0, 25, "LED", "GP25", 0 } };
static const struct mbutton pico_buttons[] = { { 1, 1, "BOOTSEL", 1 } };
static const struct mfixed pico_fixed[] = { { 0, 24, 1 } };

#define N(a) (int)(sizeof a / sizeof a[0])
static const struct mboard mboards[] = {
    { "pico", pico_leds, N(pico_leds), pico_buttons, N(pico_buttons),
      pico_fixed, N(pico_fixed), 0, rp2040_pins_changed },
};

#define MAXLED 8
#define MAXBUTTON 8

struct mstate {
    struct sim *s;
    const struct mboard *b;
    int led[MAXLED];
    int known[MAXLED];
    int pressed[MAXBUTTON];
};

static double now_ms(const struct mstate *x)
{
    struct sim *s = x->s;
    if (s->now_ns)
        return s->now_ns(s) / 1e6;
    return x->b->hz ? (double)s->cycles / (x->b->hz / 1000.0) : 0;
}

static void pin_out(void *ctx, int port, int bit, int level)
{
    struct mstate *x = ctx;
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
