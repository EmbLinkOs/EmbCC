/* GNU C initializes a static struct's flexible array member, and the
 * object grows to hold the elements (sizeof stays the struct's). EmbCC
 * kept the struct's size: `g` was a 4-byte symbol, the elements were
 * dropped, and reading them returned whatever followed -- 18, not 42. */
// expect-exit: 42
struct fam { int n; int d[]; };
static struct fam g = { 3, { 10, 20, 12 } };
struct fam g2 = { 1 };
static const struct fam g3 = { 2, { 5, 6 } };
struct cfam { char c; short s[]; };
struct cfam g4 = { 1, { 2, 3, 4 } };
int f(void) { static struct fam sl = { 1, { 41 } }; return sl.d[0]; }
int main(void) {
    int bad = 0;
    if (g.d[0] + g.d[1] + g.d[2] != 42 || sizeof g != sizeof(int)) bad |= 1;
    if (g2.n != 1) bad |= 2;
    if (g3.d[1] != 6) bad |= 4;
    if (g4.s[2] != 4) bad |= 8;
    if (f() != 41) bad |= 16;
    return bad ? bad : 42;
}
