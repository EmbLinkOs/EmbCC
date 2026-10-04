// expect-exit: 42
/* A dense switch on a value just loaded -- an interpreter's dispatch --
 * where the loaded register is the table index itself. Opcodes of every
 * width and signedness, values past the table and negative ones (which
 * must take the default), a switch on a 64-bit value, and enough live
 * values that the index sits in r8-r15, whose encoding needs REX.X. */
typedef unsigned long long u64;

__attribute__((noinline)) static long run_u8(const unsigned char *p, int n)
{
    long acc = 0;
    for (int pc = 0; pc < n; pc++) {
        switch (p[pc]) {
        case 0: acc += 1; break;
        case 1: acc *= 3; break;
        case 2: acc -= 7; break;
        case 3: acc ^= 0x55; break;
        case 4: acc += acc; break;
        case 5: acc -= 1; break;
        case 6: acc |= 0x100; break;
        default: acc += 1000; break;
        }
    }
    return acc;
}

__attribute__((noinline)) static long run_s8(const signed char *p, int n)
{
    long acc = 0;
    for (int pc = 0; pc < n; pc++) {
        switch (p[pc]) {
        case 0: acc += 2; break;
        case 1: acc *= 5; break;
        case 2: acc -= 3; break;
        case 3: acc ^= 0x33; break;
        case 4: acc += 11; break;
        case 5: acc -= 13; break;
        default: acc -= 100; break;
        }
    }
    return acc;
}

__attribute__((noinline)) static long run_i(const int *p, int n, long a, long b,
                                            long c, long d, long e, long f)
{
    long acc = 0;
    for (int pc = 0; pc < n; pc++) {
        switch (p[pc]) {
        case 0: acc += a; break;
        case 1: acc += b; break;
        case 2: acc += c; break;
        case 3: acc += d; break;
        case 4: acc += e; break;
        case 5: acc += f; break;
        case 6: acc += a * b; break;
        case 7: acc -= c * d; break;
        default: acc += 7; break;
        }
        a += 1; b -= 1; c ^= 3; d += 2; e -= 2; f ^= 5;
    }
    return acc + a + b + c + d + e + f;
}

__attribute__((noinline)) static int run_l(const u64 *p, int n)
{
    int acc = 0;
    for (int pc = 0; pc < n; pc++) {
        switch (p[pc]) {
        case 0: acc += 1; break;
        case 1: acc += 10; break;
        case 2: acc += 100; break;
        case 3: acc += 1000; break;
        case 4: acc += 10000; break;
        default: acc -= 1; break;
        }
    }
    return acc;
}

/* Eight accumulators live across the dispatch: the opcode lands in r15,
 * whose index encoding needs the REX.X bit. */
__attribute__((noinline)) static long crowd(const unsigned char *p, int n, long a0, long a1, long a2, long a3)
{
    long a4 = a0 ^ a1, a5 = a2 + a3, a6 = a0 - a3, a7 = a1 * 3;
    for (int pc = 0; pc < n; pc++) {
        switch (p[pc]) {
        case 0: a0 += a1; a4 ^= a5; break;
        case 1: a1 -= a2; a5 += a6; break;
        case 2: a2 ^= a3; a6 -= a7; break;
        case 3: a3 += a0; a7 ^= a4; break;
        case 4: a0 -= a7; a4 += a3; break;
        case 5: a1 ^= a6; a5 -= a2; break;
        case 6: a2 += a5; a6 ^= a1; break;
        default: a3 -= a4; a7 += a0; break;
        }
    }
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

/* A small stack machine: with the stack, the variables, sp and pc all
 * live, the opcode lands in one of r8-r15. */
__attribute__((noinline)) static unsigned vm(const signed char *prog, unsigned arg)
{
    unsigned stack[16], var[4];
    int sp = 0, pc = 0;
    var[0] = arg; var[1] = var[2] = var[3] = 0;
    for (;;) {
        int op = prog[pc++];
        switch (op) {
        case 0: stack[sp++] = (unsigned)prog[pc++]; break;
        case 1: stack[sp++] = var[prog[pc++]]; break;
        case 2: var[prog[pc++]] = stack[--sp]; break;
        case 3: sp--; stack[sp - 1] += stack[sp]; break;
        case 4: sp--; stack[sp - 1] -= stack[sp]; break;
        case 5: sp--; stack[sp - 1] *= stack[sp]; break;
        case 6: { int t = prog[pc++]; if (stack[--sp]) pc = t; break; }
        case 7: stack[sp] = stack[sp - 1]; sp++; break;
        case 8: return stack[sp - 1];
        default: return 0xbad;
        }
    }
}

int main(void)
{
    /* var1 = 0; do { var1 += var0; var0 -= 1; } while (var0); return var1 */
    static const signed char prog[] = {
        0, 0, 2, 1,                 /* var1 = 0 */
        1, 1, 1, 0, 3, 2, 1,        /* var1 += var0 */
        1, 0, 0, 1, 4, 7, 2, 0,     /* var0 -= 1, keep a copy */
        6, 4,                       /* while (var0) */
        1, 1, 8
    };
    static const signed char bad[] = { 9 };
    if (vm(prog, 10) != 55 || vm(prog, 100) != 5050 || vm(bad, 1) != 0xbad)
        return 5;
    {
        static const unsigned char pc[] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 200, 3, 1, 0, 6 };
        if (crowd(pc, (int)sizeof pc, 3, 5, 7, 11) != -217L) return 6;
    }
    static const unsigned char pu[] = { 0, 1, 2, 3, 4, 5, 6, 7, 200, 255, 1, 0 };
    static const signed char ps[] = { 0, 1, -1, 2, 3, -128, 4, 5, 6, 127, 1 };
    static const int pi[] = { 0, 7, 3, -5, 8, 1, 6, 2, 100000, 4, 5 };
    static const u64 pl[] = { 0, 4, 3, 1ull << 32, 2, 5, 0xffffffffull, 1,
                              0x100000003ull };
    if (run_u8(pu, (int)sizeof pu) != 8476) return 1;
    if (run_s8(ps, (int)sizeof ps) != -2070) return 2;
    if (run_i(pi, (int)(sizeof pi / sizeof pi[0]), 3, 5, 7, 11, 13, 17) != 50)
        return 3;
    if (run_l(pl, (int)(sizeof pl / sizeof pl[0])) != 11107) return 4;
    return 42;
}
