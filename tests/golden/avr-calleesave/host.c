/* The host's probe: the same calls with nothing to check -- it gives the
 * results the target's must equal. */
#include <stdint.h>
typedef uint16_t u16;
typedef u16 (*fn_t)(u16);
volatile u16 probe_bad;
volatile unsigned char probe_ybad;
u16 probe(fn_t f, u16 a) { return f(a); }
