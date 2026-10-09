/* State machines whose switch the optimizer threads: an arm that sets
 * the state to a constant jumps straight to that state's case, through a
 * copy of the loop's own latch and exit tests (src/opt/swthread.c).
 * Each machine is checked against the same machine written with if/else
 * and no switch, over inputs that visit every transition -- the
 * CoreMark tokenizer; cases that start at 10 (so the operand is
 * `state - 10`); arms whose next state is computed, not constant; a
 * 64-bit state; negative states; two cases sharing one arm; a state
 * out of every case's range (the default) and back; the state read after
 * the loop; `continue` and `break` out of the middle; and a machine in a
 * loop inside another loop. */
// expect-exit: 42
typedef unsigned int u32;

static const char *const words[] = {
    "5012", "1234", "-874", "+122", "35.54", "-.123", "+0.6", "-110.700",
    "5.500e+3", "-.123e-2", "-87e+832", "+0.6e-12", "T0.3e-1F", "-T.T++Tq",
    "1T3.4e4z", "34.0e-T^", "", "+", "-", ".", "e", "1e", "1.e+", "..",
    "+-1", "0.0e-0", "9", "99999999999", "1.2.3", "1e+2e+3",
};
#define NWORDS ((int)(sizeof words / sizeof words[0]))

/* ---- 1. CoreMark's tokenizer --------------------------------------- */
enum { S_START, S_INT, S_S1, S_FLOAT, S_S2, S_EXP, S_INVALID };
static int digit(char c) { return c >= '0' && c <= '9'; }

__attribute__((noinline)) int tok_switch(const char *p, u32 *count)
{
    int st = S_START;
    for (; *p && *p != ','; p++) {
        char c = *p;
        switch (st) {
        case S_START:
            if (digit(c)) st = S_INT;
            else if (c == '+' || c == '-') st = S_S1;
            else if (c == '.') st = S_FLOAT;
            else { st = S_INVALID; count[S_INVALID]++; }
            break;
        case S_S1:
            if (digit(c)) st = S_INT;
            else if (c == '.') st = S_FLOAT;
            else st = S_INVALID;
            break;
        case S_INT:
            if (c == '.') st = S_FLOAT;
            else if (!digit(c)) st = S_INVALID;
            break;
        case S_FLOAT:
            if (c == 'E' || c == 'e') st = S_S2;
            else if (!digit(c)) st = S_INVALID;
            break;
        case S_S2:
            if (c == '+' || c == '-') st = S_EXP;
            else st = S_INVALID;
            break;
        case S_EXP:
            if (!digit(c)) st = S_INVALID;
            break;
        default:
            break;
        }
    }
    count[st]++;
    return st;
}

__attribute__((noinline)) int tok_ref(const char *p, u32 *count)
{
    int st = S_START;
    for (; *p && *p != ','; p++) {
        char c = *p;
        if (st == S_START) {
            if (digit(c)) st = S_INT;
            else if (c == '+' || c == '-') st = S_S1;
            else if (c == '.') st = S_FLOAT;
            else { st = S_INVALID; count[S_INVALID]++; }
        } else if (st == S_S1) {
            if (digit(c)) st = S_INT;
            else if (c == '.') st = S_FLOAT;
            else st = S_INVALID;
        } else if (st == S_INT) {
            if (c == '.') st = S_FLOAT;
            else if (!digit(c)) st = S_INVALID;
        } else if (st == S_FLOAT) {
            if (c == 'E' || c == 'e') st = S_S2;
            else if (!digit(c)) st = S_INVALID;
        } else if (st == S_S2) {
            if (c == '+' || c == '-') st = S_EXP;
            else st = S_INVALID;
        } else if (st == S_EXP) {
            if (!digit(c)) st = S_INVALID;
        }
    }
    count[st]++;
    return st;
}

/* ---- 2. cases from 10, a computed arm, a shared arm, a default ------- */
__attribute__((noinline)) long walk_switch(const unsigned char *s, int n)
{
    int st = 10;
    long acc = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        switch (st) {
        case 10: st = c & 1 ? 11 : 12; acc += 1; break;
        case 11: st = 10 + (c & 3); acc += 3; break;      /* computed */
        case 12: case 13: st = c > 128 ? 14 : 10; acc += 7; break;
        case 14: st = 99; acc += 11; break;               /* to the default */
        default: st = c & 4 ? 13 : 10; acc += 13; break;
        }
        acc = acc * 3 + st;
    }
    return acc * 31 + st;
}

__attribute__((noinline)) long walk_ref(const unsigned char *s, int n)
{
    int st = 10;
    long acc = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (st == 10) { st = c & 1 ? 11 : 12; acc += 1; }
        else if (st == 11) { st = 10 + (c & 3); acc += 3; }
        else if (st == 12 || st == 13) { st = c > 128 ? 14 : 10; acc += 7; }
        else if (st == 14) { st = 99; acc += 11; }
        else { st = c & 4 ? 13 : 10; acc += 13; }
        acc = acc * 3 + st;
    }
    return acc * 31 + st;
}

/* ---- 3. a 64-bit state, negative states, continue and break ---------- */
__attribute__((noinline)) long long wide_switch(const unsigned char *s, int n)
{
    long long st = -2;
    long long acc = 0;
    int i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        if (c == 0x7f)
            break;                                  /* out of the middle */
        switch (st) {
        case -2: st = c < 64 ? -1 : 0; break;
        case -1: st = 1; if (c & 8) continue; break;   /* skips the add */
        case 0: st = c & 16 ? -2 : 2; break;
        case 1: st = c & 2 ? 0 : -1; break;
        case 2: st = 0x100000000LL; break;            /* past 32 bits: default */
        default: st = -2; acc ^= 0x5555; break;
        }
        acc = acc * 5 + st;
    }
    return acc + st * 7;
}

__attribute__((noinline)) long long wide_ref(const unsigned char *s, int n)
{
    long long st = -2;
    long long acc = 0;
    int i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        if (c == 0x7f)
            break;
        if (st == -2) st = c < 64 ? -1 : 0;
        else if (st == -1) { st = 1; if (c & 8) continue; }
        else if (st == 0) st = c & 16 ? -2 : 2;
        else if (st == 1) st = c & 2 ? 0 : -1;
        else if (st == 2) st = 0x100000000LL;
        else { st = -2; acc ^= 0x5555; }
        acc = acc * 5 + st;
    }
    return acc + st * 7;
}

/* ---- 4. a machine in a loop in a loop (the workload's shape) --------- */
__attribute__((noinline)) u32 nested(const char *buf, int rounds)
{
    u32 acc = 0, count[8];
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 8; i++) count[i] = 0;
        const char *p = buf;
        while (*p) {
            acc = acc * 3u + (u32)tok_switch(p, count);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
        for (int i = 0; i < 8; i++) acc += count[i] << i;
    }
    return acc;
}

__attribute__((noinline)) u32 nested_ref(const char *buf, int rounds)
{
    u32 acc = 0, count[8];
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 8; i++) count[i] = 0;
        const char *p = buf;
        while (*p) {
            acc = acc * 3u + (u32)tok_ref(p, count);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
        for (int i = 0; i < 8; i++) acc += count[i] << i;
    }
    return acc;
}

static char buf[1024];
static unsigned char bytes[600];

int main(void)
{
    int bad = 0;
    u32 c1[8] = { 0 }, c2[8] = { 0 };
    for (int k = 0; k < NWORDS; k++)
        if (tok_switch(words[k], c1) != tok_ref(words[k], c2))
            bad |= 1;
    for (int i = 0; i < 8; i++)
        if (c1[i] != c2[i])
            bad |= 2;

    int n = 0;
    for (int k = 0; n < 1000; k = (k * 7 + 3) % NWORDS) {
        for (const char *t = words[k]; *t && n < 1000; t++)
            buf[n++] = *t;
        buf[n++] = ',';
    }
    buf[n] = 0;
    if (nested(buf, 3) != nested_ref(buf, 3))
        bad |= 4;

    unsigned x = 12345;
    for (int i = 0; i < 600; i++) {
        x = x * 1103515245u + 12345u;
        bytes[i] = (unsigned char)(x >> 16);
    }
    for (int len = 0; len <= 600; len += 37)
        if (walk_switch(bytes, len) != walk_ref(bytes, len))
            bad |= 8;
    for (int len = 0; len <= 600; len += 41)
        if (wide_switch(bytes, len) != wide_ref(bytes, len))
            bad |= 16;
    bytes[300] = 0x7f;                              /* the break */
    if (wide_switch(bytes, 600) != wide_ref(bytes, 600))
        bad |= 32;

    return bad ? 100 + bad : 42;
}
