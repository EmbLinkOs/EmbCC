/* strfromd, strfromf, strfroml, C23 §7.24.1.3: snprintf of one value.
 * The format is "%", an optional precision, and one of aAeEfFgG --
 * anything else is undefined, and is refused here with -1 rather than
 * handed to printf with an argument it does not describe. strfroml puts
 * the L in. A file of their own, so a program that prints does not link
 * them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int strfrom(char *restrict s, size_t n, const char *restrict fmt,
                   int is_long, double d, long double ld)
{
    char f[16];
    size_t k = 0;
    if (fmt[0] != '%')
        return -1;
    f[k++] = '%';
    size_t i = 1;
    if (fmt[i] == '.') {
        f[k++] = fmt[i++];
        while (fmt[i] >= '0' && fmt[i] <= '9' && k < sizeof f - 3)
            f[k++] = fmt[i++];
    }
    if (!fmt[i] || !strchr("aAeEfFgG", fmt[i]) || fmt[i + 1])
        return -1;
    if (is_long)
        f[k++] = 'L';
    f[k++] = fmt[i];
    f[k] = 0;
    return is_long ? snprintf(s, n, f, ld) : snprintf(s, n, f, d);
}

int strfromd(char *restrict s, size_t n, const char *restrict fmt, double fp)
{
    return strfrom(s, n, fmt, 0, fp, 0);
}

int strfromf(char *restrict s, size_t n, const char *restrict fmt, float fp)
{
    return strfrom(s, n, fmt, 0, fp, 0);
}

int strfroml(char *restrict s, size_t n, const char *restrict fmt,
             long double fp)
{
    return strfrom(s, n, fmt, 1, 0, fp);
}
