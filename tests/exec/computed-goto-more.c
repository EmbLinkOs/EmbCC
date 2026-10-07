/* GNU computed goto, the shapes computed-goto.c does not reach, written
 * for the embedded backends that lower &&label as a code address of their
 * own (Thumb's has bit 0 set, AVR's is a word address in program memory)
 * and for the register allocator, which now sees a computed goto's real
 * successors and keeps values in registers across one:
 *   1. a threaded interpreter whose bytecode loops, with several values
 *      live across every dispatch;
 *   2. values read at the TOP of a loop closed by a backward `goto *p`
 *      and never after -- dead at their last read if the jump falls
 *      through, so their registers go to the values defined below;
 *   3. a label address passed through a call and stored in a struct;
 *   4. label addresses compared for equality, one of each pair having
 *      gone through memory and a call;
 *   5. a label far from the instruction that takes its address, both
 *      ways -- past a Thumb adr's 4 KiB and every short form's reach.
 * Refereed against clang; run at -O0/-O1/-O2/-Os. */
// expect-exit: 42

struct cont { int tag; void *where; };

/* A volatile cell, so no compiler can see through the call: one that
 * knows pass() returns its argument turns `goto *pass(p)` into a branch,
 * deletes the labels' blocks, and their addresses then compare equal. */
static void *volatile cell;
static __attribute__((noinline)) void *pass(void *p)
{
    cell = p;
    return cell;
}

static __attribute__((noinline)) struct cont mk(int tag, void *w)
{
    struct cont c;
    c.tag = tag;
    c.where = w;
    return c;
}

/* 1. op codes: HALT, ADDN (acc += n), DEC (n--), JNZ t (if n, pc = t),
 * SETN k (n = k), MULACC k (acc *= k) */
static long interp(const unsigned char *code)
{
    void *const tab[6] = { &&op_halt, &&op_addn, &&op_dec, &&op_jnz,
                           &&op_setn, &&op_mulacc };
    long acc = 0, n = 0, steps = 0, mix = 1;
    int pc = 0;
#define NEXT do { steps++; mix = mix * 3 + steps; goto *tab[code[pc++]]; } while (0)
    NEXT;
op_addn:   acc += n; NEXT;
op_dec:    n--; NEXT;
op_jnz:    { int t = code[pc++]; if (n) pc = t; } NEXT;
op_setn:   n = code[pc++]; NEXT;
op_mulacc: acc *= code[pc++]; NEXT;
op_halt:   return acc * 1000 + steps * 10 + (mix & 7);
#undef NEXT
}

/* 2. k1..k8 are read at L and nowhere after it; the eight values below
 * them are each live across the next, so a register a k gives up at its
 * last read is taken at once */
static long backward(int n, long k1, long k2, long k3, long k4)
{
    void *again = pass(&&L);
    long k5 = k1 + 4, k6 = k2 + 4, k7 = k3 + 4, k8 = k4 + 4;
    long sum = 0;
L:  sum += k1 * 1000 + k2 * 100 + k3 * 10 + k4;
    sum += k5 * 1000 + k6 * 100 + k7 * 10 + k8;
    {
        long x1 = sum * 3, x2 = sum ^ n, x3 = sum + n * 7, x4 = x1 - x2;
        long x5 = x3 * 5, x6 = x4 ^ x5, x7 = x6 + x1, x8 = x7 - x2;
        sum += (x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8) & 1;
        sum -= (x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8) & 1;
    }
    if (--n > 0)
        goto *again;
    return sum;
}

/* 3. through a call, a struct and an array of structs */
static int via_struct(int sel)
{
    struct cont cs[3];
    cs[0] = mk(10, &&ten);
    cs[1] = mk(20, &&twenty);
    cs[2] = mk(30, &&thirty);
    struct cont c = cs[sel];
    goto *pass(c.where);
ten:    return c.tag + 1;
twenty: return c.tag + 2;
thirty: return c.tag + 3;
}

/* 4. equality */
static int compare(int sel)
{
    void *a = &&la, *b = &&lb;
    void *p = sel ? b : a;
    int r = 0;
    if (p == &&lb) r |= 1;
    if (pass(a) == &&la) r |= 2;
    if (a != b) r |= 4;
    if (pass(&&la) == pass(&&la)) r |= 8;
    if (pass(&&la) != pass(&&lb)) r |= 16;
    goto *pass(p);
la: return r + 100;
lb: return r + 200;
}

/* 5. far: 512 statements of filler between each label and the code that
 * takes its address -- 128 on AVR, whose part has 32 KiB of flash for
 * all of it, where 512 of them at -O0 are 47 KiB (its label address is
 * an absolute word address in any case, and 128 still put each label
 * past rjmp's 4 KiB there) */
static volatile unsigned g;
#define F1 g ^= g << 1;
#define F8 F1 F1 F1 F1 F1 F1 F1 F1
#define F64 F8 F8 F8 F8 F8 F8 F8 F8
#if defined(__AVR__)
#define F512 F64 F64
#else
#define F512 F64 F64 F64 F64 F64 F64 F64 F64
#endif
static int far(int sel)
{
    int r = 0;
    void *p;
    goto start;
back:
    return r + 1;
skip:
    F512
    return -1;
start:
    p = pass(sel ? &&back : &&fwd);
    r = 10;
    if (sel > 5) goto skip;
    goto *p;
    F512
fwd:
    return r + 2;
}

int main(void)
{
    static const unsigned char prog[] = {
        4, 5,           /* SETN 5 */
        1,              /* L: ADDN */
        2,              /* DEC */
        3, 2,           /* JNZ L */
        5, 2,           /* MULACC 2 */
        0               /* HALT */
    };
    /* 5+4+3+2+1 = 15, doubled; 1 + 5*3 + 2 dispatches = 18 */
    long r = interp(prog);
    if (r / 1000 != 30) return 1;
    if (r % 1000 / 10 != 18) return 2;
    if (backward(4, 1, 2, 3, 4) != 4 * (1234 + 5678)) return 3;
    if (via_struct(0) != 11 || via_struct(1) != 22 || via_struct(2) != 33)
        return 4;
    if (compare(0) != 100 + 30) return 5;
    if (compare(1) != 200 + 31) return 6;
    if (far(0) != 12) return 7;
    if (far(1) != 11) return 8;
    return 42;
}
