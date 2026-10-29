/* Thumb code that addresses from Align(pc, 4): 64-bit constants loaded
 * from a literal pool (each used twice, so the pool is the shorter), and
 * a switch whose table is reached by adr.w (a case placed before it).
 * Each function is a section of its own, and each must be word-aligned
 * for the offsets the compiler worked out to hold wherever the linker
 * puts it. pad() is two bytes, so the next starts on a halfword unless
 * it asks for more. */
void pad(void) { }

unsigned long long mix(unsigned long long x)
{
    return x * 0x123456789abcdef1ULL + (x ^ 0x0fedcba987654321ULL) +
           (x & 0x123456789abcdef1ULL) - (x | 0x0fedcba987654321ULL);
}

void pad2(void) { }

int sw(const unsigned char *p, int n)
{
    int r = 0;
    for (;;) {
        r += *p++;
        if (--n == 0)
            return r;
        switch (*p & 7) {
        case 0: continue;
        case 1: r += 5; continue;
        case 2: r += 7; break;
        case 3: continue;
        case 4: r += 11; break;
        case 5: r += 13; continue;
        case 6: r -= 1; break;
        case 7: r *= 2; continue;
        }
        r ^= 1;
    }
}
