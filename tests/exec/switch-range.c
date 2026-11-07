/* A switch whose index is only ever one of its cases leaves the bound
 * test out (src/opt/rangecheck.c, pass_switchrange): every assignment of
 * the state is a case constant, a copy of one, or a choice between two.
 * Checked both ways -- machines whose state stays inside the table (cases
 * from 0, and from 10, so the index is `st - 10`), and machines where it
 * can leave it (a constant past the last case, a state taken from the
 * caller), which must still reach `default` -- each against the same
 * machine written with if/else. */
// expect-exit: 42
typedef unsigned int u32;

static const unsigned char in[] = "abc~de~~fghij~klmnopq~rs~tuv~wxyz~~0123~";
#define N ((int)sizeof in - 1)

__attribute__((noinline)) u32 m0(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        switch (st) {
        case 0: a += 1; st = b == '~' ? 1 : 2; break;
        case 1: a += 3; st = 3; break;
        case 2: a += 5; st = b & 1 ? 0 : 3; break;
        case 3: a += 7; st = 0; break;
        default: a += 1000; break;
        }
    }
    return a * 4 + (u32)st;
}
static u32 m0_ref(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        if (st == 0) { a += 1; st = b == '~' ? 1 : 2; }
        else if (st == 1) { a += 3; st = 3; }
        else if (st == 2) { a += 5; st = b & 1 ? 0 : 3; }
        else if (st == 3) { a += 7; st = 0; }
        else a += 1000;
    }
    return a * 4 + (u32)st;
}

__attribute__((noinline)) u32 m10(void)
{
    int st = 10; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        int nx = 13;
        switch (st) {
        case 10: a += 2; nx = b > 'm' ? 11 : 12; break;
        case 11: a += 4; nx = 12; break;
        case 12: a += 8; break;
        case 13: a += 16; nx = 10; break;
        default: a += 1000; break;
        }
        st = nx;
    }
    return a + (u32)st;
}
static u32 m10_ref(void)
{
    int st = 10; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        int nx = 13;
        if (st == 10) { a += 2; nx = b > 'm' ? 11 : 12; }
        else if (st == 11) { a += 4; nx = 12; }
        else if (st == 12) a += 8;
        else if (st == 13) { a += 16; nx = 10; }
        else a += 1000;
        st = nx;
    }
    return a + (u32)st;
}

/* past the table: a constant no case has */
__attribute__((noinline)) u32 out_k(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        switch (st) {
        case 0: a += 1; st = in[i] == '~' ? 99 : 1; break;
        case 1: a += 2; st = 2; break;
        case 2: a += 4; st = 0; break;
        case 3: a += 8; st = 0; break;
        default: a += 100; st = 0; break;
        }
    }
    return a;
}
/* from the caller */
__attribute__((noinline)) u32 out_p(int st)
{
    u32 a = 0;
    for (int i = 0; i < 6; i++) {
        switch (st) {
        case 0: a += 1; st = 1; break;
        case 1: a += 2; st = 2; break;
        case 2: a += 4; st = 3; break;
        case 3: a += 8; st = 0; break;
        default: a += 100; st = 0; break;
        }
    }
    return a;
}

/* States made by arithmetic, not constants -- `(x != 0) << 1`, an and, an
 * offset (what arithmetic if-conversion leaves) -- inside the table, so
 * the bound goes; and the same shapes reaching past it, `b & 7` against
 * four cases and `(x != 0) << 2`, which must still find default. */
__attribute__((noinline)) u32 arith_in(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        switch (st) {
        case 0: a += 1; st = (b != '~') << 1; break;     /* 0 or 2 */
        case 1: a += 3; st = (int)(b & 1) + 2; break;    /* 2 or 3 */
        case 2: a += 5; st = (int)(b & 3); break;        /* 0..3 */
        case 3: a += 7; st = 1; break;
        default: a += 1000; break;
        }
    }
    return a * 4 + (u32)st;
}

__attribute__((noinline)) u32 arith_out(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        switch (st) {
        case 0: a += 1; st = (int)(b & 7); break;        /* up to 7 */
        case 1: a += 3; st = 2; break;
        case 2: a += 5; st = 3; break;
        case 3: a += 7; st = 0; break;
        default: a += 1000; st = 0; break;
        }
    }
    return a * 4 + (u32)st;
}

__attribute__((noinline)) u32 arith_shl(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        switch (st) {
        case 0: a += 1; st = (b == 'a' || b == '~') << 2; break;  /* 0 or 4 */
        case 1: a += 3; st = 2; break;
        case 2: a += 5; st = 3; break;
        case 3: a += 7; st = 0; break;
        default: a += 1000; st = 1; break;
        }
    }
    return a * 4 + (u32)st;
}

static u32 arith_shl_ref(void)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        if (st == 0) { a += 1; st = (b == 'a' || b == '~') << 2; }
        else if (st == 1) { a += 3; st = 2; }
        else if (st == 2) { a += 5; st = 3; }
        else if (st == 3) { a += 7; st = 0; }
        else { a += 1000; st = 1; }
    }
    return a * 4 + (u32)st;
}

/* A state that steps itself: `st = st + 1`, a loop's counter. Each
 * value comes from the one before, so the proof meets st again asking a
 * range one lower than the one it started from -- which is no proof: the
 * counter runs to the case past the table, and the step down from 0
 * below it. Both must find default. */
static volatile int lim = 40;
__attribute__((noinline)) u32 step_up(void)
{
    u32 a = 0;
    for (int op = 0; op < lim; op++) {
        switch (op) {
        case 0: a += 1; break;
        case 1: a += 2; break;
        case 2: a += 4; break;
        case 3: a += 8; break;
        case 4: a += 16; break;
        case 5: a += 32; break;
        case 6: a += 64; break;
        case 7: a += 128; break;
        default: a += 1000; break;
        }
    }
    return a;
}

__attribute__((noinline)) u32 step_down(void)
{
    int st = 7; u32 a = 0;
    for (int i = 0; i < lim; i++) {
        switch (st) {
        case 0: a += 1; st = st - 1; break;
        case 1: a += 2; st = st - 1; break;
        case 2: a += 4; st = st - 1; break;
        case 3: a += 8; st = st - 1; break;
        case 4: a += 16; st = st - 1; break;
        case 5: a += 32; st = st - 1; break;
        case 6: a += 64; st = st - 1; break;
        case 7: a += 128; st = st - 1; break;
        default: a += 100; break;
        }
    }
    return a;
}

static u32 arith_ref(int out)
{
    int st = 0; u32 a = 0;
    for (int i = 0; i < N; i++) {
        u32 b = in[i];
        if (!out) {
            if (st == 0) { a += 1; st = (b != '~') << 1; }
            else if (st == 1) { a += 3; st = (int)(b & 1) + 2; }
            else if (st == 2) { a += 5; st = (int)(b & 3); }
            else if (st == 3) { a += 7; st = 1; }
            else a += 1000;
        } else {
            if (st == 0) { a += 1; st = (int)(b & 7); }
            else if (st == 1) { a += 3; st = 2; }
            else if (st == 2) { a += 5; st = 3; }
            else if (st == 3) { a += 7; st = 0; }
            else { a += 1000; st = 0; }
        }
    }
    return a * 4 + (u32)st;
}

int main(void)
{
    int bad = 0;
    if (m0() != m0_ref()) bad |= 1;
    if (m10() != m10_ref()) bad |= 2;
    u32 k = 0;
    {
        int st = 0; u32 a = 0;
        for (int i = 0; i < N; i++) {
            if (st == 0) { a += 1; st = in[i] == '~' ? 99 : 1; }
            else if (st == 1) { a += 2; st = 2; }
            else if (st == 2) { a += 4; st = 0; }
            else if (st == 3) { a += 8; st = 0; }
            else { a += 100; st = 0; }
        }
        k = a;
    }
    if (out_k() != k) bad |= 4;
    if (out_p(7) != 100 + 1 + 2 + 4 + 8 + 1 || out_p(-1) != 116 ||
        out_p(2) != 4 + 8 + 1 + 2 + 4 + 8)
        bad |= 8;
    if (arith_in() != arith_ref(0)) bad |= 16;
    if (arith_out() != arith_ref(1)) bad |= 32;
    if (arith_shl() != arith_shl_ref()) bad |= 64;
    if (step_up() != 255 + 32 * 1000) bad |= 128;
    if (step_down() != 255 + 32 * 100) bad |= 128;
    return bad ? bad : 42;
}
