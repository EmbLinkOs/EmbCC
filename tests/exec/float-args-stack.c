/* Floating-point parameters past the last argument register, which the
 * caller passes on the stack. At -O2 one whose home the allocator made a
 * register went by way of an integer register into a stack slot it does
 * not have: x86-64 refused the whole function (the dead-slot guard), and
 * aarch64 stored the ninth double a gigabyte above sp and then read a v
 * register nothing had loaded -- a data abort on the harness, and
 * whatever that address held on a board with no MMU. swap() is two
 * float arguments whose homes are each other's argument register.
 *
 * call() and callf() are the CALLER's half: arguments computed into
 * registers that must go out on the stack. x86-64 read the slot such a
 * value does not have (refused at -O2), and aarch64 read the first temp
 * slot in its place -- h() got some other value twice. */
// expect-exit: 42
__attribute__((noinline)) double d10(double a, double b, double c, double d, double e,
    double f, double g, double h, double i, double j)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10; }
__attribute__((noinline)) float f10(float a, float b, float c, float d, float e,
    float f, float g, float h, float i, float j)
{ return j - i + h - g + f - e + d - c + b - a; }
__attribute__((noinline)) double mix(int k, double a, float b, double c, long l, double d,
    float e, double f, double g, double h, int m, double i, float j, double z)
{ return k + a + b + c + l + d + e + f + g + h + m + i + j + z * 100; }
__attribute__((noinline)) double swap(double a, double b) { return b - a * 3; }
__attribute__((noinline)) double h(double a, double b, double c, double d,
    double e, double f, double g, double hh, double i, double j)
{ return a + b + c + d + e + f + g + hh + i * 100 + j * 1000; }
__attribute__((noinline)) float hf(float a, float b, float c, float d,
    float e, float f, float g, float hh, float i)
{ return a + b + c + d + e + f + g + hh + i * 100; }
__attribute__((noinline)) double call(double v, double w)
{ double x = v * w, y = v + w; return h(v, w, x, y, v, w, x, y, x * 3, y * 5); }
__attribute__((noinline)) float callf(float v, float w)
{ float x = v * w; return hf(v, w, x, v, w, x, v, w, x * 3); }
int main(void)
{
    int bad = 0;
    if (d10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 385) bad |= 1;
    if (f10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 5) bad |= 2;
    if (mix(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14) != 91 + 1400) bad |= 4;
    if (swap(2, 10) != 4) bad |= 8;
    if (call(2, 3) != 32 + 1800 + 25000) bad |= 16;
    if (callf(2, 3) != 27 + 1800) bad |= 32;
    return bad ? bad : 42;
}
