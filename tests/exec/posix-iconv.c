/* iconv: 59 conversions, each one's result, errno, input left over and
 * output bytes as glibc 2.36 gives them (two rows marked G are where
 * glibc departs from Unicode or reads fewer names), a reset, and an
 * unknown encoding. Rows marked H assume a little-endian machine, as
 * glibc's unmarked UTF-16/UTF-32/UCS-2 follow the machine's order, and
 * W a 4-byte wchar_t: elsewhere they are skipped. The whole //TRANSLIT
 * table was compared with glibc's, code point by code point, when it
 * was made; a few of its entries are here. */
// expect-exit: 42
// needs-libc: <iconv.h>, from lib/libc
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define G 1                    /* glibc answers otherwise */
#define H 2                    /* little-endian machines */
#define W 4                    /* a 4-byte little-endian wchar_t */

struct k {
    const char *to, *from, *in;
    size_t n, room;
    int flags;
    const char *want;
};

#define I(to, from, in, n, room, f, want) { to, from, in, n, room, f, want }
static const struct k g_k[] = {
    I("UTF-16", "UTF-8", "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10, 64, H, "0 - left=0 out=fffe6100e900ac203dd800de"),
    I("UTF-16LE", "UTF-8", "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10, 64, 0, "0 - left=0 out=6100e900ac203dd800de"),
    I("UTF-16BE", "UTF-8", "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10, 64, 0, "0 - left=0 out=006100e920acd83dde00"),
    I("UTF-32", "UTF-8", "a\xf0\x9f\x98\x80", 5, 64, H, "0 - left=0 out=fffe00006100000000f60100"),
    I("UTF-32LE", "UTF-8", "a\xf0\x9f\x98\x80", 5, 64, 0, "0 - left=0 out=6100000000f60100"),
    I("UTF-32BE", "UTF-8", "a\xf0\x9f\x98\x80", 5, 64, 0, "0 - left=0 out=000000610001f600"),
    I("UCS-2", "UTF-8", "a\xc3\xa9", 3, 64, H, "0 - left=0 out=6100e900"),
    I("UCS-2", "UTF-8", "a\xf0\x9f\x98\x80", 5, 64, H, "-1 EILSEQ left=4 out=6100"),
    I("UCS-2LE", "UTF-8", "a\xc3\xa9", 3, 64, 0, "0 - left=0 out=6100e900"),
    I("UCS-2BE", "UTF-8", "a\xc3\xa9", 3, 64, 0, "0 - left=0 out=006100e9"),
    I("UCS-4", "UTF-8", "a\xc3\xa9", 3, 64, 0, "0 - left=0 out=00000061000000e9"),
    I("UCS-4LE", "UTF-8", "a\xc3\xa9", 3, 64, 0, "0 - left=0 out=61000000e9000000"),
    I("WCHAR_T", "UTF-8", "a\xc3\xa9", 3, 64, W, "0 - left=0 out=61000000e9000000"),
    I("UTF-8", "WCHAR_T", "a\0\0\0\xe9\0\0\0", 8, 64, W, "0 - left=0 out=61c3a9"),
    I("UTF-8", "UTF-16", "\xff\xfe" "a\0\xe9\0", 6, 64, 0, "0 - left=0 out=61c3a9"),
    I("UTF-8", "UTF-16", "\xfe\xff" "\0a\0\xe9", 6, 64, 0, "0 - left=0 out=61c3a9"),
    I("UTF-8", "UTF-16", "\0a\0\xe9", 4, 64, H, "0 - left=0 out=e68480eea480"),
    I("UTF-8", "UTF-16LE", "\xff\xfe" "a\0", 4, 64, 0, "0 - left=0 out=efbbbf61"),
    I("UTF-8", "UTF-16LE", "=\xd8\0\xde", 4, 64, 0, "0 - left=0 out=f09f9880"),
    I("UTF-8", "UTF-16LE", "=\xd8", 2, 64, 0, "-1 EINVAL left=2 out="),
    I("UTF-8", "UTF-16LE", "\0\xde" "a\0", 4, 64, 0, "-1 EILSEQ left=4 out="),
    I("UTF-8", "UTF-16LE", "a", 1, 64, 0, "-1 EINVAL left=1 out="),
    I("UTF-8", "UTF-32", "\xff\xfe\0\0" "a\0\0\0", 8, 64, 0, "0 - left=0 out=61"),
    I("UTF-8", "UTF-32LE", "\0\0\x11\0", 4, 64, 0, "-1 EILSEQ left=4 out="),
    I("UTF-8", "UCS-2", "a\0", 2, 64, H, "0 - left=0 out=61"),
    I("UTF-8", "UCS-4", "\0\0\0a", 4, 64, 0, "0 - left=0 out=61"),
    I("ASCII", "UTF-8", "ab\xc3\xa9" "c", 5, 64, 0, "-1 EILSEQ left=3 out=6162"),
    I("ASCII//TRANSLIT", "UTF-8", "ab\xc3\xa9" "c", 5, 64, 0, "1 - left=0 out=61623f63"),
    I("ASCII//IGNORE", "UTF-8", "ab\xc3\xa9" "c", 5, 64, 0, "-1 EILSEQ left=0 out=616263"),
    I("UTF-8", "ASCII", "a\x80" "b", 3, 64, 0, "-1 EILSEQ left=2 out=61"),
    I("ISO-8859-1", "UTF-8", "\xc3\xa9\xe2\x82\xac", 5, 64, 0, "-1 EILSEQ left=3 out=e9"),
    I("ISO-8859-15", "UTF-8", "\xc3\xa9\xe2\x82\xac", 5, 64, 0, "0 - left=0 out=e9a4"),
    I("ISO-8859-15", "UTF-8", "\xc2\xa4", 2, 64, 0, "-1 EILSEQ left=2 out="),
    I("UTF-8", "ISO-8859-15", "\xa4\xbe\xe9", 3, 64, 0, "0 - left=0 out=e282acc5b8c3a9"),
    I("CP1252", "UTF-8", "\xe2\x82\xac\xe2\x80\x99\xc3\xa9", 8, 64, 0, "0 - left=0 out=8092e9"),
    I("UTF-8", "CP1252", "\x80\x92\xe9", 3, 64, 0, "0 - left=0 out=e282ace28099c3a9"),
    I("UTF-8", "CP1252", "a\x81", 2, 64, 0, "-1 EILSEQ left=1 out=61"),
    I("UTF-8", "WINDOWS-1252", "\x9f", 1, 64, 0, "0 - left=0 out=c5b8"),
    I("UTF-16", "latin1", "\xe9", 1, 64, H, "0 - left=0 out=fffee900"),
    I("utf8", "Utf_8", "\xc3\xa9", 2, 64, G, "0 - left=0 out=c3a9"), /* glibc: EINVAL, reading no name it has not listed */
    I("UTF-8", "UTF-8", "a\xc3", 2, 64, 0, "-1 EINVAL left=1 out=61"),
    I("UTF-8", "UTF-8", "a\xc3(", 3, 64, 0, "-1 EILSEQ left=2 out=61"),
    I("UTF-8", "UTF-8", "\xc0\xaf", 2, 64, 0, "-1 EILSEQ left=2 out="),
    I("UTF-8", "UTF-8", "\xe0\x80\xaf", 3, 64, 0, "-1 EILSEQ left=3 out="),
    I("UTF-8", "UTF-8", "\xed\xa0\x80", 3, 64, 0, "-1 EILSEQ left=3 out="),
    I("UTF-8", "UTF-8", "\xf4\x90\x80\x80", 4, 64, G, "-1 EILSEQ left=4 out="), /* glibc takes UTF-8 past U+10FFFF */
    I("UTF-8", "UTF-8", "\xf4\x8f\xbf\xbf", 4, 64, 0, "0 - left=0 out=f48fbfbf"),
    I("UTF-8", "UTF-8", "\xef\xbb\xbf" "a", 4, 64, 0, "0 - left=0 out=efbbbf61"),
    I("UTF-8", "UTF-8", "\x80", 1, 64, 0, "-1 EILSEQ left=1 out="),
    I("UTF-8//IGNORE", "UTF-8", "a\x80" "b", 3, 64, 0, "-1 EILSEQ left=0 out=6162"),
    I("UTF-8", "UTF-8", "a\xc3\xa9\xe2\x82\xac", 6, 3, 0, "-1 E2BIG left=3 out=61c3a9"),
    I("UTF-16", "UTF-8", "ab", 2, 3, H, "-1 E2BIG left=2 out=fffe"),
    I("UTF-16", "UTF-8", "ab", 2, 1, H, "-1 E2BIG left=2 out="),
    I("LATIN1", "UTF-8", "a\xe2\x82\xac" "b", 5, 64, 0, "-1 EILSEQ left=4 out=61"),
    I("LATIN1//TRANSLIT", "UTF-8", "a\xe2\x82\xac" "b", 5, 64, 0, "1 - left=0 out=6145555262"),
    I("ASCII//TRANSLIT", "UTF-8", "\xe2\x82\xac\xc2\xbd\xf0\x9d\x90\x80\xe2\x80\x8b" "a", 13, 64, 0, "4 - left=0 out=45555220312f32204161"),
    I("UTF-16//TRANSLIT", "UTF-8", "\xe2\x82\xac", 3, 64, H, "0 - left=0 out=fffeac20"),
    I("LATIN1//TRANSLIT//IGNORE", "UTF-8", "\xe2\x82\xac" "\xe4\xb8\x80", 6, 64, 0, "2 - left=0 out=4555523f"),
    I("UTF-16", "UTF-8", "", 0, 64, H, "0 - left=0 out="),
};

static const char *err_name(int e)
{
    return e == EILSEQ ? "EILSEQ" : e == EINVAL ? "EINVAL" :
           e == E2BIG ? "E2BIG" : "?";
}

int main(void)
{
    const uint16_t one = 1;
    int little = *(const unsigned char *)&one;
    int bad = 0;
    for (size_t i = 0; i < sizeof g_k / sizeof g_k[0]; i++) {
        const struct k *k = &g_k[i];
        if ((k->flags & (H | W)) && !little)
            continue;
        if ((k->flags & W) && sizeof(wchar_t) != 4)
            continue;
#ifdef __GLIBC__
        if (k->flags & G)
            continue;
#endif
        iconv_t cd = iconv_open(k->to, k->from);
        if (cd == (iconv_t)-1) {
            printf("%s <- %s: no converter\n", k->to, k->from);
            bad++;
            continue;
        }
        char ob[64], got[256], *in = (char *)k->in, *out = ob;
        size_t il = k->n, ol = k->room;
        errno = 0;
        size_t r = iconv(cd, &in, &il, &out, &ol);
        int n = snprintf(got, sizeof got, "%ld %s left=%zu out=", (long)r,
                         r != (size_t)-1 ? "-" : err_name(errno), il);
        for (char *p = ob; p < out; p++)
            n += snprintf(got + n, sizeof got - n, "%02x", (unsigned char)*p);
        iconv_close(cd);
        if (strcmp(got, k->want)) {
            printf("%s <- %s #%zu: %s, want %s\n", k->to, k->from, i, got,
                   k->want);
            bad++;
        }
    }
    if (bad)
        return 1;

    errno = 0;
    if (iconv_open("NOSUCH", "UTF-8") != (iconv_t)-1 || errno != EINVAL)
        return 2;

    /* after a reset UTF-16 writes its byte-order mark again */
    iconv_t cm = iconv_open("UTF-16", "UTF-8");
    char ob[16], *out = ob, *in = "a";
    size_t il = 1, ol = sizeof ob;
    if (cm == (iconv_t)-1 ||
        iconv(cm, &in, &il, &out, &ol) || iconv(cm, 0, 0, &out, &ol))
        return 3;
    in = "b";
    il = 1;
    if (iconv(cm, &in, &il, &out, &ol) || out - ob != 8 ||
        memcmp(ob, ob + 4, 2) || ((unsigned char)ob[0] != 0xFF && little))
        return 4;
    iconv_close(cm);
    return 42;
}
