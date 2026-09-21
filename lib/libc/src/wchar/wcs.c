/* Wide string and wide memory handling, C11 §7.29.4.
 *
 * The <string.h> functions over wchar_t. They are written out rather
 * than derived from the narrow ones because a wchar_t is four bytes:
 * `wcslen` is not `strlen / 4`, and `wmemchr` cannot be `memchr` --
 * a wide character's bytes may individually match the sought byte
 * without the character matching at all.
 *
 * `wcscoll` and `wcsxfrm` collate in the "C" locale, where the order is
 * the code point order. That is `wcscmp`, and saying so is better than
 * a separate copy that would be identical.
 */
#include <wchar.h>
#include <stddef.h>

size_t wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t wcsnlen(const wchar_t *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i])
        i++;
    return i;
}

wchar_t *wcscpy(wchar_t *__restrict d, const wchar_t *__restrict s)
{
    wchar_t *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

wchar_t *wcsncpy(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n)
{
    wchar_t *r = d;
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    /* The rest is PADDED with zeros, not merely terminated -- which is
     * strncpy's behaviour and is as surprising here as it is there, so
     * it is preserved rather than quietly improved. */
    for (; i < n; i++)
        d[i] = 0;
    return r;
}

wchar_t *wcscat(wchar_t *__restrict d, const wchar_t *__restrict s)
{
    wcscpy(d + wcslen(d), s);
    return d;
}

wchar_t *wcsncat(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n)
{
    wchar_t *p = d + wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++)
        p[i] = s[i];
    p[i] = 0;                     /* always terminated, unlike wcsncpy */
    return d;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    /* Compared as the unsigned values of the code points: a wchar_t is
     * signed here, and a signed comparison would order U+80000000 --
     * which cannot occur -- before U+0041. Unsigned is what the
     * standard means by "the values of the characters". */
    return (unsigned)*a < (unsigned)*b ? -1 : (unsigned)*a > (unsigned)*b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned)a[i] < (unsigned)b[i] ? -1 : 1;
        if (!a[i])
            break;
    }
    return 0;
}

/* The "C" locale collates by code point, so this IS wcscmp. */
int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }

size_t wcsxfrm(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n)
{
    size_t len = wcslen(s);
    if (len < n)
        wcscpy(d, s);
    else if (n)
        d[0] = 0;
    return len;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (; *s; s++)
        if (*s == c)
            return (wchar_t *)s;
    return c == 0 ? (wchar_t *)s : NULL;   /* the NUL is findable */
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *last = NULL;
    for (;; s++) {
        if (*s == c)
            last = s;
        if (!*s)
            break;
    }
    return (wchar_t *)last;
}

wchar_t *wcsstr(const wchar_t *h, const wchar_t *n)
{
    if (!*n)
        return (wchar_t *)h;      /* an empty needle is found at once */
    for (; *h; h++) {
        const wchar_t *a = h, *b = n;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return (wchar_t *)h;
    }
    return NULL;
}

static int in_set(wchar_t c, const wchar_t *set)
{
    for (; *set; set++)
        if (*set == c)
            return 1;
    return 0;
}

size_t wcsspn(const wchar_t *s, const wchar_t *set)
{
    size_t i = 0;
    while (s[i] && in_set(s[i], set))
        i++;
    return i;
}

size_t wcscspn(const wchar_t *s, const wchar_t *set)
{
    size_t i = 0;
    while (s[i] && !in_set(s[i], set))
        i++;
    return i;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set)
{
    for (; *s; s++)
        if (in_set(*s, set))
            return (wchar_t *)s;
    return NULL;
}

/* The reentrant form, which is the only one C11 has for wide strings --
 * the narrow strtok's hidden state was a mistake nobody repeated. */
wchar_t *wcstok(wchar_t *__restrict s, const wchar_t *__restrict sep,
                wchar_t **__restrict save)
{
    if (!s)
        s = *save;
    if (!s)
        return NULL;
    s += wcsspn(s, sep);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    wchar_t *end = s + wcscspn(s, sep);
    if (*end) {
        *end = 0;
        *save = end + 1;
    } else {
        *save = NULL;
    }
    return s;
}

wchar_t *wmemcpy(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return d;
}

wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n)
{
    /* Backwards when the regions overlap the wrong way, exactly as
     * memmove: the forward loop would overwrite source it has not read
     * yet. */
    if (d < s || d >= s + n) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i-- > 0;)
            d[i] = s[i];
    }
    return d;
}

wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = c;
    return d;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return (unsigned)a[i] < (unsigned)b[i] ? -1 : 1;
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == c)
            return (wchar_t *)(s + i);
    return NULL;
}
