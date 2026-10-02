/* The M1 acceptance program (docs/internals/status.md, milestone M1). */
// expect-exit: 42
static int twice(int x) { return x + x; }
int main(void) { return twice(21); }
