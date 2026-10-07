/* TriCore inline asm on the board (tests/golden/tricore-asm.sh): every
 * statement form of src/arch/tricore/asm.c's vocabulary that a program can
 * observe, with operands in data registers ("d"), address registers ("a"),
 * memory ("m"), constants ("i"), read-write ("+d") and bound register
 * variables. Each check returns its own number on failure; 42 is success. */
static volatile unsigned words[2] = { 0x11223344u, 0x55667788u };
#define word words[0]
static volatile unsigned cell;

static int checks(void)
{
    unsigned r, t;
    unsigned *p = (unsigned *)&cell;

    __asm__ volatile("mov %0, %1" : "=d"(r) : "i"(-5));
    if (r != 0xfffffffbu) return 1;
    __asm__ volatile("mov.u %0, 0xbeef\n movh %1, 0x1234" : "=d"(r), "=d"(t));
    if (r != 0xbeefu || t != 0x12340000u) return 2;
    __asm__ volatile("add %0, %1, %2" : "=d"(r) : "d"(40u), "d"(2u));
    if (r != 42) return 3;
    __asm__ volatile("sh %0, %1, -4; sha %0, %0, 1" : "=d"(r) : "d"(0x80u));
    if (r != 0x10) return 4;
    __asm__ volatile("lt.u %0, %1, %2" : "=d"(r) : "d"(1u), "d"(0xffffffffu));
    if (r != 1) return 5;
    r = 7;
    __asm__ volatile("addi %0, %0, -3" : "+d"(r));
    if (r != 4) return 6;
    __asm__ volatile("mtcr biv, %1\n isync\n mfcr %0, biv" : "=d"(r) : "d"(0x8000u));
    if (r != 0x8000u) return 7;
    __asm__ volatile("ld.w %0, [%1]0" : "=d"(r) : "a"(&word));
    if (r != 0x11223344u) return 8;
    __asm__ volatile("st.h [%1]2, %0" : : "d"(0xabcdu), "a"(&word));
    if (word != 0xabcd3344u || words[1] != 0x55667788u) return 9;
    /* an "m" operand is an ADDRESS register holding the lvalue's address,
     * written [%0] */
    __asm__ volatile("st.w [%0]0, %1" : "=m"(cell) : "d"(0x1234u));
    if (cell != 0x1234u) return 10;
    __asm__ volatile("st.w [%0]0, %1" : : "a"(p), "d"(0x5a5au) : "memory");
    if (cell != 0x5a5au) return 11;
    r = 9;
    __asm__ volatile("swap.w [%1]0, %0" : "+d"(r) : "a"(p) : "memory");
    if (r != 0x5a5au || cell != 9) return 12;
    __asm__ volatile("mov d3, 9\n mov d2, 77\n cmpswap.w [%1]0, e2\n mov %0, d2"
                     : "=d"(r) : "a"(p) : "d2", "d3", "memory");
    if (r != 9 || cell != 77) return 13;
    {
        register unsigned v __asm__("d5") = 6;
        __asm__ volatile("mul %0, %0, 7" : "+d"(v));
        if (v != 42) return 14;
    }
    {
        void *q;
        __asm__ volatile("lea %0, [%1]8\n mov.aa %0, %0" : "=a"(q) : "a"(p));
        if ((char *)q != (char *)p + 8) return 15;
    }
    /* through a4, so the two fields cannot hold the same number */
    __asm__ volatile("mov.aa a4, %1\n mov.d %0, a4" : "=d"(r) : "a"(p)
                     : "a4");
    if (r != (unsigned)p) return 16;
    {
        void *q;
        __asm__ volatile("mov.a %0, %1" : "=a"(q) : "d"(0x1234u));
        if ((unsigned)q != 0x1234u) return 17;
    }
    __asm__ volatile("nop\n dsync\n debug" ::: "memory");
    return 42;
}

int main(void)
{
    return checks();
}
