/* sizeof of a wide or prefixed string literal is an integer constant
 * expression, as it is in GCC and clang: a _Static_assert or an array
 * size may use it. EmbCC folded only the plain literal at parse time and
 * refused the rest ("_Static_assert needs a constant integer expression"),
 * so a header asserting its UTF-16 table's size did not compile. Each
 * size is one element per code unit, the NUL included -- a surrogate
 * pair is two -- whatever wchar_t's width is (AVR's two bytes). */
// expect-exit: 42

#define WSZ sizeof(__WCHAR_TYPE__)

_Static_assert(sizeof("ab") == 3, "plain");
_Static_assert(sizeof(u8"é") == 3, "u8: two UTF-8 bytes and the NUL");
_Static_assert(sizeof(L"ab") == 3 * WSZ, "L");
_Static_assert(sizeof(u"ab") == 3 * sizeof(__CHAR16_TYPE__), "u");
_Static_assert(sizeof(u"\U0001F600") == 3 * sizeof(__CHAR16_TYPE__),
               "u: a surrogate pair and the NUL");
_Static_assert(sizeof(U"ab") == 3 * sizeof(__CHAR32_TYPE__), "U");
_Static_assert(sizeof(L"ab" "cd") == 5 * WSZ, "concatenated");

char by_u32[sizeof(U"xyz")];                /* an array sized by one */
static volatile int sink;

int main(void)
{
    /* and the same sizes at run time, where sema has always typed them */
    const __CHAR16_TYPE__ *p = u"\U0001F600";
    if (sizeof by_u32 != 4 * sizeof(__CHAR32_TYPE__)) return 1;
    if (sizeof(L"ab") != 3 * WSZ) return 2;
    if (p[0] != 0xD83D || p[1] != 0xDE00 || p[2] != 0) return 3;
    sink = (int)sizeof(u"ab");
    if (sink != (int)(3 * sizeof(__CHAR16_TYPE__))) return 4;
    return 42;
}
