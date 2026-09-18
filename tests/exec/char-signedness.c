/* Plain `char`, `wchar_t` and `char32_t` take their signedness from the
 * TARGET: char is signed on x86-64 and unsigned on aarch64 (AAPCS64), wchar_t
 * is int on x86-64 ELF and unsigned int on aarch64 ELF, char32_t is unsigned
 * everywhere. Each check compares what the type system DOES against what the
 * target's own predefined macros SAY — the two disagreed on aarch64 until
 * this test existed, silently changing any code that read a char as signed.
 */
// expect-exit: 42
int main(void)
{
    char c = (char)0xFE;               /* -2 if signed, 254 if unsigned */
#ifdef __CHAR_UNSIGNED__
    if (c != 254) return 1;
    if ((char)-1 < 0) return 2;
#else
    if (c != -2) return 1;
    if (!((char)-1 < 0)) return 2;
#endif

    /* a string literal's elements are plain char */
    const char *s = "\xFE";
    if (s[0] != c) return 3;

    /* L"" elements are wchar_t: __WCHAR_TYPE__ says which */
    __WCHAR_TYPE__ w = L"\xFFFFFFFF"[0];
    int wide_is_unsigned = L"\xFFFFFFFF"[0] > 0;
    if (wide_is_unsigned != ((__WCHAR_TYPE__)-1 > 0)) return 4;
    (void)w;

    /* U"" elements are char32_t, unsigned on every target */
    if (!(U"\xFFFFFFFF"[0] > 0)) return 5;
    return 42;
}
