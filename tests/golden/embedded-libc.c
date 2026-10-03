/* EmbCC's C library, running on a board.
 *
 * lib/libc was compiled for every embedded target and run on none, and
 * that is how sqrt, hypot and cabs went on calling themselves forever on
 * all four of them. This program goes through the library's surface --
 * formatting, scanning, conversions, strings, sorting, the allocator, the
 * math -- and prints what it gets.
 *
 * The reference is the SAME library built by EmbCC for x86-64: one source,
 * so a difference is the compiler's for that target, not the library's
 * and not the host's. Every floating-point result is printed with %a, so
 * an ulp shows. Nothing here depends on what may differ between targets:
 * no `long` (four bytes or eight), no pointer values, no long double.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __x86_64__
/* The bare-metal backend's console: the harness's own output routine. */
extern void writec(int c);
long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    (void)fd;
    for (unsigned long i = 0; i < n; i++)
        writec(p[i]);
    return (long)n;
}
#endif

static int icmp(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return a < b ? -1 : a > b;
}

static int scmp(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

static volatile double vd[] = {
    0.0, -0.0, 0.5, 1.0, -1.5, 2.0, 3.141592653589793, 10.0, 100.25,
    1e-5, 1e10, -123.456, 0.1, 2.5e-310, 1.7976931348623157e308
};

static void formatting(void)
{
    char b[64];
    int n;

    printf("[%d] [%i] [%u] [%o] [%x] [%X] [%c] [%s] [%%]\n",
           -42, 17, 4000000000u, 511, 48879, 48879, 'Q', "str");
    printf("[%5d] [%-5d] [%05d] [%+d] [% d] [%.3d] [%*d] [%-*d]\n",
           42, 42, 42, 42, 42, 7, 6, 99, 6, 99);
    printf("[%#o] [%#x] [%#X] [%hhd] [%hd] [%hu] [%lld] [%llu] [%llx]\n",
           8, 255, 255, (signed char)-1, (short)-32768, (unsigned short)65535,
           -9223372036854775807LL - 1, 18446744073709551615ULL,
           0x0123456789abcdefULL);
    printf("[%zu] [%.2s] [%10s] [%-10s] [%.0s]\n",
           sizeof(int), "abcdef", "right", "left", "gone");
    for (unsigned i = 0; i < sizeof vd / sizeof vd[0]; i++) {
        double d = vd[i];
        if (i == 13 || i == 14) {
            printf("%e %g %a\n", d, d, d);
            continue;
        }
        printf("%f %.3f %e %.2E %g %G %.10g %a %+.1f %08.2f\n",
               d, d, d, d, d, d, d, d, d, d);
    }
    printf("%f %e %g %F\n", (double)INFINITY, -(double)INFINITY,
           (double)NAN, (double)INFINITY);
    /* long double through the varargs: x87 on the reference, binary128
     * on RISC-V, a double on Cortex-M -- so only values all three hold
     * exactly, which print the same in decimal */
    printf("%Lf %.3Le %Lg %.1Lf\n", 1.5L, 1234.5L, 0.25L,
           strtold("-6.125", NULL) * 2);
    n = snprintf(b, 8, "%d-%s", 123456, "xyz");
    printf("snprintf %d [%s]\n", n, b);
    n = snprintf(NULL, 0, "%.20f", 1.0 / 3.0);
    printf("measure %d\n", n);
    sprintf(b, "%08.3f|%-8.3e|", -3.14159, 2.71828);
    puts(b);
}

static void scanning(void)
{
    int a = 0, c = 0;
    unsigned u = 0;
    double d = 0;
    float f = 0;
    char w[16] = "";
    char ch = 0;
    int n = sscanf("  -17 0x1f 3.75e2 hello z 2.5", "%d %i %lf %15s %c %f",
                   &a, &c, &d, w, &ch, &f);
    printf("sscanf %d: %d %d %a %s %c %a\n", n, a, c, d, w, ch, (double)f);
    n = sscanf("255 077", "%x %o", &u, &a);
    printf("sscanf %d: %u %d\n", n, u, a);
    n = sscanf("12abc", "%d%3s", &a, w);
    printf("sscanf %d: %d %s\n", n, a, w);
}

static void conversions(void)
{
    static const char *const nums[] = {
        "0", "-0", "1", "3.5", "-2.25e-3", "1e308", "1e-320", "0x1.8p3",
        "inf", "-nan", "123456789012345678901234567890", "0.1",
        "2.2250738585072014e-308", "4.9e-324", "  +7.", "1e400", ".5e1x"
    };
    for (unsigned i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        char *end;
        errno = 0;
        double d = strtod(nums[i], &end);
        printf("strtod %s -> %a rest[%s] errno %d\n", nums[i], d, end,
               errno == ERANGE);
    }
    printf("strtol %lld %lld %lld\n", (long long)strtol("-0x2a", NULL, 0),
           (long long)strtol("0777", NULL, 0), (long long)strtol("z", NULL, 36));
    printf("strtoul %llu strtoll %lld strtoull %llu\n",
           (unsigned long long)strtoul("4294967295", NULL, 10),
           strtoll("-9223372036854775808", NULL, 10),
           strtoull("18446744073709551615", NULL, 10));
    errno = 0;
    long long big = strtoll("99999999999999999999", NULL, 10);
    printf("overflow %lld %d\n", big, errno == ERANGE);
    printf("atoi %d atof %a\n", atoi("  -123xyz"), atof("6.02e23"));
}

static void strings(void)
{
    char a[64], b[64];
    const char *hay = "the quick brown fox jumps over the lazy dog";
    strcpy(a, "hello");
    strcat(a, ", world");
    strncpy(b, a, 5);
    b[5] = 0;
    printf("%s|%s|%zu|%d|%d|%d\n", a, b, strlen(a), strcmp(a, b) > 0,
           strncmp(a, b, 5), memcmp("abc", "abd", 3) < 0);
    printf("%s|%s|%s|%s\n", strchr(hay, 'q'), strrchr(hay, 'o'),
           strstr(hay, "lazy"), strpbrk(hay, "xyz"));
    printf("%zu %zu\n", strspn(hay, "the "), strcspn(hay, "z"));
    memset(a, '*', 10);
    a[10] = 0;
    memmove(a + 2, a, 5);
    memcpy(b, "0123456789", 11);
    memmove(b + 3, b, 6);
    printf("%s|%s|%d\n", a, b, memchr(b, 'x', 10) == NULL);
    strcpy(a, "a,b;;c,d");
    for (char *t = strtok(a, ",;"); t; t = strtok(NULL, ",;"))
        printf("<%s>", t);
    printf("\n");
    for (int c = 0; c < 128; c += 9)
        printf("%d%d%d%d%d%c%c", !!isalpha(c), !!isdigit(c), !!isspace(c),
               !!isupper(c), !!ispunct(c), toupper(c) == c ? '=' : 'u',
               tolower(c) == c ? '=' : 'l');
    printf("\n");
}

static void sorting(void)
{
    int a[23];
    const char *s[] = { "pear", "apple", "fig", "kiwi", "date", "banana" };
    unsigned x = 12345;
    for (int i = 0; i < 23; i++) {
        x = x * 1103515245u + 12345u;
        a[i] = (int)(x >> 16) % 1000 - 500;
    }
    qsort(a, 23, sizeof a[0], icmp);
    for (int i = 0; i < 23; i++)
        printf("%d ", a[i]);
    qsort(s, 6, sizeof s[0], scmp);
    for (int i = 0; i < 6; i++)
        printf("%s ", s[i]);
    int key = a[11];
    int *hit = bsearch(&key, a, 23, sizeof a[0], icmp);
    printf("found %d at %d\n", key, hit ? (int)(hit - a) : -1);
    srand(7);
    printf("rand %d %d %d\n", rand(), rand(), rand());
    printf("abs %d %lld div %d %d\n", abs(-5), llabs(-6LL), div(-7, 2).quot,
           div(-7, 2).rem);
}

static void heap(void)
{
    /* Contents, not addresses: an address is the target's business. */
    char *p[16];
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) {
        p[i] = malloc((size_t)(i * 37 + 1));
        if (!p[i]) { printf("malloc %d failed\n", i); return; }
        memset(p[i], i + 1, (size_t)(i * 37 + 1));
    }
    for (int i = 0; i < 16; i += 2) {
        free(p[i]);
        p[i] = NULL;
    }
    for (int i = 1; i < 16; i += 2) {
        p[i] = realloc(p[i], (size_t)(i * 50 + 3));
        for (int k = 0; k < i * 37 + 1; k++)
            sum += (unsigned char)p[i][k];
    }
    int *z = calloc(100, sizeof *z);
    for (int i = 0; i < 100; i++)
        sum += (unsigned)z[i];
    free(z);
    for (int i = 1; i < 16; i += 2)
        free(p[i]);
    printf("heap sum %u\n", sum);
}

static void maths(void)
{
    static const double x[] = { 0.0, 0.25, 0.5, 0.9, 1.0, 2.0, 3.0, 10.0,
                                -0.75, -3.5, 100.0, 1e-8, 700.0 };
    for (unsigned i = 0; i < sizeof x / sizeof x[0]; i++) {
        double v = x[i], a = fabs(v);
        printf("%a: %a %a %a %a %a %a %a\n", v, sin(v), cos(v), tan(v),
               atan(v), exp(v), sinh(v), tanh(v));
        printf("  %a %a %a %a %a %a\n", sqrt(a), cbrt(v), log(a + 1),
               log10(a + 1), pow(a + 0.5, 2.5), atan2(v, 1.5));
        printf("  %a %a %a %a %a %a %a\n", floor(v), ceil(v), trunc(v),
               round(v), fmod(v, 0.7), hypot(v, 2.0), expm1(v / 10));
        if (a <= 1)
            printf("  %a %a %a\n", asin(v), acos(v), log1p(v));
    }
    int e;
    double m = frexp(1234.5, &e);
    double ip, fp = modf(-7.25, &ip);
    printf("frexp %a %d ldexp %a modf %a %a\n", m, e, ldexp(0.75, 12), fp, ip);
    printf("float %a %a %a\n", (double)sqrtf(2.0f), (double)sinf(1.0f),
           (double)powf(1.5f, 3.0f));
    printf("lround %lld llround %lld\n", (long long)lround(2.5),
           llround(-3.5));
}

int main(void)
{
    formatting();
    scanning();
    conversions();
    strings();
    sorting();
    heap();
    maths();
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
