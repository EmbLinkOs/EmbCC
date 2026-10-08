/* An integer call argument whose value lives in an xmm register. Value
 * numbering merges the char argument `zero` with the 0.0f of the float
 * arithmetic -- both are `const.4 0` -- and the float uses outvote the
 * argument, a soft integer use, so the value gets an FP home and no slot.
 * The x86-64 integer-argument path had no movq for that and refused the
 * function (fuzz seeds 7306 and 7581). */
// expect-exit: 42
__attribute__((noinline)) double f0(unsigned long long a, unsigned char b, unsigned short c, float d)
{
    return (double)a + b + c + d;
}
volatile float vx = 1.5f;
double g(float x)
{
    unsigned char zero = 0;
    float y = (0.0f - x) * (0.0f - x) + (0.0f + x);
    return f0(7, zero, 3, y - 0.0f);
}
int main(void) { return g(vx) == 7 + 0 + 3 + (2.25f + 1.5f) ? 42 : 1; }
