/* The parse of a printf format, written once for both of its instances:
 * format.c includes it for formats of bytes and wide.c for formats of wide
 * characters. Before including, the includer defines
 *
 *   FMT_CH                 the format's character type
 *   FMT_RUN                the name of the function this makes
 *   FMT_TEXT(o, s, n)      writes n characters of the format's own text
 *   FMT_CHAR(o, l, ap, flags, width)        a %c (l: %lc)
 *   FMT_STR(o, l, ap, flags, width, prec)   a %s (l: %ls)
 *
 * and gets `static int FMT_RUN(struct out *o, const FMT_CH *fmt,
 * va_list *ap)`: the count of characters, or -1 with errno EILSEQ after an
 * encoding error (o->bad). The conversions are ASCII in either width.
 */

static int FMT_RUN(struct out *o, const FMT_CH *fmt, va_list *ap)
{
    for (const FMT_CH *p = fmt; *p; ) {
        if (*p != '%') {
            const FMT_CH *s = p;
            while (*p && *p != '%') p++;
            FMT_TEXT(o, s, (size_t)(p - s));
            continue;
        }
        p++;
        if (*p == '%') { __fmt_emit(o, "%", 1); p++; continue; }

        int flags = 0;
        for (;; p++) {
            if (*p == '-')      flags |= F_MINUS;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_HASH;
            else if (*p == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(*ap, int);
            if (width < 0) { flags |= F_MINUS; width = -width; }
            p++;
        } else while (*p >= '0' && *p <= '9') width = width * 10 + (int)(*p++ - '0');

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(*ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (int)(*p++ - '0');
            if (prec < 0) prec = -1;    /* a negative * precision is as if absent */
        }

        /* Length modifiers. `z`, `t` and `j` matter as much as the rest:
         * a size_t printed with %u is wrong on LP64 and silently right on
         * ILP32, which is the worst way for a bug to behave. */
        enum { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF,
               L_MAX, L_LDOUBLE } len = L_INT;
        for (;;) {
            if (*p == 'h')      { len = (len == L_SHORT) ? L_CHAR : L_SHORT; p++; }
            else if (*p == 'l') { len = (len == L_LONG) ? L_LLONG : L_LONG; p++; }
            else if (*p == 'z') { len = L_SIZE; p++; }
            else if (*p == 't') { len = L_PTRDIFF; p++; }
            else if (*p == 'j') { len = L_MAX; p++; }
            else if (*p == 'L') { len = L_LDOUBLE; p++; }
            else if (*p == 'w') {
                /* C23's wN and wfN: the int of N bits, or the fast one --
                 * read as whichever standard type has that size */
                int fast = 0, bits = 0, size;
                p++;
                if (*p == 'f') { fast = 1; p++; }
                while (*p >= '0' && *p <= '9') bits = bits * 10 + (int)(*p++ - '0');
                size = !fast ? bits / 8
                     : bits == 8 ? (int)sizeof(int_fast8_t)
                     : bits == 16 ? (int)sizeof(int_fast16_t)
                     : bits == 32 ? (int)sizeof(int_fast32_t)
                     : (int)sizeof(int_fast64_t);
                len = size == 1 ? L_CHAR : size == (int)sizeof(short) ? L_SHORT
                    : size == (int)sizeof(int) ? L_INT
                    : size == (int)sizeof(long) ? L_LONG : L_LLONG;
                break;
            }
            else break;
        }

        FMT_CH conv = *p;
        if (conv) p++;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            switch (len) {
            case L_CHAR:    v = (signed char)va_arg(*ap, int); break;
            case L_SHORT:   v = (short)va_arg(*ap, int); break;
            case L_LONG:    v = va_arg(*ap, long); break;
            case L_LLONG:   v = va_arg(*ap, long long); break;
            case L_SIZE:    v = (long long)va_arg(*ap, size_t); break;
            case L_PTRDIFF: v = (long long)va_arg(*ap, ptrdiff_t); break;
            case L_MAX:     v = (long long)va_arg(*ap, intmax_t); break;
            default:        v = va_arg(*ap, int); break;
            }
            const char *sign = v < 0 ? "-" : (flags & F_PLUS) ? "+"
                             : (flags & F_SPACE) ? " " : "";
            unsigned long long u = v < 0 ? (unsigned long long)-(v + 1) + 1
                                         : (unsigned long long)v;
            __fmt_int(o, u, 10, 0, flags, width, prec, sign, "");
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'b': case 'B': {
            unsigned long long v;
            switch (len) {
            case L_CHAR:    v = (unsigned char)va_arg(*ap, unsigned); break;
            case L_SHORT:   v = (unsigned short)va_arg(*ap, unsigned); break;
            case L_LONG:    v = va_arg(*ap, unsigned long); break;
            case L_LLONG:   v = va_arg(*ap, unsigned long long); break;
            case L_SIZE:    v = va_arg(*ap, size_t); break;
            case L_PTRDIFF: v = (unsigned long long)va_arg(*ap, ptrdiff_t); break;
            case L_MAX:     v = (unsigned long long)va_arg(*ap, uintmax_t); break;
            default:        v = va_arg(*ap, unsigned); break;
            }
            int base = conv == 'o' ? 8 : conv == 'u' ? 10
                     : conv == 'b' || conv == 'B' ? 2 : 16;
            const char *pfx = "";
            if ((flags & F_HASH) && v) {
                if (conv == 'x') pfx = "0x";
                else if (conv == 'X') pfx = "0X";
                else if (conv == 'b') pfx = "0b";       /* C23 */
                else if (conv == 'B') pfx = "0B";
                else if (conv == 'o') pfx = "0";
            }
            __fmt_int(o, v, base, conv == 'X', flags, width, prec, "", pfx);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        case 'a': case 'A':
            if (len == L_LDOUBLE)
                __fmt_ldouble(o, va_arg(*ap, long double), (char)conv, flags,
                              width, prec);
            else
                __fmt_double(o, va_arg(*ap, double), (char)conv, flags,
                             width, prec);
            break;
        case 'c':
            FMT_CHAR(o, len == L_LONG, ap, flags, width);
            break;
        case 's':
            FMT_STR(o, len == L_LONG, ap, flags, width, prec);
            break;
        case 'p': {
            void *v = va_arg(*ap, void *);
            if (!v) {
                if (!(flags & F_MINUS)) __fmt_pad(o, ' ', width - 5);
                __fmt_emit(o, "(nil)", 5);
                if (flags & F_MINUS) __fmt_pad(o, ' ', width - 5);
            } else {
                __fmt_int(o, (unsigned long long)(size_t)v, 16, 0, flags,
                          width, prec, "", "0x");
            }
            break;
        }
        case 'n': {
            /* The count goes into the type the modifier names: %hhn into
             * a signed char, %lln into a long long -- an int stored into a
             * char is three bytes of someone else's memory. */
            void *q = va_arg(*ap, void *);
            if (!q) break;
            switch (len) {
            case L_CHAR:    *(signed char *)q = (signed char)o->n; break;
            case L_SHORT:   *(short *)q = (short)o->n; break;
            case L_LONG:    *(long *)q = o->n; break;
            case L_LLONG:   *(long long *)q = o->n; break;
            case L_SIZE:    *(size_t *)q = (size_t)o->n; break;
            case L_PTRDIFF: *(ptrdiff_t *)q = o->n; break;
            case L_MAX:     *(intmax_t *)q = o->n; break;
            default:        *(int *)q = o->n; break;
            }
            break;
        }
        case 0:
            return o->n;               /* a trailing '%': stop, do not run off */
        default:
            /* An unknown conversion prints itself, which is more use than
             * silence when a format string has a typo. */
            __fmt_emit(o, "%", 1);
            FMT_TEXT(o, p - 1, 1);
            break;
        }
        if (o->bad) {
            errno = EILSEQ;
            return -1;
        }
    }
    return o->n;
}
