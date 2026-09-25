/* Variadic functions on ARMv7-M (D-015).
 *
 * AAPCS32 passes a variadic argument exactly as it passes a named one,
 * so a `va_list` is a bare pointer at the next one — and the prologue's
 * job is to spill r0-r3 immediately below the caller's stack arguments
 * so that one pointer walks from the registers into them.
 *
 * Covered: reading ints, an eight-byte type (which is eight-ALIGNED in
 * the argument area, so the walk has to round up), a double (a variadic
 * float arrives promoted to one), the case where four NAMED parameters
 * spend the register file before the list starts, and va_copy — which
 * on this ABI is a pointer assignment and not a record copy.
 *
 * The reference is the host. Whether these match another ARM
 * toolchain's idea of the same calls is asked separately, by the
 * cross-compiled pairing in thumb-exec.sh.
 */
#include <stdarg.h>
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
static void h16(unsigned long long v){ for(int i=60;i>=0;i-=4) writec("0123456789abcdef"[(int)((v>>i)&15ULL)]); writec(' '); }

static int sum(int n, ...)
{ va_list ap; int t = 0; va_start(ap, n);
  for (int i = 0; i < n; i++) t += va_arg(ap, int);
  va_end(ap); return t; }

static long long mixed(int n, ...)
{ va_list ap; long long t = 0; va_start(ap, n);
  for (int i = 0; i < n; i++) {
      if (i & 1) t += va_arg(ap, long long);
      else       t += va_arg(ap, int);
  }
  va_end(ap); return t; }

static void doubles(int n, ...)
{ va_list ap; va_start(ap, n);
  for (int i = 0; i < n; i++) h16((unsigned long long)(long long)(va_arg(ap, double) * 8.0));
  va_end(ap); writec('\n'); }

static int firstfour(int a, int b, int c, int d, ...)
{ va_list ap; int t = a+b+c+d; va_start(ap, d);
  t += va_arg(ap, int); t += va_arg(ap, int);
  va_end(ap); return t; }

static int copied(int n, ...)
{ va_list ap, bp; int t = 0; va_start(ap, n);
  va_copy(bp, ap);
  t += va_arg(ap, int);
  t += va_arg(bp, int) * 100;   /* the copy starts over */
  va_end(bp); va_end(ap); return t; }

int main(void)
{
  putn(sum(5, 1,2,3,4,5));
  putn(sum(1, 42));
  putn(firstfour(1,2,3,4, 50, 600));
  putn(copied(1, 7));
  writec('\n');
  h16((unsigned long long)mixed(4, 1, 100000000000LL, 2, 200000000000LL));
  writec('\n');
  doubles(3, 1.5, 0.25, -2.0);
  puts_("==END==\n");
  return 0;
}
