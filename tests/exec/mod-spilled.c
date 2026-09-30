/* A remainder whose left operand lives in MEMORY: twenty-five values are
 * live at once, more than any backend's register file holds, so some are
 * spilled and each comes back through its slot into the backend's scratch
 * -- where the remainder lowering must not also put its quotient. aarch64
 * computes a % b as sdiv q, a, b; msub r, q, b, a, and msub reads a after
 * sdiv has written q. With q in the scratch a was loaded into, every
 * spilled a read back as its own quotient. Signed and unsigned, and a
 * negative dividend, whose remainder takes the dividend's sign. */
// expect-exit: 42
__attribute__((noinline)) int smod(const int *v, int d)
{
    int a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3], a4 = v[4], a5 = v[5], a6 = v[6], a7 = v[7], a8 = v[8], a9 = v[9], a10 = v[10], a11 = v[11], a12 = v[12], a13 = v[13], a14 = v[14], a15 = v[15], a16 = v[16], a17 = v[17], a18 = v[18], a19 = v[19], a20 = v[20], a21 = v[21], a22 = v[22], a23 = v[23], a24 = v[24];
    return a0 % d + a1 % d + a2 % d + a3 % d + a4 % d + a5 % d + a6 % d + a7 % d + a8 % d + a9 % d + a10 % d + a11 % d + a12 % d + a13 % d + a14 % d + a15 % d + a16 % d + a17 % d + a18 % d + a19 % d + a20 % d + a21 % d + a22 % d + a23 % d + a24 % d;
}
__attribute__((noinline)) unsigned umod(const unsigned *v, unsigned d)
{
    unsigned a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3], a4 = v[4], a5 = v[5], a6 = v[6], a7 = v[7], a8 = v[8], a9 = v[9], a10 = v[10], a11 = v[11], a12 = v[12], a13 = v[13], a14 = v[14], a15 = v[15], a16 = v[16], a17 = v[17], a18 = v[18], a19 = v[19], a20 = v[20], a21 = v[21], a22 = v[22], a23 = v[23], a24 = v[24];
    return a0 % d + a1 % d + a2 % d + a3 % d + a4 % d + a5 % d + a6 % d + a7 % d + a8 % d + a9 % d + a10 % d + a11 % d + a12 % d + a13 % d + a14 % d + a15 % d + a16 % d + a17 % d + a18 % d + a19 % d + a20 % d + a21 % d + a22 % d + a23 % d + a24 % d;
}
int main(void)
{
    int v[25]; unsigned u[25];
    int want = 0; unsigned uwant = 0;
    for (int k = 0; k < 25; k++) {
        v[k] = (k & 1 ? -1 : 1) * (k * 37 + 11);
        u[k] = (unsigned)k * 2654435761u + 7u;
        want += v[k] % 7;
        uwant += u[k] % 13u;
    }
    if (smod(v, 7) != want) return 1;
    if (umod(u, 13u) != uwant) return 2;
    return 42;
}
