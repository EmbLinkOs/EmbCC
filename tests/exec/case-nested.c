/* A case or default label may be anywhere in its switch's body, not only
 * directly in it: in a nested block, inside an if, inside a loop -- which
 * the switch then enters in its middle (Duff's device). A nested switch's
 * labels stay its own. EmbCC refused every one of these ("labels inside a
 * nested block are not supported"); the jump to such a label is a jump
 * into the statement that holds it, as C11 6.8.4.2 has it. */
// expect-exit: 42

static volatile int vz;     /* a zero the optimizer cannot see */

/* Duff's device: the switch enters the do-while at the case for the
 * remainder, so the loop has eight entries */
static void duff(char *to, const char *from, int count)
{
    int n = (count + 7) / 8;
    switch (count % 8) {
    case 0: do { *to++ = *from++;
    case 7:      *to++ = *from++;
    case 6:      *to++ = *from++;
    case 5:      *to++ = *from++;
    case 4:      *to++ = *from++;
    case 3:      *to++ = *from++;
    case 2:      *to++ = *from++;
    case 1:      *to++ = *from++;
            } while (--n > 0);
    }
}

/* cases in a nested block, in the arm of an if the test would skip, and
 * a default inside a block */
static int blocks(int k)
{
    int r = 100;
    switch (k) {
    case 1: {
        int t = 5;
        r = t;
    case 2:
        r += 20;                   /* 1: 25; 2: 120 (t never assigned) */
    }
        break;
    case 3:
        if (vz) {
    case 4:
            r = 4;                 /* reached for 4 though vz is 0 */
        } else
            r = 3;
        break;
        {
    default:
            r = -1;
        }
    }
    return r;
}

/* a nested switch keeps its own labels: its case 1 is not the outer's */
static int nested(int a, int b)
{
    int r = 0;
    switch (a) {
    case 1:
        switch (b) {
        case 1: r = 11; break;
        case 2: r = 12; break;
        default: r = 10;
        }
        break;
    case 2: {
        r = 2;
    case 3:
        r += 30;                   /* 2: 32; 3: 30 */
    }
    }
    return r;
}

/* entering a while and a for in their middles: the loop then runs as
 * written from there */
static int loops(int k)
{
    int r = 0, i = 0;
    switch (k) {
    case 0:
        while (i < 3) {
            r += 1;
    case 1:
            r += 10;
            i++;
        }
        break;
    case 2:
        for (i = 0; i < 2; i++) {
            r += 100;
    case 3:
            r += 1000;
        }
    }
    return r;
}

int main(void)
{
    char a[40], b[40];
    for (int i = 0; i < 40; i++)
        a[i] = (char)(i * 7 + 1);
    for (int c = 1; c <= 37; c += 3) {
        for (int i = 0; i < 40; i++)
            b[i] = 0;
        duff(b, a, c);
        for (int i = 0; i < 40; i++)
            if (b[i] != (i < c ? a[i] : 0))
                return 1;
    }
    if (blocks(1) != 25 || blocks(4) != 4 || blocks(3) != 3 ||
        blocks(9) != -1)
        return 2;
    if (blocks(2) != 120)
        return 3;
    if (nested(1, 1) != 11 || nested(1, 2) != 12 || nested(1, 7) != 10 ||
        nested(2, 0) != 32 || nested(3, 0) != 30 || nested(5, 1) != 0)
        return 4;
    /* while from case 0: 3 rounds of 11 = 33; from case 1, i is 0 and
     * the first round skips the +1: 10 + 11 + 11 = 32 */
    if (loops(0) != 33 || loops(1) != 32)
        return 5;
    /* for from case 2: 2 rounds of 1100; from case 3, i is still 0 (the
     * for's init is skipped): 1000, then i=1: 1100 -> 2100 */
    if (loops(2) != 2200 || loops(3) != 2100)
        return 6;
    return 42;
}
