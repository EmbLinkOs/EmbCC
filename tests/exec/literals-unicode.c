/* String and character literals past ASCII, on both targets. Until this
 * test, EmbCC stored each element of a wide literal as ONE BYTE widened
 * afterwards — L"\x263A" held 0x3A, L"é" held two elements 0xC3 0xA9 — and
 * rejected \u / \U outright. Literals are now decoded into code points and
 * raw units (lex.c lit_decode) and encoded once at their final width
 * (lit_encode): UTF-8, UTF-16 with surrogates, or UTF-32. gcc referees every
 * value below, on each target.
 */
// expect-exit: 42
typedef __WCHAR_TYPE__ wchar;
typedef unsigned short c16;
typedef unsigned int c32;

/* the global-initializer path (the driver's relocation interning) */
static const c32 *g32 = U"\U0001F600z";
static const c16 g16[] = u"\U0001F600";
static const wchar gw[] = L"é\x263A";

#if '\x41' != 65 || L'é' != 0xE9
#error "#if character constants disagree with the code"
#endif

static int same(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

int main(void)
{
    /* wide: full-width escapes, \u, and UTF-8 source decoded to one element */
    const wchar *w = L"\x263AéZé";
    if ((c32)w[0] != 0x263A || (c32)w[1] != 0xE9 || (c32)w[2] != 'Z' ||
        (c32)w[3] != 0xE9 || w[4] != 0) return 1;
    if (sizeof(L"ab") != 3 * sizeof(wchar)) return 2;

    /* char32_t: a 4-byte UTF-8 source character is one element */
    const c32 *u32 = U"😀\U0001F600";
    if (u32[0] != 0x1F600 || u32[1] != 0x1F600 || u32[2] != 0) return 3;

    /* char16_t: past the BMP becomes a surrogate pair */
    const c16 *u16 = u"é\U0001F600";
    if (u16[0] != 0xE9 || u16[1] != 0xD83D || u16[2] != 0xDE00 || u16[3] != 0)
        return 4;
    if (sizeof(u"ab") != 6) return 5;

    /* narrow and u8: \u encodes as UTF-8, source UTF-8 passes through,
     * \x stays a single raw byte */
    if (!same("é", "\xC3\xA9", 3) || !same(u8"é", "\xC3\xA9", 3) ||
        !same("é", "\xC3\xA9", 3) || sizeof("é") != 3) return 6;

    /* concatenation takes the widest prefix: the "é" is re-encoded as UTF-32 */
    const wchar *cat = L"a" "é";
    if ((c32)cat[0] != 'a' || (c32)cat[1] != 0xE9 || cat[2] != 0) return 7;

    /* array initializers read each element at its real width */
    wchar wa[] = L"é\U0001F600";
    if (sizeof wa != 3 * sizeof(wchar) || (c32)wa[0] != 0xE9 ||
        (c32)wa[1] != 0x1F600) return 8;

    /* globals */
    if (g32[0] != 0x1F600 || g32[1] != 'z') return 9;
    if (g16[0] != 0xD83D || g16[1] != 0xDE00 || sizeof g16 != 6) return 10;
    if ((c32)gw[0] != 0xE9 || (c32)gw[1] != 0x263A) return 11;

    /* character constants */
    if (L'é' != 0xE9 || U'\U0001F600' != 0x1F600 || u'é' != 0xE9) return 12;
    if ('\x41' != 65) return 13;
    /* a plain constant is its byte read as plain char: -1 or 255 by target */
    if ('\xFF' != (char)0xFF) return 14;
    return 42;
}
