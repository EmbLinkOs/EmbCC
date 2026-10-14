/* The classic offsetof, `((size_t)&((T *)0)->m)`, in every place C asks
 * for an integer constant expression: a _Static_assert, an array size, an
 * enumerator, a case label and a static initializer. Headers written for
 * no particular compiler spell offsetof this way -- the macOS SDK's
 * <stddef.h> for a compiler without __GNUC__, and many a vendor HAL --
 * and gcc and clang fold it. EmbCC folded only __builtin_offsetof, and
 * EmbLinkRTOS's _Static_assert on a struct offset stopped the build.
 * Also a member at a constant address that is not 0, which a register
 * map is: `&((regs *)0x40000000)->ctrl`.
 */
// expect-exit: 42
typedef unsigned long size_t_;

struct s {
    int a, b;
    char c[3];
    struct { short x; } in;
    long long q;
};
struct regs { volatile unsigned ctrl, stat, data[4]; };

#define OFF(T, m) ((size_t_)&((T *)0)->m)

_Static_assert(OFF(struct s, b) == sizeof(int), "b follows a");
_Static_assert(OFF(struct s, c[2]) == 2 * sizeof(int) + 2, "an element of c");
_Static_assert(OFF(struct regs, data[2]) == 4 * sizeof(unsigned), "an element of data");

static char room[OFF(struct s, in.x) + 1];
enum { E_IN = (int)OFF(struct s, in) };
static const size_t_ off_q = OFF(struct s, q);
static const size_t_ stat_reg = (size_t_)&((struct regs *)0x40000000)->stat;

static int which(size_t_ v)
{
    switch (v) {
    case OFF(struct s, b): return 1;
    case OFF(struct s, in): return 2;
    default: return 0;
    }
}

int main(void)
{
    int r = 0;
    if (sizeof room == OFF(struct s, in) + 1) r += 10;
    if (E_IN == (int)OFF(struct s, in)) r += 10;
    if (off_q > OFF(struct s, in) && off_q < sizeof(struct s)) r += 10;
    if (stat_reg == 0x40000000 + sizeof(unsigned)) r += 6;
    if (which(OFF(struct s, b)) == 1 && which(OFF(struct s, in)) == 2) r += 6;
    return r;
}
