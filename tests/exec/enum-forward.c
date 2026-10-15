/* A forward reference to an enum before its body (GNU C). */
// expect-exit: 42
int printf(const char *, ...);
enum later;
enum later *lp;
static inline int foo(void) { enum incomplete3 *ptr3 = 0; return ptr3 == 0; }
enum later { L0, L1 = 5 };
enum later lv = L1;
int main(void)
{
    lp = &lv;
    printf("%d %d\n", *lp, foo());
    return *lp == 5 && foo() ? 42 : 1;
}
