/* An unsigned int converted to floating point, where the value came from a
 * negative int. On x86-64 and aarch64 the conversion is the signed 64-bit
 * instruction applied to the value zero-extended -- and the extension was
 * assumed rather than made: the unsigned value was read at eight bytes,
 * trusting its upper half to be zero. An integer a call returns comes
 * back in the whole of the return register with whatever the callee left
 * above bit 31, so (double)(unsigned)f() converted the sign bits as part
 * of the value: -104634 instead of 4294862662 (random programs found it).
 * Here the value arrives from a call, a merge, a parameter and a load,
 * and is converted to double and float and compared with a double. */
// expect-exit: 42
static volatile double vd = -104634.5;
static volatile int vi[3] = { -1, -104634, 7 };

__attribute__((noinline)) static int toint(double d) { return (int)d; }
__attribute__((noinline)) static double viacall(double g) { return (double)(unsigned)toint(g); }
__attribute__((noinline)) static float viacallf(double g) { return (float)(unsigned)toint(g); }
__attribute__((noinline)) static double viamerge(int k, double g)
{ unsigned u = k ? (unsigned)toint(g) : (unsigned)toint(-g); return (double)u; }
__attribute__((noinline)) static double viaparam(unsigned u) { return (double)u; }
__attribute__((noinline)) static int below(double h, double g) { return h < (unsigned)toint(g); }

int main(void)
{
    double big = 4294967296.0 - 104634.0;          /* exact */
    if (viacall(vd) != big) return 1;
    if (viacallf(vd) != (float)4294862662.0) return 2;
    if (viamerge(1, vd) != big || viamerge(0, vd) != 104634.0) return 3;
    if (viaparam((unsigned)vi[1]) != big || viaparam((unsigned)vi[0]) != 4294967295.0) return 4;
    if ((double)(unsigned)vi[1] != big || (float)(unsigned)vi[0] != 4294967296.0f) return 5;
    if (!below(-11785.0, vd) || below(4294967295.5, vd)) return 6;
    return 42;
}
