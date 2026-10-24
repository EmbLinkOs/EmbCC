/* The parse of a scanf format, written once for both of its instances:
 * scan.c includes it for byte input and wide.c for wide input. Before
 * including, the includer defines
 *
 *   SCAN_CH                the format's character type
 *   SCAN_RUN               the name of the function this makes
 *   SCAN_FVAL(f)           the format character f as an input value
 *   SCAN_FSPACE(f)         is the format character f white space
 *   SCAN_SPACE(c)          is the input character c (never EOF) white space
 *   SCAN_DEST              where %c, %s and %[ store, with
 *     SCAN_DOPEN(d, assign, l, ap)  taking the argument (l: wchar_t *)
 *     SCAN_DPUT(d, l, c)            one input character; 0 if it cannot be
 *                                   converted to what is stored
 *     SCAN_DCLOSE(d, l, term)       the field is over (term: terminate it);
 *                                   0 if a character was left half read
 *   SCAN_SET               a %[ set, with
 *     SCAN_SETUP(set, at, end)      the format's set is [at, end)
 *     SCAN_IN(set, c)               is c in it
 *
 * and gets `static int SCAN_RUN(struct scan *s, const SCAN_CH *fmt,
 * va_list *ap)`, a scanf's result. A character that cannot be converted
 * is a matching failure with errno EILSEQ.
 */

static int SCAN_RUN(struct scan *s, const SCAN_CH *fmt, va_list *ap)
{
    int assigned = 0, c;

    for (const SCAN_CH *f = fmt; *f; f++) {
        /* Whitespace in the format matches any run of whitespace, INCLUDING
         * none at all -- so a trailing " " in a format never fails. */
        if (SCAN_FSPACE(*f)) {
            while ((c = sgetc(s)) != EOF && SCAN_SPACE(c))
                ;
            sungetc(s, c);
            continue;
        }
        if (*f != '%') {
            c = sgetc(s);
            if (c == EOF || c != SCAN_FVAL(*f)) {
                sungetc(s, c);
                goto done;             /* matching failure */
            }
            continue;
        }

        f++;
        int assign = 1, width = INT_MAX, len = L_INT;
        if (*f == '*') { assign = 0; f++; }
        if (*f >= '0' && *f <= '9') {
            width = 0;
            while (*f >= '0' && *f <= '9')
                width = width * 10 + (int)(*f++ - '0');
            if (width == 0)
                goto done;             /* %0d: undefined; refuse to guess */
        }
        switch (*f) {
        case 'h': f++; if (*f == 'h') { len = L_CHAR; f++; } else len = L_SHORT;
                  break;
        case 'l': f++; if (*f == 'l') { len = L_LLONG; f++; } else len = L_LONG;
                  break;
        case 'j': len = L_INTMAX; f++; break;
        case 'z': len = L_SIZE; f++; break;
        case 't': len = L_PTRDIFF; f++; break;
        case 'L': len = L_LDOUBLE; f++; break;
        default: break;
        }

        /* Every conversion but %c, %[ and %n skips leading whitespace
         * first, and that skipping does NOT count as input for the
         * "EOF before any conversion" test below. */
        SCAN_CH conv = *f;
        int is_l = len == L_LONG;
        if (conv != 'c' && conv != '[' && conv != 'n') {
            while ((c = sgetc(s)) != EOF && SCAN_SPACE(c))
                ;
            if (c == EOF)
                goto done;
            sungetc(s, c);
        }

        switch (conv) {
        case 'd': case 'u':
            if (!__scan_int(s, 10, width, assign, ap, len))
                goto done;
            assigned += assign;
            break;
        case 'i':
            if (!__scan_int(s, 0, width, assign, ap, len))
                goto done;
            assigned += assign;
            break;
        case 'o':
            if (!__scan_int(s, 8, width, assign, ap, len))
                goto done;
            assigned += assign;
            break;
        case 'x': case 'X':
            if (!__scan_int(s, 16, width, assign, ap, len))
                goto done;
            assigned += assign;
            break;
        case 'p':
            if (!__scan_ptr(s, width, assign, ap))
                goto done;
            assigned += assign;
            break;
        case 'a': case 'A': case 'e': case 'E':
        case 'f': case 'F': case 'g': case 'G':
            /* The float conversions all read the same syntax; they differ
             * only in printf. L_LONG here means `double`, not `long`. */
            if (!__scan_flt(s, width, assign, ap,
                            len == L_LONG ? L_DOUBLE :
                            len == L_LDOUBLE ? L_LDOUBLE : 0))
                goto done;
            assigned += assign;
            break;
        case 'c': {
            /* No whitespace skip, and no terminating NUL: %c reads exactly
             * `width` characters (default 1). */
            int n = width == INT_MAX ? 1 : width, got = 0;
            SCAN_DEST d;
            SCAN_DOPEN(&d, assign, is_l, ap);
            while (got < n && (c = sgetc(s)) != EOF) {
                if (!SCAN_DPUT(&d, is_l, c)) goto bad;
                got++;
            }
            if (got < n)
                goto done;
            if (!SCAN_DCLOSE(&d, is_l, 0)) goto bad;
            assigned += assign;
            break;
        }
        case 's': {
            /* The space that ends the field goes back; when the WIDTH ends
             * it, nothing was read past it, and nothing goes back -- "%3s"
             * on "abcdef" leaves "def", not "cdef". */
            int n = 0;
            SCAN_DEST d;
            SCAN_DOPEN(&d, assign, is_l, ap);
            while (width-- && (c = sgetc(s)) != EOF) {
                if (SCAN_SPACE(c)) { sungetc(s, c); break; }
                if (!SCAN_DPUT(&d, is_l, c)) goto bad;
                n++;
            }
            if (n == 0)
                goto done;
            if (!SCAN_DCLOSE(&d, is_l, 1)) goto bad;
            assigned += assign;
            break;
        }
        case '[': {
            /* A ']' FIRST is a literal ']', not the terminator -- the one
             * piece of %[ syntax everyone forgets. */
            const SCAN_CH *at = f + 1, *end;
            int negate = 0, n = 0;
            if (*at == '^') { negate = 1; at++; }
            end = at;
            if (*end == ']') end++;
            while (*end && *end != ']') end++;
            SCAN_SET set;
            SCAN_SETUP(&set, at, end);
            SCAN_DEST d;
            SCAN_DOPEN(&d, assign, is_l, ap);
            while (width-- && (c = sgetc(s)) != EOF) {
                if (SCAN_IN(&set, c) == negate) { sungetc(s, c); break; }
                if (!SCAN_DPUT(&d, is_l, c)) goto bad;
                n++;
            }
            if (n == 0)
                goto done;             /* matching failure */
            if (!SCAN_DCLOSE(&d, is_l, 1)) goto bad;
            f = *end ? end : end - 1;  /* the loop's f++ steps past ']' */
            assigned += assign;
            break;
        }
        case 'n':
            /* §7.21.6.2p12: %n is not a conversion, so it does not count
             * toward the return value and cannot cause a failure. */
            if (assign)
                __scan_store_int(ap, len, (unsigned long long)s->nread, 0);
            break;
        case '%':
            c = sgetc(s);
            if (c != '%') { sungetc(s, c); goto done; }
            break;
        default:
            goto done;                 /* an unknown conversion stops here */
        }
    }
    goto done;

bad:
    errno = EILSEQ;
done:
    /* The one character read ahead and not used goes back to the input --
     * "12x" through %d leaves the 'x' for whatever reads next. */
    if (s->pushed && s->unget)
        s->unget(s->ctx, s->back);
    /* §7.21.6.2p16 distinguishes two failures, and callers rely on it:
     * an INPUT failure (the input ended) before the first conversion
     * completed returns EOF, while a MATCHING failure (there was input, it
     * was the wrong shape) returns the number assigned, which may be 0.
     *
     * So the test is "did we reach end of input", not "did we consume
     * nothing" -- sscanf("x", "%d", &n) consumes nothing and must still
     * return 0, because there is an 'x' there to be re-read. */
    if (assigned == 0 && s->hit_eof)
        return EOF;
    return assigned;
}
