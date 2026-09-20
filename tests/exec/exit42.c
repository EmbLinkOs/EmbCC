/* The M1 acceptance program, verbatim from docs/design/roadmap.md. */
// expect-exit: 42
static int twice(int x) { return x + x; }
int main(void) { return twice(21); }
