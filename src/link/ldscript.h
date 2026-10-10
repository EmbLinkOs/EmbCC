/* GNU ld linker scripts: the part of the language a firmware build uses.
 *
 * Included once, by link.c, which owns the layout; this file is the
 * parser and the tree it builds. A Cortex-M or RISC-V project arrives with
 * a script -- STM32CubeMX's, a vendor SDK's, FreeRTOS's demos, CMSIS's
 * gcc_arm.ld -- and the script, not the linker, says where flash and RAM
 * are, which sections are kept where, and what the startup code calls
 * the bounds of .data and .bss. Without it every one of those projects
 * had to be rewritten to -Ttext/-Tdata and the bracket symbols embld
 * invents, which no existing startup file uses.
 *
 * What is here (docs/tools/embld.md has the list a user reads):
 *   ENTRY, MEMORY, SECTIONS, REGION_ALIAS, INCLUDE, INPUT, GROUP,
 *   STARTUP, SEARCH_DIR, EXTERN, ASSERT, PROVIDE, PROVIDE_HIDDEN, HIDDEN,
 *   OUTPUT_FORMAT/OUTPUT_ARCH/TARGET (read and checked for nothing),
 *   output sections with an address, (NOLOAD), AT(), ALIGN(), SUBALIGN(),
 *   > region, AT> region, =fill and /DISCARD/; input descriptions with
 *   KEEP, SORT and its variants, EXCLUDE_FILE, archive:member patterns
 *   and COMMON; BYTE/SHORT/LONG/QUAD/SQUAD, FILL and assignments to `.`;
 *   expressions with C's operators and ALIGN, ORIGIN, LENGTH, ADDR,
 *   LOADADDR, SIZEOF, ALIGNOF, DEFINED, MIN, MAX, ABSOLUTE, LOG2CEIL,
 *   CONSTANT and SIZEOF_HEADERS.
 *
 * What is refused, by name, because a script that uses it describes an
 * image this linker would not build: PHDRS, OVERLAY, INSERT,
 * NOCROSSREFS, ONLY_IF_RO/ONLY_IF_RW, INPUT_SECTION_FLAGS, output
 * section types other than NOLOAD, and DATA_SEGMENT_*.
 */

/* ---- the tree --------------------------------------------------------- */

enum lx_kind { LX_NUM, LX_SYM, LX_DOT, LX_UN, LX_BIN, LX_COND, LX_FN };

enum lx_op {
    LO_ADD = 1, LO_SUB, LO_MUL, LO_DIV, LO_MOD, LO_SHL, LO_SHR,
    LO_AND, LO_OR, LO_XOR, LO_LAND, LO_LOR,
    LO_EQ, LO_NE, LO_LT, LO_LE, LO_GT, LO_GE,
    LO_NEG, LO_NOT, LO_BNOT
};

enum lx_fn {
    LF_ALIGN, LF_ALIGN2, LF_ORIGIN, LF_LENGTH, LF_ADDR, LF_LOADADDR,
    LF_SIZEOF, LF_ALIGNOF, LF_DEFINED, LF_MAX, LF_MIN, LF_ABSOLUTE,
    LF_LOG2CEIL, LF_CONSTANT, LF_SIZEOF_HEADERS
};

struct lx {
    int kind, op;               /* lx_kind; the lx_op, or the lx_fn */
    long long num;              /* LX_NUM */
    const char *name;           /* LX_SYM, or the name LX_FN takes */
    struct lx *a, *b, *c;
};

enum ls_kind { LS_ASSIGN, LS_INPUT, LS_DATA, LS_ASSERT, LS_FILL, LS_OSEC };

/* One input section description: `KEEP(*crt0.o(SORT(.text.*) .text))`. */
struct ls_input {
    const char *file;           /* file pattern; "*" for any */
    const char **fexcl;         /* EXCLUDE_FILE before the file pattern */
    int nfexcl;
    const char **sec;           /* section patterns */
    const char ***sexcl;        /* per pattern: EXCLUDE_FILE before it */
    int *nsexcl;
    int *sort;                  /* per pattern: 0, or LSORT_* */
    int nsec;
    int keep;
    int common;                 /* `*(COMMON)` */
    int fsort;                  /* SORT(*)(...): files in name order */
};

enum { LSORT_NAME = 1, LSORT_ALIGN, LSORT_INIT_PRIORITY, LSORT_FILE };

struct ls_stmt {
    int kind, line;
    const char *file;           /* the script it came from, for errors */
    /* LS_ASSIGN */
    const char *sym;            /* "." for the location counter */
    int aop;                    /* 0 for `=`, else the lx_op of `op=` */
    int provide;                /* PROVIDE / PROVIDE_HIDDEN */
    int in_sections;            /* inside SECTIONS (`.` exists there) */
    struct lx *e;               /* ASSIGN, DATA, ASSERT, FILL */
    const char *msg;            /* ASSERT */
    int dsize;                  /* DATA: 1, 2, 4 or 8 */
    int dsigned;                /* SQUAD */
    struct ls_input in;         /* INPUT */
    int osec;                   /* LS_OSEC: index into ls_script.osecs */
    /* Filled by the layout in link.c: the input sections an LS_INPUT
     * claimed (insecs indices, in placement order), and where an LS_DATA
     * sits in its section and what it holds. */
    int *list;
    int nlist, caplist;
    long long doff, dval;
};

struct ls_osec {
    const char *name;
    int line;
    const char *file;
    int discard;                /* /DISCARD/ */
    int noload;                 /* (NOLOAD) */
    struct lx *addr, *at, *align, *subalign, *fill;
    const char *vregion, *lregion;
    struct ls_stmt *body;
    int nbody, capbody;
    int orphan;                 /* made by the linker for an unplaced input */
    /* layout results, one pass at a time (link.c) */
    long long vma, lma, size, al, fillv;  /* al: the alignment used */
    int nobits, exec, write, has_input, laid, region;
    int shndx;                  /* in the output, 0 if it is not written */
};

struct ls_region {
    const char *name;
    const char *attrs;          /* "rx", "xrw", "!w": for the map file */
    struct lx *org, *len;
    long long origin, length, cur, delta;
    int has_delta;
};

struct ls_alias { const char *alias, *region; };

struct ls_script {
    const char *entry;
    struct ls_region *reg;
    int nreg, capreg;
    struct ls_alias *alias;
    int nalias, capalias;
    struct ls_stmt *cmds;       /* top level and SECTIONS, in order */
    int ncmd, capcmd;
    struct ls_osec *osecs;
    int nosec, caposec;
    const char **inputs;        /* INPUT, GROUP, STARTUP */
    int ninputs, capinputs;
    const char **dirs;          /* SEARCH_DIR, then the script's own */
    int ndirs, capdirs;
    const char **externs;       /* EXTERN */
    int nextern, capextern;
    int has_common;             /* some description says COMMON */
};

/* ---- the parser ---------------------------------------------------- */

struct lp {
    const char *src, *p;
    const char *file;
    int line;
    struct ls_script *sc;
    int depth;                  /* INCLUDE nesting */
};

static void lp_die(struct lp *p, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    die("%s:%d: %s", p->file, p->line, msg);
}

#define LS_PUSH(arr, n, cap) \
    do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 8; \
         (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr)); } } while (0)

static void lp_skip(struct lp *p)
{
    for (;;) {
        char c = *p->p;
        if (c == '\n') {
            p->line++;
            p->p++;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' ||
                   c == '\v') {
            p->p++;
        } else if (c == '/' && p->p[1] == '*') {
            const char *e = strstr(p->p + 2, "*/");
            if (!e)
                lp_die(p, "a comment that is never closed");
            for (const char *q = p->p; q < e; q++)
                if (*q == '\n')
                    p->line++;
            p->p = e + 2;
        } else {
            return;
        }
    }
}

static int lp_peek(struct lp *p)
{
    lp_skip(p);
    return (unsigned char)*p->p;
}

static int lp_accept(struct lp *p, char c)
{
    if (lp_peek(p) != (unsigned char)c)
        return 0;
    p->p++;
    return 1;
}

static void lp_expect(struct lp *p, char c)
{
    if (!lp_accept(p, c)) {
        char got[24];
        if (*p->p)
            snprintf(got, sizeof got, "'%.12s'", p->p);
        else
            snprintf(got, sizeof got, "the end of the script");
        lp_die(p, "expected '%c', found %s", c, got);
    }
}

static int is_sym_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '.' || c == '$';
}

static int is_sym_char(int c)
{
    return is_sym_start(c) || (c >= '0' && c <= '9');
}

/* A file or section name pattern: everything up to a separator. */
static int is_pat_char(int c)
{
    return c && !strchr(" \t\r\n\f\v(){},;=\"", c);
}

static const char *lp_str(struct lp *p)
{
    const char *s = ++p->p;
    while (*p->p && *p->p != '"') {
        if (*p->p == '\n')
            lp_die(p, "a string that runs past the end of the line");
        p->p++;
    }
    if (!*p->p)
        lp_die(p, "a string that is never closed");
    return xstrndup(s, (size_t)(p->p++ - s));
}

/* An identifier in an expression, or a quoted name. NULL if none. */
static const char *lp_ident(struct lp *p)
{
    lp_skip(p);
    if (*p->p == '"')
        return lp_str(p);
    if (!is_sym_start((unsigned char)*p->p))
        return NULL;
    const char *s = p->p;
    while (is_sym_char((unsigned char)*p->p))
        p->p++;
    return xstrndup(s, (size_t)(p->p - s));
}

/* A pattern, a section name or a file name. NULL if none. */
static const char *lp_pat(struct lp *p)
{
    lp_skip(p);
    if (*p->p == '"')
        return lp_str(p);
    const char *s = p->p;
    while (is_pat_char((unsigned char)*p->p))
        p->p++;
    if (p->p == s)
        return NULL;
    return xstrndup(s, (size_t)(p->p - s));
}

/* Is the next thing the keyword `kw` (not just a name starting with it)? */
static int lp_kw(struct lp *p, const char *kw)
{
    size_t n = strlen(kw);
    lp_skip(p);
    if (strncmp(p->p, kw, n) != 0 || is_sym_char((unsigned char)p->p[n]))
        return 0;
    p->p += n;
    return 1;
}

static struct lx *lx_new(int kind)
{
    struct lx *e = xcalloc(1, sizeof *e);
    e->kind = kind;
    return e;
}

static struct lx *lp_expr(struct lp *p);

static long long lp_number(struct lp *p)
{
    char *end;
    unsigned long long v;
    const char *s = p->p;
    /* ld's own suffixes: 0x...h is not used in practice, K and M are. */
    v = strtoull(s, &end, 0);
    if (end == s)
        lp_die(p, "a number was expected");
    if (*end == 'K' || *end == 'k') {
        v *= 1024;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        v *= 1024 * 1024;
        end++;
    }
    if (is_sym_char((unsigned char)*end))
        lp_die(p, "'%.*s' is not a number", (int)(end - s + 1), s);
    p->p = end;
    return (long long)v;
}

static const struct { const char *name; int fn, nargs; } lx_fns[] = {
    { "ALIGN", LF_ALIGN, -1 }, { "NEXT", LF_ALIGN, 1 },
    { "ORIGIN", LF_ORIGIN, 1 }, { "org", LF_ORIGIN, 1 },
    { "LENGTH", LF_LENGTH, 1 }, { "len", LF_LENGTH, 1 },
    { "ADDR", LF_ADDR, 1 }, { "LOADADDR", LF_LOADADDR, 1 },
    { "SIZEOF", LF_SIZEOF, 1 }, { "ALIGNOF", LF_ALIGNOF, 1 },
    { "DEFINED", LF_DEFINED, 1 }, { "MAX", LF_MAX, 2 }, { "MIN", LF_MIN, 2 },
    { "ABSOLUTE", LF_ABSOLUTE, 1 }, { "LOG2CEIL", LF_LOG2CEIL, 1 },
    { "CONSTANT", LF_CONSTANT, 1 },
};

static struct lx *lp_primary(struct lp *p)
{
    int c = lp_peek(p);
    struct lx *e;
    if (c == '(') {
        p->p++;
        e = lp_expr(p);
        lp_expect(p, ')');
        return e;
    }
    if (c == '-' || c == '!' || c == '~' || c == '+') {
        p->p++;
        e = lp_primary(p);
        if (c == '+')
            return e;
        struct lx *u = lx_new(LX_UN);
        u->op = c == '-' ? LO_NEG : c == '!' ? LO_NOT : LO_BNOT;
        u->a = e;
        return u;
    }
    if (c >= '0' && c <= '9') {
        e = lx_new(LX_NUM);
        e->num = lp_number(p);
        return e;
    }
    if (c == '.' && !is_sym_char((unsigned char)p->p[1])) {
        p->p++;
        return lx_new(LX_DOT);
    }
    const char *id = lp_ident(p);
    if (!id)
        lp_die(p, "an expression was expected, found '%.12s'", p->p);
    if (!strcmp(id, "SIZEOF_HEADERS") || !strcmp(id, "sizeof_headers")) {
        e = lx_new(LX_FN);
        e->op = LF_SIZEOF_HEADERS;
        return e;
    }
    if (lp_peek(p) == '(') {
        if (!strncmp(id, "DATA_SEGMENT_", 13) || !strcmp(id, "SEGMENT_START"))
            lp_die(p, "%s() lays out a dynamically linked image; embld makes "
                   "static ones and does not support it", id);
        for (size_t k = 0; k < sizeof lx_fns / sizeof lx_fns[0]; k++) {
            if (strcmp(id, lx_fns[k].name))
                continue;
            p->p++;
            e = lx_new(LX_FN);
            e->op = lx_fns[k].fn;
            switch (e->op) {
            case LF_ORIGIN: case LF_LENGTH: case LF_ADDR: case LF_LOADADDR:
            case LF_SIZEOF: case LF_ALIGNOF: case LF_DEFINED: case LF_CONSTANT:
                e->name = lp_pat(p);
                if (!e->name)
                    lp_die(p, "%s() needs a name", id);
                break;
            default:
                e->a = lp_expr(p);
                if (lp_accept(p, ',')) {
                    if (e->op != LF_ALIGN && e->op != LF_MAX &&
                        e->op != LF_MIN)
                        lp_die(p, "%s() takes one argument", id);
                    e->b = lp_expr(p);
                    if (e->op == LF_ALIGN)
                        e->op = LF_ALIGN2;
                } else if (e->op == LF_MAX || e->op == LF_MIN) {
                    lp_die(p, "%s() takes two arguments", id);
                }
            }
            lp_expect(p, ')');
            return e;
        }
        lp_die(p, "'%s' is not a function a linker script has", id);
    }
    e = lx_new(LX_SYM);
    e->name = id;
    return e;
}

/* Binary operators, by C's precedence. */
static int lp_binop(struct lp *p, int *prec)
{
    const char *s;
    lp_skip(p);
    s = p->p;
#define OP2(t, o, pr) if (s[0] == t[0] && s[1] == t[1]) { *prec = pr; p->p += 2; return o; }
    OP2("||", LO_LOR, 1) OP2("&&", LO_LAND, 2)
    OP2("==", LO_EQ, 6) OP2("!=", LO_NE, 6)
    OP2("<=", LO_LE, 7) OP2(">=", LO_GE, 7)
    if ((s[0] == '<' && s[1] == '<' && s[2] != '=') ||
        (s[0] == '>' && s[1] == '>' && s[2] != '=')) {
        *prec = 8;
        p->p += 2;
        return s[0] == '<' ? LO_SHL : LO_SHR;
    }
#undef OP2
    if (s[1] == '=')            /* `x += 1;` is an assignment, not x + (=1) */
        return 0;
    switch (s[0]) {
    case '|': *prec = 3; p->p++; return LO_OR;
    case '^': *prec = 4; p->p++; return LO_XOR;
    case '&': *prec = 5; p->p++; return LO_AND;
    case '<': *prec = 7; p->p++; return LO_LT;
    case '>': *prec = 7; p->p++; return LO_GT;
    case '+': *prec = 9; p->p++; return LO_ADD;
    case '-': *prec = 9; p->p++; return LO_SUB;
    case '*': *prec = 10; p->p++; return LO_MUL;
    case '/': *prec = 10; p->p++; return LO_DIV;
    case '%': *prec = 10; p->p++; return LO_MOD;
    }
    return 0;
}

static struct lx *lp_binary(struct lp *p, int minprec)
{
    struct lx *l = lp_primary(p);
    for (;;) {
        const char *save = p->p;
        int save_line = p->line, prec = 0;
        int op = lp_binop(p, &prec);
        if (!op || prec < minprec) {
            p->p = save;
            p->line = save_line;
            return l;
        }
        struct lx *b = lx_new(LX_BIN);
        b->op = op;
        b->a = l;
        b->b = lp_binary(p, prec + 1);
        l = b;
    }
}

static struct lx *lp_expr(struct lp *p)
{
    struct lx *e = lp_binary(p, 1);
    if (lp_accept(p, '?')) {
        struct lx *c = lx_new(LX_COND);
        c->a = e;
        c->b = lp_expr(p);
        lp_expect(p, ':');
        c->c = lp_expr(p);
        return c;
    }
    return e;
}

/* `=`, `+=`, ... after a symbol. -1 if what follows is not one. */
static int lp_assign_op(struct lp *p)
{
    const char *s;
    lp_skip(p);
    s = p->p;
    if (s[0] == '=' && s[1] != '=') { p->p += 1; return 0; }
    if (s[1] == '=' && strchr("+-*/&|", s[0]) && s[0]) {
        p->p += 2;
        switch (s[0]) {
        case '+': return LO_ADD;
        case '-': return LO_SUB;
        case '*': return LO_MUL;
        case '/': return LO_DIV;
        case '&': return LO_AND;
        default:  return LO_OR;
        }
    }
    if ((s[0] == '<' || s[0] == '>') && s[1] == s[0] && s[2] == '=') {
        p->p += 3;
        return s[0] == '<' ? LO_SHL : LO_SHR;
    }
    return -1;
}

static struct ls_stmt *lp_stmt(struct lp *p, struct ls_stmt **arr, int *n,
                               int *cap, int kind)
{
    LS_PUSH(*arr, *n, *cap);
    struct ls_stmt *s = &(*arr)[(*n)++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->line = p->line;
    s->file = p->file;
    s->osec = -1;
    return s;
}

/* The rest of `SYM op EXPR ;` once SYM has been read. */
static void lp_assignment(struct lp *p, struct ls_stmt *s, const char *sym,
                          int op, int provide)
{
    s->sym = sym;
    s->aop = op;
    s->provide = provide;
    s->e = lp_expr(p);
}

/* PROVIDE(sym = expr) and its relatives; the keyword has been read. */
static void lp_provide(struct lp *p, struct ls_stmt *s, int provide)
{
    lp_expect(p, '(');
    const char *sym = lp_ident(p);
    if (!sym)
        lp_die(p, "PROVIDE needs a symbol");
    int op = lp_assign_op(p);
    if (op < 0)
        lp_die(p, "PROVIDE(%s ...) needs an assignment", sym);
    lp_assignment(p, s, sym, op, provide);
    lp_expect(p, ')');
}

static void lp_assert(struct lp *p, struct ls_stmt *s)
{
    lp_expect(p, '(');
    s->e = lp_expr(p);
    lp_expect(p, ',');
    if (lp_peek(p) != '"')
        lp_die(p, "ASSERT's message must be a string");
    s->msg = lp_str(p);
    lp_expect(p, ')');
}

static void lp_end_stmt(struct lp *p)
{
    /* ld accepts an assignment's `;` as optional only at the very end of
     * a block; everywhere else it is required. Lenient on the first. */
    if (!lp_accept(p, ';') && lp_peek(p) != '}')
        lp_die(p, "expected ';' after the statement");
}

static void lp_push_str(const char ***arr, int *n, int *cap, const char *s)
{
    LS_PUSH(*arr, *n, *cap);
    (*arr)[(*n)++] = s;
}

static void lp_exclude_list(struct lp *p, const char ***arr, int *n)
{
    int cap = 0;
    lp_expect(p, '(');
    while (lp_peek(p) != ')') {
        const char *f = lp_pat(p);
        if (!f)
            lp_die(p, "EXCLUDE_FILE needs file names");
        lp_push_str(arr, n, &cap, f);
    }
    p->p++;
}

/* The section list inside an input description's parentheses. */
static void lp_section_list(struct lp *p, struct ls_input *in)
{
    int cap = 0;
    const char **excl = NULL;
    int nexcl = 0;
    lp_expect(p, '(');
    for (;;) {
        int c = lp_peek(p), sort = 0;
        if (c == ')') {
            p->p++;
            break;
        }
        if (c == ',') {
            p->p++;
            continue;
        }
        if (lp_kw(p, "EXCLUDE_FILE")) {
            lp_exclude_list(p, &excl, &nexcl);
            continue;
        }
        if (lp_kw(p, "INPUT_SECTION_FLAGS"))
            lp_die(p, "INPUT_SECTION_FLAGS is not supported");
        if (lp_kw(p, "SORT_BY_NAME") || lp_kw(p, "SORT"))
            sort = LSORT_NAME;
        else if (lp_kw(p, "SORT_BY_ALIGNMENT"))
            sort = LSORT_ALIGN;
        else if (lp_kw(p, "SORT_BY_INIT_PRIORITY"))
            sort = LSORT_INIT_PRIORITY;
        else if (lp_kw(p, "SORT_NONE"))
            sort = -1;
        const char **pats = NULL;
        int npats = 0, pcap = 0;
        if (sort) {
            /* SORT(a b) and SORT(SORT_BY_ALIGNMENT(a)): the outer one
             * decides; a nested sort is read and its patterns kept. */
            lp_expect(p, '(');
            while (lp_peek(p) != ')') {
                if (lp_kw(p, "SORT_BY_NAME") || lp_kw(p, "SORT") ||
                    lp_kw(p, "SORT_BY_ALIGNMENT") ||
                    lp_kw(p, "SORT_BY_INIT_PRIORITY") ||
                    lp_kw(p, "SORT_NONE")) {
                    lp_expect(p, '(');
                    while (lp_peek(p) != ')') {
                        const char *s2 = lp_pat(p);
                        if (!s2)
                            lp_die(p, "a section pattern was expected");
                        lp_push_str(&pats, &npats, &pcap, s2);
                    }
                    p->p++;
                    continue;
                }
                if (lp_kw(p, "EXCLUDE_FILE")) {
                    lp_exclude_list(p, &excl, &nexcl);
                    continue;
                }
                const char *s1 = lp_pat(p);
                if (!s1)
                    lp_die(p, "a section pattern was expected");
                lp_push_str(&pats, &npats, &pcap, s1);
            }
            p->p++;
            if (sort < 0)
                sort = 0;
        } else {
            const char *s0 = lp_pat(p);
            if (!s0)
                lp_die(p, "a section pattern was expected, found '%.12s'",
                       p->p);
            lp_push_str(&pats, &npats, &pcap, s0);
        }
        for (int k = 0; k < npats; k++) {
            if (in->nsec == cap) {
                cap = cap ? cap * 2 : 4;
                in->sec = xrealloc(in->sec, (size_t)cap * sizeof *in->sec);
                in->sexcl = xrealloc(in->sexcl,
                                     (size_t)cap * sizeof *in->sexcl);
                in->nsexcl = xrealloc(in->nsexcl,
                                      (size_t)cap * sizeof *in->nsexcl);
                in->sort = xrealloc(in->sort, (size_t)cap * sizeof *in->sort);
            }
            in->sec[in->nsec] = pats[k];
            in->sexcl[in->nsec] = excl;
            in->nsexcl[in->nsec] = nexcl;
            in->sort[in->nsec] = sort;
            in->nsec++;
            if (!strcmp(pats[k], "COMMON"))
                in->common = 1;
        }
        free(pats);
        excl = NULL;
        nexcl = 0;
    }
}

/* `FILEPAT(SECTIONS...)` or a bare `FILEPAT`; the file pattern may follow
 * an EXCLUDE_FILE. */
static void lp_input_desc(struct lp *p, struct ls_stmt *s, const char *file)
{
    s->kind = LS_INPUT;
    if (!file && (lp_kw(p, "SORT_BY_NAME") || lp_kw(p, "SORT"))) {
        /* `SORT(*)(.ctors)`, as avr-ld's scripts write it: the matching
         * files in name order rather than in link order */
        lp_expect(p, '(');
        file = lp_pat(p);
        if (!file)
            lp_die(p, "SORT needs a file pattern");
        lp_expect(p, ')');
        s->in.fsort = 1;
    }
    if (!file) {
        if (lp_kw(p, "EXCLUDE_FILE"))
            lp_exclude_list(p, &s->in.fexcl, &s->in.nfexcl);
        file = lp_pat(p);
        if (!file)
            lp_die(p, "an input section description was expected, found "
                   "'%.12s'", p->p);
    }
    s->in.file = file;
    if (lp_peek(p) == '(') {
        lp_section_list(p, &s->in);
    } else {
        /* a file named alone: every section in it */
        s->in.sec = xmalloc(sizeof *s->in.sec);
        s->in.sexcl = xcalloc(1, sizeof *s->in.sexcl);
        s->in.nsexcl = xcalloc(1, sizeof *s->in.nsexcl);
        s->in.sort = xcalloc(1, sizeof *s->in.sort);
        s->in.sec[0] = "*";
        s->in.nsec = 1;
    }
    if (s->in.common)
        p->sc->has_common = 1;
}

static void lp_osec_body(struct lp *p, struct ls_osec *o)
{
    for (;;) {
        int c = lp_peek(p);
        if (c == '}') {
            p->p++;
            return;
        }
        if (!c)
            lp_die(p, "output section %s is never closed", o->name);
        if (c == ';') {
            p->p++;
            continue;
        }
        struct ls_stmt *s = lp_stmt(p, &o->body, &o->nbody, &o->capbody,
                                    LS_ASSIGN);
        s->in_sections = 1;
        if (c == '.' && !is_sym_char((unsigned char)p->p[1])) {
            p->p++;
            int op = lp_assign_op(p);
            if (op < 0)
                lp_die(p, "'.' must be assigned here");
            lp_assignment(p, s, ".", op, 0);
            lp_end_stmt(p);
            continue;
        }
        if (lp_kw(p, "KEEP")) {
            lp_expect(p, '(');
            lp_input_desc(p, s, NULL);
            s->in.keep = 1;
            lp_expect(p, ')');
            continue;
        }
        if (lp_kw(p, "PROVIDE")) { lp_provide(p, s, 1); lp_end_stmt(p); continue; }
        if (lp_kw(p, "PROVIDE_HIDDEN")) { lp_provide(p, s, 2); lp_end_stmt(p); continue; }
        if (lp_kw(p, "HIDDEN")) { lp_provide(p, s, 0); lp_end_stmt(p); continue; }
        if (lp_kw(p, "ASSERT")) {
            s->kind = LS_ASSERT;
            lp_assert(p, s);
            lp_accept(p, ';');
            continue;
        }
        if (lp_kw(p, "FILL")) {
            s->kind = LS_FILL;
            lp_expect(p, '(');
            s->e = lp_expr(p);
            lp_expect(p, ')');
            lp_accept(p, ';');
            continue;
        }
        {
            static const struct { const char *kw; int size, sgn; } dk[] = {
                { "BYTE", 1, 0 }, { "SHORT", 2, 0 }, { "LONG", 4, 0 },
                { "QUAD", 8, 0 }, { "SQUAD", 8, 1 },
            };
            int hit = 0;
            for (size_t k = 0; k < sizeof dk / sizeof dk[0] && !hit; k++)
                if (lp_kw(p, dk[k].kw)) {
                    s->kind = LS_DATA;
                    s->dsize = dk[k].size;
                    s->dsigned = dk[k].sgn;
                    lp_expect(p, '(');
                    s->e = lp_expr(p);
                    lp_expect(p, ')');
                    lp_accept(p, ';');
                    hit = 1;
                }
            if (hit)
                continue;
        }
        if (lp_kw(p, "CREATE_OBJECT_SYMBOLS") || lp_kw(p, "CONSTRUCTORS")) {
            /* the first is for a.out debuggers, the second does nothing
             * in an ELF link (ld says so too) */
            o->nbody--;
            lp_accept(p, ';');
            continue;
        }
        if (lp_kw(p, "INCLUDE"))
            lp_die(p, "INCLUDE inside an output section is not supported");
        if (lp_kw(p, "EXCLUDE_FILE")) {
            s->kind = LS_INPUT;
            lp_exclude_list(p, &s->in.fexcl, &s->in.nfexcl);
            lp_input_desc(p, s, NULL);
            continue;
        }
        if (c == '"' || is_pat_char(c)) {
            const char *save = p->p;
            int save_line = p->line;
            const char *w = lp_ident(p);
            if (w) {
                int op = lp_assign_op(p);
                if (op >= 0) {
                    lp_assignment(p, s, w, op, 0);
                    lp_end_stmt(p);
                    continue;
                }
            }
            p->p = save;
            p->line = save_line;
            lp_input_desc(p, s, NULL);
            continue;
        }
        lp_die(p, "'%.12s' is not something an output section can contain",
               p->p);
    }
}

static struct ls_osec *lp_new_osec(struct ls_script *sc, const char *name)
{
    LS_PUSH(sc->osecs, sc->nosec, sc->caposec);
    struct ls_osec *o = &sc->osecs[sc->nosec++];
    memset(o, 0, sizeof *o);
    o->name = name;
    return o;
}

static void lp_output_section(struct lp *p, const char *name)
{
    struct ls_script *sc = p->sc;
    struct ls_osec *o = lp_new_osec(sc, name);
    int idx = sc->nosec - 1;
    o->line = p->line;
    o->file = p->file;
    o->discard = !strcmp(name, "/DISCARD/");
    if (lp_peek(p) != ':') {
        /* an address, a type, or both: `.data 0x20000000 (NOLOAD) :` */
        if (lp_peek(p) != '(' ||
            !(strncmp(p->p + 1, "NOLOAD", 6) == 0 ||
              strncmp(p->p + 1, "COPY", 4) == 0 ||
              strncmp(p->p + 1, "INFO", 4) == 0 ||
              strncmp(p->p + 1, "DSECT", 5) == 0 ||
              strncmp(p->p + 1, "OVERLAY", 7) == 0 ||
              strncmp(p->p + 1, "READONLY", 8) == 0 ||
              strncmp(p->p + 1, " NOLOAD", 7) == 0))
            o->addr = lp_expr(p);
        if (lp_accept(p, '(')) {
            if (lp_kw(p, "NOLOAD"))
                o->noload = 1;
            else
                lp_die(p, "output section %s: only the NOLOAD type is "
                       "supported", name);
            lp_expect(p, ')');
        }
    }
    lp_expect(p, ':');
    for (;;) {
        if (lp_kw(p, "AT")) {
            lp_expect(p, '(');
            o->at = lp_expr(p);
            lp_expect(p, ')');
        } else if (lp_kw(p, "ALIGN")) {
            lp_expect(p, '(');
            o->align = lp_expr(p);
            lp_expect(p, ')');
        } else if (lp_kw(p, "SUBALIGN")) {
            lp_expect(p, '(');
            o->subalign = lp_expr(p);
            lp_expect(p, ')');
        } else if (lp_kw(p, "ALIGN_WITH_INPUT")) {
            /* the LMA keeps the VMA's alignment, which is what the
             * layout below does anyway for AT> regions */
        } else if (lp_kw(p, "ONLY_IF_RO") || lp_kw(p, "ONLY_IF_RW")) {
            lp_die(p, "output section %s: ONLY_IF_RO/ONLY_IF_RW are not "
                   "supported", name);
        } else {
            break;
        }
    }
    lp_expect(p, '{');
    lp_osec_body(p, o);
    for (;;) {
        if (lp_accept(p, '>')) {
            o->vregion = lp_ident(p);
            if (!o->vregion)
                lp_die(p, "'>' needs a memory region");
        } else if (lp_kw(p, "AT")) {
            lp_expect(p, '>');
            o->lregion = lp_ident(p);
            if (!o->lregion)
                lp_die(p, "'AT>' needs a memory region");
        } else if (lp_peek(p) == ':' ) {
            lp_die(p, "output section %s is assigned to a program header; "
                   "PHDRS is not supported (embld makes one segment per "
                   "output section)", name);
        } else if (lp_accept(p, '=')) {
            o->fill = lp_expr(p);
        } else {
            break;
        }
    }
    lp_accept(p, ',');
    struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd, LS_OSEC);
    s->osec = idx;
    s->in_sections = 1;
}

static void lp_sections(struct lp *p)
{
    struct ls_script *sc = p->sc;
    lp_expect(p, '{');
    for (;;) {
        int c = lp_peek(p);
        if (c == '}') {
            p->p++;
            return;
        }
        if (!c)
            lp_die(p, "SECTIONS is never closed");
        if (c == ';') {
            p->p++;
            continue;
        }
        if (lp_kw(p, "ENTRY")) {
            lp_expect(p, '(');
            sc->entry = lp_ident(p);
            lp_expect(p, ')');
            continue;
        }
        if (lp_kw(p, "OVERLAY"))
            lp_die(p, "OVERLAY is not supported");
        if (lp_kw(p, "INCLUDE"))
            lp_die(p, "INCLUDE inside SECTIONS is not supported");
        {
            int prov = lp_kw(p, "PROVIDE") ? 1 : lp_kw(p, "PROVIDE_HIDDEN") ? 2
                     : lp_kw(p, "HIDDEN") ? 0 : -1;
            if (prov >= 0) {
                struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd,
                                            &sc->capcmd, LS_ASSIGN);
                s->in_sections = 1;
                lp_provide(p, s, prov);
                lp_end_stmt(p);
                continue;
            }
        }
        if (lp_kw(p, "ASSERT")) {
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSERT);
            s->in_sections = 1;
            lp_assert(p, s);
            lp_accept(p, ';');
            continue;
        }
        if (c == '.' && !is_sym_char((unsigned char)p->p[1])) {
            p->p++;
            int op = lp_assign_op(p);
            if (op < 0)
                lp_die(p, "'.' must be assigned here");
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSIGN);
            s->in_sections = 1;
            lp_assignment(p, s, ".", op, 0);
            lp_end_stmt(p);
            continue;
        }
        /* a symbol assignment or an output section: the name decides
         * nothing (both may start with a dot), what follows it does */
        const char *save = p->p;
        int save_line = p->line;
        const char *w = lp_ident(p);
        if (w) {
            int op = lp_assign_op(p);
            if (op >= 0) {
                struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd,
                                            &sc->capcmd, LS_ASSIGN);
                s->in_sections = 1;
                lp_assignment(p, s, w, op, 0);
                lp_end_stmt(p);
                continue;
            }
        }
        p->p = save;
        p->line = save_line;
        const char *name = lp_pat(p);
        if (!name)
            lp_die(p, "an output section or an assignment was expected, "
                   "found '%.12s'", p->p);
        lp_output_section(p, name);
    }
}

static void lp_memory(struct lp *p)
{
    struct ls_script *sc = p->sc;
    lp_expect(p, '{');
    while (!lp_accept(p, '}')) {
        const char *name = lp_ident(p);
        if (!name)
            lp_die(p, "a memory region name was expected, found '%.12s'",
                   p->p);
        const char *attrs = "";
        if (lp_accept(p, '(')) {        /* (rx), (xrw), (!w): attributes */
            const char *a0 = p->p;
            while (*p->p && *p->p != ')')
                p->p++;
            attrs = xstrndup(a0, (size_t)(p->p - a0));
            lp_expect(p, ')');
        }
        lp_expect(p, ':');
        LS_PUSH(sc->reg, sc->nreg, sc->capreg);
        struct ls_region *r = &sc->reg[sc->nreg++];
        memset(r, 0, sizeof *r);
        r->name = name;
        r->attrs = attrs;
        for (int k = 0; k < 2; k++) {
            const char *kw = lp_ident(p);
            if (!kw)
                lp_die(p, "region %s: ORIGIN and LENGTH were expected", name);
            int is_org = !strcmp(kw, "ORIGIN") || !strcmp(kw, "org") ||
                         !strcmp(kw, "o");
            int is_len = !strcmp(kw, "LENGTH") || !strcmp(kw, "len") ||
                         !strcmp(kw, "l");
            if (!is_org && !is_len)
                lp_die(p, "region %s: '%s' is neither ORIGIN nor LENGTH",
                       name, kw);
            lp_expect(p, '=');
            if (is_org)
                r->org = lp_expr(p);
            else
                r->len = lp_expr(p);
            if (k == 0)
                lp_expect(p, ',');
        }
        if (!r->org || !r->len)
            lp_die(p, "region %s needs both ORIGIN and LENGTH", name);
        for (int k = 0; k < sc->nreg - 1; k++)
            if (!strcmp(sc->reg[k].name, name))
                lp_die(p, "memory region %s is defined twice", name);
    }
}

/* A parenthesised list of file names: INPUT(a.o b.o), GROUP(-lc -lm). */
static void lp_file_list(struct lp *p, int as_inputs)
{
    struct ls_script *sc = p->sc;
    lp_expect(p, '(');
    while (!lp_accept(p, ')')) {
        if (lp_accept(p, ','))
            continue;
        if (lp_kw(p, "AS_NEEDED")) {
            lp_file_list(p, as_inputs);
            continue;
        }
        const char *f = lp_pat(p);
        if (!f)
            lp_die(p, "a file name was expected, found '%.12s'", p->p);
        if (as_inputs)
            lp_push_str(&sc->inputs, &sc->ninputs, &sc->capinputs, f);
    }
}

static void lp_parse(struct lp *p);

static void lp_include(struct lp *p, const char *name);

static void lp_top(struct lp *p)
{
    struct ls_script *sc = p->sc;
    for (;;) {
        int c = lp_peek(p);
        if (!c)
            return;
        if (c == ';') {
            p->p++;
            continue;
        }
        if (lp_kw(p, "ENTRY")) {
            lp_expect(p, '(');
            sc->entry = lp_ident(p);
            if (!sc->entry)
                lp_die(p, "ENTRY needs a symbol");
            lp_expect(p, ')');
        } else if (lp_kw(p, "MEMORY")) {
            lp_memory(p);
        } else if (lp_kw(p, "SECTIONS")) {
            lp_sections(p);
        } else if (lp_kw(p, "OUTPUT_FORMAT") || lp_kw(p, "OUTPUT_ARCH") ||
                   lp_kw(p, "TARGET") || lp_kw(p, "OUTPUT")) {
            /* the machine comes from the objects, and the output file
             * from -o; these name what a multi-target ld should do */
            lp_expect(p, '(');
            int depth = 1;
            while (*p->p && depth) {
                if (*p->p == '(') depth++;
                else if (*p->p == ')') depth--;
                else if (*p->p == '\n') p->line++;
                p->p++;
            }
        } else if (lp_kw(p, "SEARCH_DIR")) {
            lp_expect(p, '(');
            const char *d = lp_pat(p);
            if (!d)
                lp_die(p, "SEARCH_DIR needs a directory");
            lp_push_str(&sc->dirs, &sc->ndirs, &sc->capdirs, d);
            lp_expect(p, ')');
        } else if (lp_kw(p, "INPUT") || lp_kw(p, "GROUP")) {
            lp_file_list(p, 1);
        } else if (lp_kw(p, "STARTUP")) {
            lp_file_list(p, 1);
        } else if (lp_kw(p, "EXTERN")) {
            lp_expect(p, '(');
            while (!lp_accept(p, ')')) {
                if (lp_accept(p, ','))
                    continue;
                const char *s = lp_ident(p);
                if (!s)
                    lp_die(p, "EXTERN needs symbols");
                lp_push_str(&sc->externs, &sc->nextern, &sc->capextern, s);
            }
        } else if (lp_kw(p, "INCLUDE")) {
            const char *f = lp_pat(p);
            if (!f)
                lp_die(p, "INCLUDE needs a file name");
            lp_include(p, f);
        } else if (lp_kw(p, "REGION_ALIAS")) {
            lp_expect(p, '(');
            if (lp_peek(p) != '"')
                lp_die(p, "REGION_ALIAS's alias must be a string");
            const char *a = lp_str(p);
            lp_expect(p, ',');
            const char *r = lp_ident(p);
            if (!r)
                lp_die(p, "REGION_ALIAS needs a region");
            lp_expect(p, ')');
            LS_PUSH(sc->alias, sc->nalias, sc->capalias);
            sc->alias[sc->nalias].alias = a;
            sc->alias[sc->nalias].region = r;
            sc->nalias++;
        } else if (lp_kw(p, "FORCE_COMMON_ALLOCATION") ||
                   lp_kw(p, "INHIBIT_COMMON_ALLOCATION") ||
                   lp_kw(p, "NOCROSSREFS_TO")) {
            if (lp_peek(p) == '(')
                lp_file_list(p, 0);
        } else if (lp_kw(p, "NOCROSSREFS") || lp_kw(p, "INSERT") ||
                   lp_kw(p, "PHDRS") || lp_kw(p, "OVERLAY") ||
                   lp_kw(p, "VERSION")) {
            const char *e = p->p, *b = e;
            while (b > p->src && is_sym_char((unsigned char)b[-1]))
                b--;
            lp_die(p, "%.*s is not supported by embld", (int)(e - b), b);
        } else if (lp_kw(p, "ASSERT")) {
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSERT);
            lp_assert(p, s);
        } else if (lp_kw(p, "PROVIDE")) {
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSIGN);
            lp_provide(p, s, 1);
            lp_end_stmt(p);
        } else if (lp_kw(p, "PROVIDE_HIDDEN")) {
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSIGN);
            lp_provide(p, s, 2);
            lp_end_stmt(p);
        } else if (lp_kw(p, "HIDDEN")) {
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSIGN);
            lp_provide(p, s, 0);
            lp_end_stmt(p);
        } else {
            const char *w = lp_ident(p);
            int op = w ? lp_assign_op(p) : -1;
            if (!w || op < 0)
                lp_die(p, "'%.16s' is not a linker script command embld "
                       "knows", w ? w : p->p);
            struct ls_stmt *s = lp_stmt(p, &sc->cmds, &sc->ncmd, &sc->capcmd,
                                        LS_ASSIGN);
            lp_assignment(p, s, w, op, 0);
            lp_end_stmt(p);
        }
    }
}

static void lp_parse(struct lp *p)
{
    lp_top(p);
}

static const char *ls_dirname(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return ".";
    return xstrndup(path, (size_t)(slash - path));
}

static unsigned char *read_file(const char *path, long *len);

/* INCLUDE: the file, found as given, or in a SEARCH_DIR, or beside the
 * script that names it. */
static void lp_include(struct lp *p, const char *name)
{
    char buf[4096];
    const char *found = NULL;
    FILE *f;
    if (p->depth > 16)
        lp_die(p, "INCLUDE nests more than 16 deep");
    if ((f = fopen(name, "rb")) != NULL) {
        fclose(f);
        found = name;
    }
    for (int k = 0; !found && k < p->sc->ndirs; k++) {
        snprintf(buf, sizeof buf, "%s/%s", p->sc->dirs[k], name);
        if ((f = fopen(buf, "rb")) != NULL) {
            fclose(f);
            found = xstrndup(buf, strlen(buf));
        }
    }
    if (!found) {
        snprintf(buf, sizeof buf, "%s/%s", ls_dirname(p->file), name);
        if ((f = fopen(buf, "rb")) != NULL) {
            fclose(f);
            found = xstrndup(buf, strlen(buf));
        }
    }
    if (!found)
        lp_die(p, "cannot find the INCLUDEd file '%s'", name);
    long len;
    unsigned char *src = read_file(found, &len);
    char *text = xmalloc((size_t)len + 1);
    memcpy(text, src, (size_t)len);
    text[len] = 0;
    struct lp q = { text, text, found, 1, p->sc, p->depth + 1 };
    lp_parse(&q);
}

static struct ls_script *ls_parse_file(const char *path)
{
    long len;
    unsigned char *src = read_file(path, &len);
    char *text = xmalloc((size_t)len + 1);
    memcpy(text, src, (size_t)len);
    text[len] = 0;
    for (long k = 0; k < len; k++)
        if (text[k] == 0)
            die("%s: a linker script is text, and this has a NUL byte at "
                "offset %ld", path, k);
    struct ls_script *sc = xcalloc(1, sizeof *sc);
    struct lp p = { text, text, path, 1, sc, 0 };
    lp_parse(&p);
    return sc;
}

/* ---- matching -------------------------------------------------------- */

/* A shell glob: *, ? and [...] (with ranges and a leading ! or ^). */
static int ls_glob(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            while (pat[1] == '*')
                pat++;
            if (!pat[1])
                return 1;
            for (; *s; s++)
                if (ls_glob(pat + 1, s))
                    return 1;
            return ls_glob(pat + 1, s);
        }
        if (!*s)
            return 0;
        if (*pat == '?')
            continue;
        if (*pat == '[') {
            const char *q = pat + 1;
            int neg = *q == '!' || *q == '^', hit = 0;
            if (neg)
                q++;
            for (; *q && (*q != ']' || q == pat + 1 + neg); q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2])
                        hit = 1;
                    q += 2;
                } else if (*q == *s) {
                    hit = 1;
                }
            }
            if (!*q)                /* no closing ]: a literal [ */
                return *s == '[' && ls_glob(pat + 1, s + 1);
            if (hit == neg)
                return 0;
            pat = q;
            continue;
        }
        if (*pat != *s)
            return 0;
    }
    return !*s;
}

static const char *ls_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Does a file pattern name this input? `name` is how the linker calls the
 * object: a path, or "ARCHIVE(MEMBER)". A pattern with a colon matches
 * archive:member; one without matches a file's path, an archive's path
 * (so `libc.a ( * )` reaches every member, as STM32CubeIDE's /DISCARD/
 * uses it), or a member's own name. Paths are tried whole and by their
 * last component, because ld compares against the name as the command
 * line gave it and that is rarely the pattern's spelling. */
static int ls_file_match(const char *pat, const char *name)
{
    const char *lp = strchr(name, '(');
    char arch[1024], mem[512];
    int is_member = lp && name[strlen(name) - 1] == ')';
    if (!strcmp(pat, "*"))
        return 1;
    if (is_member) {
        size_t an = (size_t)(lp - name), mn = strlen(lp + 1) - 1;
        if (an >= sizeof arch || mn >= sizeof mem)
            return 0;
        memcpy(arch, name, an);
        arch[an] = 0;
        memcpy(mem, lp + 1, mn);
        mem[mn] = 0;
    }
    const char *colon = strchr(pat, ':');
    if (colon) {
        char ap[1024];
        size_t n = (size_t)(colon - pat);
        if (n >= sizeof ap)
            return 0;
        memcpy(ap, pat, n);
        ap[n] = 0;
        const char *mp = colon + 1;
        if (!is_member)                     /* `:file` is a plain file */
            return !ap[0] && (ls_glob(mp, name) || ls_glob(mp, ls_base(name)));
        if (ap[0] && !ls_glob(ap, arch) && !ls_glob(ap, ls_base(arch)))
            return 0;
        return !*mp || ls_glob(mp, mem);
    }
    if (is_member)
        return ls_glob(pat, arch) || ls_glob(pat, ls_base(arch)) ||
               ls_glob(pat, mem);
    return ls_glob(pat, name) || ls_glob(pat, ls_base(name));
}

static int ls_file_excluded(const char **ex, int n, const char *name)
{
    for (int k = 0; k < n; k++)
        if (ls_file_match(ex[k], name))
            return 1;
    return 0;
}
