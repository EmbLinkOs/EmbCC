/* A type name in an expression may define a struct without a tag --
 * sizeof, __builtin_offsetof, a compound literal -- as newlib's fallback
 * _Alignof does (`__offsetof(struct { char __a; x __b; }, __b)`), which
 * made every _Alignof in a program built against newlib for aarch64 an
 * error: "block-scope type definitions are not supported". */
// expect-exit: 42
int main(void) { int a = sizeof(struct { char c; int i; }); int b = ((struct { int x, y; }){ 1, 2 }).y; return a + (int)__builtin_offsetof(struct { char c; long l; }, l) + b + 32 - (int)sizeof(long); }
