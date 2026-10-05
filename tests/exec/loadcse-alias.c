/* Redundant loads and dead stores, told apart by base and offset.
 *
 * Value numbering, load CSE and dead-store elimination keep a remembered
 * load across a store when both are a constant offset from the SAME base
 * value and their byte ranges do not meet: q->wr = ... cannot change
 * q->len. Every function below is a case where the ranges DO meet, or
 * where the bases look alike and are not the same value, so a version of
 * that rule that keeps a load (or deletes a store) it must not gets a
 * different answer:
 *
 *  - a store to one byte, or to half, of a word that was just loaded;
 *  - a store through another pointer that points into the same object;
 *  - a store through base + offset whose base is a DIFFERENT value that
 *    happens to reach the same bytes, and the same base reached by a
 *    subtract and by an add of a negative offset;
 *  - one pointer name that takes a new value on every loop trip;
 *  - the same across a branch, where it is load CSE that remembers;
 *  - a store that a later, narrower store does not fully overwrite, and
 *    a store a read through another pointer still sees.
 *
 * Bytes are written through unsigned char and halves through a union, so
 * the program means the same thing to every compiler; the expected
 * values do not depend on byte order. */
// expect-exit: 42

union word { unsigned w; unsigned short h[2]; unsigned char b[4]; };

struct rec { unsigned a, b, c; union word u; };

/* A byte inside the remembered word. */
__attribute__((noinline)) static unsigned byte_in_word(unsigned *p)
{
    unsigned x = *p;
    ((unsigned char *)p)[1] = 0xff;
    return *p - x;                  /* not 0: the word changed */
}

/* Half of it, through the union: the same base, offsets 0 and 2. */
__attribute__((noinline)) static unsigned half_in_word(union word *u)
{
    unsigned x = u->w;
    u->h[1] = 0x1234;
    return u->w - x;
}

/* A word over a remembered byte. */
__attribute__((noinline)) static unsigned word_over_byte(union word *u)
{
    unsigned x = u->b[3];
    u->w = 0x01010101u;
    return u->b[3] + x;             /* 1 + the old byte */
}

/* Another pointer into the same object. */
__attribute__((noinline)) static unsigned other_ptr(unsigned *p, unsigned *q)
{
    unsigned x = p[1];
    *q = 7;
    return p[1] * 10 + x;
}

/* p - 1 + 2 is p + 1: the walk to the base must do the arithmetic,
 * negative offset included, and find the same bytes. */
__attribute__((noinline)) static unsigned back_one(unsigned *p)
{
    unsigned *m = p - 1;
    unsigned x = p[1];
    m[2] = 9;
    return p[1] * 10 + x;
}

/* m = p - 1 is a subtract and p[-1] an add of -4: the walk must give
 * the two the same sign to see the same bytes. */
__attribute__((noinline)) static unsigned minus_one(unsigned *p)
{
    unsigned *m = p - 1;
    unsigned x = *m;
    p[-1] = 5;
    return *m * 10 + x;
}

/* A different base VALUE that reaches the same bytes: m is p plus a
 * number the compiler cannot see. Offsets alone (4 against 8) would call
 * the two disjoint. */
__attribute__((noinline)) static unsigned other_base(unsigned *p, long k)
{
    unsigned *m = p + k;
    unsigned x = p[1];
    m[2] = 9;
    return p[1] * 10 + x;
}

/* One name, a new value each trip: what p[1] was written as is the next
 * trip's p[0]. */
__attribute__((noinline)) static unsigned chain(unsigned *p, int n)
{
    unsigned s = 0;
    for (int i = 0; i < n; i++) {
        unsigned x = p[0];
        p[1] = x * 2;               /* disjoint from p[0] now ... */
        p = p + 1;                  /* ... and exactly the next p[0] */
        s += x;
    }
    return s;
}

/* Through a struct: c is at 8, and u at 12 is not touched by a store to
 * a; a store to the third byte of u is. */
__attribute__((noinline)) static unsigned in_struct(struct rec *r)
{
    unsigned c = r->c, w = r->u.w;
    r->a = 100;                     /* disjoint from c and u */
    r->u.b[2] = 0;                  /* inside u */
    return (r->c - c) + (r->u.w != w) * 3;  /* 0 + 3 */
}

/* Across a branch, where the reload is load CSE's to remove or keep. */
__attribute__((noinline)) static unsigned across(union word *u, int k)
{
    unsigned x = u->w;
    if (k)
        u->b[0] = 0x55;
    else
        u->h[1] = 0x55;
    return u->w != x;
}

/* Dead stores: a word store survives a later byte store that covers
 * only part of it, and a byte store survives a later store to the byte
 * beside it. */
__attribute__((noinline)) static void partial_over(union word *u)
{
    u->w = 0x44444444u;             /* the word ... */
    u->b[0] = 0x11;                 /* ... only partly overwritten */
    u->b[1] = 0x22;                 /* not covered by b[2] */
    u->b[2] = 0x33;
}

/* A store read in between through another pointer is not dead. */
__attribute__((noinline)) static unsigned read_between(unsigned *p, unsigned *q)
{
    p[1] = 5;
    unsigned x = q[0];              /* q == p + 1: reads the 5 */
    p[1] = 6;
    return x;
}

int main(void)
{
    union word w;
    unsigned arr[4];
    struct rec r;
    int bad = 0;

    w.w = 0;
    if (byte_in_word(&w.w) == 0) bad |= 1;
    w.w = 0;
    if (half_in_word(&w) == 0) bad |= 2;
    w.w = 0x02020202u;
    if (word_over_byte(&w) != 3) bad |= 4;

    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;
    if (other_ptr(arr, &arr[1]) != 72) bad |= 8;
    /* from arr + 1, so that p - 1 is still inside arr */
    arr[2] = 2;
    if (back_one(&arr[1]) != 92 || arr[2] != 9) bad |= 16;
    arr[2] = 2;
    if (other_base(&arr[1], -1) != 92 || arr[2] != 9) bad |= 16;
    arr[0] = 2;
    if (minus_one(&arr[1]) != 52 || arr[0] != 5) bad |= 16;
    arr[0] = 1; arr[1] = 0; arr[2] = 0; arr[3] = 0;
    if (chain(arr, 3) != 7 || arr[3] != 8) bad |= 16;

    r.a = 1; r.b = 2; r.c = 3; r.u.w = 0xffffffffu;
    if (in_struct(&r) != 3 || r.a != 100) bad |= 32;

    w.w = 0;
    if (across(&w, 1) != 1) bad |= 64;
    w.w = 0;
    if (across(&w, 0) != 1) bad |= 128;

    w.w = 0;
    partial_over(&w);
    if (w.b[0] != 0x11 || w.b[1] != 0x22 || w.b[2] != 0x33 || w.b[3] != 0x44)
        bad |= 256;

    arr[1] = 0;
    if (read_between(arr, &arr[1]) != 5 || arr[1] != 6) bad |= 512;

    return bad ? bad & 0x7f ? bad & 0x7f : 1 : 42;
}
