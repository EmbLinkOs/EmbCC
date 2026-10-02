/* An attribute at the START of a struct member declaration applies to
 * every declarator in it, as a trailing one applies to its own: gcc and
 * clang accept `__attribute__((aligned(16))) char t[40];`, and EmbCC
 * refused it ("expected a member type"). The layouts are pinned by
 * static assertions that clang agrees with. */
// expect-exit: 42
#include <stddef.h>
struct A { char c; __attribute__((aligned(16))) char t[40]; int x; };
struct B { char c; char t[40] __attribute__((aligned(16))); int x; };
struct C { char c; __attribute__((aligned(8))) short a, b; };
_Static_assert(offsetof(struct A, t) == 16, "leading aligned");
_Static_assert(offsetof(struct B, t) == 16, "trailing aligned");
_Static_assert(sizeof(struct A) == sizeof(struct B), "same layout");
_Static_assert(offsetof(struct C, a) == 8 && offsetof(struct C, b) == 16, "applies to every declarator");
int main(void) { return 42; }
