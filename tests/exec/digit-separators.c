/* C23's digit separator, 1'000'000, in decimal, hex, binary and floating
 * constants, in a macro argument and in #if. The preprocessor took the
 * ' for a character literal: two of them paired up by luck, an odd one
 * ran to the end of the line and hid what followed -- here, TEN. */
// expect-exit: 42
// no-gcc-reference: C23 digit separators; the reference builds strict C11
#define TEN 10
#define ID(x) x
int big = 1'000'000;   /* an even number of separators */
int odd = 1'000 + TEN; /* an odd one, a macro after it on the line */
unsigned hex = 0xFF'FF;
double d = 1'0.2'5;
long b = 0b1010'1010;
#if 1'000 > 999
int pp = 1;
#else
int pp = 0;
#endif
int main(void)
{
    int arg = ID(2'000);
    return big == 1000000 && odd == 1010 && hex == 0xFFFF && d == 10.25 &&
           b == 170 && pp && arg == 2000 ? 42 : 1;
}
