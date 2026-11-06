/* A copy block entered from two branches in a row (src/opt/latch.c):
 * `if (alpha && !in_word) words++;` leaves `brz alpha -> L; brnz in_word
 * -> L; words = words0 + 1` with L the copy `words = words0` and a jump
 * back. The copy goes above the first branch and both branches go on to
 * where L went. Each counter here is checked against the same count made
 * without a branch, over text that takes every path: both conditions,
 * either, neither, at the start and the end of a word, and through the
 * loop's last character. Also the other polarities (`a || b`, `a && b`),
 * a counter the fall-through arm reads before it writes it, two counters
 * in one copy block, and a 64-bit one. */
// expect-exit: 42
typedef unsigned int u32;
typedef unsigned long long u64;

static const char *const texts[] = {
    "", "a", "A", "ab", "a b", " a", "a ", "  ", "Hello, World 42!",
    "the quick brown fox jumps over 13 lazy dogs",
    "x1y2z3 4w 55 abc DEF ghi--jkl  mno", "ZZ top 99 zz", "...a..b..c",
    "aBcDeFg HiJk lMnOp", "1 2 3 4 5 6 7 8 9", "end.",
};
#define NT ((int)(sizeof texts / sizeof texts[0]))

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

__attribute__((noinline)) u32 words_br(const char *p)
{
    u32 words = 0;
    int in_word = 0;
    for (; *p; p++) {
        int alpha = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        if (alpha && !in_word)
            words++;
        in_word = alpha;
    }
    return words;
}

__attribute__((noinline)) u32 words_ref(const char *p)
{
    u32 words = 0;
    int prev = 0;
    for (; *p; p++) {
        int a = is_alpha(*p);
        words += (u32)(a & (prev ^ 1));
        prev = a;
    }
    return words;
}

/* a || b: the copy block is reached when both are false */
__attribute__((noinline)) u32 either_br(const char *p)
{
    u32 n = 0;
    for (; *p; p++) {
        if (*p == ' ' || *p == '1')
            n += 3;
    }
    return n;
}

__attribute__((noinline)) u32 either_ref(const char *p)
{
    u32 n = 0;
    for (; *p; p++)
        n += 3u * (u32)((*p == ' ') | (*p == '1'));
    return n;
}

/* a && b, two counters, one updated on each side */
__attribute__((noinline)) u32 both_br(const char *p, u32 *other)
{
    u32 n = 0, m = 0;
    int prev = 0;
    for (; *p; p++) {
        int d = *p >= '0' && *p <= '9';
        if (d && prev)
            n++;
        else
            m += 2;
        prev = d;
    }
    *other = m;
    return n;
}

__attribute__((noinline)) u32 both_ref(const char *p, u32 *other)
{
    u32 n = 0, m = 0;
    int prev = 0;
    for (; *p; p++) {
        int d = *p >= '0' && *p <= '9';
        u32 t = (u32)(d & prev);
        n += t;
        m += 2u * (t ^ 1u);
        prev = d;
    }
    *other = m;
    return n;
}

/* the arm reads the counter before writing it, and a 64-bit count */
__attribute__((noinline)) u64 wide_br(const char *p)
{
    u64 n = 0x100000000ull;
    int up = 0;
    for (; *p; p++) {
        int u = *p >= 'A' && *p <= 'Z';
        if (u && !up)
            n = n * 2 + 1;
        up = u;
    }
    return n;
}

__attribute__((noinline)) u64 wide_ref(const char *p)
{
    u64 n = 0x100000000ull;
    int up = 0;
    for (; *p; p++) {
        int u = *p >= 'A' && *p <= 'Z';
        u64 t = (u64)(u & (up ^ 1));
        n = n + t * (n + 1);
        up = u;
    }
    return n;
}

int main(void)
{
    int bad = 0;
    for (int i = 0; i < NT; i++) {
        u32 o1, o2;
        if (words_br(texts[i]) != words_ref(texts[i]))
            bad |= 1;
        if (either_br(texts[i]) != either_ref(texts[i]))
            bad |= 2;
        if (both_br(texts[i], &o1) != both_ref(texts[i], &o2) || o1 != o2)
            bad |= 4;
        if (wide_br(texts[i]) != wide_ref(texts[i]))
            bad |= 8;
    }
    /* the counts themselves, so the reference cannot be wrong alike */
    if (words_br("Hello, World 42!") != 2 || words_br("a b c") != 3)
        bad |= 16;
    return bad ? bad : 42;
}
