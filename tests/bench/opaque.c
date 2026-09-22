/* A separate translation unit, so the call in call_overhead cannot be
 * inlined by anybody. Without this the benchmark measures whether the
 * compiler noticed, not what a call costs. */
long opaque_add(long a, long b) { return a + b; }
