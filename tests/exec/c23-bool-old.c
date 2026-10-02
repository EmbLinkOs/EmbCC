/* Pre-C23 code that names its own bool, true and false keeps them: the
 * typedef and the enum constants are found before EmbCC's C23 fallback
 * (c23-bool.c), and a local `false` shadows the enum constant. */
// expect-exit: 42
typedef int bool;
enum { false, true };
static int count(bool x) { return x * 2; }
int main(void)
{
    bool b = 3;                      /* an int here, not _Bool */
    int r = count(b) + true;         /* 6 + 1 */
    { int false = 40; r += false; }  /* a local wins */
    return r == 47 && sizeof(bool) == sizeof(int) && sizeof(true) == sizeof(int) ? 42 : 1;
}
