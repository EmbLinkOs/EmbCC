/* An enumeration with no negative value has type unsigned int, as GCC
 * documents ("Normally, the type is unsigned int if there are no negative
 * values in the enumeration, otherwise int") and clang agrees; its
 * CONSTANTS are int, as C says. EmbCC made the type int, so code ported
 * from GCC computed differently wherever the sign shows: an enum variable
 * stepped below zero, compared, or widened.
 */
// expect-exit: 42
enum color { RED, GREEN, BLUE };
enum level { LOW = -1, MID, HIGH };

static volatile int vz;

int main(void)
{
    int r = 0;
    enum color c = RED;
    c = (enum color)(c - 1 + vz);              /* unsigned: wraps */
    if (c > BLUE) r += 10;
    if ((long long)c > 0) r += 8;              /* zero-extended */
    enum level l = LOW;
    l = (enum level)(l + vz);
    if (l < MID) r += 10;                      /* int: negative */
    if (RED - 1 < 0) r += 7;                   /* the constants are int */
    if ((enum color)-1 > 0 && !((enum level)-1 > 0)) r += 7;
    return r;
}
