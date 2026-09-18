#include "lex.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../target/target.h"

void lex_init(struct lexer *lx, const char *file, const char *src)
{
    lx->file = file;
    lx->src = src;
    lx->p = src;
    lx->line_start = src;
    lx->line = 1;
    lex_next(lx);
}

static void skip_space_and_comments(struct lexer *lx)
{
    for (;;) {
        while (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' ||
               *lx->p == '\n') {
            if (*lx->p == '\n') {
                lx->line++;
                lx->line_start = lx->p + 1;
            }
            lx->p++;
        }
        if (lx->p[0] == '/' && lx->p[1] == '/') {
            while (*lx->p && *lx->p != '\n')
                lx->p++;
            continue;
        }
        if (lx->p[0] == '/' && lx->p[1] == '*') {
            int start = lx->line;
            lx->p += 2;
            while (!(lx->p[0] == '*' && lx->p[1] == '/')) {
                if (!*lx->p)
                    diag_fatal(lx->file, start, "unterminated comment");
                if (*lx->p == '\n') {
                    lx->line++;
                    lx->line_start = lx->p + 1;
                }
                lx->p++;
            }
            lx->p += 2;
            continue;
        }
        return;
    }
}

static const struct {
    const char *word;
    enum tok_kind kind;
} keywords[] = {
    { "int", TOK_KW_INT },
    { "char", TOK_KW_CHAR },
    { "short", TOK_KW_SHORT },
    { "long", TOK_KW_LONG },
    { "float", TOK_KW_FLOAT },
    { "double", TOK_KW_DOUBLE },
    { "_Bool", TOK_KW_BOOL },
    { "_Static_assert", TOK_KW_STATIC_ASSERT },
    { "_Generic", TOK_KW_GENERIC },
    { "_Alignof", TOK_KW_ALIGNOF },
    { "__alignof__", TOK_KW_ALIGNOF },
    { "__alignof", TOK_KW_ALIGNOF },
    { "_Alignas", TOK_KW_ALIGNAS },
    { "typeof", TOK_KW_TYPEOF },
    { "__typeof__", TOK_KW_TYPEOF },
    { "__typeof", TOK_KW_TYPEOF },
    { "_Atomic", TOK_KW_ATOMIC },
    { "unsigned", TOK_KW_UNSIGNED },
    { "signed", TOK_KW_SIGNED },
    { "void", TOK_KW_VOID },
    { "sizeof", TOK_KW_SIZEOF },
    { "return", TOK_KW_RETURN },
    { "static", TOK_KW_STATIC },
    { "extern", TOK_KW_EXTERN },
    { "struct", TOK_KW_STRUCT },
    { "union", TOK_KW_UNION },
    { "enum", TOK_KW_ENUM },
    { "typedef", TOK_KW_TYPEDEF },
    { "const", TOK_KW_CONST },
    { "volatile", TOK_KW_VOLATILE },
    { "__volatile__", TOK_KW_VOLATILE },
    { "restrict", TOK_KW_RESTRICT },
    { "asm", TOK_KW_ASM },
    { "__asm__", TOK_KW_ASM },
    { "inline", TOK_KW_INLINE },
    { "__inline", TOK_KW_INLINE },
    { "__inline__", TOK_KW_INLINE },
    { "__attribute__", TOK_KW_ATTRIBUTE },
    { "__attribute", TOK_KW_ATTRIBUTE },
    { "if", TOK_KW_IF },
    { "else", TOK_KW_ELSE },
    { "while", TOK_KW_WHILE },
    { "for", TOK_KW_FOR },
    { "break", TOK_KW_BREAK },
    { "continue", TOK_KW_CONTINUE },
    { "goto", TOK_KW_GOTO },
    { "do", TOK_KW_DO },
    { "switch", TOK_KW_SWITCH },
    { "case", TOK_KW_CASE },
    { "default", TOK_KW_DEFAULT },
};

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- literal elements ------------------------------------------------ */

static int hex_digits(const char **p, int want, unsigned long *out)
{
    unsigned long v = 0;
    for (int i = 0; i < want; i++) {
        int d = hex_val((unsigned char)**p);
        if (d < 0)
            return 0;
        v = v * 16 + (unsigned long)d;
        (*p)++;
    }
    *out = v;
    return 1;
}

/* The escape after a backslash (*p just past it). */
static struct litch decode_escape(const char **p, const char *file, int line)
{
    struct litch c = { 0, 1 };
    int ch = (unsigned char)**p;
    switch (ch) {
    case 'n': (*p)++; c.v = '\n'; return c;
    case 't': (*p)++; c.v = '\t'; return c;
    case 'r': (*p)++; c.v = '\r'; return c;
    case 'a': (*p)++; c.v = '\a'; return c;
    case 'b': (*p)++; c.v = '\b'; return c;
    case 'f': (*p)++; c.v = '\f'; return c;
    case 'v': (*p)++; c.v = '\v'; return c;
    case 'e': (*p)++; c.v = 27; return c;      /* GNU extension: ESC */
    case '\\': (*p)++; c.v = '\\'; return c;
    case '\'': (*p)++; c.v = '\''; return c;
    case '"': (*p)++; c.v = '"'; return c;
    case '?': (*p)++; c.v = '?'; return c;
    case 'x': {
        (*p)++;
        if (hex_val((unsigned char)**p) < 0)
            diag_fatal(file, line, "\\x used with no following hex digits");
        /* Every digit belongs to the escape, however many: the VALUE is
         * kept whole here and narrowed (with gcc's warning) only once the
         * literal's unit width is known. */
        unsigned long v = 0;
        int d, big = 0;
        while ((d = hex_val((unsigned char)**p)) >= 0) {
            if (v >> 60)
                big = 1;
            v = v * 16 + (unsigned long)d;
            (*p)++;
        }
        c.v = big ? ~0UL : v;
        return c;
    }
    case 'u':
    case 'U': {
        /* A universal character name: exactly 4 or 8 hex digits naming a
         * code point, which is ENCODED at the literal's width. */
        int want = ch == 'u' ? 4 : 8;
        (*p)++;
        unsigned long v;
        if (!hex_digits(p, want, &v))
            diag_fatal(file, line, "\\%c needs %d hex digits", ch, want);
        if (v >= 0xD800 && v <= 0xDFFF)
            diag_fatal(file, line, "\\%c%0*lX is not a valid universal character "
                       "(a UTF-16 surrogate)", ch, want, v);
        if (v > 0x10FFFF)
            diag_fatal(file, line, "\\%c%0*lX is outside the UCS codespace",
                       ch, want, v);
        c.v = v;
        c.raw = 0;
        return c;
    }
    default:
        if (ch >= '0' && ch <= '7') {     /* octal, at most three digits */
            unsigned long v = 0;
            for (int i = 0; i < 3 && **p >= '0' && **p <= '7'; i++) {
                v = v * 8 + (unsigned long)(**p - '0');
                (*p)++;
            }
            c.v = v;
            return c;
        }
        diag_fatal(file, line, "unknown escape '\\%c' in a literal", ch);
        return c;
    }
}

/* A source character: a code point decoded from UTF-8, or — for a byte that
 * does not start a valid UTF-8 sequence — that byte, raw. */
static struct litch decode_source(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    struct litch c = { s[0], 0 };
    int len = s[0] < 0x80 ? 1 : (s[0] & 0xE0) == 0xC0 ? 2
            : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 1) {
        (*p)++;
        return c;
    }
    if (len > 1) {
        unsigned long v = s[0] & (0x7FU >> len);
        int i;
        for (i = 1; i < len && (s[i] & 0xC0) == 0x80; i++)
            v = (v << 6) | (s[i] & 0x3F);
        static const unsigned long min[] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (i == len && v >= min[len] && v <= 0x10FFFF &&
            !(v >= 0xD800 && v <= 0xDFFF)) {
            *p += len;
            c.v = v;
            return c;
        }
    }
    (*p)++;                     /* not UTF-8: keep the byte itself */
    c.raw = 1;
    return c;
}

struct litch lit_decode(const char **p, int esc, const char *file, int line)
{
    return esc ? decode_escape(p, file, line) : decode_source(p);
}

char *lit_encode(const struct litch *lc, int n, int width, long *nunits,
                 const char *file, int line)
{
    /* Worst case: four UTF-8 bytes, or two UTF-16 units, per element. */
    unsigned char *out = xcalloc((size_t)(4 * n + 1), (size_t)width);
    long u = 0;
    unsigned long max = width == 4 ? 0xFFFFFFFFUL
                      : width == 2 ? 0xFFFFUL : 0xFFUL;
#define PUT(val)                                                            \
    do {                                                                    \
        unsigned long pv_ = (val);                                          \
        for (int b_ = 0; b_ < width; b_++)                                  \
            out[(size_t)u * (size_t)width + (size_t)b_] =                   \
                (unsigned char)(pv_ >> (8 * b_));                           \
        u++;                                                                \
    } while (0)
    for (int i = 0; i < n; i++) {
        unsigned long v = lc[i].v;
        if (lc[i].raw) {
            if (v > max)
                fprintf(stderr, "embcc: %s:%d: warning: escape sequence out "
                        "of range for a %d-byte element; truncated, as gcc "
                        "does\n", file, line, width);
            PUT(v & max);
        } else if (width == 1) {
            if (v < 0x80) {
                PUT(v);
            } else if (v < 0x800) {
                PUT(0xC0 | (v >> 6)); PUT(0x80 | (v & 0x3F));
            } else if (v < 0x10000) {
                PUT(0xE0 | (v >> 12)); PUT(0x80 | ((v >> 6) & 0x3F));
                PUT(0x80 | (v & 0x3F));
            } else {
                PUT(0xF0 | (v >> 18)); PUT(0x80 | ((v >> 12) & 0x3F));
                PUT(0x80 | ((v >> 6) & 0x3F)); PUT(0x80 | (v & 0x3F));
            }
        } else if (width == 2 && v > 0xFFFF) {
            v -= 0x10000;                         /* a surrogate pair */
            PUT(0xD800 | (v >> 10));
            PUT(0xDC00 | (v & 0x3FF));
        } else {
            PUT(v);
        }
    }
    PUT(0);
#undef PUT
    *nunits = u;
    return (char *)out;
}

long lit_char_value(struct litch c, int pfx, int *uns, const char *file,
                    int line)
{
    *uns = 0;
    if (pfx == 0) {
        /* One byte, read as the target's plain char. A character that is
         * more than one byte in UTF-8 would be gcc's multi-character constant,
         * whose value is implementation-defined; refused rather than guessed. */
        if (!c.raw && c.v > 0x7F)
            diag_fatal(file, line, "character U+%04lX does not fit in one "
                       "byte; write it as a wide constant (L'...')", c.v);
        if (c.v > 0xFF)
            fprintf(stderr, "embcc: %s:%d: warning: escape sequence out of "
                    "range for a character constant; truncated, as gcc "
                    "does\n", file, line);
        unsigned long b = c.v & 0xFF;
        if (target_get() == TARGET_AARCH64)
            return (long)b;                        /* char is unsigned */
        return b > 0x7F ? (long)b - 0x100 : (long)b;
    }
    if (pfx == 'u') {
        if (!c.raw && c.v > 0xFFFF)
            diag_fatal(file, line, "U+%04lX needs two UTF-16 code units and "
                       "cannot be one u'' constant", c.v);
        if (c.v > 0xFFFF)
            fprintf(stderr, "embcc: %s:%d: warning: escape sequence out of "
                    "range for char16_t; truncated, as gcc does\n", file, line);
        return (long)(c.v & 0xFFFF);          /* char16_t promotes to int */
    }
    if (c.v > 0xFFFFFFFFUL)
        fprintf(stderr, "embcc: %s:%d: warning: escape sequence out of range "
                "for a 32-bit character; truncated, as gcc does\n", file, line);
    unsigned long v = c.v & 0xFFFFFFFFUL;
    if (pfx == 'U' || target_get() == TARGET_AARCH64) {
        *uns = 1;                    /* char32_t, or aarch64's unsigned wchar_t */
        return (long)v;
    }
    return v > 0x7FFFFFFFUL ? (long)v - 0x100000000L : (long)v;  /* int wchar_t */
}

void lex_next(struct lexer *lx)
{
    struct token *t = &lx->tok;

    skip_space_and_comments(lx);
    t->line = lx->line;
    t->col = (int)(lx->p - lx->line_start) + 1;
    t->text = NULL;
    t->num = 0;
    t->str_width = 1;
    t->str_prefix = 0;

    t->lit = NULL;
    t->nlit = 0;

    if (!*lx->p) {
        t->kind = TOK_EOF;
        return;
    }

    int pfx = 0;   /* a literal's encoding prefix: 0, 'L', 'u' or 'U' */

    /* An encoding prefix on a string or char literal: L"" u8"" u"" U"" and
     * L'' u'' U''. Consume it and remember the element width; the '"' / '\''
     * lexing below then runs at lx->p. A wide CHAR constant stays a plain int
     * (its value is the code point); only a wide STRING carries its width so
     * its .rodata is emitted at 2 or 4 bytes per element. */
    {
        const char *q = lx->p;
        int w = 0, adv = 0;
        pfx = 0;
        if ((q[0] == 'L' || q[0] == 'U') && (q[1] == '"' || q[1] == '\'')) {
            w = 4; adv = 1;
        } else if (q[0] == 'u' && q[1] == '8' && q[2] == '"') {
            w = 1; adv = 2;
        } else if (q[0] == 'u' && (q[1] == '"' || q[1] == '\'')) {
            w = 2; adv = 1;
        }
        if (adv) {
            lx->p += adv;
            pfx = adv == 1 ? q[0] : 0;                 /* u8 is plain char */
            if (*lx->p == '"') {
                t->str_width = w;
                t->str_prefix = (char)pfx;
            }
        }
    }

    /* A floating constant: digits with a '.', or an exponent, or the
     * leading-dot form. Decided BEFORE the integer path so 1.5 never
     * lexes as 1 followed by .5 (see also the '.' operator case). */
    if (isdigit((unsigned char)*lx->p) ||
        (*lx->p == '.' && isdigit((unsigned char)lx->p[1]))) {
        int is_hex = lx->p[0] == '0' &&
                     (lx->p[1] == 'x' || lx->p[1] == 'X');
        const char *scan = lx->p;
        int looks_float = 0;
        if (!is_hex) {
            while (isdigit((unsigned char)*scan))
                scan++;
            if (*scan == '.') {
                looks_float = 1;
            } else if ((*scan == 'e' || *scan == 'E') &&
                       (isdigit((unsigned char)scan[1]) ||
                        ((scan[1] == '+' || scan[1] == '-') &&
                         isdigit((unsigned char)scan[2])))) {
                looks_float = 1;
            }
        } else {
            /* A hex float REQUIRES a binary exponent 'p'/'P' (C99 6.4.4.2):
             * scan past the hex digits + an optional '.' and look for it, so
             * 0x1.8p3 / 0x1p-4 lex as floats while 0x10 stays an integer.
             * strtod below parses the hex-float form directly. */
            scan = lx->p + 2;               /* past "0x" */
            while (isxdigit((unsigned char)*scan) || *scan == '.')
                scan++;
            if (*scan == 'p' || *scan == 'P')
                looks_float = 1;
        }
        if (looks_float) {
            char *fend;
            double d = strtod(lx->p, &fend);
            t->kind = TOK_FNUM;
            t->fnum = d;
            t->fnum_is_float = 0;
            t->fnum_is_ld = 0;
            if (*fend == 'f' || *fend == 'F') {
                t->fnum_is_float = 1;
                fend++;
            } else if (*fend == 'l' || *fend == 'L') {
                t->fnum_is_ld = 1;
                t->text = xstrndup(lx->p, (size_t)(fend - lx->p));
                fend++;
            }
            if (isalnum((unsigned char)*fend) || *fend == '.')
                diag_fatal(lx->file, lx->line,
                           "malformed floating constant");
            lx->p = fend;
            return;
        }
    }

    if (isdigit((unsigned char)*lx->p)) {
        char *end;
        int hex = lx->p[0] == '0' &&
                  (lx->p[1] == 'x' || lx->p[1] == 'X');
        unsigned long v = strtoul(lx->p, &end, 0);
        int has_u = 0, has_l = 0;
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') {
            if (*end == 'u' || *end == 'U')
                has_u = 1;
            else
                has_l = 1;
            end++;
        }
        if (isalnum((unsigned char)*end) || *end == '_' || *end == '.')
            diag_fatal(lx->file, lx->line,
                       "malformed integer constant");
        t->kind = TOK_NUM;
        t->num = (long)v;
        /* C99 typing: decimal grows int -> long; hex additionally
         * passes through the unsigned types. Suffixes force it. */
        t->num_long = has_l || v > (unsigned long)INT_MAX;
        t->num_uns = has_u;
        if (hex && !has_u) {
            if (v > (unsigned long)INT_MAX && v <= 0xffffffffUL) {
                t->num_uns = 1;
                t->num_long = has_l;
            } else if (v > (unsigned long)LONG_MAX) {
                t->num_uns = 1;
            }
        }
        if (!hex && !has_u && !has_l && v > (unsigned long)LONG_MAX)
            diag_fatal(lx->file, lx->line,
                       "integer constant out of range for long");
        lx->p = end;
        return;
    }

    if (isalpha((unsigned char)*lx->p) || *lx->p == '_') {
        const char *start = lx->p;
        while (isalnum((unsigned char)*lx->p) || *lx->p == '_')
            lx->p++;
        size_t n = (size_t)(lx->p - start);
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
            if (strlen(keywords[i].word) == n &&
                memcmp(keywords[i].word, start, n) == 0) {
                t->kind = keywords[i].kind;
                return;
            }
        }
        t->kind = TOK_IDENT;
        t->text = xstrndup(start, n);
        return;
    }

    /* Operators that pair with '=' (or double themselves) share one
     * shape: base, base=, and for some basebase / basebase=. */
    switch (*lx->p) {
    case '(': t->kind = TOK_LPAREN; break;
    case ')': t->kind = TOK_RPAREN; break;
    case '{': t->kind = TOK_LBRACE; break;
    case '}': t->kind = TOK_RBRACE; break;
    case '[': t->kind = TOK_LBRACKET; break;
    case ']': t->kind = TOK_RBRACKET; break;
    case ',': t->kind = TOK_COMMA; break;
    case ';': t->kind = TOK_SEMI; break;
    case '~': t->kind = TOK_TILDE; break;
    case '?': t->kind = TOK_QUESTION; break;
    case ':': t->kind = TOK_COLON; break;
    case '+':
        if (lx->p[1] == '+') { t->kind = TOK_PLUSPLUS; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_PLUSEQ; lx->p++; }
        else t->kind = TOK_PLUS;
        break;
    case '-':
        if (lx->p[1] == '-') { t->kind = TOK_MINUSMINUS; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_MINUSEQ; lx->p++; }
        else if (lx->p[1] == '>') { t->kind = TOK_ARROW; lx->p++; }
        else t->kind = TOK_MINUS;
        break;
    case '*':
        if (lx->p[1] == '=') { t->kind = TOK_STAREQ; lx->p++; }
        else t->kind = TOK_STAR;
        break;
    case '/':
        if (lx->p[1] == '=') { t->kind = TOK_SLASHEQ; lx->p++; }
        else t->kind = TOK_SLASH;
        break;
    case '%':
        if (lx->p[1] == '=') { t->kind = TOK_PERCENTEQ; lx->p++; }
        else t->kind = TOK_PERCENT;
        break;
    case '^':
        if (lx->p[1] == '=') { t->kind = TOK_CARETEQ; lx->p++; }
        else t->kind = TOK_CARET;
        break;
    case '=':
        if (lx->p[1] == '=') { t->kind = TOK_EQEQ; lx->p++; }
        else t->kind = TOK_ASSIGN;
        break;
    case '!':
        if (lx->p[1] == '=') { t->kind = TOK_NEQ; lx->p++; }
        else t->kind = TOK_BANG;
        break;
    case '&':
        if (lx->p[1] == '&') { t->kind = TOK_ANDAND; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_AMPEQ; lx->p++; }
        else t->kind = TOK_AMP;
        break;
    case '|':
        if (lx->p[1] == '|') { t->kind = TOK_OROR; lx->p++; }
        else if (lx->p[1] == '=') { t->kind = TOK_PIPEEQ; lx->p++; }
        else t->kind = TOK_PIPE;
        break;
    case '<':
        if (lx->p[1] == '<' && lx->p[2] == '=') {
            t->kind = TOK_SHLEQ;
            lx->p += 2;
        } else if (lx->p[1] == '<') {
            t->kind = TOK_SHL;
            lx->p++;
        } else if (lx->p[1] == '=') {
            t->kind = TOK_LE;
            lx->p++;
        } else {
            t->kind = TOK_LT;
        }
        break;
    case '>':
        if (lx->p[1] == '>' && lx->p[2] == '=') {
            t->kind = TOK_SHREQ;
            lx->p += 2;
        } else if (lx->p[1] == '>') {
            t->kind = TOK_SHR;
            lx->p++;
        } else if (lx->p[1] == '=') {
            t->kind = TOK_GE;
            lx->p++;
        } else {
            t->kind = TOK_GT;
        }
        break;
    case '#': {
        /* a line marker from the preprocessor: # LINE "FILE" */
        const char *p = lx->p + 1;
        while (*p == ' ')
            p++;
        if (!isdigit((unsigned char)*p))
            diag_fatal(lx->file, lx->line,
                       "stray '#' (preprocessor directives are handled "
                       "before the lexer)");
        char *end;
        long ln = strtol(p, &end, 10);
        p = end;
        while (*p == ' ')
            p++;
        if (*p != '"')
            diag_fatal(lx->file, lx->line, "malformed line marker");
        const char *fstart = ++p;
        while (*p && *p != '"')
            p++;
        lx->file = xstrndup(fstart, (size_t)(p - fstart));
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
        lx->p = p;
        lx->line_start = p;
        lx->line = (int)ln;
        lex_next(lx); /* the marker produced no token; go again */
        return;
    }
    case '\'': {
        lx->p++;
        struct litch c;
        const char *q = lx->p;
        if (*q == '\\') {
            q++;
            c = lit_decode(&q, 1, lx->file, lx->line);
        } else if (*q && *q != '\'' && *q != '\n') {
            c = lit_decode(&q, 0, lx->file, lx->line);
        } else {
            diag_fatal(lx->file, lx->line, "empty character constant");
            return;
        }
        lx->p = q;
        if (*lx->p != '\'')
            diag_fatal(lx->file, lx->line, *lx->p && *lx->p != '\n'
                       ? "a character constant holds one character "
                         "(multi-character constants are not supported)"
                       : "unterminated character constant");
        int uns;
        t->kind = TOK_NUM; /* a character constant is an int (or wide) value */
        t->num = lit_char_value(c, pfx, &uns, lx->file, lx->line);
        t->num_long = 0;
        t->num_uns = uns;
        break;
    }
    case '"': {
        lx->p++;
        /* Decode the body into elements; encode at this literal's own width.
         * The parser re-encodes when adjacent literals concatenate. */
        size_t cap = 16;
        int n = 0;
        struct litch *lc = xmalloc(cap * sizeof *lc);
        const char *q = lx->p;
        while (*q && *q != '"' && *q != '\n') {
            if ((size_t)n == cap) {
                cap *= 2;
                lc = xrealloc(lc, cap * sizeof *lc);
            }
            if (*q == '\\') {
                q++;
                lc[n++] = lit_decode(&q, 1, lx->file, lx->line);
            } else {
                lc[n++] = lit_decode(&q, 0, lx->file, lx->line);
            }
        }
        lx->p = q;
        if (*lx->p != '"')
            diag_fatal(lx->file, lx->line, "unterminated string literal");
        t->kind = TOK_STR;
        t->lit = lc;
        t->nlit = n;
        t->text = lit_encode(lc, n, t->str_width, &t->num, lx->file, lx->line);
        break;
    }
    case '.':
        if (lx->p[1] == '.' && lx->p[2] == '.') {
            t->kind = TOK_ELLIPSIS;
            lx->p += 2;
        } else {
            t->kind = TOK_DOT;
        }
        break;
    default:
        diag_fatal(lx->file, lx->line,
                   "character '%c' is not supported yet", *lx->p);
    }
    lx->p++;
}

const char *tok_describe(const struct token *t)
{
    static char buf[64];
    switch (t->kind) {
    case TOK_EOF: return "end of file";
    case TOK_NUM:
        snprintf(buf, sizeof buf, "number %ld", t->num);
        return buf;
    case TOK_FNUM: return "a floating constant";
    case TOK_STR: return "a string literal";
    case TOK_ELLIPSIS: return "'...'";
    case TOK_IDENT:
        snprintf(buf, sizeof buf, "'%s'", t->text);
        return buf;
    case TOK_KW_INT: return "'int'";
    case TOK_KW_CHAR: return "'char'";
    case TOK_KW_SHORT: return "'short'";
    case TOK_KW_LONG: return "'long'";
    case TOK_KW_FLOAT: return "'float'";
    case TOK_KW_DOUBLE: return "'double'";
    case TOK_KW_BOOL: return "'_Bool'";
    case TOK_KW_STATIC_ASSERT: return "'_Static_assert'";
    case TOK_KW_GENERIC: return "'_Generic'";
    case TOK_KW_ALIGNOF: return "'_Alignof'";
    case TOK_KW_ALIGNAS: return "'_Alignas'";
    case TOK_KW_TYPEOF: return "'typeof'";
    case TOK_KW_ATOMIC: return "'_Atomic'";
    case TOK_KW_UNSIGNED: return "'unsigned'";
    case TOK_KW_SIGNED: return "'signed'";
    case TOK_KW_SIZEOF: return "'sizeof'";
    case TOK_KW_VOID: return "'void'";
    case TOK_KW_RETURN: return "'return'";
    case TOK_KW_STATIC: return "'static'";
    case TOK_KW_EXTERN: return "'extern'";
    case TOK_KW_STRUCT: return "'struct'";
    case TOK_KW_UNION: return "'union'";
    case TOK_KW_ENUM: return "'enum'";
    case TOK_KW_TYPEDEF: return "'typedef'";
    case TOK_KW_CONST: return "'const'";
    case TOK_KW_VOLATILE: return "'volatile'";
    case TOK_KW_RESTRICT: return "'restrict'";
    case TOK_KW_ASM: return "'asm'";
    case TOK_KW_INLINE: return "'inline'";
    case TOK_KW_ATTRIBUTE: return "'__attribute__'";
    case TOK_DOT: return "'.'";
    case TOK_ARROW: return "'->'";
    case TOK_KW_IF: return "'if'";
    case TOK_KW_ELSE: return "'else'";
    case TOK_KW_WHILE: return "'while'";
    case TOK_KW_FOR: return "'for'";
    case TOK_KW_BREAK: return "'break'";
    case TOK_KW_CONTINUE: return "'continue'";
    case TOK_KW_GOTO: return "'goto'";
    case TOK_KW_DO: return "'do'";
    case TOK_KW_SWITCH: return "'switch'";
    case TOK_KW_CASE: return "'case'";
    case TOK_KW_DEFAULT: return "'default'";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_LBRACE: return "'{'";
    case TOK_RBRACE: return "'}'";
    case TOK_LBRACKET: return "'['";
    case TOK_RBRACKET: return "']'";
    case TOK_COMMA: return "','";
    case TOK_SEMI: return "';'";
    case TOK_PLUS: return "'+'";
    case TOK_MINUS: return "'-'";
    case TOK_STAR: return "'*'";
    case TOK_SLASH: return "'/'";
    case TOK_PERCENT: return "'%'";
    case TOK_AMP: return "'&'";
    case TOK_PIPE: return "'|'";
    case TOK_CARET: return "'^'";
    case TOK_TILDE: return "'~'";
    case TOK_SHL: return "'<<'";
    case TOK_SHR: return "'>>'";
    case TOK_ASSIGN: return "'='";
    case TOK_EQEQ: return "'=='";
    case TOK_NEQ: return "'!='";
    case TOK_LT: return "'<'";
    case TOK_GT: return "'>'";
    case TOK_LE: return "'<='";
    case TOK_GE: return "'>='";
    case TOK_ANDAND: return "'&&'";
    case TOK_OROR: return "'||'";
    case TOK_BANG: return "'!'";
    case TOK_PLUSEQ: return "'+='";
    case TOK_MINUSEQ: return "'-='";
    case TOK_STAREQ: return "'*='";
    case TOK_SLASHEQ: return "'/='";
    case TOK_PERCENTEQ: return "'%='";
    case TOK_AMPEQ: return "'&='";
    case TOK_PIPEEQ: return "'|='";
    case TOK_CARETEQ: return "'^='";
    case TOK_SHLEQ: return "'<<='";
    case TOK_SHREQ: return "'>>='";
    case TOK_PLUSPLUS: return "'++'";
    case TOK_MINUSMINUS: return "'--'";
    case TOK_QUESTION: return "'?'";
    case TOK_COLON: return "':'";
    }
    return "?";
}
