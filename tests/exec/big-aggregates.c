/* Aggregates far larger than an immediate offset reaches: zero-filled by
 * their initializer and copied whole, at sizes that are and are not a
 * multiple of the word, on every target. */
// expect-exit: 42
#define NI __attribute__((noinline))
struct big { long long v[300]; char tail[3]; };          /* 2403 bytes */
struct huge { int v[2000]; };                             /* 8000 bytes */
#if __SIZEOF_POINTER__ == 8
/* 40003 bytes: past aarch64's 32760, and more RAM than the 32-bit boards
 * the same source runs on have to spare */
struct giant { int v[10000]; char t[3]; };
static struct giant ga, gb;
NI int copy_giant(struct giant *d, const struct giant *s) { *d = *s; return d->v[9999] + d->v[0] + d->t[2]; }
NI void zero_giant(struct giant *d) { *d = (struct giant){ { 0 } }; }
#endif
NI long long sum_big(const struct big *b) { long long s = 0; for (int i = 0; i < 300; i++) s += b->v[i]; return s + b->tail[0] + b->tail[2]; }
NI struct big make(int k) { struct big b = { { 0 } }; b.v[k] = k; b.tail[2] = 9; return b; }
NI int copy_huge(struct huge *d, const struct huge *s) { *d = *s; return d->v[1999] + d->v[0]; }
int main(void)
{
    long long ll[300] = { 0 };
    for (int i = 0; i < 300; i++) if (ll[i]) return 1;
    struct big b = { { 0 } };
    b.v[299] = 5; b.tail[2] = 7;
    if (sum_big(&b) != 12) return 2;
    struct big c = make(250);
    if (sum_big(&c) != 259) return 3;
    struct big d = c;                     /* a 2403-byte copy */
    d.v[0] = 1;
    if (sum_big(&d) != 260 || sum_big(&c) != 259) return 4;
    static struct huge h1, h2;
    h1.v[0] = 3; h1.v[1999] = 4;
    if (copy_huge(&h2, &h1) != 7) return 5;
    struct odd { char c[2049]; };          /* 2049: the tail byte */
    struct odd o1 = { { 0 } }, o2;
    o1.c[2048] = 1;
    for (int i = 0; i < 2049; i++) o2.c[i] = 9;
    o2 = o1;                              /* a 2049-byte copy */
    if (o2.c[2048] != 1 || o2.c[2047] != 0 || o2.c[0] != 0) return 6;
#if __SIZEOF_POINTER__ == 8
    ga.v[0] = 1; ga.v[9999] = 2; ga.t[2] = 3; ga.v[5000] = 7;
    if (copy_giant(&gb, &ga) != 6 || gb.v[5000] != 7) return 7;
    zero_giant(&gb);
    if (gb.v[9999] || gb.v[0] || gb.t[2] || gb.v[5000]) return 8;
#endif
    return 42;
}
