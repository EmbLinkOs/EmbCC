/* <regex.h>: a parser to a tree, and a backtracking matcher that finds
 * POSIX's match -- the leftmost, and of those the longest.
 *
 * ---- matching ----------------------------------------------------------------
 *
 * The matcher walks the tree in continuation-passing style: matching a
 * node is told what must follow it (a chain of `struct k` on the C
 * stack), so alternation and repetition backtrack by returning. At each
 * start position every way of matching is tried and the longest end is
 * kept, with the captures of the first way to reach it in greedy order;
 * a match that reaches the end of the string cannot be bettered and
 * stops the search at once. With REG_NOSUB and no back-reference the
 * first match is enough.
 *
 * A repetition of a single character, set or `.` -- by far the commonest
 * -- is a loop, not a recursion per character, so the stack does not
 * grow with the length of the line. Everything else costs a few frames
 * per node matched. A pattern that would take more than STEP_LIMIT steps
 * at one position is REG_ESPACE rather than a hang: backtracking can be
 * exponential, and an answer that never comes is worse than an error.
 */
#define _GNU_SOURCE 1
#include <regex.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "posix.h"

#define STEP_LIMIT 20000000L
#define DUP_MAX 255                    /* RE_DUP_MAX */

enum {
    N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WBEG, N_WEND,
    N_BUFBEG, N_BUFEND, N_GROUP, N_BREF, N_CAT, N_ALT, N_REP, N_EMPTY
};

struct node {
    int type;
    int c;                     /* N_CHAR; N_GROUP/N_BREF: the number */
    unsigned char *set;        /* N_SET: 256 bits */
    int min, max;              /* N_REP; max -1 for no limit */
    struct node **kid;         /* N_CAT, N_ALT; N_GROUP/N_REP: kid[0] */
    int nkid;
};

struct prog {
    struct node *root;
    int nsub, cflags, has_bref;
};

/* ---- the parser -------------------------------------------------------------- */

struct parse {
    const char *p, *end;
    int ere, icase, err, nsub, has_bref;
    int open[10];              /* \N refers to a closed group only */
};

static struct node *mk(struct parse *ps, int type)
{
    struct node *n = calloc(1, sizeof *n);
    if (!n)
        ps->err = REG_ESPACE;
    else
        n->type = type;
    return n;
}

static void free_node(struct node *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->nkid; i++)
        free_node(n->kid[i]);
    free(n->kid);
    free(n->set);
    free(n);
}

static int push(struct parse *ps, struct node *n, struct node *kid)
{
    struct node **k = realloc(n->kid, (size_t)(n->nkid + 1) * sizeof *k);
    if (!k) {
        ps->err = REG_ESPACE;
        free_node(kid);
        return -1;
    }
    n->kid = k;
    n->kid[n->nkid++] = kid;
    return 0;
}

static void set_bit(unsigned char *s, int c) { s[(unsigned char)c >> 3] |= 1u << (c & 7); }
static int has_bit(const unsigned char *s, int c) { return s[(unsigned char)c >> 3] >> (c & 7) & 1; }

static int class_match(const char *name, size_t n, int c)
{
#define CL(s, f) if (n == sizeof s - 1 && !memcmp(name, s, n)) return f(c) != 0;
    CL("alpha", isalpha) CL("digit", isdigit) CL("alnum", isalnum)
    CL("upper", isupper) CL("lower", islower) CL("space", isspace)
    CL("blank", isblank) CL("punct", ispunct) CL("print", isprint)
    CL("graph", isgraph) CL("cntrl", iscntrl) CL("xdigit", isxdigit)
#undef CL
    return -1;
}

/* a bracket expression, after its '[' */
static struct node *bracket(struct parse *ps)
{
    struct node *n = mk(ps, N_SET);
    if (!n)
        return NULL;
    n->set = calloc(32, 1);
    if (!n->set) {
        ps->err = REG_ESPACE;
        return n;
    }
    int neg = 0;
    if (ps->p < ps->end && *ps->p == '^') {
        neg = 1;
        ps->p++;
    }
    int first = 1, prev = -1;
    for (;;) {
        if (ps->p >= ps->end) {
            ps->err = REG_EBRACK;
            return n;
        }
        int c = (unsigned char)*ps->p++;
        if (c == ']' && !first)
            break;
        first = 0;
        if (c == '[' && ps->p < ps->end &&
            (*ps->p == ':' || *ps->p == '=' || *ps->p == '.')) {
            char kind = *ps->p++;
            const char *s = ps->p;
            while (ps->p + 1 < ps->end && !(ps->p[0] == kind && ps->p[1] == ']'))
                ps->p++;
            if (ps->p + 1 >= ps->end) {
                ps->err = REG_EBRACK;
                return n;
            }
            size_t len = (size_t)(ps->p - s);
            ps->p += 2;
            if (kind == ':') {
                if (class_match(s, len, 'a') < 0) {
                    ps->err = REG_ECTYPE;
                    return n;
                }
                for (int ch = 1; ch < 256; ch++)
                    if (class_match(s, len, ch) > 0)
                        set_bit(n->set, ch);
                prev = -1;
                continue;
            }
            /* [=c=] and [.c.]: one character in the "C" locale */
            if (len != 1) {
                ps->err = REG_ECOLLATE;
                return n;
            }
            c = (unsigned char)s[0];
            if (kind == '=') {
                set_bit(n->set, c);
                prev = -1;
                continue;
            }
        }
        /* a range: the end may itself be [.c.] */
        if (ps->p + 1 < ps->end && *ps->p == '-' && ps->p[1] != ']') {
            ps->p++;
            int hi = (unsigned char)*ps->p++;
            if (hi == '[' && ps->p + 2 < ps->end && *ps->p == '.' &&
                ps->p[2] == '.') {
                hi = (unsigned char)ps->p[1];
                ps->p += 4;
            }
            if (hi < c) {
                ps->err = REG_ERANGE;
                return n;
            }
            for (int ch = c; ch <= hi; ch++)
                set_bit(n->set, ch);
            prev = -1;
            continue;
        }
        set_bit(n->set, c);
        prev = c;
    }
    (void)prev;
    if (ps->icase)
        for (int ch = 0; ch < 256; ch++)
            if (has_bit(n->set, ch)) {
                set_bit(n->set, tolower(ch));
                set_bit(n->set, toupper(ch));
            }
    if (neg) {
        for (int i = 0; i < 32; i++)
            n->set[i] = (unsigned char)~n->set[i];
        n->set[0] &= (unsigned char)~1u;          /* never NUL */
        n->c = 1;                                  /* REG_NEWLINE: not \n */
    }
    return n;
}

static struct node *alternation(struct parse *ps, int depth);

static struct node *char_node(struct parse *ps, int c)
{
    if (ps->icase && isalpha(c)) {
        struct node *n = mk(ps, N_SET);
        if (!n)
            return NULL;
        n->set = calloc(32, 1);
        if (!n->set) {
            ps->err = REG_ESPACE;
            return n;
        }
        set_bit(n->set, tolower(c));
        set_bit(n->set, toupper(c));
        return n;
    }
    struct node *n = mk(ps, N_CHAR);
    if (n)
        n->c = c;
    return n;
}

static struct node *class_node(struct parse *ps, int which)
{
    struct node *n = mk(ps, N_SET);
    if (!n)
        return NULL;
    n->set = calloc(32, 1);
    if (!n->set) {
        ps->err = REG_ESPACE;
        return n;
    }
    for (int ch = 1; ch < 256; ch++) {
        int in = (which == 'w' || which == 'W') ? (isalnum(ch) || ch == '_')
                                                : isspace(ch) != 0;
        if (in != (which == 'W' || which == 'S'))
            set_bit(n->set, ch);
    }
    return n;
}

/* one atom, or NULL at the end of a branch (ps->err says whether that
 * was an error) */
static struct node *atom(struct parse *ps, int depth, int at_start)
{
    if (ps->p >= ps->end)
        return NULL;
    int c = (unsigned char)*ps->p;
    if (ps->ere) {
        if (c == '|' || (c == ')' && depth))
            return NULL;
        if ((c == '*' || c == '+' || c == '?') && at_start) {
            ps->err = REG_BADRPT;         /* nothing to repeat */
            return NULL;
        }
        if (c == '{' && at_start && ps->p + 1 < ps->end &&
            isdigit((unsigned char)ps->p[1])) {
            ps->err = REG_BADRPT;
            return NULL;
        }
    }
    ps->p++;
    switch (c) {
    case '.':
        return mk(ps, N_ANY);
    case '[':
        return bracket(ps);
    case '^':
        if (ps->ere || at_start)
            return mk(ps, N_BOL);
        return char_node(ps, c);
    case '$':
        if (ps->ere || ps->p >= ps->end ||
            (ps->p + 1 < ps->end && ps->p[0] == '\\' &&
             (ps->p[1] == ')' || ps->p[1] == '|')))
            return mk(ps, N_EOL);
        return char_node(ps, c);
    case '*':
        if (!ps->ere && at_start)
            return char_node(ps, c);
        break;
    case '(':
        if (ps->ere)
            goto group;
        break;
    case ')':
        if (ps->ere) {
            if (!depth)
                return char_node(ps, c);      /* glibc: a lone ) is itself */
            ps->err = REG_EPAREN;
            return NULL;
        }
        break;
    case '\\': {
        if (ps->p >= ps->end) {
            ps->err = REG_EESCAPE;
            return NULL;
        }
        int e = (unsigned char)*ps->p++;
        if (!ps->ere && e == '(')
            goto group;
        if (!ps->ere && (e == ')' || e == '|')) {
            ps->p -= 2;                    /* the caller's to see */
            return NULL;
        }
        if (e >= '1' && e <= '9') {
            if (e - '0' > ps->nsub || ps->open[e - '0']) {
                ps->err = REG_ESUBREG;
                return NULL;
            }
            struct node *n = mk(ps, N_BREF);
            if (n)
                n->c = e - '0';
            ps->has_bref = 1;
            return n;
        }
        switch (e) {
        case '<': return mk(ps, N_WBEG);
        case '>': return mk(ps, N_WEND);
        case 'b': return mk(ps, N_WORDB);
        case 'B': return mk(ps, N_NWORDB);
        case '`': return mk(ps, N_BUFBEG);
        case '\'': return mk(ps, N_BUFEND);
        case 'w': case 'W': case 's': case 'S': return class_node(ps, e);
        }
        if (!ps->ere && (e == '{' || e == '+' || e == '?') && at_start)
            return char_node(ps, e);
        return char_node(ps, e);
    }
    }
    return char_node(ps, c);

group: {
        int num = ++ps->nsub;
        if (num < 10)
            ps->open[num] = 1;
        struct node *g = mk(ps, N_GROUP);
        if (!g)
            return NULL;
        g->c = num;
        struct node *in = alternation(ps, depth + 1);
        if (ps->err) {
            free_node(in);
            free_node(g);
            return NULL;
        }
        if (ps->ere) {
            if (ps->p >= ps->end || *ps->p != ')') {
                ps->err = REG_EPAREN;
                free_node(in);
                free_node(g);
                return NULL;
            }
            ps->p++;
        } else {
            if (ps->p + 1 >= ps->end || ps->p[0] != '\\' || ps->p[1] != ')') {
                ps->err = REG_EPAREN;
                free_node(in);
                free_node(g);
                return NULL;
            }
            ps->p += 2;
        }
        if (num < 10)
            ps->open[num] = 0;
        if (!in)
            in = mk(ps, N_EMPTY);
        push(ps, g, in);
        return g;
    }
}

/* after an atom: *, +, ?, {m,n} -- with \ in front where basic syntax
 * wants it; several apply in turn ("a**") */
static struct node *repeats(struct parse *ps, struct node *a)
{
    for (;;) {
        if (ps->p >= ps->end)
            return a;
        int min, max;
        const char *p = ps->p;
        int bs = !ps->ere && *p == '\\' && p + 1 < ps->end;
        int c = (unsigned char)(bs ? p[1] : p[0]);
        if (c == '*' && !bs) {
            min = 0;
            max = -1;
            ps->p++;
        } else if ((c == '+' || c == '?') && (ps->ere ? !bs : bs)) {
            min = c == '+';
            max = c == '+' ? -1 : 1;
            ps->p += bs ? 2 : 1;
        } else if (c == '{' && (ps->ere ? !bs : bs)) {
            const char *q = p + (bs ? 2 : 1);
            /* after an atom a brace always opens an interval, as glibc
             * reads it: "a{x" is REG_EBRACE, "a{,2}" is a{0,2} */
            min = 0;
            int digits = 0;
            while (q < ps->end && isdigit((unsigned char)*q)) {
                if (min <= DUP_MAX)
                    min = min * 10 + (*q - '0');
                q++;
                digits++;
            }
            max = min;
            if (q < ps->end && *q == ',') {
                q++;
                max = -1;
                if (q < ps->end && isdigit((unsigned char)*q)) {
                    max = 0;
                    while (q < ps->end && isdigit((unsigned char)*q)) {
                        if (max <= DUP_MAX)
                            max = max * 10 + (*q - '0');
                        q++;
                    }
                }
            }
            if (!digits && max == min) {     /* "{}" or "{x" */
                ps->err = q >= ps->end || *q != '}' ? REG_EBRACE : REG_BADBR;
                return a;
            }
            if (bs) {
                if (q + 1 >= ps->end || q[0] != '\\' || q[1] != '}') {
                    ps->err = q >= ps->end ? REG_EBRACE : REG_BADBR;
                    return a;
                }
                q += 2;
            } else {
                if (q >= ps->end) {
                    ps->err = REG_EBRACE;
                    return a;
                }
                if (*q != '}') {
                    ps->err = REG_BADBR;
                    return a;
                }
                q++;
            }
            if (min > DUP_MAX || max > DUP_MAX || (max >= 0 && max < min)) {
                ps->err = REG_BADBR;
                return a;
            }
            ps->p = q;
        } else {
            return a;
        }
        if (a->type == N_BOL || a->type == N_EOL) {
            if (ps->ere) {
                ps->err = REG_BADRPT;
                return a;
            }
        }
        if (a->type == N_REP && !ps->ere) {
            /* basic "a**" glibc refuses; extended "a+?" it nests */
            ps->err = REG_BADRPT;
            return a;
        }
        struct node *r = mk(ps, N_REP);
        if (!r)
            return a;
        r->min = min;
        r->max = max;
        if (push(ps, r, a))
            return NULL;
        a = r;
    }
}

static struct node *branch(struct parse *ps, int depth)
{
    struct node *cat = mk(ps, N_CAT);
    if (!cat)
        return NULL;
    int at_start = 1;
    for (;;) {
        struct node *a = atom(ps, depth, at_start);
        if (!a || ps->err)
            break;
        /* basic syntax: a * right after ^ is literal, as at the start */
        int after_bol = a->type == N_BOL;
        if (!(after_bol && !ps->ere))
            a = repeats(ps, a);
        if (ps->err) {
            free_node(a);
            break;
        }
        if (push(ps, cat, a))
            break;
        at_start = after_bol && !ps->ere;
    }
    return cat;
}

static struct node *alternation(struct parse *ps, int depth)
{
    struct node *alt = mk(ps, N_ALT);
    if (!alt)
        return NULL;
    for (;;) {
        struct node *b = branch(ps, depth);
        if (!b || push(ps, alt, b) || ps->err)
            return alt;
        if (ps->p >= ps->end)
            return alt;
        if (ps->ere && *ps->p == '|') {
            ps->p++;
            continue;
        }
        if (!ps->ere && ps->p + 1 < ps->end && ps->p[0] == '\\' &&
            ps->p[1] == '|') {
            ps->p += 2;
            continue;
        }
        if (!depth)
            ps->err = REG_EPAREN;           /* a basic \) with nothing open */
        return alt;
    }
}

WEAK int regcomp(regex_t *__restrict re, const char *__restrict pattern,
                 int cflags)
{
    struct parse ps;
    memset(&ps, 0, sizeof ps);
    ps.p = pattern;
    ps.end = pattern + strlen(pattern);
    ps.ere = cflags & REG_EXTENDED;
    ps.icase = cflags & REG_ICASE;
    struct node *root = alternation(&ps, 0);
    if (!ps.err && ps.p < ps.end)
        ps.err = REG_EPAREN;
    if (ps.err || !root) {
        free_node(root);
        return ps.err ? ps.err : REG_ESPACE;
    }
    struct prog *pg = malloc(sizeof *pg);
    if (!pg) {
        free_node(root);
        return REG_ESPACE;
    }
    pg->root = root;
    pg->nsub = ps.nsub;
    pg->cflags = cflags;
    pg->has_bref = ps.has_bref;
    re->re_nsub = (size_t)ps.nsub;
    re->__prog = pg;
    re->__cflags = cflags;
    return 0;
}

WEAK void regfree(regex_t *re)
{
    struct prog *pg = re->__prog;
    if (pg) {
        free_node(pg->root);
        free(pg);
    }
    re->__prog = NULL;
}

/* ---- the matcher --------------------------------------------------------------- */

enum { K_SEQ, K_REP, K_GROUP };

struct k {
    int type;
    const struct node *n;
    int idx;                   /* K_SEQ: next kid; K_REP: iterations done */
    long start;                /* K_REP: where this iteration began */
    long gstart;               /* K_GROUP: where the group began */
    const struct k *next;
};

struct m {
    const char *s;
    long lo, hi;               /* the string is s[lo, hi) */
    int eflags, cflags, first_only;
    regmatch_t *cap, *best;
    int ncap;
    long best_end;
    int found, spent;
    long steps;
};

static int is_word(int c) { return isalnum(c) || c == '_'; }

static int match(struct m *m, const struct node *n, long i, const struct k *k);

static int done(struct m *m, long i)
{
    if (!m->found || i > m->best_end) {
        m->found = 1;
        m->best_end = i;
        memcpy(m->best, m->cap, (size_t)m->ncap * sizeof *m->cap);
    }
    /* the end of the string is as long as a match gets */
    return m->first_only || i == m->hi;
}

static int next(struct m *m, const struct k *k, long i)
{
    if (!k)
        return done(m, i);
    switch (k->type) {
    case K_SEQ:
        if (k->idx < k->n->nkid) {
            struct k nk = { K_SEQ, k->n, k->idx + 1, 0, 0, k->next };
            return match(m, k->n->kid[k->idx], i, &nk);
        }
        return next(m, k->next, i);
    case K_GROUP: {
        int g = k->n->c;
        regmatch_t old = m->cap[g];
        m->cap[g].rm_so = (regoff_t)k->gstart;
        m->cap[g].rm_eo = (regoff_t)i;
        int r = next(m, k->next, i);
        m->cap[g] = old;
        return r;
    }
    default: {                          /* K_REP: one more iteration done */
        const struct node *rep = k->n;
        int count = k->idx + 1;
        /* an iteration that matched nothing would match nothing forever */
        if (i == k->start && count > rep->min)
            return 0;
        if (rep->max < 0 || count < rep->max) {
            struct k nk = { K_REP, rep, count, i, 0, k->next };
            if (match(m, rep->kid[0], i, &nk))
                return 1;
        }
        if (count >= rep->min)
            return next(m, k->next, i);
        return 0;
    }
    }
}

/* does one character at i match n, which is a single-character node */
static int one(const struct m *m, const struct node *n, long i)
{
    if (i >= m->hi)
        return 0;
    int c = (unsigned char)m->s[i];
    switch (n->type) {
    case N_CHAR:
        return c == n->c;
    case N_ANY:
        return c != 0 && !(c == '\n' && (m->cflags & REG_NEWLINE));
    default:                            /* N_SET */
        if (c == '\n' && n->c && (m->cflags & REG_NEWLINE))
            return 0;
        return has_bit(n->set, c);
    }
}

static int match(struct m *m, const struct node *n, long i, const struct k *k)
{
    if (++m->steps > STEP_LIMIT) {
        m->spent = 1;
        return 1;
    }
    switch (n->type) {
    case N_EMPTY:
        return next(m, k, i);
    case N_CHAR:
    case N_ANY:
    case N_SET:
        return one(m, n, i) ? next(m, k, i + 1) : 0;
    case N_BOL: {
        /* the start of the STRING, as glibc has it: REG_STARTEND's
         * rm_so is where the search starts, not a line's start */
        int ok = (i == 0 && !(m->eflags & REG_NOTBOL)) ||
                 (i > 0 && m->s[i - 1] == '\n' &&
                  (m->cflags & REG_NEWLINE));
        return ok ? next(m, k, i) : 0;
    }
    case N_EOL: {
        int ok = (i == m->hi && !(m->eflags & REG_NOTEOL)) ||
                 (i < m->hi && m->s[i] == '\n' && (m->cflags & REG_NEWLINE));
        return ok ? next(m, k, i) : 0;
    }
    case N_BUFBEG:
        return i == m->lo ? next(m, k, i) : 0;
    case N_BUFEND:
        return i == m->hi ? next(m, k, i) : 0;
    case N_WORDB: case N_NWORDB: case N_WBEG: case N_WEND: {
        int before = i > m->lo && is_word((unsigned char)m->s[i - 1]);
        int after = i < m->hi && is_word((unsigned char)m->s[i]);
        int ok = n->type == N_WORDB  ? before != after :
                 n->type == N_NWORDB ? before == after :
                 n->type == N_WBEG   ? !before && after : before && !after;
        return ok ? next(m, k, i) : 0;
    }
    case N_GROUP: {
        struct k nk = { K_GROUP, n, 0, 0, i, k };
        return match(m, n->kid[0], i, &nk);
    }
    case N_BREF: {
        regmatch_t g = m->cap[n->c];
        if (g.rm_so < 0)
            return 0;
        long len = g.rm_eo - g.rm_so;
        if (i + len > m->hi)
            return 0;
        for (long j = 0; j < len; j++) {
            int a = (unsigned char)m->s[g.rm_so + j];
            int b = (unsigned char)m->s[i + j];
            if (a != b && !((m->cflags & REG_ICASE) && tolower(a) == tolower(b)))
                return 0;
        }
        return next(m, k, i + len);
    }
    case N_CAT: {
        if (!n->nkid)
            return next(m, k, i);
        struct k nk = { K_SEQ, n, 1, 0, 0, k };
        return match(m, n->kid[0], i, &nk);
    }
    case N_ALT:
        for (int a = 0; a < n->nkid; a++)
            if (match(m, n->kid[a], i, k))
                return 1;
        return 0;
    default: {                          /* N_REP */
        const struct node *kid = n->kid[0];
        if (kid->type == N_CHAR || kid->type == N_ANY || kid->type == N_SET) {
            /* one character at a time: count the run, then give back
             * from the longest */
            long run = 0, cap = n->max < 0 ? LONG_MAX : n->max;
            while (run < cap && one(m, kid, i + run))
                run++;
            for (long j = run; j >= n->min; j--) {
                if (next(m, k, i + j))
                    return 1;
                if (++m->steps > STEP_LIMIT) {
                    m->spent = 1;
                    return 1;
                }
            }
            return 0;
        }
        if (n->max != 0) {
            struct k nk = { K_REP, n, 0, i, 0, k };
            if (match(m, kid, i, &nk))
                return 1;
        }
        return n->min == 0 ? next(m, k, i) : 0;
    }
    }
}

WEAK int regexec(const regex_t *__restrict re, const char *__restrict s,
                 size_t nmatch, regmatch_t pmatch[__restrict], int eflags)
{
    const struct prog *pg = re->__prog;
    if (!pg)
        return REG_BADPAT;
    if (pg->cflags & REG_NOSUB)
        nmatch = 0;
    long lo = 0, hi;
    if ((eflags & REG_STARTEND) && pmatch) {
        lo = pmatch[0].rm_so;
        hi = pmatch[0].rm_eo;
    } else {
        hi = (long)strlen(s);
    }
    int ncap = pg->nsub + 1;
    regmatch_t stack_cap[20];
    regmatch_t *cap = ncap * 2 <= 20 ? stack_cap : malloc((size_t)ncap * 2 * sizeof *cap);
    if (!cap)
        return REG_ESPACE;
    struct m m;
    m.s = s;
    m.lo = lo;
    m.hi = hi;
    m.eflags = eflags;
    m.cflags = pg->cflags;
    m.first_only = !nmatch && !pg->has_bref;
    m.cap = cap;
    m.best = cap + ncap;
    m.ncap = ncap;
    int result = REG_NOMATCH;
    for (long start = lo; start <= hi; start++) {
        for (int g = 0; g < ncap; g++)
            cap[g].rm_so = cap[g].rm_eo = -1;
        m.found = 0;
        m.spent = 0;
        m.steps = 0;
        match(&m, pg->root, start, NULL);
        if (m.spent) {
            result = REG_ESPACE;
            break;
        }
        if (m.found) {
            m.best[0].rm_so = (regoff_t)start;
            m.best[0].rm_eo = (regoff_t)m.best_end;
            for (size_t g = 0; g < nmatch; g++)
                pmatch[g] = (int)g < ncap ? m.best[g]
                                          : (regmatch_t){ -1, -1 };
            result = 0;
            break;
        }
        /* a pattern anchored at the start of the text cannot match later,
         * unless a newline makes a new line there */
        if (pg->root->nkid == 1 && pg->root->kid[0]->nkid) {
            int t = pg->root->kid[0]->kid[0]->type;
            if (t == N_BUFBEG ||
                (t == N_BOL && !(pg->cflags & REG_NEWLINE)))
                break;
        }
    }
    if (cap != stack_cap)
        free(cap);
    return result;
}

static const char *const g_msgs[] = {
    "Success", "No match", "Invalid regular expression",
    "Invalid collation character", "Invalid character class name",
    "Trailing backslash", "Invalid back reference",
    "Unmatched [, [^, [:, [., or [=", "Unmatched ( or \\(",
    "Unmatched \\{", "Invalid content of \\{\\}", "Invalid range end",
    "Memory exhausted", "Invalid preceding regular expression",
    "Premature end of regular expression", "Regular expression too big",
    "Unmatched ) or \\)",
};

WEAK size_t regerror(int err, const regex_t *__restrict re,
                     char *__restrict buf, size_t n)
{
    (void)re;
    const char *msg = err >= 0 && err < (int)(sizeof g_msgs / sizeof *g_msgs)
                          ? g_msgs[err] : "Unknown error";
    size_t len = strlen(msg) + 1;
    if (buf && n) {
        size_t c = len < n ? len : n;
        memcpy(buf, msg, c - 1);
        buf[c - 1] = 0;
    }
    return len;
}
