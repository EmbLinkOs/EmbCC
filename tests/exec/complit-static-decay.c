/* An array compound literal that initializes a pointer at file scope is
 * an object of its own, and the pointer points at it. It was flattened
 * into the pointer, and refused: "a scalar takes exactly one
 * initializer". The same nested in a struct initializer. */
// expect-exit: 42
int *p = (int[]){ 10, 20, 30 };
static const char *const *names = (const char *const[]){ "a", "bc" };
struct S { int a; int *v; } s = { 1, (int[]){ 7, 8 } };
int main(void) { return p[2] + names[1][1] - 'c' + s.v[1] + 4 == 42 ? 42 : 1; }
