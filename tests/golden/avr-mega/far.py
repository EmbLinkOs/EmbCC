"""far.py OUT: far.c, a program for the ATmega2560 past 128 KiB of flash
(420 functions of 32-bit arithmetic, called in turn, two of them also
through pointers, in code and in data), and far.expect, the hash it must
print, computed here."""
import random, sys
out = sys.argv[1]
random.seed(3)
N = 420
src = ['#include <avr/io.h>', '#include <stdio.h>',
       'static void putc0(char c) { while (!(UCSR0A & (1 << UDRE0))) ; UDR0 = (unsigned char)c; }',
       'int putchar(int c) { putc0((char)c); return c; }',
       'unsigned long h = 1;']
fs = []
for i in range(N):
    a, b = random.randrange(1, 1 << 16) | 1, random.randrange(1, 1 << 30)
    fs.append((a, b))
    src.append(f'__attribute__((noinline)) unsigned long f{i}(unsigned long x, int k) '
               f'{{ x = x * {a}UL + {b}UL; x ^= x >> 7; x += (unsigned long)k * {i}UL; return x; }}')
src += ['typedef unsigned long (*fn_t)(unsigned long, int);',
        f'fn_t table_fp[2] = {{ f0, f{N - 1} }};',
        'int main(void) { UCSR0B = 1 << TXEN0;',
        f'  volatile fn_t p = f{N - 1};',
        f'  unsigned long a = p(5, 3), b = table_fp[1](5, 3), c = f{N - 1}(5, 3), d = table_fp[0](5, 3), e = f0(5, 3);',
        '  printf("pointers: %s %s\\n", a == c && b == c ? "same" : "DIFFER", d == e ? "same" : "DIFFER");']
src += [f'  h = f{i}(h, {i % 7});' for i in range(N)]
src.append('  printf("h=%08lx\\n<<END>>\\n", h); for (;;) ; }')
open(f"{out}/far.c", "w").write("\n".join(src) + "\n")
ref, M = 1, 0xFFFFFFFF
for i, (a, b) in enumerate(fs):
    x = (ref * a + b) & M; x ^= x >> 7; ref = (x + (i % 7) * i) & M
open(f"{out}/far.expect", "w").write(f"pointers: same same\nh={ref:08x}\n")
