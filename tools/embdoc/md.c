/* md -- the Markdown the documentation is written in, rendered to HTML.
 *
 * The language is GitHub's (CommonMark with the GFM table extension), cut
 * down to what docs/ uses, and each construct is rendered as github.com
 * renders it -- so a page reads the same on the site as in the repository,
 * and, above all, a heading's anchor is the slug GitHub gives it, or every
 * `file.md#section` link written against GitHub breaks here.
 *
 * Blocks: ATX and setext headings; paragraphs; fenced code (``` and ~~~,
 * the info string's first word as the language) and indented code;
 * bullet and ordered lists, nested by indentation, tight or loose, with
 * lazy continuation lines and blocks inside items; block quotes; thematic
 * breaks; GFM tables with column alignment and \| in cells.
 * Inlines: *em* and _em_, **strong** (CommonMark's delimiter rules, so
 * snake_case and 2 * 3 stay text); `code`; [text](dest "title"); <scheme:>
 * autolinks and bare http(s) URLs; backslash escapes and hard breaks;
 * entity references.
 *
 * A heading's id is GitHub's slug of its text (lower case; letters,
 * digits, _ and - kept; each space a -), a repeat numbered -1, -2 ... as
 * github-slugger numbers them. A link's destination goes to the page's
 * callback (embdoc.c), which knows where every file of the site ends up.
 *
 * Raw HTML is NOT passed through: every < in the text is written &lt;, so
 * <stdio.h> reads as itself and nothing in a page can inject markup. Three
 * forms are understood instead, and embdoc writes its own HTML for them:
 * an anchor <a id="NAME"></a> (or name=), which the docs put above
 * headings that other pages link to by a short id; <code>...</code>; and
 * <!-- comments -->, which are dropped, as GitHub does not show them
 * either. Not supported: images, reference-style links, footnotes, task
 * lists, strikethrough, and HTML blocks of any other kind (they render as
 * text). */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "embdoc.h"

/* ---- memory and buffers ---------------------------------------------- */

static void oom(void)
{
    fprintf(stderr, "embdoc: out of memory\n");
    exit(2);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

static void buf_room(struct buf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra + 1)
        cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_add(struct buf *b, const char *s, size_t n)
{
    buf_room(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void buf_puts(struct buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_putc(struct buf *b, int c)
{
    char ch = (char)c;
    buf_add(b, &ch, 1);
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    buf_room(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

void buf_html(struct buf *b, const char *s, size_t n)
{
    size_t run = 0;
    for (size_t k = 0; k < n; k++) {
        const char *e = NULL;
        switch (s[k]) {
        case '&': e = "&amp;"; break;
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '"': e = "&quot;"; break;
        default: break;
        }
        if (!e)
            continue;
        buf_add(b, s + run, k - run);
        buf_puts(b, e);
        run = k + 1;
    }
    buf_add(b, s + run, n - run);
}

void buf_json(struct buf *b, const char *s, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c == '"' || c == '\\') {
            buf_putc(b, '\\');
            buf_putc(b, c);
        } else if (c == '\n') {
            buf_puts(b, "\\n");
        } else if (c < 0x20) {
            buf_printf(b, "\\u%04x", c);
        } else {
            buf_putc(b, c);
        }
    }
}

void buf_free(struct buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* ---- characters ------------------------------------------------------- */

/* The code point at s[i], its byte length in *len. A byte that does not
 * start valid UTF-8 is taken as itself, one byte long. */
static unsigned u8_at(const char *s, size_t n, size_t i, int *len)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[i];
    int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    *len = 1;
    if (c < 0x80 || k == 0 || i + (size_t)k >= n)
        return c;
    unsigned v = c & (0x3F >> k);
    for (int j = 1; j <= k; j++) {
        if ((u[i + (size_t)j] & 0xC0) != 0x80)
            return c;
        v = (v << 6) | (u[i + (size_t)j] & 0x3F);
    }
    *len = k + 1;
    return v;
}

/* The code point that ends just before s[i]; a space at the start. */
static unsigned u8_before(const char *s, size_t i)
{
    if (i == 0)
        return ' ';
    size_t b = i - 1;
    while (b > 0 && i - b < 4 && ((unsigned char)s[b] & 0xC0) == 0x80)
        b--;
    int len;
    unsigned c = u8_at(s, i, b, &len);
    return b + (size_t)len == i ? c : (unsigned char)s[i - 1];
}

static int u_space(unsigned c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

/* Punctuation and symbols, as CommonMark's flanking rules and GitHub's
 * slugs see them: ASCII punctuation and the Unicode blocks of dashes,
 * quotes, arrows, operators, box drawing and the like. Letters of every
 * script are not. */
static int u_punct(unsigned c)
{
    if (c < 0x80)
        return c > 0x20 && c < 0x7F && !isalnum((int)c);
    if (c >= 0xA1 && c <= 0xBF)
        return c != 0xAA && c != 0xB5 && c != 0xBA;
    return c == 0xD7 || c == 0xF7 ||
           (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) ||
           (c >= 0x2190 && c <= 0x23FF) || (c >= 0x2500 && c <= 0x27BF) ||
           (c >= 0x2900 && c <= 0x2BFF) || (c >= 0x2E00 && c <= 0x2E7F) ||
           (c >= 0x3001 && c <= 0x303F) || (c >= 0xFE10 && c <= 0xFE6F) ||
           (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0x1F000 && c <= 0x1FAFF);
}

static void u8_put(struct buf *b, unsigned c)
{
    char t[4];
    if (c < 0x80) {
        buf_putc(b, (int)c);
    } else if (c < 0x800) {
        t[0] = (char)(0xC0 | (c >> 6));
        t[1] = (char)(0x80 | (c & 0x3F));
        buf_add(b, t, 2);
    } else if (c < 0x10000) {
        t[0] = (char)(0xE0 | (c >> 12));
        t[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        t[2] = (char)(0x80 | (c & 0x3F));
        buf_add(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (c >> 18));
        t[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        t[3] = (char)(0x80 | (c & 0x3F));
        buf_add(b, t, 4);
    }
}

/* Lower case for a slug: ASCII, and the capitals of Latin-1, Greek and
 * Cyrillic, which is every script a heading here is likely to use. */
static unsigned u_lower(unsigned c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
        return c + 32;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    return c;
}

/* ---- the text of rendered HTML --------------------------------------- */

static const struct { const char *name; unsigned cp; } named[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 0xA0 }, { "ndash", 0x2013 },
    { "mdash", 0x2014 }, { "hellip", 0x2026 }, { "times", 0xD7 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "rarr", 0x2192 },
    { "larr", 0x2190 }, { "le", 0x2264 }, { "ge", 0x2265 },
    { "ne", 0x2260 }, { "deg", 0xB0 }, { "micro", 0xB5 },
    { "sect", 0xA7 }, { "middot", 0xB7 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
};

/* An entity reference at s[0..n): its length, its code point in *cp
 * (0 for a name this table does not know). 0 when it is not one. */
static size_t entity(const char *s, size_t n, unsigned *cp)
{
    size_t k = 1;
    unsigned v = 0;
    *cp = 0;
    if (n < 3 || s[0] != '&')
        return 0;
    if (s[1] == '#') {
        int hex = k + 1 < n && (s[2] == 'x' || s[2] == 'X');
        k = hex ? 3 : 2;
        size_t d0 = k;
        while (k < n && k - d0 < (hex ? 6u : 7u) &&
               (hex ? isxdigit((unsigned char)s[k]) : isdigit((unsigned char)s[k]))) {
            int c = (unsigned char)s[k];
            v = v * (hex ? 16 : 10) +
                (unsigned)(isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
            k++;
        }
        if (k == d0 || k >= n || s[k] != ';')
            return 0;
        *cp = v == 0 || v > 0x10FFFF ? 0xFFFD : v;
        return k + 1;
    }
    if (!isalpha((unsigned char)s[1]))
        return 0;
    while (k < n && k < 33 && isalnum((unsigned char)s[k]))
        k++;
    if (k >= n || s[k] != ';')
        return 0;
    for (size_t j = 0; j < sizeof named / sizeof named[0]; j++)
        if (strlen(named[j].name) == k - 1 && !memcmp(named[j].name, s + 1, k - 1))
            *cp = named[j].cp;
    return k + 1;
}

char *html_text(const char *html, size_t n)
{
    struct buf b = { 0 };
    buf_add(&b, "", 0);
    for (size_t k = 0; k < n; k++) {
        if (html[k] == '<') {
            while (k < n && html[k] != '>')
                k++;
            continue;
        }
        unsigned cp;
        size_t e = html[k] == '&' ? entity(html + k, n - k, &cp) : 0;
        if (e && cp) {
            u8_put(&b, cp);
            k += e - 1;
            continue;
        }
        buf_putc(&b, html[k]);
    }
    return b.p;
}

/* ---- the page's allocations ------------------------------------------ */

/* Everything the block tree and its lines take, freed together when the
 * page is rendered. */
static void **pool;
static int npool, cappool;

static void *palloc(size_t n)
{
    if (npool == cappool) {
        cappool = cappool ? cappool * 2 : 256;
        pool = xrealloc(pool, (size_t)cappool * sizeof *pool);
    }
    void *p = xmalloc(n);
    memset(p, 0, n);
    pool[npool++] = p;
    return p;
}

static void pool_free(void)
{
    for (int k = 0; k < npool; k++)
        free(pool[k]);
    npool = 0;
}

/* ---- lines ------------------------------------------------------------ */

/* A line of the current container: its text without the container's
 * prefix (a list item's indentation, a quote's >) and its source line. */
struct ln {
    const char *s;
    int no;
};

static const char *skip_sp(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int is_blank(const char *s)
{
    return *skip_sp(s) == 0;
}

/* The columns of leading white space, a tab reaching the next stop of 4. */
static int indent_of(const char *s)
{
    int col = 0;
    for (; *s == ' ' || *s == '\t'; s++)
        col = *s == '\t' ? (col + 4) & ~3 : col + 1;
    return col;
}

/* s without its first n columns of white space. A tab that straddles the
 * cut leaves the columns past it as spaces. */
static const char *strip_cols(const char *s, int n)
{
    int col = 0;
    const char *p = s;
    while (col < n && (*p == ' ' || *p == '\t')) {
        int next = *p == '\t' ? (col + 4) & ~3 : col + 1;
        if (next > n) {
            size_t rest = strlen(p + 1);
            char *r = palloc((size_t)(next - n) + rest + 1);
            memset(r, ' ', (size_t)(next - n));
            memcpy(r + (next - n), p + 1, rest + 1);
            return r;
        }
        col = next;
        p++;
    }
    return p;
}

/* ---- block starts ------------------------------------------------------ */

/* An ATX heading's level (1..6) for the text after the indentation. */
static int atx(const char *t)
{
    int k = 0;
    while (t[k] == '#')
        k++;
    if (k < 1 || k > 6)
        return 0;
    return t[k] == 0 || t[k] == ' ' || t[k] == '\t' ? k : 0;
}

/* A code fence: the length of its run of ` or ~ (3 or more), else 0. */
static int fence(const char *t, char *ch)
{
    char c = *t;
    int k = 0;
    if (c != '`' && c != '~')
        return 0;
    while (t[k] == c)
        k++;
    if (k < 3)
        return 0;
    if (c == '`' && strchr(t + k, '`'))
        return 0;           /* a backtick fence's info string has none */
    *ch = c;
    return k;
}

/* A thematic break: three or more of one of * - _, spaces between. */
static int is_hr(const char *t)
{
    char c = *t;
    int k = 0;
    if (c != '*' && c != '-' && c != '_')
        return 0;
    for (; *t; t++) {
        if (*t == c)
            k++;
        else if (*t != ' ' && *t != '\t')
            return 0;
    }
    return k >= 3;
}

/* A setext underline: = or - only (then spaces). Its level, else 0. */
static int setext(const char *t)
{
    char c = *t;
    if (c != '=' && c != '-')
        return 0;
    while (*t == c)
        t++;
    return is_blank(t) ? (c == '=' ? 1 : 2) : 0;
}

/* A list item's marker at t: its width, 0 when there is none. *ord is 1
 * for an ordered item, *num its number, *mc the bullet or the delimiter. */
static int marker(const char *t, int *ord, int *num, char *mc)
{
    if (*t == '-' || *t == '*' || *t == '+') {
        if (t[1] && t[1] != ' ' && t[1] != '\t')
            return 0;
        *ord = 0;
        *num = 0;
        *mc = *t;
        return 1;
    }
    int k = 0, v = 0;
    while (k < 9 && isdigit((unsigned char)t[k])) {
        v = v * 10 + (t[k] - '0');
        k++;
    }
    if (k == 0 || (t[k] != '.' && t[k] != ')'))
        return 0;
    if (t[k + 1] && t[k + 1] != ' ' && t[k + 1] != '\t')
        return 0;
    *ord = 1;
    *num = v;
    *mc = t[k];
    return k + 1;
}

/* Whether a line starts a block that ends a paragraph in the SAME
 * container: CommonMark lets a list interrupt a paragraph only with a
 * non-empty item, and an ordered one only when it starts at 1. */
static int starts_block(const char *s)
{
    if (indent_of(s) >= 4)
        return 0;
    const char *t = skip_sp(s);
    char c;
    if (atx(t) || is_hr(t) || *t == '>' || fence(t, &c) ||
        !strncmp(t, "<!--", 4))
        return 1;
    int ord, num, w = marker(t, &ord, &num, &c);
    return w && !is_blank(t + w) && (!ord || num == 1);
}

/* ...and what ends a lazy continuation line of a quote or a list item:
 * there any list marker starts an item of its own. */
static int ends_lazy(const char *s)
{
    if (starts_block(s))
        return 1;
    int ord, num;
    char c;
    return indent_of(s) < 4 && marker(skip_sp(s), &ord, &num, &c);
}

/* A paragraph's line, as opposed to a heading, a break or a fence -- the
 * only kind of line a lazy one can continue. */
static int para_line(const char *s)
{
    const char *t = skip_sp(s);
    char c;
    return *t && !atx(t) && !is_hr(t) && !fence(t, &c);
}

/* ---- table rows ------------------------------------------------------- */

struct cell {
    const char *s;
    size_t n;
};

/* A table row's cells, trimmed: pointers into s. A backslash keeps the
 * next character in its cell, so \| is a pipe inside one. A closing |
 * ends the row without an empty cell after it. Returns the count;
 * fills c only up to max. */
static int cells(const char *s, struct cell *c, int max)
{
    const char *p = skip_sp(s);
    int k = 0;
    if (*p == '|')
        p++;
    for (;;) {
        const char *b = p;
        while (*p && *p != '|') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        const char *e = p;
        while (b < e && (*b == ' ' || *b == '\t'))
            b++;
        while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        if (*p == 0 && e == b)
            break;
        if (k < max) {
            c[k].s = b;
            c[k].n = (size_t)(e - b);
        }
        k++;
        if (*p == 0)
            break;
        p++;
    }
    return k;
}

/* A table's delimiter row: its column count, the alignments in al. */
static int delim_row(const char *s, char *al, int max)
{
    if (indent_of(s) >= 4 || !strchr(s, '|') || !strchr(s, '-'))
        return 0;
    struct cell c[128];
    int n = cells(s, c, 128);
    if (n < 1 || n > 128 || n > max)
        return 0;
    for (int k = 0; k < n; k++) {
        const char *p = c[k].s, *e = c[k].s + c[k].n;
        int left = p < e && *p == ':', right = e > p && e[-1] == ':';
        if (left)
            p++;
        if (right && e > p)
            e--;
        if (p == e)
            return 0;
        for (; p < e; p++)
            if (*p != '-')
                return 0;
        al[k] = left && right ? 'c' : left ? 'l' : right ? 'r' : 0;
    }
    return n;
}

/* Whether a table starts at L[i]: a row of cells with a pipe in it, then
 * a delimiter row with as many. */
static int table_at(const struct ln *L, int n, int i, char *al, int max)
{
    if (i + 1 >= n || indent_of(L[i].s) >= 4 || !strchr(L[i].s, '|'))
        return 0;
    int nd = delim_row(L[i + 1].s, al, max);
    if (!nd)
        return 0;
    struct cell c[1];
    return cells(L[i].s, c, 1) == nd ? nd : 0;
}

/* ---- the block tree --------------------------------------------------- */

enum { B_PARA, B_HEAD, B_CODE, B_HR, B_QUOTE, B_LIST, B_ITEM, B_TABLE };

struct blk {
    int kind;
    int level;          /* heading: 1..6; list: 1 when ordered */
    int start;          /* ordered list: its first number */
    int tight;          /* list */
    int blank_before;   /* a blank line separates it from what precedes */
    struct ln *l;       /* paragraph, heading, code, table: its lines */
    int nl;
    const char *lang;   /* code: the info string's first word, or NULL */
    char *align;        /* table: per column, 'l', 'c', 'r' or 0 */
    int ncol;
    struct blk *kid;    /* quote, list, item: what each contains */
    struct blk *next;
};

static struct blk *parse(const struct ln *L, int n);

static struct blk *new_blk(int kind)
{
    struct blk *b = palloc(sizeof *b);
    b->kind = kind;
    return b;
}

static struct ln *new_lines(int n)
{
    return palloc((size_t)(n ? n : 1) * sizeof(struct ln));
}

/* A fenced code block from L[i]: returns the line after its close. */
static int parse_fence(const struct ln *L, int n, int i, struct blk *b)
{
    int ind = indent_of(L[i].s);
    const char *t = skip_sp(L[i].s);
    char ch;
    int len = fence(t, &ch);
    const char *info = skip_sp(t + len);
    size_t w = 0;
    while (info[w] && info[w] != ' ' && info[w] != '\t')
        w++;
    b->kind = B_CODE;
    if (w) {
        char *l = palloc(w + 1);
        memcpy(l, info, w);
        l[w] = 0;
        b->lang = l;
    }
    int j = i + 1;
    b->l = new_lines(n - j);
    for (; j < n; j++) {
        const char *u = skip_sp(L[j].s);
        char c2 = 0;
        int k = indent_of(L[j].s) < 4 ? fence(u, &c2) : 0;
        /* the close: as long a run of the same character, nothing after */
        if (k >= len && c2 == ch && is_blank(u + k)) {
            j++;
            break;
        }
        b->l[b->nl].s = strip_cols(L[j].s, ind);
        b->l[b->nl].no = L[j].no;
        b->nl++;
    }
    return j;
}

/* An indented code block from L[i]. */
static int parse_indented(const struct ln *L, int n, int i, struct blk *b)
{
    int j = i, last = i;
    b->kind = B_CODE;
    b->l = new_lines(n - i);
    for (; j < n; j++) {
        if (!is_blank(L[j].s) && indent_of(L[j].s) < 4)
            break;
        b->l[b->nl].s = strip_cols(L[j].s, 4);
        b->l[b->nl].no = L[j].no;
        b->nl++;
        if (!is_blank(L[j].s))
            last = b->nl;
    }
    b->nl = last;           /* trailing blank lines are not code */
    while (j > i && is_blank(L[j - 1].s))
        j--;
    return j;
}

/* The lines of a block quote from L[i], their > removed. */
static int parse_quote(const struct ln *L, int n, int i, struct blk *b)
{
    struct ln *q = new_lines(n - i);
    int m = 0, j = i, in_fence = 0;
    for (; j < n; j++) {
        const char *s = L[j].s, *t = skip_sp(s);
        if (indent_of(s) < 4 && *t == '>') {
            t++;
            if (*t == ' ' || *t == '\t')
                t++;
            char c;
            if (fence(skip_sp(t), &c))
                in_fence = !in_fence;
        } else if (!is_blank(s) && m > 0 && !in_fence && para_line(q[m - 1].s) &&
                   !ends_lazy(s)) {
            t = skip_sp(s);     /* a lazy continuation of its paragraph */
        } else {
            break;
        }
        q[m].s = t;
        q[m].no = L[j].no;
        m++;
    }
    b->kind = B_QUOTE;
    b->kid = parse(q, m);
    return j;
}

/* One list item from L[i], whose marker is t (width w): its content
 * lines, the marker's indentation removed. Returns the line after it,
 * not counting blank lines that end it. */
static int parse_item(const struct ln *L, int n, int i, const char *t, int w,
                      struct blk *item)
{
    const char *s = L[i].s;
    int ind = indent_of(s);
    const char *after = t + w, *content = skip_sp(after);
    int sp = indent_of(after);
    int col;
    if (*content == 0) {
        col = ind + w + 1;              /* an empty first line */
    } else if (sp > 4) {
        col = ind + w + 1;              /* content that is indented code */
        content = after + 1;
    } else {
        col = ind + w + sp;
    }
    struct ln *il = new_lines(n - i);
    int m = 0, j = i + 1, in_fence = 0;
    char c;
    il[m].s = content;
    il[m].no = L[i].no;
    m++;
    if (fence(content, &c))
        in_fence = 1;
    while (j < n) {
        const char *u = L[j].s;
        if (is_blank(u)) {
            int k = j;
            while (k < n && is_blank(L[k].s))
                k++;
            if (k >= n || indent_of(L[k].s) < col)
                break;
            for (; j < k; j++) {
                il[m].s = "";
                il[m].no = L[j].no;
                m++;
            }
            continue;
        }
        if (indent_of(u) >= col) {
            il[m].s = strip_cols(u, col);
        } else if (!in_fence && para_line(il[m - 1].s) && !ends_lazy(u)) {
            il[m].s = skip_sp(u);       /* lazy continuation */
        } else {
            break;
        }
        if (fence(skip_sp(il[m].s), &c))
            in_fence = !in_fence;
        il[m].no = L[j].no;
        m++;
        j++;
    }
    item->kind = B_ITEM;
    item->kid = parse(il, m);
    return j;
}

/* A list from L[i]: items of one kind, blank lines allowed between. */
static int parse_list(const struct ln *L, int n, int i, struct blk *list)
{
    const char *t = skip_sp(L[i].s);
    int ord, num;
    char mc;
    int w = marker(t, &ord, &num, &mc);
    struct blk **tail = &list->kid;
    list->kind = B_LIST;
    list->level = ord;
    list->start = num;
    list->tight = 1;
    for (;;) {
        struct blk *item = new_blk(B_ITEM);
        int j = parse_item(L, n, i, t, w, item);
        for (struct blk *k = item->kid; k; k = k->next)
            if (k != item->kid && k->blank_before)
                list->tight = 0;
        *tail = item;
        tail = &item->next;
        int k = j;
        while (k < n && is_blank(L[k].s))
            k++;
        if (k >= n || indent_of(L[k].s) >= 4)
            return j;
        const char *t2 = skip_sp(L[k].s);
        int o2, n2;
        char c2;
        int w2 = marker(t2, &o2, &n2, &c2);
        if (!w2 || o2 != ord || c2 != mc || is_hr(t2))
            return j;
        if (k > j)
            list->tight = 0;
        i = k;
        t = t2;
        w = w2;
    }
}

/* A table from L[i]: the header row, then rows up to a blank line or the
 * start of another block. */
static int parse_table(const struct ln *L, int n, int i, int ncol, char *al,
                       struct blk *b)
{
    int j = i + 2;
    while (j < n && !is_blank(L[j].s) && !starts_block(L[j].s))
        j++;
    b->kind = B_TABLE;
    b->ncol = ncol;
    b->align = palloc((size_t)ncol);
    memcpy(b->align, al, (size_t)ncol);
    b->l = new_lines(j - i);
    b->l[b->nl++] = L[i];
    for (int k = i + 2; k < j; k++)
        b->l[b->nl++] = L[k];
    return j;
}

/* An ATX heading's text: without the #s, the spaces round it, and an
 * optional closing run of #s. */
static const char *atx_text(const char *t, int level)
{
    const char *p = skip_sp(t + level);
    size_t e = strlen(p);
    while (e > 0 && (p[e - 1] == ' ' || p[e - 1] == '\t'))
        e--;
    size_t h = e;
    while (h > 0 && p[h - 1] == '#')
        h--;
    if (h < e && (h == 0 || p[h - 1] == ' ' || p[h - 1] == '\t')) {
        e = h;
        while (e > 0 && (p[e - 1] == ' ' || p[e - 1] == '\t'))
            e--;
    }
    char *r = palloc(e + 1);
    memcpy(r, p, e);
    r[e] = 0;
    return r;
}

/* The blocks in L[0..n), a container's lines. */
static struct blk *parse(const struct ln *L, int n)
{
    struct blk *head = NULL, **tail = &head;
    int blank = 0, i = 0;
    while (i < n) {
        const char *s = L[i].s;
        if (is_blank(s)) {
            blank = 1;
            i++;
            continue;
        }
        const char *t = skip_sp(s);
        int ind = indent_of(s);
        struct blk *b = new_blk(B_PARA);
        char ch, al[128];
        int ord, num, ncol;
        if (ind >= 4) {
            i = parse_indented(L, n, i, b);
        } else if (fence(t, &ch)) {
            i = parse_fence(L, n, i, b);
        } else if (atx(t)) {
            b->kind = B_HEAD;
            b->level = atx(t);
            b->l = new_lines(1);
            b->l[0].s = atx_text(t, b->level);
            b->l[0].no = L[i].no;
            b->nl = 1;
            i++;
        } else if (is_hr(t)) {
            b->kind = B_HR;
            i++;
        } else if (*t == '>') {
            i = parse_quote(L, n, i, b);
        } else if (!strncmp(t, "<!--", 4)) {
            /* a comment, to the line that closes it: not shown */
            const char *e = strstr(t + 4, "-->");
            while (!e && i + 1 < n) {
                i++;
                e = strstr(L[i].s, "-->");
            }
            i++;
            continue;
        } else if (marker(t, &ord, &num, &ch)) {
            i = parse_list(L, n, i, b);
        } else if ((ncol = table_at(L, n, i, al, 128)) != 0) {
            i = parse_table(L, n, i, ncol, al, b);
        } else {
            int j = i + 1;
            while (j < n) {
                const char *u = L[j].s;
                int lv = indent_of(u) < 4 ? setext(skip_sp(u)) : 0;
                if (is_blank(u))
                    break;
                if (lv) {
                    b->kind = B_HEAD;
                    b->level = lv;
                    break;
                }
                if (starts_block(u) || table_at(L, n, j, al, 128))
                    break;
                j++;
            }
            b->l = new_lines(j - i);
            for (int k = i; k < j; k++)
                b->l[b->nl++] = L[k];
            i = b->kind == B_HEAD ? j + 1 : j;
        }
        b->blank_before = blank;
        blank = 0;
        *tail = b;
        tail = &b->next;
    }
    return head;
}

/* ---- inlines ---------------------------------------------------------- */

/* A run of the output: HTML (c == 0), or a run of * or _ that emphasis
 * may still take characters from. A run keeps its closing tags in pre
 * and its opening tags in post: the characters it has left stand
 * between them. */
struct piece {
    int c;
    int n, orig;
    int open, close;
    int dead;           /* off the delimiter stack */
    struct buf pre, post;
};

/* Where an inline is: inside a link's text (no links there), or in a
 * table cell. */
enum { IN_LINK = 1, IN_CELL = 2 };

struct inl {
    struct md_page *pg;
    const char *s;
    size_t n;
    const int *lno;     /* the source line of each line of s */
    int nl;
    int flags;          /* IN_LINK, IN_CELL */
    int visible;        /* wrote something besides anchors and spaces */
    struct piece *p;
    int np, capp;
};

static struct buf *lit(struct inl *in)
{
    if (in->np == 0 || in->p[in->np - 1].c != 0) {
        if (in->np == in->capp) {
            in->capp = in->capp ? in->capp * 2 : 16;
            in->p = xrealloc(in->p, (size_t)in->capp * sizeof *in->p);
        }
        memset(&in->p[in->np], 0, sizeof in->p[0]);
        in->np++;
    }
    return &in->p[in->np - 1].pre;
}

static void put(struct inl *in, const char *s, size_t n)
{
    buf_add(lit(in), s, n);
}

static void puts_(struct inl *in, const char *s)
{
    buf_puts(lit(in), s);
}

static void put_html(struct inl *in, const char *s, size_t n)
{
    buf_html(lit(in), s, n);
}

static int line_at(const struct inl *in, size_t pos)
{
    int k = 0;
    for (size_t i = 0; i < pos && i < in->n; i++)
        if (in->s[i] == '\n')
            k++;
    if (!in->lno || in->nl == 0)
        return 0;
    return in->lno[k < in->nl ? k : in->nl - 1];
}

static void inline_html(struct md_page *pg, const char *s, size_t n,
                        const int *lno, int nl, int flags, struct buf *out,
                        int *visible);

/* A backtick run's length at s[i]. */
static size_t ticks(const char *s, size_t n, size_t i)
{
    size_t k = i;
    while (k < n && s[k] == '`')
        k++;
    return k - i;
}

/* Where the code span opened by k backticks at s[i] closes: the index of
 * its closing run, or 0 when there is none. */
static size_t code_close(const char *s, size_t n, size_t i, size_t k)
{
    for (size_t j = i + k; j < n;) {
        if (s[j] != '`') {
            j++;
            continue;
        }
        size_t r = ticks(s, n, j);
        if (r == k)
            return j;
        j += r;
    }
    return 0;
}

/* A code span's content: line ends as spaces, and one space stripped
 * from each end when both have one and it is not all spaces. In a table
 * cell, a span that is one word -- thumbv7em-none-eabi -- is class "nw",
 * which the stylesheet keeps on one line: a cell may not break it at a
 * hyphen, and the table scrolls in its own box instead. */
static void code_span(struct buf *b, const char *s, size_t n, int cell)
{
    size_t a = 0, e = n, k;
    for (k = 0; k < n && (s[k] == ' ' || s[k] == '\n'); k++)
        ;
    if (k < n && n >= 2 && (s[0] == ' ' || s[0] == '\n') &&
        (s[n - 1] == ' ' || s[n - 1] == '\n')) {
        a = 1;
        e = n - 1;
    }
    int word = cell && e > a;
    for (k = a; k < e && word; k++)
        if (s[k] == ' ' || s[k] == '\n')
            word = 0;
    buf_puts(b, word ? "<code class=\"nw\">" : "<code>");
    for (k = a; k < e; k++) {
        if (s[k] == '\n')
            buf_putc(b, ' ');
        else
            buf_html(b, s + k, 1);
    }
    buf_puts(b, "</code>");
}

/* <a id="NAME"></a> (or name=, single quotes, spaces): its length, the
 * name in id. 0 when s is not one, or the name is not a plain one. */
static size_t anchor_tag(const char *s, size_t n, char *id, size_t cap)
{
    size_t k = 2;
    if (n < 10 || strncmp(s, "<a", 2) != 0 || (s[2] != ' ' && s[2] != '\t'))
        return 0;
    while (k < n && (s[k] == ' ' || s[k] == '\t'))
        k++;
    if (k + 3 < n && !strncmp(s + k, "id=", 3))
        k += 3;
    else if (k + 5 < n && !strncmp(s + k, "name=", 5))
        k += 5;
    else
        return 0;
    char q = s[k];
    if (q != '"' && q != '\'')
        return 0;
    size_t b = ++k;
    while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '-' ||
                     s[k] == '_' || s[k] == '.' || s[k] == ':'))
        k++;
    if (k == b || k >= n || s[k] != q || k - b >= cap)
        return 0;
    memcpy(id, s + b, k - b);
    id[k - b] = 0;
    k++;
    while (k < n && (s[k] == ' ' || s[k] == '\t'))
        k++;
    if (k + 5 > n || strncmp(s + k, "></a>", 5) != 0)
        return 0;
    return k + 5;
}

static void add_id(struct md_page *pg, const char *id)
{
    if (pg->nids == pg->capids) {
        pg->capids = pg->capids ? pg->capids * 2 : 16;
        pg->ids = xrealloc(pg->ids, (size_t)pg->capids * sizeof *pg->ids);
    }
    pg->ids[pg->nids++] = xstrdup(id);
}

/* The scheme of a destination ("http" from "http://..."), length 0 for a
 * relative one. */
static size_t scheme(const char *s, size_t n)
{
    size_t k = 0;
    if (n == 0 || !isalpha((unsigned char)s[0]))
        return 0;
    while (k < n && k < 32 && (isalnum((unsigned char)s[k]) || s[k] == '+' ||
                               s[k] == '.' || s[k] == '-'))
        k++;
    return k >= 2 && k < n && s[k] == ':' ? k : 0;
}

/* A link: <a href=...>, the destination given to the page's callback,
 * which knows where every file of the site ends up. A scheme other than
 * http, https or mailto is refused (#). */
static void link_open(struct inl *in, const char *dest, size_t dn,
                      const char *title, size_t tn, int line)
{
    char *d = xstrndup(dest, dn);
    char *href;
    size_t sc = scheme(d, dn);
    if (sc && !((sc == 4 && !strncmp(d, "http", 4)) ||
                (sc == 5 && !strncmp(d, "https", 5)) ||
                (sc == 6 && !strncmp(d, "mailto", 6))))
        href = xstrdup("#");
    else if (in->pg->link)
        href = in->pg->link(in->pg->arg, d, line);
    else
        href = xstrdup(d);
    puts_(in, "<a href=\"");
    put_html(in, href, strlen(href));
    puts_(in, "\"");
    if (title) {
        puts_(in, " title=\"");
        put_html(in, title, tn);
        puts_(in, "\"");
    }
    puts_(in, ">");
    free(href);
    free(d);
}

/* The text between [ and ], rendered as the link's content. */
static void link_text(struct inl *in, size_t a, size_t e)
{
    struct buf t = { 0 };
    int vis = 0;
    inline_html(in->pg, in->s + a, e - a, NULL, 0,
                IN_LINK | (in->flags & IN_CELL), &t, &vis);
    if (t.n)
        put(in, t.p, t.n);
    in->visible = 1;
    buf_free(&t);
}

/* Undo backslash escapes in a link destination or title. */
static char *unescape(const char *s, size_t n, size_t *out)
{
    char *r = xmalloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1]))
            i++;
        r[k++] = s[i];
    }
    r[k] = 0;
    *out = k;
    return r;
}

/* [text](dest "title") at s[i]: its length, 0 when it is not a link. */
static size_t try_link(struct inl *in, size_t i)
{
    const char *s = in->s;
    size_t n = in->n, p = i + 1;
    int depth = 1;
    while (p < n) {
        if (s[p] == '\\' && p + 1 < n) {
            p += 2;
            continue;
        }
        if (s[p] == '`') {
            size_t k = ticks(s, n, p), c = code_close(s, n, p, k);
            p = c ? c + k : p + k;
            continue;
        }
        if (s[p] == '[')
            depth++;
        if (s[p] == ']') {
            depth--;
            if (depth == 0)
                break;
        }
        p++;
    }
    if (p + 1 >= n || s[p + 1] != '(')
        return 0;
    size_t te = p, q = p + 2;
    while (q < n && (s[q] == ' ' || s[q] == '\t' || s[q] == '\n'))
        q++;
    size_t da, de;
    if (q < n && s[q] == '<') {
        da = ++q;
        while (q < n && s[q] != '>' && s[q] != '\n' && s[q] != '<')
            q++;
        if (q >= n || s[q] != '>')
            return 0;
        de = q++;
    } else {
        int par = 0;
        da = q;
        while (q < n && (unsigned char)s[q] > ' ') {
            if (s[q] == '\\' && q + 1 < n) {
                q += 2;
                continue;
            }
            if (s[q] == '(')
                par++;
            if (s[q] == ')') {
                if (par == 0)
                    break;
                par--;
            }
            q++;
        }
        de = q;
    }
    size_t ta = 0, tl = 0;
    int has_title = 0;
    size_t r = q;
    while (r < n && (s[r] == ' ' || s[r] == '\t' || s[r] == '\n'))
        r++;
    if (r > q && r < n && (s[r] == '"' || s[r] == '\'' || s[r] == '(')) {
        char close = s[r] == '(' ? ')' : s[r];
        size_t t = r + 1;
        while (t < n && s[t] != close) {
            if (s[t] == '\\' && t + 1 < n)
                t++;
            t++;
        }
        if (t >= n)
            return 0;
        ta = r + 1;
        tl = t - ta;
        has_title = 1;
        r = t + 1;
        while (r < n && (s[r] == ' ' || s[r] == '\t' || s[r] == '\n'))
            r++;
    }
    if (r >= n || s[r] != ')')
        return 0;

    size_t dn, tn = 0;
    char *dest = unescape(s + da, de - da, &dn);
    char *title = has_title ? unescape(s + ta, tl, &tn) : NULL;
    link_open(in, dest, dn, title, tn, line_at(in, i));
    link_text(in, i + 1, te);
    puts_(in, "</a>");
    free(dest);
    free(title);
    return r + 1 - i;
}

/* <scheme:...>: its length, 0 when s[i] does not start one. */
static size_t try_autolink(struct inl *in, size_t i)
{
    const char *s = in->s + i + 1;
    size_t n = in->n - i - 1, sc = scheme(s, n), k = sc;
    if (!sc)
        return 0;
    while (k < n && s[k] != '>' && s[k] != '<' && (unsigned char)s[k] > ' ')
        k++;
    if (k >= n || s[k] != '>')
        return 0;
    link_open(in, s, k, NULL, 0, line_at(in, i));
    put_html(in, s, k);
    puts_(in, "</a>");
    in->visible = 1;
    return k + 2;
}

/* A bare http:// or https:// URL (GFM's extended autolink): its length,
 * 0 when s[i] does not start one. Trailing punctuation is the sentence's,
 * and so is a ) that does not close a ( in the URL. */
static size_t try_bare_url(struct inl *in, size_t i)
{
    const char *s = in->s + i;
    size_t n = in->n - i, k;
    if (n >= 8 && !strncmp(s, "https://", 8))
        k = 8;
    else if (n >= 7 && !strncmp(s, "http://", 7))
        k = 7;
    else
        return 0;
    size_t dom = k;
    int dots = 0;
    while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '-' ||
                     s[k] == '_' || s[k] == '.')) {
        if (s[k] == '.')
            dots++;
        k++;
    }
    if (k == dom || !dots)
        return 0;
    while (k < n && !isspace((unsigned char)s[k]) && s[k] != '<')
        k++;
    for (;;) {
        char c = s[k - 1];
        if (strchr("?!.,:*_~'\"", c)) {
            k--;
            continue;
        }
        if (c == ')') {
            int open = 0, close = 0;
            for (size_t j = 0; j < k; j++) {
                open += s[j] == '(';
                close += s[j] == ')';
            }
            if (close > open) {
                k--;
                continue;
            }
        }
        break;
    }
    link_open(in, s, k, NULL, 0, line_at(in, i));
    put_html(in, s, k);
    puts_(in, "</a>");
    in->visible = 1;
    return k;
}

/* A run of * or _ at s[i]: a piece with CommonMark's flanking rules. */
static size_t delim_run(struct inl *in, size_t i)
{
    const char *s = in->s;
    char c = s[i];
    size_t k = i;
    while (k < in->n && s[k] == c)
        k++;
    int len;
    unsigned before = u8_before(s, i);
    unsigned after = k < in->n ? u8_at(s, in->n, k, &len) : ' ';
    int left = !u_space(after) &&
               (!u_punct(after) || u_space(before) || u_punct(before));
    int right = !u_space(before) &&
                (!u_punct(before) || u_space(after) || u_punct(after));
    if (in->np == in->capp) {
        in->capp = in->capp ? in->capp * 2 : 16;
        in->p = xrealloc(in->p, (size_t)in->capp * sizeof *in->p);
    }
    struct piece *p = &in->p[in->np++];
    memset(p, 0, sizeof *p);
    p->c = c;
    p->n = p->orig = (int)(k - i);
    if (c == '*') {
        p->open = left;
        p->close = right;
    } else {
        p->open = left && (!right || u_punct(before));
        p->close = right && (!left || u_punct(after));
    }
    in->visible = 1;
    return k - i;
}

/* CommonMark's process_emphasis, over the pieces: each closer, left to
 * right, takes the nearest opener of its character that the rule of 3
 * allows -- two characters for <strong> when both have two, else one for
 * <em> -- and the runs between them stop being delimiters. */
static void emphasis(struct inl *in)
{
    for (int ci = 0; ci < in->np; ci++) {
        struct piece *cl = &in->p[ci];
        if (!cl->c || !cl->close)
            continue;
        while (cl->n > 0) {
            int oi;
            for (oi = ci - 1; oi >= 0; oi--) {
                struct piece *op = &in->p[oi];
                if (op->c != cl->c || !op->open || op->dead || op->n == 0)
                    continue;
                if ((op->close || cl->open) && (op->orig + cl->orig) % 3 == 0 &&
                    !(op->orig % 3 == 0 && cl->orig % 3 == 0))
                    continue;
                break;
            }
            if (oi < 0)
                break;
            struct piece *op = &in->p[oi];
            int use = op->n >= 2 && cl->n >= 2 ? 2 : 1;
            const char *otag = use == 2 ? "<strong>" : "<em>";
            const char *ctag = use == 2 ? "</strong>" : "</em>";
            op->n -= use;
            cl->n -= use;
            /* a later match is outside the earlier ones: its open tag
             * goes first, its close tag last */
            struct buf t = { 0 };
            buf_puts(&t, otag);
            if (op->post.n)
                buf_add(&t, op->post.p, op->post.n);
            buf_free(&op->post);
            op->post = t;
            buf_puts(&cl->pre, ctag);
            for (int k = oi + 1; k < ci; k++)
                in->p[k].dead = 1;
        }
    }
}

/* The index in s[0..n) where pat starts, at or after from: n if nowhere. */
static size_t find(const char *s, size_t n, size_t from, const char *pat)
{
    size_t m = strlen(pat);
    for (size_t k = from; k + m <= n; k++)
        if (!memcmp(s + k, pat, m))
            return k;
    return n;
}

/* <code>...</code> at s[i]: written as a code span, the entity
 * references inside kept. Its length, 0 when s[i] does not start one. */
static size_t code_tag(struct inl *in, size_t i)
{
    const char *s = in->s;
    size_t n = in->n;
    if (n - i < 13 || strncmp(s + i, "<code>", 6) != 0)
        return 0;
    size_t e = find(s, n, i + 6, "</code>");
    if (e == n || find(s, e, i + 6, "<") != e)
        return 0;
    puts_(in, "<code>");
    for (size_t k = i + 6; k < e; k++) {
        unsigned cp;
        size_t el = s[k] == '&' ? entity(s + k, e - k, &cp) : 0;
        if (el) {
            put(in, s + k, el);
            k += el - 1;
        } else {
            put_html(in, s + k, 1);
        }
    }
    puts_(in, "</code>");
    in->visible = 1;
    return e + 7 - i;
}

/* One construct at s[i]; returns the index after it. */
static size_t inline_step(struct inl *in, size_t i)
{
    const char *s = in->s;
    size_t n = in->n, k;
    char c = s[i];
    unsigned cp;

    if (c == '\\' && i + 1 < n && s[i + 1] == '\n') {
        puts_(in, "<br />\n");
        return i + 2;
    }
    if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
        put_html(in, s + i + 1, 1);
        in->visible = 1;
        return i + 2;
    }
    if (c == '`') {
        k = ticks(s, n, i);
        size_t e = code_close(s, n, i, k);
        in->visible = 1;
        if (!e) {
            put(in, s + i, k);      /* no closing run: the ticks are text */
            return i + k;
        }
        code_span(lit(in), s + i + k, e - i - k, in->flags & IN_CELL);
        return e + k;
    }
    if (c == '*' || c == '_')
        return i + delim_run(in, i);
    if (c == '[' && !(in->flags & IN_LINK)) {
        k = try_link(in, i);
        if (k)
            return i + k;
    }
    if (c == '<') {
        char id[128];
        k = anchor_tag(s + i, n - i, id, sizeof id);
        if (k) {
            puts_(in, "<a id=\"");
            put_html(in, id, strlen(id));
            puts_(in, "\"></a>");
            add_id(in->pg, id);
            return i + k;
        }
        if (!strncmp(s + i, "<!--", 4) && find(s, n, i + 4, "-->") < n)
            return find(s, n, i + 4, "-->") + 3;    /* a comment: not shown */
        k = code_tag(in, i);
        if (k)
            return i + k;
        k = (in->flags & IN_LINK) ? 0 : try_autolink(in, i);
        if (k)
            return i + k;
    }
    if (c == '&') {
        k = entity(s + i, n - i, &cp);
        if (k) {
            put(in, s + i, k);
            in->visible = 1;
            return i + k;
        }
    }
    if (c == 'h' && !(in->flags & IN_LINK) &&
        (i == 0 || strchr(" \t\n*_~(", s[i - 1]))) {
        k = try_bare_url(in, i);
        if (k)
            return i + k;
    }
    if (c == ' ' || c == '\t') {
        k = i;
        while (k < n && (s[k] == ' ' || s[k] == '\t'))
            k++;
        if (k < n && s[k] == '\n')        /* two spaces end a line hard */
            puts_(in, k - i >= 2 ? "<br />\n" : "\n");
        else if (k < n)
            put(in, s + i, k - i);
        return k < n && s[k] == '\n' ? k + 1 : k;
    }
    if (c == '\n') {
        puts_(in, "\n");
        return i + 1;
    }
    /* text, up to the next character that may start something */
    k = i + 1;
    while (k < n && !strchr("\\`*_[<&h \t\n", s[k]))
        k++;
    put_html(in, s + i, k - i);
    in->visible = 1;
    return k;
}

/* Render s[0..n) -- a paragraph's text, lines joined by \n -- as HTML:
 * the constructs into pieces, then emphasis matched across them. lno
 * gives each line's source line, for the link callback; *visible is set
 * when the result shows anything besides anchors and white space. */
static void inline_html(struct md_page *pg, const char *s, size_t n,
                        const int *lno, int nl, int flags, struct buf *out,
                        int *visible)
{
    struct inl in;
    memset(&in, 0, sizeof in);
    in.pg = pg;
    in.s = s;
    in.n = n;
    in.lno = lno;
    in.nl = nl;
    in.flags = flags;
    for (size_t i = 0; i < n;)
        i = inline_step(&in, i);
    emphasis(&in);
    for (int p = 0; p < in.np; p++) {
        struct piece *q = &in.p[p];
        if (q->pre.n)
            buf_add(out, q->pre.p, q->pre.n);
        for (int k = 0; k < q->n; k++)
            buf_putc(out, q->c);
        if (q->post.n)
            buf_add(out, q->post.p, q->post.n);
        buf_free(&q->pre);
        buf_free(&q->post);
    }
    free(in.p);
    if (visible)
        *visible = in.visible;
}

/* ---- rendering -------------------------------------------------------- */

/* The text of a block's lines joined with \n, each without its leading
 * white space, the last without its trailing; their source lines in lno. */
static char *join(const struct blk *b, int **lno)
{
    struct buf t = { 0 };
    *lno = palloc((size_t)(b->nl ? b->nl : 1) * sizeof(int));
    buf_add(&t, "", 0);
    for (int k = 0; k < b->nl; k++) {
        if (k)
            buf_putc(&t, '\n');
        buf_puts(&t, skip_sp(b->l[k].s));
        (*lno)[k] = b->l[k].no;
    }
    while (t.n > 0 && (t.p[t.n - 1] == ' ' || t.p[t.n - 1] == '\t'))
        t.p[--t.n] = 0;
    char *r = palloc(t.n + 1);
    memcpy(r, t.p, t.n + 1);
    buf_free(&t);
    return r;
}

/* GitHub's anchor for a heading's text: lower case; only letters,
 * digits, _, - and spaces kept; each space a -. */
static char *slug(const char *text)
{
    struct buf b = { 0 };
    size_t n = strlen(text);
    buf_add(&b, "", 0);
    for (size_t k = 0; k < n;) {
        int len;
        unsigned c = u8_at(text, n, k, &len);
        k += (size_t)len;
        if (c == ' ')
            buf_putc(&b, '-');
        else if (c == '-' || c == '_' || (c < 0x80 && isalnum((int)c)))
            buf_putc(&b, (int)u_lower(c));
        else if (c >= 0x80 && !u_space(c) && !u_punct(c) && c != 0xAD &&
                 !(c >= 0x80 && c < 0xA0) && !(c >= 0x200B && c <= 0x200F))
            u8_put(&b, u_lower(c));
    }
    return b.p;
}

static int has_id(const struct md_page *pg, const char *id)
{
    for (int k = 0; k < pg->nh; k++)
        if (!strcmp(pg->h[k].id, id))
            return 1;
    return 0;
}

static void heading(struct md_page *pg, const struct blk *b)
{
    int *lno;
    char *src = join(b, &lno);
    struct buf h = { 0 };
    inline_html(pg, src, strlen(src), lno, b->nl, 0, &h, NULL);
    buf_add(&h, "", 0);
    char *text = html_text(h.p, h.n);
    /* GitHub's slug, made unique as github-slugger does: the second
     * "usage" is usage-1, and a -N that is taken already is skipped */
    char *base = slug(text), *id = xstrdup(base);
    for (int k = 1; has_id(pg, id); k++) {
        free(id);
        struct buf t = { 0 };
        buf_printf(&t, "%s-%d", base, k);
        id = t.p;
    }
    free(base);
    if (pg->nh == pg->caph) {
        pg->caph = pg->caph ? pg->caph * 2 : 32;
        pg->h = xrealloc(pg->h, (size_t)pg->caph * sizeof *pg->h);
    }
    struct md_heading *mh = &pg->h[pg->nh++];
    mh->level = b->level;
    mh->line = b->nl ? b->l[0].no : 0;
    mh->id = id;
    mh->text = text;
    mh->html = xstrndup(h.p, h.n);
    mh->at = pg->html.n;
    buf_printf(&pg->html, "<h%d id=\"", b->level);
    buf_html(&pg->html, id, strlen(id));
    buf_puts(&pg->html, "\">");
    buf_add(&pg->html, h.p, h.n);
    buf_puts(&pg->html, "<a class=\"hl\" href=\"#");
    buf_html(&pg->html, id, strlen(id));
    buf_printf(&pg->html, "\" aria-label=\"Link to this section\">#</a></h%d>\n",
               b->level);
    buf_free(&h);
}

static void code_block(struct md_page *pg, const struct blk *b)
{
    struct buf *o = &pg->html;
    buf_puts(o, "<div class=\"codeblock\"");
    if (b->lang) {
        buf_puts(o, " data-lang=\"");
        buf_html(o, b->lang, strlen(b->lang));
        buf_puts(o, "\"");
    }
    buf_puts(o, "><pre><code");
    if (b->lang) {
        buf_puts(o, " class=\"language-");
        buf_html(o, b->lang, strlen(b->lang));
        buf_puts(o, "\"");
    }
    buf_puts(o, ">");
    for (int k = 0; k < b->nl; k++) {
        buf_html(o, b->l[k].s, strlen(b->l[k].s));
        buf_putc(o, '\n');
    }
    buf_puts(o, "</code></pre></div>\n");
}

/* A cell's text with GFM's \| made a plain |, before inline parsing. */
static void table_row(struct md_page *pg, const struct blk *b, int r)
{
    struct cell *c = palloc((size_t)b->ncol * sizeof *c);
    int nc = cells(b->l[r].s, c, b->ncol);
    const char *tag = r == 0 ? "th" : "td";
    int lno = b->l[r].no;
    buf_puts(&pg->html, "<tr>\n");
    for (int k = 0; k < b->ncol; k++) {
        buf_printf(&pg->html, "<%s", tag);
        if (b->align[k])
            buf_printf(&pg->html, " style=\"text-align:%s\"",
                       b->align[k] == 'l' ? "left" :
                       b->align[k] == 'c' ? "center" : "right");
        buf_puts(&pg->html, ">");
        if (k < nc) {
            struct buf t = { 0 };
            buf_add(&t, "", 0);
            for (size_t j = 0; j < c[k].n; j++) {
                if (c[k].s[j] == '\\' && j + 1 < c[k].n && c[k].s[j + 1] == '|')
                    continue;
                buf_putc(&t, c[k].s[j]);
            }
            inline_html(pg, t.p, t.n, &lno, 1, IN_CELL, &pg->html, NULL);
            buf_free(&t);
        }
        buf_printf(&pg->html, "</%s>\n", tag);
    }
    buf_puts(&pg->html, "</tr>\n");
}

static void table(struct md_page *pg, const struct blk *b)
{
    buf_puts(&pg->html, "<div class=\"table-wrap\"><table>\n<thead>\n");
    table_row(pg, b, 0);
    buf_puts(&pg->html, "</thead>\n");
    if (b->nl > 1) {
        buf_puts(&pg->html, "<tbody>\n");
        for (int r = 1; r < b->nl; r++)
            table_row(pg, b, r);
        buf_puts(&pg->html, "</tbody>\n");
    }
    buf_puts(&pg->html, "</table></div>\n");
}

/* Before a block's tag: a line break, unless one is there or the tag
 * follows an <li> directly. */
static void block_break(struct buf *o)
{
    if (o->n == 0 || o->p[o->n - 1] == '\n')
        return;
    if (o->n >= 4 && !memcmp(o->p + o->n - 4, "<li>", 4))
        return;
    buf_putc(o, '\n');
}

static void render(struct md_page *pg, const struct blk *b, int tight)
{
    struct buf *o = &pg->html;
    for (; b; b = b->next) {
        if (b->kind != B_PARA || !tight)
            block_break(o);
        switch (b->kind) {
        case B_PARA: {
            int *lno;
            char *src = join(b, &lno);
            struct buf h = { 0 };
            int vis = 0;
            inline_html(pg, src, strlen(src), lno, b->nl, 0, &h, &vis);
            /* a paragraph of anchors only places them, unwrapped */
            if (tight || !vis) {
                buf_add(o, h.p, h.n);
                if (!vis)
                    buf_putc(o, '\n');
            } else {
                buf_puts(o, "<p>");
                buf_add(o, h.p, h.n);
                buf_puts(o, "</p>\n");
            }
            buf_free(&h);
            break;
        }
        case B_HEAD:
            heading(pg, b);
            break;
        case B_CODE:
            code_block(pg, b);
            break;
        case B_HR:
            buf_puts(o, "<hr />\n");
            break;
        case B_QUOTE:
            buf_puts(o, "<blockquote>\n");
            render(pg, b->kid, 0);
            buf_puts(o, "</blockquote>\n");
            break;
        case B_LIST:
            if (b->level && b->start != 1)
                buf_printf(o, "<ol start=\"%d\">\n", b->start);
            else
                buf_puts(o, b->level ? "<ol>\n" : "<ul>\n");
            for (const struct blk *it = b->kid; it; it = it->next) {
                buf_puts(o, "<li>");
                render(pg, it->kid, b->tight);
                buf_puts(o, "</li>\n");
            }
            buf_puts(o, b->level ? "</ol>\n" : "</ul>\n");
            break;
        case B_TABLE:
            table(pg, b);
            break;
        default:
            break;
        }
    }
}

void md_render(struct md_page *pg, const char *src, size_t len)
{
    /* the lines, each NUL-terminated, a \r before the \n dropped */
    char *text = palloc(len + 1);
    memcpy(text, src, len);
    text[len] = 0;
    int n = 1;
    for (size_t k = 0; k < len; k++)
        n += text[k] == '\n';
    struct ln *L = new_lines(n);
    int m = 0;
    char *p = text;
    for (;;) {
        char *e = strchr(p, '\n');
        if (e)
            *e = 0;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r')
            p[l - 1] = 0;
        L[m].s = p;
        L[m].no = m + 1;
        m++;
        if (!e)
            break;
        p = e + 1;
    }
    if (m > 0 && L[m - 1].s[0] == 0)
        m--;                    /* the final newline ends a line, starts none */
    buf_add(&pg->html, "", 0);
    render(pg, parse(L, m), 0);
    pool_free();
}

void md_free(struct md_page *pg)
{
    buf_free(&pg->html);
    for (int k = 0; k < pg->nh; k++) {
        free(pg->h[k].id);
        free(pg->h[k].text);
        free(pg->h[k].html);
    }
    free(pg->h);
    for (int k = 0; k < pg->nids; k++)
        free(pg->ids[k]);
    free(pg->ids);
    pg->h = NULL;
    pg->ids = NULL;
    pg->nh = pg->caph = pg->nids = pg->capids = 0;
}
