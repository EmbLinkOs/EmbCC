/* A storage class after the type specifiers, or among them, as C allows
 * (obsolescent, C11 6.11.5, and GCC's -Wold-style-declaration): `const
 * static uint8_t table[]` is common in vendor code. EmbCC stopped with
 * "expected a type before 'static'" -- and `int static n;`, `unsigned
 * extern int c;`, `long typedef L;` and `int register r` the same way.
 * Each must mean what the leading form means: a static local keeps its
 * value, a static object has internal linkage, a typedef names a type.
 */
// expect-exit: 42
const static unsigned char table[] = { 1, 2, 3 };
int const static k = 4;
unsigned extern int c;
unsigned int c = 7;
struct s { int x; } extern d;
struct s d = { 3 };
volatile static unsigned char e = 5;
long typedef L;
L lg = 1;                               /* a type, not a variable */
int static inline twice(int v) { return 2 * v; }

static int counter(void)
{
    int static n;
    unsigned static int m = 10;
    return ++n + (int)m++;
}

int main(void)
{
    int register r = table[2];          /* 3 */
    const static int local = 2;
    int got = counter();                /* 11 */
    got = counter();                    /* 13 */
    if (sizeof(L) != sizeof(long)) return 1;
    return got + r + k + (int)c + d.x + e + twice(local) + table[0] + (int)lg + 1;
}
