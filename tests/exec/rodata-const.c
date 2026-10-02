/* Const objects live in .rodata -- in flash on a microcontroller, copied
 * to RAM by the startup only on AVR, whose loads cannot reach flash --
 * and the ones holding addresses carry their relocations there: a table
 * of function pointers, of strings, a pointer to another const object.
 * Each is read back here, on every board. */
// expect-exit: 42
struct op { const char *name; int (*fn)(int); int k; };
static int dbl(int x) { return 2 * x; }
static int inc(int x) { return x + 1; }
static const struct op ops[] = { { "dbl", dbl, 3 }, { "inc", inc, 4 } };
const char *const words[] = { "zero", "one", "two" };
static const int vals[3] = { 10, 20, 12 };
typedef const struct cfg { int a; const int *p; } cfg_t;
cfg_t cfg = { 5, &vals[2] };
const int *const pv = &vals[1];
const char *where = "data";          /* a pointer to const: writable */
int main(void)
{
    static const int local[] = { 3, 4, 6 };
    int s = 0;
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
        s += ops[i].fn(ops[i].k) + (ops[i].name[0] == 'd' || ops[i].name[0] == 'i') - 1;
    s += words[2][1] == 'w';                         /* 11 + 1 */
    s += *cfg.p + cfg.a;                             /* + 17 */
    s += *pv - 20;                                   /* + 0 */
    for (unsigned i = 0; i < 3; i++)
        s += local[i];                               /* + 13 */
    where = words[1];
    return s == 42 && where[0] == 'o' ? 42 : 1;
}
