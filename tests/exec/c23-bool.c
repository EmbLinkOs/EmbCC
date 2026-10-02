/* C23 makes bool, true and false keywords, and gcc 15 compiles C23 by
 * default: code written against it uses all three without <stdbool.h>.
 * EmbCC takes them as names of last resort -- see c23-bool-old.c for a
 * unit that defines them itself, and c23-bool-stdbool.c for the macros.
 * As keywords, true has type bool (sizeof 1) and #if true is taken. */
// expect-exit: 42
// no-gcc-reference: C23 keywords; the reference builds strict C11
static bool g = true;
static bool garr[3] = { true, false, true };
static bool flip(bool b) { return !b; }
int main(void)
{
    bool a = false;
    int bad = 0;
    if (sizeof(true) != 1 || sizeof(bool) != 1) bad |= 1;
    if (_Generic(true, bool: 1, default: 0) != 1) bad |= 2;
    if (!g || garr[1] || !garr[2]) bad |= 4;
    if (flip(a) != true || (bool)5 != true || (int)true != 1 || false != 0) bad |= 8;
#if true
    int pp = 1;
#else
    int pp = 0;
#endif
    if (!pp) bad |= 16;
    return bad ? 100 + bad : 42;
}
