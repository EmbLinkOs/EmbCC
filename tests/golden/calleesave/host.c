/* The host's probe: the same calls, no registers to check -- it supplies
 * the results the target's must equal. */
typedef unsigned u32;
typedef u32 (*fn_t)(u32, u32, u32, u32);
u32 probe(fn_t f, u32 a, u32 *result) { *result = f(a, 3, 5, 7); return 0; }
