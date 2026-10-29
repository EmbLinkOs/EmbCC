// expect-exit: 42
/* Loops whose trip count is not known, unrolled with every exit kept
 * (src/opt/unroll.c, unrx_one) when their bodies are short (the longer
 * ones here check the loops left alone): each copy keeps its exits, the carried
 * values are renamed from copy to copy, and an exit from the middle goes
 * through a stub that moves them back. What can go wrong is which copy's
 * value an exit sees, so every loop below is left from each position in
 * the unrolled body -- after 0, 1, 2, 3, 4 and more iterations -- and
 * what it leaves behind is checked, not only what it returns. */

struct node { struct node *next; int key; int val; };

static struct node pool[16];

/* the list walk: a side exit and the test at the bottom, two carried
 * values (the link and the node) */
__attribute__((noinline))
static struct node **find(struct node **pp, int key)
{
    while (*pp && (*pp)->key < key)
        pp = &(*pp)->next;
    return pp;
}

/* a scan for a terminator, no side exit: the length is how far it got */
__attribute__((noinline))
static int len(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (int)(p - s);
}

/* two exits to two places, and three carried values, one of them read
 * only after the loop */
__attribute__((noinline))
static int search(const int *a, int stop, int *pos)
{
    int i = 0, last = -1;
    const int *p = a;
    for (;;) {
        if (*p == stop) {
            *pos = i;
            return last;
        }
        if (*p < 0)
            break;
        last = *p;
        p++;
        i++;
    }
    *pos = -i - 1;
    return last;
}

/* the carried value updated in the middle, an exit on each side of it */
__attribute__((noinline))
static unsigned hash_until(const unsigned char *s, unsigned char stop)
{
    unsigned h = 5381;
    for (;;) {
        unsigned char c = *s;
        if (c == stop)
            return h;
        h = h * 33 + c;
        if (h & 0x80000000u)
            return h ^ 1;
        s++;
        if (!c)
            return h + 7;
    }
}

/* strchr: two returns laid out in the loop's middle, moved out of it
 * first, and `for (;;)`'s jump back */
__attribute__((noinline))
static const char *chr(const char *p, int c)
{
    for (;;) {
        if (*p == c)
            return p;
        if (!*p)
            return 0;
        p++;
    }
}

/* a value computed once in the body and read by the exit after it */
__attribute__((noinline))
static int first_over(const int *a, int lim)
{
    for (;;) {
        int v = *a++ * 3;
        if (v > lim)
            return v;
    }
}

int main(void)
{
    /* a sorted list 0, 3, 6, ..., 45 */
    struct node *head = 0, **tail = &head;
    for (int i = 0; i < 16; i++) {
        pool[i].key = 3 * i;
        pool[i].val = i;
        pool[i].next = 0;
        *tail = &pool[i];
        tail = &pool[i].next;
    }
    for (int key = -1; key <= 50; key++) {
        struct node **pp = find(&head, key);
        int want = key <= 0 ? 0 : (key + 2) / 3;     /* first key >= key */
        if (want >= 16) {
            if (*pp || pp != &pool[15].next)
                return 1;
        } else {
            if (*pp != &pool[want])
                return 2;
            if (want == 0 ? pp != &head : pp != &pool[want - 1].next)
                return 3;
        }
    }

    static const char hay[] = "abcdefghijklm";
    for (int c = 'a'; c <= 'n'; c++) {
        const char *want = c <= 'm' ? hay + (c - 'a') : 0;
        if (chr(hay, c) != want)
            return 7;
    }
    if (chr(hay, 0) != hay + 13)
        return 8;
    static const int vals[10] = { 1, 4, 2, 8, 5, 7, 3, 9, 6, 100 };
    static const int want_v[8] = { 12, 12, 12, 24, 24, 24, 300, 300 };
    for (int lim = 3; lim <= 31; lim += 4)
        if (first_over(vals, lim) != want_v[(lim - 3) / 4])
            return 9;

    static const char *strs[] = { "", "a", "ab", "abc", "abcd", "abcde",
                                  "abcdef", "abcdefg", "abcdefgh",
                                  "abcdefghi", "abcdefghijklmnopq" };
    for (int k = 0; k < 11; k++)
        if (len(strs[k]) != (k < 10 ? k : 17))
            return 4;

    /* what search returns and stores, worked out on the host: an
     * EmbCC-compiled loop here would be unrolled the same way */
    static const int arr[12] = { 5, 8, 1, 9, 4, 7, 2, 6, 3, 11, 10, -1 };
    static const int want_s[13][2] = {
        { 10, -12 }, { 8, 2 }, { 7, 6 }, { 6, 8 }, { 9, 4 }, { -1, 0 },
        { 2, 7 }, { 4, 5 }, { 5, 1 }, { 1, 3 }, { 11, 10 }, { 3, 9 },
        { 10, -12 } };
    for (int stop = 0; stop <= 12; stop++) {
        int pos = 99;
        if (search(arr, stop, &pos) != want_s[stop][0] ||
            pos != want_s[stop][1])
            return 5;
    }

    static const unsigned char txt[] = "unrolled loops keep their exits";
    static const unsigned want_h[26] = {
        0x9b51792bu, 0x9b51792bu, 0x9b51792bu, 0x3b027806u, 0x20d18781u,
        0x9b51792bu, 0x9b51792bu, 0x9b51792bu, 0x9b51792bu, 0x9b51792bu,
        0x9b51792bu, 0x7c9f1ae9u, 0x9b51792bu, 0x0002b61au, 0x0b88b33au,
        0x9b51792bu, 0x9b51792bu, 0x005979c8u, 0x9b51792bu, 0x9b51792bu,
        0x00001505u, 0x9b51792bu, 0x9b51792bu, 0x9b51792bu, 0x9b51792bu,
        0x9b51792bu };
    for (int stop = 'a'; stop <= 'z'; stop++)
        if (hash_until(txt, (unsigned char)stop) != want_h[stop - 'a'])
            return 6;
    return 42;
}
