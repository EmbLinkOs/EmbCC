/* A scalar's initializer may be braced, `int x = { 5 };` (C89), and C23
 * adds the empty `= {}`, zero of the type -- for a global, a static
 * local, an automatic, a pointer, a floating type and a compound
 * literal. EmbCC took braces only for aggregates: "a brace initializer
 * cannot appear here" in a function, "expected an expression" at file
 * scope. */
// expect-exit: 42
int g = { 7 };
static double gd = {};
static int *gp = {};
const int gc = { 5 };
int main(void)
{
    int x = { 5 };
    int z = {};
    double d = {};
    int *p = {};
    static int s = { 3 };
    static long sz = {};
    int q = { 4 };
    int *cl = &(int){};
    float f = { 2.5f };
    return g + (int)gd + (gp == 0) + gc + x + z + (int)d + (p == 0) + s + (int)sz + q + *cl + (int)(f * 2) + 11;
}
