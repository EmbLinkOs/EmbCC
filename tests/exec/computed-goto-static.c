/* GNU computed goto through STATIC data: the label addresses are in a
 * table the compiler lays out in .data or .rodata, so each is its
 * function's address plus the label's offset in it -- a relocation the
 * linker resolves (with the Thumb bit on ARMv7-M, as a word address on
 * AVR), where computed-goto.c's are built in code. And `&&b - &&a`, the
 * GCC manual's offset table: a number only the compiler's own layout
 * knows, in an int array with no relocation at all.
 *   1. a threaded interpreter dispatching through `static void *const
 *      ops[]`, its bytecode looping;
 *   2. the same table's entries equal to &&label taken in code;
 *   3. `static const int off[] = { &&a - &&a, &&b - &&a, ... }` with
 *      `goto *(&&a + off[i])`, and the (char *) spelling of both;
 *   4. two functions with tables of their own, so a label's offset is
 *      from its function and not from the start of .text.
 * Refereed against clang; run at -O0/-O1/-O2/-Os. */
// expect-exit: 42

static int pad(int x) { return x * 3 + 1; }   /* run() is not at .text's start */

/* 1, 2. op codes: 0 HALT, 1 ADDN (acc += n), 2 DEC (n--), 3 JNZ t,
 * 4 SETN k, 5 SAME (counts table entries equal to code's &&label) */
static long run(const unsigned char *code)
{
    static void *const ops[] = { &&op_halt, &&op_addn, &&op_dec, &&op_jnz,
                                 &&op_setn, &&op_same };
    long acc = 0, n = 0, steps = 0, same = 0;
    int pc = 0;
#define NEXT do { steps++; goto *ops[code[pc++]]; } while (0)
    NEXT;
op_addn: acc += n; NEXT;
op_dec:  n--; NEXT;
op_jnz:  { int t = code[pc++]; if (n) pc = t; } NEXT;
op_setn: n = code[pc++]; NEXT;
op_same:
    same += ops[0] == &&op_halt;
    same += ops[3] == &&op_jnz;
    same += ops[5] == &&op_same;
    same += ops[1] != ops[2];
    NEXT;
op_halt: return acc * 1000 + steps * 10 + same;
#undef NEXT
}

/* 3. the GCC manual's offset table */
static int offsets(int i)
{
    static const int off[] = { &&l0 - &&l0, &&l1 - &&l0, &&l2 - &&l0,
                               &&l3 - &&l0 };
    goto *(&&l0 + off[i]);
l0: return 10;
l1: return 11;
l2: return 12;
l3: return 13;
}

static int offsets_char(int i)
{
    static const int off[] = { (char *)&&m2 - (char *)&&m0,
                               (char *)&&m1 - (char *)&&m0,
                               (char *)&&m0 - (char *)&&m0 };
    goto *((char *)&&m0 + off[i]);
m0: return 20;
m1: return 21;
m2: return 22;
}

/* 4. a second table, in a second function */
static int pick(int i)
{
    static void *tab[] = { &&p0, &&p1, &&p2 };
    int r = 100;
    goto *tab[i % 3];
p0: r += 1;
p1: r += 2;
p2: r += 4;
    return r;
}

int main(void)
{
    static const unsigned char prog[] = {
        5,              /* SAME */
        4, 4,           /* SETN 4 */
        1,              /* L: ADDN */
        2,              /* DEC */
        3, 3,           /* JNZ L */
        0               /* HALT */
    };
    /* 4+3+2+1 = 10; 1 + 1 + 4*3 + 1 = 15 dispatches; 4 entries equal */
    long r = run(prog);
    if (r / 1000 != 10) return 1;
    if (r % 1000 / 10 != 15) return 2;
    if (r % 10 != 4) return 3;
    if (offsets(0) != 10 || offsets(1) != 11 || offsets(2) != 12 ||
        offsets(3) != 13)
        return 4;
    if (offsets_char(0) != 22 || offsets_char(1) != 21 ||
        offsets_char(2) != 20)
        return 5;
    if (pick(0) != 107 || pick(1) != 106 || pick(5) != 104) return 6;
    if (pad(1) != 4) return 7;
    return 42;
}
