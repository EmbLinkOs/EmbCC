/* K&R definitions and functions with no prototype, as C17 and earlier
 * have them (and GCC in every standard): the parameters typed by the
 * declarations before the body, an argument passed as the promotions
 * leave it (a float a double, a char an int) and converted at entry,
 * `int f()` called with arguments, a later prototype, and a pointer to a
 * function with no prototype. */
// expect-exit: 42
int add(a, b) int a; int b; { return a + b; }
double half(x) float x; { return x / 2; }
char twice(c) char c; { return (char)(c * 2); }
long sum3(a, b, c) long a; int b; short c; { return a + b + c; }
int noargs() { return 7; }
int later();
int (*fp)();
int use_later(void) { return later(3, 4); }
int later(int x, int y) { return x * y; }
int main(void)
{
    fp = add;
    if (add(40, 2) != 42) return 1;
    if (half(5.0f) != 2.5) return 2;
    if (twice(21) != 42) return 3;
    if (sum3(30L, 10, 2) != 42) return 4;
    if (noargs() != 7) return 5;
    if (use_later() != 12) return 6;
    if (fp(20, 22) != 42) return 7;
    return 42;
}
