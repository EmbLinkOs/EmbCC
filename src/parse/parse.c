#include <setjmp.h>
#include <stdarg.h>
#include "parse.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../lex/lex.h"
#include "../sema/ldfloat.h"
#include "../sema/type.h"

/* Tags (struct/union/enum) live in their own namespace; typedef names
 * live in the ordinary one and must be known DURING parsing (the
 * classic C ambiguity), so both tables belong to the parser. */
/* (struct tagdef, struct typedefent and enum tag_kind live in ast.h: a
 * tool that answers questions about a unit needs to walk them.) */

struct attrs { int packed; int aligned; int weak; int noreturn;
               const char *section;
               int sret; /* embcc_sret on a parameter (type.h sret_first) */
               int nothrow;
               /* format(archetype, string-index, first-to-check): 1 printf,
                * 2 scanf, 0 none. This is how a function says "my nth
                * argument is a format string" without the compiler
                * hard-coding the names of the standard library. */
               int fmt_kind, fmt_idx, fmt_first;
               /* constructor / destructor: run before main, or at exit.
                * The function's address goes in .init_array/.fini_array
                * and the startup code walks them. */
               int ctor, dtor;
               /* The hints EmbCC acts on. `used` keeps a symbol the
                * compiler would otherwise drop; `unused` says not to
                * warn about one; always_inline/noinline are the
                * inliner's two overrides; deprecated and
                * warn_unused_result are diagnostics the DECLARATION
                * asks for. */
               int used, unused, always_inline, noinline;
               int deprecated, warn_unused_result;
               const char *vis;      /* visibility("...") */
};

struct parser {
    struct lexer lx;
    struct unit *unit;
    jmp_buf *recover;     /* where a syntax error resumes (NULL: it stops
                           * the compile, as it did before recovery) */
    int nerrors;          /* syntax errors reported in this parse */
    int prev_line, prev_end_col;  /* just past the token before this one —
                                   * where a missing ';' belongs */
    int semi_line, semi_col;      /* where the last missing ';' was reported,
                                   * so the same one cannot repeat */
    struct tagdef *tags;
    struct typedefent *typedefs;
    struct econst **econst_tail;
    int seq;              /* current top-level item, for econst seq */
    int alignas_out;      /* alignment from a `_Alignas(...)` on the current
                           * declaration specifiers; read + reset where the
                           * declaration applies its alignment (like `aligned`) */
    int vla_ok;           /* inside a function (its parameters or body) and
                           * not in a struct body: a non-constant array size
                           * makes a VLA rather than an error */
    /* Where an attribute found in a pointer-declarator position goes.
     * GCC accepts `extern void *__attribute__((weak)) f(void);` and
     * applies `weak` to the DECLARATION -- there is nothing else in a
     * declaration for it to mean. parse_stars has nowhere to put one,
     * so it drops it here and the enclosing declaration takes it.
     *
     * The slot belongs to the PARSER rather than being a pointer to the
     * declaration's own attrs, so that forgetting to turn it off cannot
     * write through a dangling pointer into a dead stack frame. Where
     * it is off, an attribute in that position is still refused,
     * because then there really is nowhere for it to go. */
    struct attrs attr_slot;
    int attr_carry_on;
    /* Where the last declarator's name token was, so a parameter can be
     * pointed AT rather than at the function's line -- an editor renaming
     * one has to edit the name, not the first column of the signature. */
    int decl_name_line, decl_name_col;
    int fn_plines[MAX_PARAMS], fn_pcols[MAX_PARAMS];
    const char *fn_pnames[MAX_PARAMS]; /* the parameter names of the last
                           * function declarator inside parentheses
                           * (`(*f(int a))[3]`) */
};

static struct token *cur(struct parser *ps) { return &ps->lx.tok; }
static void advance(struct parser *ps)
{
    /* Where the token being left off ends: the scanner sits just past it,
     * which is where an omitted ';' would have gone. */
    ps->prev_line = ps->lx.tok.line;
    ps->prev_end_col = (int)(ps->lx.p - ps->lx.line_start) + 1;
    lex_next(&ps->lx);
}

static struct tagdef *find_tag(struct parser *ps, const char *tag)
{
    for (struct tagdef *t = ps->tags; t; t = t->next)
        if (strcmp(t->tag, tag) == 0)
            return t;
    return NULL;
}

static struct type *find_typedef(struct parser *ps, const char *name)
{
    for (struct typedefent *t = ps->typedefs; t; t = t->next)
        if (strcmp(t->name, name) == 0)
            return t->ty;
    return NULL;
}

/* A syntax error: recorded, then the parser resumes at the nearest recovery
 * point — the enclosing statement, or the next external declaration — so one
 * run reports every independent problem (docs/tools/diagnostics.md T2). Where no
 * recovery point is set, it stops the compile, as every error once did. */
static void parse_error_at(struct parser *ps, int line, int col,
                           const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_verror_at(ps->lx.file, line, col, fmt, ap);
    va_end(ap);
    ps->nerrors++;
    if (ps->recover)
        longjmp(*ps->recover, 1);
    fatal_unwind();
}

/* Where parsing can start again after an error: past the next `;` at this
 * brace depth, or at the `}` that ends the enclosing block (which the block
 * loop consumes). `top` also consumes that `}`, since nothing encloses a
 * declaration. At least one token is always consumed, so recovery cannot
 * spin on the token it failed at. */
static void resync(struct parser *ps, int top)
{
    int depth = 0;
    if (cur(ps)->kind != TOK_EOF)
        advance(ps);
    for (;;) {
        enum tok_kind k = cur(ps)->kind;
        if (k == TOK_EOF)
            return;
        if (k == TOK_LBRACE) {
            depth++;
        } else if (k == TOK_RBRACE) {
            if (depth == 0) {
                if (top)
                    advance(ps);
                return;
            }
            depth--;
        } else if (k == TOK_SEMI && depth == 0) {
            advance(ps);
            return;
        }
        advance(ps);
    }
}

/* The same where the location is a declaration rather than a token, so
 * there is no column to point at. */
static void parse_error_line(struct parser *ps, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_verror_at(ps->lx.file, line, 0, fmt, ap);
    va_end(ap);
    ps->nerrors++;
    if (ps->recover)
        longjmp(*ps->recover, 1);
    fatal_unwind();
}

static void expect(struct parser *ps, enum tok_kind kind, const char *what)
{
    if (cur(ps)->kind == kind) {
        advance(ps);
        return;
    }
    /* A missing ';' is the one syntax error whose fix is never in doubt:
     * it goes at the end of what came before, and saying so is worth more
     * than naming the token that tripped over it. */
    if (kind == TOK_SEMI && ps->prev_line > 0 &&
        !(ps->semi_line == cur(ps)->line && ps->semi_col == cur(ps)->col)) {
        ps->semi_line = cur(ps)->line;
        ps->semi_col = cur(ps)->col;
        diag_error_at(ps->lx.file, cur(ps)->line, cur(ps)->col,
                      "expected %s before %s", what, tok_describe(cur(ps)));
        diag_set_id("E0002");
        diag_note_at(ps->lx.file, ps->prev_line, ps->prev_end_col,
                     "insert ';' here");
        diag_fixit_at(ps->lx.file, ps->prev_line, ps->prev_end_col,
                      ps->prev_end_col, ";");
        ps->nerrors++;
        /* Carry on as if it were there, rather than resynchronising: the
         * rest of the file parses, so a second missing ';' is reported too
         * (and both are fixed at once). The position is remembered so the
         * same one cannot be "inserted" twice and spin. */
        return;
    }
    parse_error_at(ps, cur(ps)->line, cur(ps)->col, "expected %s before %s",
                   what, tok_describe(cur(ps)));
    advance(ps);
}

/* C keywords the subset does not implement lex as identifiers; naming
 * them here turns "'struct' is not declared" into an honest "not
 * supported yet". Grows emptier as M2 proceeds. */
static const char *const reserved_unsupported[] = {
    "auto", "goto", "register",
};

static void reject_reserved(struct parser *ps, const char *name, int line, int col)
{
    for (size_t i = 0;
         i < sizeof reserved_unsupported / sizeof reserved_unsupported[0];
         i++)
        if (strcmp(reserved_unsupported[i], name) == 0)
            parse_error_at(ps, line, col,
                    "'%s' is not supported yet (see docs/design/roadmap.md M2)",
                    name);
}

/* ---- types ---- */

static int tok_is_type_start(enum tok_kind k)
{
    return k == TOK_KW_INT || k == TOK_KW_CHAR || k == TOK_KW_SHORT ||
           k == TOK_KW_LONG || k == TOK_KW_INT128 || k == TOK_KW_UNSIGNED ||
           k == TOK_KW_SIGNED || k == TOK_KW_VOID ||
           k == TOK_KW_STRUCT || k == TOK_KW_UNION || k == TOK_KW_ENUM ||
           k == TOK_KW_CONST || k == TOK_KW_VOLATILE ||
           k == TOK_KW_FLOAT || k == TOK_KW_DOUBLE || k == TOK_KW_BOOL ||
           k == TOK_KW_COMPLEX ||
           k == TOK_KW_ALIGNAS || k == TOK_KW_TYPEOF || k == TOK_KW_ATOMIC;
}

/* const/restrict are accepted and IGNORED (no const-correctness enforcement).
 * VOLATILE is honored for codegen: it must reach the accessed type so the
 * optimizer never CSEs/removes a volatile access (MMIO). Returns 1 if a
 * `volatile` was among the qualifiers consumed. */
static int skip_quals(struct parser *ps)
{
    int vol = 0;
    for (;;) {
        enum tok_kind k = cur(ps)->kind;
        if (k == TOK_KW_CONST || k == TOK_KW_RESTRICT) { advance(ps); continue; }
        if (k == TOK_KW_VOLATILE) { vol = 1; advance(ps); continue; }
        if (k == TOK_KW_ATOMIC) {
            /* `_Atomic(type)` is a specifier, not a qualifier — leave it for the
             * type-spec handler. Bare `_Atomic` is a qualifier: EmbCC maps atomic
             * to volatile, so an aligned scalar load/store still happens and is
             * not reordered/removed. (Lock-prefixed RMW still needs __atomic_*.) */
            struct lexer save = ps->lx;
            advance(ps);
            if (cur(ps)->kind == TOK_LPAREN) { ps->lx = save; break; }
            vol = 1; continue;
        }
        break;
    }
    return vol;
}

/* Does a type begin at the current token — including typedef names,
 * which only the parser's table can decide. */
static int at_type_start(struct parser *ps)
{
    if (tok_is_type_start(cur(ps)->kind))
        return 1;
    return cur(ps)->kind == TOK_IDENT &&
           (find_typedef(ps, cur(ps)->text) != NULL ||
            strcmp(cur(ps)->text, "__builtin_va_list") == 0 ||
            strcmp(cur(ps)->text, "__int128_t") == 0 ||
            strcmp(cur(ps)->text, "__uint128_t") == 0);
}

static struct type *parse_fn_params(struct parser *ps, struct type *ret);
static struct type *parse_fn_params_named(struct parser *ps, struct type *ret,
                                          const char **names);
/* The GNU attributes EmbCC honors; everything else is parsed and dropped.
 * section("name") is honored on file-scope variables and refused (never
 * dropped) anywhere else — a silently-ignored section is a table the
 * linker script's bracket symbols never find. */
/* Match `name`, `__name`, or `__name__` against a base attribute name. */
static int attr_is(const char *n, const char *base)
{
    if (strcmp(n, base) == 0)
        return 1;
    size_t bl = strlen(base);
    return strncmp(n, "__", 2) == 0 &&
           strncmp(n + 2, base, bl) == 0 &&
           strcmp(n + 2 + bl, "__") == 0;
}

/* ---- what EmbCC does with each attribute ---------------------------------
 *
 * Every attribute gets one of three answers, and the point of writing
 * them in a table is that there is no fourth one -- "nobody looked".
 * An attribute that is skipped because it was never considered is how
 * __attribute__((constructor)) went unimplemented on a tree EmbCC
 * compiles, with a function that had to run before main simply never
 * running and nothing saying so.
 *
 *   HONOURED  recorded, and something acts on it.
 *
 *   REFUSED   it would change the code that has to be generated, and
 *             EmbCC does not generate that code. Ignoring it produces a
 *             program that compiles, links, runs and does something
 *             else, so it is an error naming the attribute (THE RULE).
 *
 *   NOOP      accepted, and it genuinely does nothing HERE -- not
 *             "not yet", but "there is nothing for it to turn off".
 *             `may_alias` disables type-based alias analysis and EmbCC
 *             has none, so its absence is already the conservative
 *             answer; `no_sanitize` names sanitizers that do not exist.
 *             Each entry says which.
 *
 * Anything not in the table is unknown, and is warned about under
 * -Wattributes and then ignored -- GCC's behaviour, and the thing that
 * would have caught the constructor bug on the day it was written.
 */
enum attr_disp { ATTR_HONOURED, ATTR_REFUSED, ATTR_NOOP };

struct attr_entry {
    const char *name;
    enum attr_disp disp;
    /* For REFUSED, what would go wrong; for NOOP, why there is nothing
     * to do. Printed, so it has to be true. */
    const char *why;
};

static const struct attr_entry attr_table[] = {
    /* ---- honoured ---- */
    { "packed",        ATTR_HONOURED, NULL },
    { "aligned",       ATTR_HONOURED, NULL },
    { "weak",          ATTR_HONOURED, NULL },
    { "noreturn",      ATTR_HONOURED, NULL },
    { "nothrow",       ATTR_HONOURED, NULL },
    { "section",       ATTR_HONOURED, NULL },
    { "format",        ATTR_HONOURED, NULL },
    { "constructor",   ATTR_HONOURED, NULL },
    { "destructor",    ATTR_HONOURED, NULL },
    { "used",          ATTR_HONOURED, NULL },
    { "unused",        ATTR_HONOURED, NULL },
    { "visibility",    ATTR_HONOURED, NULL },
    /* Honoured where the inliner runs, which is -O2: always_inline
     * overrides the SIZE budget and nothing else, because every other
     * reason the inliner declines is a thing it cannot do rather than
     * a thing it chose against. `-fremarks` names whichever applied. */
    { "always_inline", ATTR_HONOURED, NULL },
    { "noinline",      ATTR_HONOURED, NULL },
    { "deprecated",    ATTR_HONOURED, NULL },
    { "warn_unused_result", ATTR_HONOURED, NULL },
    { "embcc_sret",    ATTR_HONOURED, NULL },   /* EmbCC's own */

    /* ---- refused ---- */
    { "naked",     ATTR_REFUSED,
      "the prologue the function says it must not have would be emitted "
      "anyway, and its own asm would run on a frame it did not set up" },
    { "interrupt", ATTR_REFUSED,
      "the handler would return with an ordinary return instead of the "
      "interrupt return the CPU needs, and without saving the registers" },
    { "cleanup",   ATTR_REFUSED,
      "the cleanup function would never run" },
    { "ms_abi",    ATTR_REFUSED,
      "the arguments would be passed in System V's registers" },
    { "sysv_abi",  ATTR_REFUSED,
      "the arguments would be passed in the other convention's registers" },

    /* ---- no-ops, each for a stated reason ---- */
    { "may_alias",  ATTR_NOOP,
      "EmbCC does no type-based alias analysis, so not doing it is "
      "already what this asks for" },
    { "no_sanitize", ATTR_NOOP, "EmbCC has no sanitizers to turn off" },
    { "no_sanitize_address", ATTR_NOOP, "EmbCC has no sanitizers" },
    { "no_sanitize_undefined", ATTR_NOOP, "EmbCC has no sanitizers" },
    { "no_instrument_function", ATTR_NOOP,
      "EmbCC emits no instrumentation calls" },
    { "hot",       ATTR_NOOP, "EmbCC does not reorder code by frequency" },
    { "cold",      ATTR_NOOP, "EmbCC does not reorder code by frequency" },
    { "flatten",   ATTR_NOOP,
      "EmbCC's inliner works from the call site, not from a whole "
      "function's subtree" },
    { "pure",      ATTR_NOOP,
      "EmbCC does not eliminate repeated calls, so knowing a call could "
      "be eliminated buys nothing" },
    { "const",     ATTR_NOOP, "as pure: no calls are eliminated" },
    { "malloc",    ATTR_NOOP,
      "it says the result aliases nothing, which only an alias analysis "
      "could use" },
    { "leaf",      ATTR_NOOP, "nothing here reasons across a call" },
    { "artificial", ATTR_NOOP, "it marks a line for a debugger's stepping" },
    { "gnu_inline", ATTR_NOOP, "EmbCC emits an inline function as an ordinary one" },
    { "nonnull",   ATTR_NOOP, "EmbCC does not check argument values" },
    { "returns_nonnull", ATTR_NOOP, "EmbCC does not track null-ness" },
    { "alloc_size", ATTR_NOOP, "EmbCC has no object-size checking" },
    { "alloc_align", ATTR_NOOP, "EmbCC has no object-size checking" },
    { "sentinel",  ATTR_NOOP, "EmbCC does not check variadic terminators" },
    { "fallthrough", ATTR_NOOP,
      "EmbCC does not warn about a case falling through" },
    { "optimize",  ATTR_NOOP,
      "EmbCC's optimisation level is per compilation, not per function" },
    /* This one belongs under REFUSED and is deliberately not there:
     * newlib's headers put it on setjmp, so refusing it would stop a
     * corpus this compiler is tested against from building at all.
     * Ignoring it is only wrong above -O0, where a value kept in a
     * register across the call could survive the second return.
     * docs/developer/todo.md records that. */
    { "returns_twice", ATTR_NOOP,
      "refusing it would stop newlib's <setjmp.h> from compiling; see "
      "docs/developer/todo.md" },
};

static const struct attr_entry *attr_lookup(const char *n)
{
    for (unsigned i = 0; i < sizeof attr_table / sizeof attr_table[0]; i++)
        if (attr_is(n, attr_table[i].name))
            return &attr_table[i];
    return NULL;
}

/* Consume a run of `__attribute__((...))`. packed / aligned(N) (struct
 * layout), weak (symbol binding) and constructor/destructor (static
 * initialisation) are recorded in `out`; every other attribute is
 * skipped along with its balanced parenthesized arguments, unless
 * attr_unimplemented() says skipping it would be a lie.
 * Callers pass out=NULL where no attribute is meaningful (member/param). */
static void parse_attributes(struct parser *ps, struct attrs *out)
{
    while (cur(ps)->kind == TOK_KW_ATTRIBUTE) {
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after __attribute__");
        expect(ps, TOK_LPAREN, "a second '(' after __attribute__");
        while (cur(ps)->kind != TOK_RPAREN && cur(ps)->kind != TOK_EOF) {
            const char *name = cur(ps)->kind == TOK_IDENT ? cur(ps)->text
                                                          : NULL;
            advance(ps);
            long arg = -1;
            const char *sarg = NULL;
            int aline = cur(ps)->line;
            int fkind = 0;
            long fidx = 0, ffirst = 0;
            if (cur(ps)->kind == TOK_LPAREN) {
                advance(ps);
                if (name && attr_is(name, "format") &&
                    cur(ps)->kind == TOK_IDENT) {
                    /* Both arguments are 1-based and count the format
                     * string itself, as GCC defines them. An archetype
                     * this compiler does not check (strftime, strfmon)
                     * leaves fkind 0 and the attribute is skipped like
                     * any other. */
                    const char *arch = cur(ps)->text;
                    if (attr_is(arch, "printf") || attr_is(arch, "gnu_printf"))
                        fkind = 1;
                    else if (attr_is(arch, "scanf") ||
                             attr_is(arch, "gnu_scanf"))
                        fkind = 2;
                    advance(ps);
                    if (cur(ps)->kind == TOK_COMMA) {
                        advance(ps);
                        if (cur(ps)->kind == TOK_NUM) {
                            fidx = cur(ps)->num;
                            advance(ps);
                        }
                        if (cur(ps)->kind == TOK_COMMA) {
                            advance(ps);
                            if (cur(ps)->kind == TOK_NUM) {
                                ffirst = cur(ps)->num;
                                advance(ps);
                            }
                        }
                    }
                }
                if (cur(ps)->kind == TOK_NUM)
                    arg = cur(ps)->num;
                else if (cur(ps)->kind == TOK_STR)
                    sarg = cur(ps)->text;
                int depth = 1;
                while (depth > 0 && cur(ps)->kind != TOK_EOF) {
                    if (cur(ps)->kind == TOK_LPAREN) depth++;
                    else if (cur(ps)->kind == TOK_RPAREN) depth--;
                    advance(ps);
                }
            }
            if (name) {
                const struct attr_entry *ae = attr_lookup(name);
                if (!ae)
                    diag_warn_opt(ps->lx.file, aline, 0, "attributes",
                        "attribute '%s' is not one EmbCC knows, and is "
                        "ignored", name);
                else if (ae->disp == ATTR_REFUSED)
                    parse_error_line(ps, aline,
                        "__attribute__((%s)) is not supported: %s",
                        name, ae->why);
            }
            if (name && out) {
                if (attr_is(name, "packed")) out->packed = 1;
                else if (attr_is(name, "weak")) out->weak = 1;
                else if (attr_is(name, "noreturn")) out->noreturn = 1;
                else if (attr_is(name, "nothrow")) out->nothrow = 1;
                else if (attr_is(name, "embcc_sret")) out->sret = 1;
                else if (attr_is(name, "used")) out->used = 1;
                else if (attr_is(name, "unused")) out->unused = 1;
                else if (attr_is(name, "always_inline")) out->always_inline = 1;
                else if (attr_is(name, "noinline")) out->noinline = 1;
                else if (attr_is(name, "deprecated")) out->deprecated = 1;
                else if (attr_is(name, "warn_unused_result"))
                    out->warn_unused_result = 1;
                else if (attr_is(name, "visibility")) {
                    /* The four ELF visibilities. Anything else is a
                     * typo that would otherwise mean "default". */
                    if (!sarg || (strcmp(sarg, "default") &&
                                  strcmp(sarg, "hidden") &&
                                  strcmp(sarg, "protected") &&
                                  strcmp(sarg, "internal")))
                        parse_error_line(ps, aline,
                            "visibility attribute wants \"default\", "
                            "\"hidden\", \"protected\" or \"internal\"");
                    out->vis = sarg;
                }
                else if (attr_is(name, "constructor") ||
                         attr_is(name, "destructor")) {
                    /* A priority orders the array, and EmbCC emits one
                     * .init_array in source order. Accepting the
                     * argument and ignoring it would run them in the
                     * wrong order, which is the whole point of writing
                     * one -- so it is refused and the plain form is
                     * not. */
                    if (arg >= 0)
                        parse_error_line(ps, aline,
                            "__attribute__((%s(%ld))) is not supported: "
                            "EmbCC emits one .init_array in source order "
                            "and cannot honour a priority", name, arg);
                    if (attr_is(name, "constructor")) out->ctor = 1;
                    else out->dtor = 1;
                }
                else if (attr_is(name, "aligned"))
                    out->aligned = arg > 0 ? (int)arg : 16;
                else if (attr_is(name, "format") && fkind && fidx > 0) {
                    out->fmt_kind = fkind;
                    out->fmt_idx = (int)fidx;
                    out->fmt_first = (int)ffirst;
                }
                else if (attr_is(name, "section")) {
                    if (!sarg || !*sarg)
                        parse_error_line(ps, aline,
                                   "section attribute needs a section "
                                   "name string");
                    out->section = sarg;
                }
            } else if (name && attr_is(name, "section")) {
                parse_error_line(ps, aline,
                           "section attribute is only supported on "
                           "file-scope variables");
            }
            if (cur(ps)->kind == TOK_COMMA)
                advance(ps);
            else
                break;
        }
        expect(ps, TOK_RPAREN, "')'");
        expect(ps, TOK_RPAREN, "a second ')' to close __attribute__");
    }
}

static struct type *parse_stars(struct parser *ps, struct type *t);
static struct expr *parse_cond(struct parser *ps);
static int size_fold(const struct expr *e, long *out);
static struct type *ce_type(const struct expr *e);
static struct type *parse_array_dims(struct parser *ps, struct type *t);
static struct expr *new_expr(enum expr_kind kind, int line, int col);
static struct type *parse_type_spec(struct parser *ps, int allow_body);
static void parse_static_assert(struct parser *ps);
static struct expr *parse_initializer(struct parser *ps);
static struct stmt *parse_block(struct parser *ps);

/* Declarator over a base type (C11 6.7.6): pointers, then a name — or a
 * parenthesized declarator — then array and function suffixes. The
 * parenthesized one binds looser than the suffixes after it (`int
 * (*fp)(void)`, `int (*pa)[4]`, `int (*arr[2])(void)`, `void
 * (*signal(int, void (*)(int)))(int)`, `char (*f(void))[6]`): the
 * suffixes are read first, on the base, then the declarator inside the
 * parentheses over that type. A `(params)` right after a name is read here
 * only inside parentheses — outside, callers handle a function declarator
 * themselves; its parameter names (a definition needs them) are left in
 * ps->fn_pnames. name_out is NULL when no name appeared (legal in
 * prototypes and abstract declarators). */
static struct type *declarator(struct parser *ps, struct type *base,
                               const char **name_out, int nested)
{
    base = parse_stars(ps, base);
    *name_out = NULL;
    if (cur(ps)->kind == TOK_LPAREN) {
        struct lexer open = ps->lx;
        advance(ps);
        /* `(` starts a parenthesized declarator when what follows could
         * begin one. A star always could (`int (*fp)(void)`), and that
         * was the only case handled -- but so can a plain NAME, and
         * `int (f)(int);` is the redundantly-parenthesized spelling the
         * standard allows and real headers use, most often because a
         * macro of the same name has to be suppressed: `(isdigit)(c)`.
         * It was rejected outright with "expected a name before '('".
         *
         * The name must not be a typedef name, because then the
         * parentheses are a parameter list instead -- `int (size_t)` is
         * a function type, not a variable called size_t. That one token
         * of lookahead is the whole difference between the two. */
        if (cur(ps)->kind == TOK_STAR ||
            (cur(ps)->kind == TOK_IDENT && !at_type_start(ps))) {
            /* past the parentheses to the suffixes */
            int depth = 1;
            while (depth > 0) {
                if (cur(ps)->kind == TOK_EOF)
                    parse_error_line(ps, open.tok.line,
                               "unbalanced '(' in a declarator");
                advance(ps);
                if (cur(ps)->kind == TOK_LPAREN)
                    depth++;
                else if (cur(ps)->kind == TOK_RPAREN)
                    depth--;
            }
            advance(ps);
            struct type *outer = base;
            /* The suffix's parameter names are captured, because
             * `int (f)(int x) { … }` is a DEFINITION and the body needs
             * `x`. They go into a LOCAL array, never straight into
             * ps->fn_pnames: that array is shared by every declarator
             * being parsed, and a parameter can itself be a
             * parenthesized declarator. Writing it here directly made
             * `void (*signal(int sig, void (*handler)(int)))(int)` lose
             * `sig` -- the inner `(int)` of `handler` overwrote slot 0
             * while the outer parameter list was still filling it. */
            const char *pn[MAX_PARAMS];
            int have_pn = 0;
            if (cur(ps)->kind == TOK_LPAREN) {
                for (int pi = 0; pi < MAX_PARAMS; pi++)
                    pn[pi] = NULL;
                outer = parse_fn_params_named(ps, base, pn);
                have_pn = 1;
            } else if (cur(ps)->kind == TOK_LBRACKET)
                outer = parse_array_dims(ps, base);
            struct lexer after = ps->lx;
            ps->lx = open;
            advance(ps);
            struct type *t = declarator(ps, outer, name_out, 1);
            expect(ps, TOK_RPAREN, "')'");
            ps->lx = after;
            /* Publish them only when the parentheses held a bare name,
             * so this suffix IS the declared function's parameter list.
             * `t == outer` says exactly that: anything else -- a star, a
             * nested function declarator, an array -- derived a new type
             * inside, and then ps->fn_pnames already holds whatever that
             * derivation set, which is the right answer. The positions
             * are not recovered; they drive a caret, and this spelling
             * is rare enough that a column is worth less than not
             * corrupting a sibling's name. */
            if (have_pn && t == outer) {
                for (int pi = 0; pi < MAX_PARAMS; pi++) {
                    ps->fn_pnames[pi] = pn[pi];
                    ps->fn_plines[pi] = 0;
                    ps->fn_pcols[pi] = 0;
                }
            }
            return t;
        }
        ps->lx = open; /* not a parenthesized declarator */
    }
    if (cur(ps)->kind == TOK_IDENT) {
        *name_out = cur(ps)->text;
        ps->decl_name_line = cur(ps)->line;
        ps->decl_name_col = cur(ps)->col;
        advance(ps);
    }
    if (nested && cur(ps)->kind == TOK_LPAREN)
        return parse_fn_params_named(ps, base, ps->fn_pnames);
    return parse_array_dims(ps, base);
}

static struct type *parse_declarator(struct parser *ps, struct type *base,
                                     const char **name_out)
{
    return declarator(ps, base, name_out, 0);
}

/* An abstract type name (a cast target, a sizeof operand): a declarator
 * with the name omitted — so `void (*)(void)` and `int (*)[4]` parse, not
 * just pointer stars. parse_declarator already allows a missing name. */
static struct type *parse_type_name(struct parser *ps, struct type *base)
{
    const char *unused = NULL;
    return parse_declarator(ps, base, &unused);
}

/* The '(params)' of a function TYPE (as in a function pointer). */
/* Attributes before a parameter's type. Only embcc_sret means anything
 * there — the C++ front-end's mark for the ABI's indirect-result pointer —
 * and only on the first parameter. */
static int param_sret_attr(struct parser *ps, int index)
{
    if (cur(ps)->kind != TOK_KW_ATTRIBUTE)
        return 0;
    struct token *at = cur(ps);
    struct attrs a = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, NULL };
    parse_attributes(ps, &a);
    if (a.sret && index != 0)
        parse_error_at(ps, at->line, at->col,
                "embcc_sret marks the first parameter only");
    return a.sret;
}

static struct type *parse_fn_params(struct parser *ps, struct type *ret)
{
    /* Nor can a parameter carry one. */
    ps->attr_carry_on = 0;

    return parse_fn_params_named(ps, ret, NULL);
}

/* ... and the parameters' names into names[] (NULL where unnamed) */
static struct type *parse_fn_params_named(struct parser *ps, struct type *ret,
                                          const char **names)
{
    expect(ps, TOK_LPAREN, "'('");
    int saved_vla_ok = ps->vla_ok;
    ps->vla_ok = 1;   /* prototype scope: `int a[n]`, `int a[*]` */
    struct type *pt[MAX_PARAMS];
    int n = 0, varargs = 0, sret = 0;

    if (cur(ps)->kind == TOK_KW_VOID) {
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind != TOK_RPAREN)
            ps->lx = save;
    }
    if (cur(ps)->kind != TOK_RPAREN) {
        for (;;) {
            if (cur(ps)->kind == TOK_ELLIPSIS) {
                varargs = 1;
                advance(ps);
                break;
            }
            if (param_sret_attr(ps, n))
                sret = 1;
            struct type *spec = parse_type_spec(ps, 0);
            if (!spec)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a parameter type before %s",
                           tok_describe(cur(ps)));
            const char *pname;
            struct type *t = parse_declarator(ps, spec, &pname);
            if (names && n < MAX_PARAMS) {
                names[n] = pname;
                if (names == ps->fn_pnames) {
                    ps->fn_plines[n] = ps->decl_name_line;
                    ps->fn_pcols[n] = ps->decl_name_col;
                }
            }
            if (t->kind == TY_ARRAY)
                t = ty_ptr(t->pointee); /* C's adjustment */
            if (t->kind == TY_FUNC)
                t = ty_ptr(t);
            if (t->kind == TY_VOID)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "a parameter cannot have type void");
            if (n >= MAX_PARAMS)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "too many parameters in a function type");
            pt[n++] = t;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')'");
    ps->vla_ok = saved_vla_ok;
    struct type *ft = ty_func(ret, pt, n, varargs);
    ft->sret_first = sret;
    return ft;
}

static struct type *parse_struct_body(struct parser *ps, struct type *t,
                                      const struct attrs *lead);
static void parse_enum_body(struct parser *ps);

/* struct/union/enum specifier, after the keyword was consumed. */
static struct type *parse_tagged(struct parser *ps, enum tag_kind kind,
                                 int allow_body, int line)
{
    /* GNU C allows attributes right after the keyword —
     * `struct __attribute__((packed)) { ... } x;` — as well as after the
     * closing brace (parse_struct_body). Leading ones are collected here and
     * applied to the body exactly as trailing ones are. Between the tag and
     * the '{' is NOT a place gcc accepts one, so neither does EmbCC. */
    struct attrs lead = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, NULL };
    parse_attributes(ps, &lead);
    const char *tag = NULL;
    if (cur(ps)->kind == TOK_IDENT) {
        tag = cur(ps)->text;
        advance(ps);
    }
    if (kind == TAG_ENUM && (lead.packed || lead.aligned))
        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                "a packed or aligned enum is not supported (EmbCC's enums "
                "are always int-sized)");

    if (cur(ps)->kind == TOK_LBRACE) {
        if (!allow_body)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "define %s at file scope (block-scope type "
                       "definitions are not supported)",
                       kind == TAG_ENUM ? "enums" : "structs/unions");
        struct type *t = NULL;
        if (tag) {
            struct tagdef *td = find_tag(ps, tag);
            if (td) {
                if (td->kind != kind)
                    parse_error_line(ps, line,
                               "'%s' is a different kind of tag", tag);
                if (kind != TAG_ENUM && td->ty->complete)
                    parse_error_line(ps, line,
                               "redefinition of '%s'", tag);
                t = td->ty;
            } else {
                td = xcalloc(1, sizeof *td);
                td->tag = tag;
                td->kind = kind;
                if (kind != TAG_ENUM)
                    td->ty = ty_struct(tag, kind == TAG_UNION);
                td->next = ps->tags;
                ps->tags = td;
                t = td->ty;
            }
        } else if (kind != TAG_ENUM) {
            t = ty_struct(NULL, kind == TAG_UNION);
        }
        if (kind == TAG_ENUM) {
            parse_enum_body(ps);
            return ty_base(TY_INT, 0);
        }
        return parse_struct_body(ps, t, &lead);
    }

    if (!tag)
        parse_error_line(ps, line, "%s needs a tag or a body",
                   kind == TAG_ENUM ? "enum" :
                   kind == TAG_UNION ? "union" : "struct");
    struct tagdef *td = find_tag(ps, tag);
    if (td) {
        if (td->kind != kind)
            parse_error_line(ps, line,
                       "'%s' is a different kind of tag", tag);
        return kind == TAG_ENUM ? ty_base(TY_INT, 0) : td->ty;
    }
    if (kind == TAG_ENUM)
        parse_error_line(ps, line, "unknown enum '%s'", tag);
    /* Forward reference: an incomplete struct, fine behind a pointer */
    td = xcalloc(1, sizeof *td);
    td->tag = tag;
    td->kind = kind;
    td->ty = ty_struct(tag, kind == TAG_UNION);
    td->next = ps->tags;
    ps->tags = td;
    return td->ty;
}

/* Consumes a type specifier if one starts here, else returns NULL with
 * nothing consumed. "unsigned"/"signed" alone mean int, as in C.
 * allow_body: may a struct/union/enum BODY appear here (file scope). */
static struct type *parse_type_spec_inner(struct parser *ps, int allow_body,
                                          int *vol);

static struct type *parse_type_spec(struct parser *ps, int allow_body)
{
    int vol = 0;
    struct type *t = parse_type_spec_inner(ps, allow_body, &vol);
    return (t && vol) ? ty_volatile(t) : t;   /* volatile reaches the type */
}

/* `_Alignas(N)` / `_Alignas(type-name)` — a C11 alignment specifier among the
 * declaration specifiers. Its value is accumulated into ps->alignas_out (the
 * strictest wins), which the declaration then applies exactly like an
 * `__attribute__((aligned(N)))`. A run is consumed so it may appear more than
 * once or interleave with the type specifiers. */
static void consume_alignas(struct parser *ps)
{
    while (cur(ps)->kind == TOK_KW_ALIGNAS) {
        int line = cur(ps)->line;
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after _Alignas");
        int a;
        if (at_type_start(ps)) {
            struct type *t = parse_type_name(ps, parse_type_spec(ps, 0));
            a = ty_align(t);
        } else {
            struct expr *e = parse_cond(ps);
            long v;
            if (!size_fold(e, &v) || v <= 0)
                parse_error_line(ps, line,
                           "_Alignas requires a positive constant alignment");
            a = (int)v;
        }
        expect(ps, TOK_RPAREN, "')' after _Alignas");
        if (a > ps->alignas_out) ps->alignas_out = a;
    }
}

static struct type *parse_type_spec_inner(struct parser *ps, int allow_body,
                                          int *vol)
{
    *vol = skip_quals(ps);
    consume_alignas(ps);
    /* GNU `typeof(x)` / `typeof(type)` — the type of an expression (never
     * evaluated, like sizeof) or a type-name. The expression's type is resolved
     * with ce_type: a variable, a deref, a `.`/`->` member, a cast — the shapes
     * kernel macros (min/max, container_of helpers) use. */
    if (cur(ps)->kind == TOK_KW_TYPEOF) {
        int line = cur(ps)->line;
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after typeof");
        struct type *t;
        if (at_type_start(ps)) {
            t = parse_type_name(ps, parse_type_spec(ps, 0));
        } else {
            struct expr *e = parse_cond(ps);
            t = ce_type(e);
            if (!t)
                parse_error_line(ps, line,
                           "typeof of an unsupported expression");
        }
        expect(ps, TOK_RPAREN, "')' after typeof");
        return t;
    }
    /* `_Atomic(type)` atomic-type-specifier — mapped to a volatile type. */
    if (cur(ps)->kind == TOK_KW_ATOMIC) {
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after _Atomic");
        struct type *t = parse_type_name(ps, parse_type_spec(ps, 0));
        expect(ps, TOK_RPAREN, "')' after _Atomic(type)");
        *vol = 1;
        return t;
    }
    /* struct/union/enum first (cannot mix with other specifiers) */
    if (cur(ps)->kind == TOK_KW_STRUCT || cur(ps)->kind == TOK_KW_UNION ||
        cur(ps)->kind == TOK_KW_ENUM) {
        enum tag_kind k = cur(ps)->kind == TOK_KW_STRUCT ? TAG_STRUCT :
                          cur(ps)->kind == TOK_KW_UNION ? TAG_UNION :
                          TAG_ENUM;
        int line = cur(ps)->line;
        advance(ps);
        /* Parsing the tag body recurses through declarations that use and reset
         * alignas_out; preserve this declaration's own across it. */
        int saved = ps->alignas_out;
        ps->alignas_out = 0;
        struct type *tt = parse_tagged(ps, k, allow_body, line);
        ps->alignas_out = saved;
        return tt;
    }
    /* a typedef name, when no specifier has appeared */
    if (cur(ps)->kind == TOK_IDENT) {
        /* __builtin_va_list is `char *` — the same representation EmbCC's
         * <stdarg.h> gives va_list — so `typedef __builtin_va_list ...`
         * (as some headers write it) resolves. */
        if (strcmp(cur(ps)->text, "__builtin_va_list") == 0) {
            advance(ps);
            return ty_ptr(ty_plain_char());
        }
        /* GCC's names for the 128-bit integers (typedefs it predeclares) */
        if (strcmp(cur(ps)->text, "__int128_t") == 0 ||
            strcmp(cur(ps)->text, "__uint128_t") == 0) {
            int u = cur(ps)->text[2] == 'u';
            advance(ps);
            return ty_base(TY_INT128, u);
        }
        struct type *td = find_typedef(ps, cur(ps)->text);
        if (!td)
            return NULL;
        advance(ps);
        return td;
    }
    /* base specifiers in any order: unsigned long int, long unsigned... */
    int uns = -1, nlong = 0, nshort = 0, nchar = 0, nint = 0, nvoid = 0;
    int nfloat = 0, ndouble = 0, nbool = 0, ncomplex = 0, n128 = 0;
    int any = 0;
    for (;;) {
        enum tok_kind k = cur(ps)->kind;
        if (k == TOK_KW_FLOAT) nfloat++;
        else if (k == TOK_KW_COMPLEX) ncomplex++;
        else if (k == TOK_KW_BOOL) nbool++;
        else if (k == TOK_KW_DOUBLE) ndouble++;
        else if (k == TOK_KW_UNSIGNED) uns = 1;
        else if (k == TOK_KW_SIGNED) uns = 0;
        else if (k == TOK_KW_LONG) nlong++;
        else if (k == TOK_KW_INT128) n128++;
        else if (k == TOK_KW_SHORT) nshort++;
        else if (k == TOK_KW_CHAR) nchar++;
        else if (k == TOK_KW_INT) nint++;
        else if (k == TOK_KW_VOID) nvoid++;
        else if (k == TOK_KW_CONST || k == TOK_KW_VOLATILE ||
                 k == TOK_KW_RESTRICT) {
            if (k == TOK_KW_VOLATILE) *vol = 1;
            advance(ps); continue;
        }
        else if (k == TOK_KW_ALIGNAS) { consume_alignas(ps); continue; }
        else if (k == TOK_KW_ATOMIC) {   /* qualifier form after a type spec */
            struct lexer save = ps->lx;
            advance(ps);
            if (cur(ps)->kind == TOK_LPAREN) { ps->lx = save; break; }
            *vol = 1; continue;
        }
        else break;
        any++;
        advance(ps);
    }
    if (!any)
        return NULL;
    if (nbool) {
        if (any > 1)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "_Bool cannot combine with other specifiers");
        return ty_base(TY_BOOL, 0);
    }
    if (ncomplex) {
        /* `T _Complex` for a floating T; a bare _Complex means double, as
         * in gcc. The GNU integer complex types are not supported. */
        if (ncomplex > 1 || uns != -1 || nchar || nshort || nint || nvoid ||
            nbool || (nfloat && ndouble) || nlong > 1 || (nlong && nfloat))
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "invalid _Complex type (only float, double and long "
                       "double _Complex are supported)");
        struct type *el = ty_base(nfloat ? TY_FLOAT
                                  : nlong ? TY_LDOUBLE : TY_DOUBLE, 0);
        return ty_complex(el);
    }
    if (nfloat || ndouble) {
        if (uns != -1 || nchar || nshort || nint || nvoid ||
            (nfloat && ndouble))
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "invalid type specifier combination");
        if (nlong > 1 || (nlong && nfloat))
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "invalid type specifier combination");
        if (nlong)
            return ty_base(TY_LDOUBLE, 0);
        return ty_base(nfloat ? TY_FLOAT : TY_DOUBLE, 0);
    }
    if (nvoid) {
        if (any > 1)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "void cannot combine with other specifiers");
        return ty_base(TY_VOID, 0);
    }
    if (n128) {
        if (n128 > 1 || nlong || nshort || nchar || nint || nvoid || nfloat ||
            ndouble || nbool || ncomplex)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "invalid type specifier combination");
        return ty_base(TY_INT128, uns == 1);
    }
    if (nlong > 2 || (nshort && nlong) || (nchar && (nshort || nlong)) ||
        (nchar && nint))
        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                   "invalid type specifier combination");
    enum ty_kind kind = nchar ? TY_CHAR :
                        nshort ? TY_SHORT :
                        nlong ? TY_LONG : TY_INT;
    /* `char` with no signed/unsigned is the target's plain char. */
    if (kind == TY_CHAR && uns == -1)
        return ty_plain_char();
    return ty_base(kind, uns == 1);
}

/* Fold whatever parse_stars dropped in the parser's slot into this
 * declaration's attributes, and close the slot behind it. */
static void take_carried(struct parser *ps, struct attrs *a)
{
    if (!ps->attr_carry_on)
        return;
    if (ps->attr_slot.weak)     a->weak = 1;
    if (ps->attr_slot.noreturn) a->noreturn = 1;
    if (ps->attr_slot.fmt_kind) {
        a->fmt_kind = ps->attr_slot.fmt_kind;
        a->fmt_idx = ps->attr_slot.fmt_idx;
        a->fmt_first = ps->attr_slot.fmt_first;
    }
    if (ps->attr_slot.packed)   a->packed = 1;
    if (ps->attr_slot.aligned > a->aligned)
        a->aligned = ps->attr_slot.aligned;
    if (ps->attr_slot.section && !a->section)
        a->section = ps->attr_slot.section;
    memset(&ps->attr_slot, 0, sizeof ps->attr_slot);
    /* The slot stays OPEN: a declaration may parse its declarator
     * twice -- once to see whether it is a function, then again after
     * a rewind -- and closing it after the first pass would refuse the
     * attribute on the second. It is closed where a context must
     * refuse instead (below). */
}

static struct type *parse_stars(struct parser *ps, struct type *t)
{
    for (;;) {
        skip_quals(ps); /* char * const p, const char *p, ... */
        /* GCC also lets an attribute sit where a qualifier can:
         * `typedef uint64_t __attribute__((may_alias)) word_t;`,
         * `int * __attribute__((unused)) p`. The ones EmbCC ignores everywhere
         * are ignored here too (may_alias is a no-op: the optimizer has no
         * type-based alias analysis). The ones it HONOURS elsewhere change
         * layout or linkage, and this position has nowhere to carry them, so
         * they are refused rather than silently dropped. */
        if (cur(ps)->kind == TOK_KW_ATTRIBUTE) {
            struct token *at_tok = cur(ps);
            struct attrs a = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, NULL };
            parse_attributes(ps, &a);
            if (ps->attr_carry_on) {
                /* The enclosing declaration will take them. */
                if (a.weak)     ps->attr_slot.weak = 1;
                if (a.noreturn) ps->attr_slot.noreturn = 1;
                if (a.fmt_kind) {
                    ps->attr_slot.fmt_kind = a.fmt_kind;
                    ps->attr_slot.fmt_idx = a.fmt_idx;
                    ps->attr_slot.fmt_first = a.fmt_first;
                }
                if (a.packed)   ps->attr_slot.packed = 1;
                if (a.aligned > ps->attr_slot.aligned)
                    ps->attr_slot.aligned = a.aligned;
                if (a.section && !ps->attr_slot.section)
                    ps->attr_slot.section = a.section;
                continue;
            }
            if (a.packed || a.aligned || a.weak || a.noreturn)
                parse_error_at(ps, at_tok->line, at_tok->col,
                        "__attribute__((%s)) is not supported in this position "
                        "(after a declarator it is; on a struct or union, put "
                        "it right after the keyword or after the closing '}')",
                        a.packed ? "packed" : a.aligned ? "aligned"
                        : a.weak ? "weak" : "noreturn");
            continue;
        }
        if (cur(ps)->kind != TOK_STAR)
            return t;
        t = ty_ptr(t);
        advance(ps);
    }
}

/* Folds an array-size expression at PARSE time. It can evaluate
 * sizeof(type) because the parser owns the typedef and tag tables —
 * which is what real headers need: newlib's fd_set is
 * `__fds_bits[_howmany(FD_SETSIZE, _NFDBITS)]`, and _NFDBITS expands to
 * ((int)sizeof(__fd_mask) * 8). Anything it cannot evaluate (an
 * identifier, sizeof of an expression) is refused by name. */
/* The unit being parsed, so size_fold can resolve enum constants (which are
 * compile-time integer constants) in constant expressions like array sizes. */
static struct unit *g_fold_unit;

/* Locals declared so far in the current function, so `sizeof(var)` in a later
 * array size — `char buf[sizeof payload]` — resolves at parse time. Flat and
 * reset per top-level item, matching EmbCC's flat local scope. */
static struct { const char *name; struct type *ty; } g_fold_locals[512];
static int g_nfold_locals;

static void fold_local_add(const char *name, struct type *ty)
{
    if (name && ty && g_nfold_locals < 512) {
        g_fold_locals[g_nfold_locals].name = name;
        g_fold_locals[g_nfold_locals].ty = ty;
        g_nfold_locals++;
    }
}

/* The type of a named variable for sizeof folding: a local in scope, else a
 * file-scope global. */
static struct type *fold_var_type(const char *name)
{
    for (int i = g_nfold_locals - 1; i >= 0; i--)
        if (strcmp(g_fold_locals[i].name, name) == 0)
            return g_fold_locals[i].ty;
    for (struct global *g = g_fold_unit ? g_fold_unit->globals : NULL;
         g; g = g->next)
        if (g->name && strcmp(g->name, name) == 0)
            return g->ty;
    return NULL;
}

/* The type of a constant-expression subset — enough to fold sizeof(EXPR) in
 * an integer-constant-expression: a cast fixes the type, `->`/`.` reach a
 * member, `*` dereferences. `sizeof(((struct V*)0)->field)` is the shape the
 * kernel uses to bound one struct's storage against another's. */
static struct type *ce_type(const struct expr *e)
{
    switch (e->kind) {
    case EXPR_VAR:
        return fold_var_type(e->name);
    case EXPR_CAST:
        return e->cast_ty;
    case EXPR_DEREF: {
        struct type *t = ce_type(e->rhs);
        return t && t->kind == TY_PTR ? t->pointee : NULL;
    }
    case EXPR_BINOP: {
        /* Pointer arithmetic: `a + n` has the pointer's type.
         *
         * This is how `sizeof a[0]` arrives, because `a[i]` is built as
         * `*(a + i)` -- so without this case the commonest length idiom
         * in C, `sizeof(a) / sizeof(a[0])`, does not fold, and an array
         * size written that way is rejected as "not a constant
         * expression". gcc accepts it, and so does every C since C89.
         *
         * An array operand decays here, which is what makes the
         * dereference above find the ELEMENT type rather than the array
         * type again. */
        struct type *lt, *rt, *t;
        if (e->op != B_ADD && e->op != B_SUB)
            return NULL;
        lt = ce_type(e->lhs);
        rt = e->op == B_ADD ? ce_type(e->rhs) : NULL;
        t = lt && (lt->kind == TY_PTR || lt->kind == TY_ARRAY) ? lt
          : rt && (rt->kind == TY_PTR || rt->kind == TY_ARRAY) ? rt
          : NULL;
        if (!t)
            return NULL;
        return t->kind == TY_ARRAY ? ty_ptr(t->pointee) : t;
    }
    case EXPR_MEMBER: {
        struct type *bt = ce_type(e->lhs);
        if (!bt)
            return NULL;
        struct type *st = e->is_arrow
                        ? (bt->kind == TY_PTR ? bt->pointee : NULL) : bt;
        if (!st || st->kind != TY_STRUCT || !st->complete)
            return NULL;
        struct member *m = ty_find_member(st, e->name);
        return m ? m->ty : NULL;
    }
    default:
        return NULL;
    }
}

static int size_fold(const struct expr *e, long *out)
{
    long a, b;

    switch (e->kind) {
    case EXPR_NUM:
        *out = e->num;
        return 1;
    case EXPR_VAR:
        /* an enumerator is an integer constant expression: `int a[N];` */
        for (struct econst *ec = g_fold_unit ? g_fold_unit->econsts : NULL;
             ec; ec = ec->next)
            if (strcmp(ec->name, e->name) == 0) {
                *out = ec->val;
                return 1;
            }
        return 0;
    case EXPR_SIZEOF: {
        struct type *t = e->cast_ty ? e->cast_ty : ce_type(e->rhs);
        if (!t || ty_size(t) == 0)
            return 0;   /* sizeof(expr) whose type this pass cannot resolve */
        *out = ty_size(t);
        return 1;
    }
    case EXPR_ALIGNOF: {
        struct type *t = e->cast_ty ? e->cast_ty : ce_type(e->rhs);
        if (!t || ty_size(t) == 0)
            return 0;
        *out = ty_align(t);
        return 1;
    }
    case EXPR_CAST:
        return size_fold(e->rhs, out);
    case EXPR_CALL:
        /* __atomic_always_lock_free / __atomic_is_lock_free are integer
         * constant expressions in gcc — `_Static_assert` uses them, and it
         * is evaluated here, before sema folds the call (sema.c
         * check_atomic_call answers the same way). */
        if (e->lhs && e->lhs->kind == EXPR_VAR && e->nargs == 1 &&
            strcmp(e->lhs->name, "__builtin_constant_p") == 0) {
            *out = size_fold(e->args[0], &a);   /* sema answers the same */
            return 1;
        }
        if (e->lhs && e->lhs->kind == EXPR_VAR && e->nargs == 2 &&
            (strcmp(e->lhs->name, "__atomic_always_lock_free") == 0 ||
             strcmp(e->lhs->name, "__atomic_is_lock_free") == 0) &&
            size_fold(e->args[0], &a)) {
            *out = a == 1 || a == 2 || a == 4 || a == 8;
            return 1;
        }
        return 0;
    case EXPR_NEG:
        if (!size_fold(e->rhs, &a)) return 0;
        *out = -a;
        return 1;
    case EXPR_BNOT:
        if (!size_fold(e->rhs, &a)) return 0;
        *out = ~a;
        return 1;
    case EXPR_NOT:
        if (!size_fold(e->rhs, &a)) return 0;
        *out = !a;
        return 1;
    case EXPR_COND: {
        /* `int buf[(N > 4) ? N : 4];` -- a constant conditional, which C11
         * 6.6 admits like any other operator and which was the one piece
         * missing here. Only the SELECTED arm has to be constant: the
         * standard evaluates one branch, so `1 ? 1 : 1/0` is well-formed
         * and folding both would reject it. */
        long c;
        if (!size_fold(e->args[0], &c))
            return 0;
        return size_fold(c ? e->args[1] : e->args[2], out);
    }
    case EXPR_BINOP:
        /* && and || short-circuit, so the right operand must neither be
         * evaluated nor be required to fold when the left one decides the
         * answer: `sizeof(long) > 8 && 1/0` is a constant expression whose
         * value is 0. */
        if (e->op == B_LAND || e->op == B_LOR) {
            if (!size_fold(e->lhs, &a))
                return 0;
            if (e->op == B_LAND ? !a : a) { *out = e->op == B_LOR; return 1; }
            if (!size_fold(e->rhs, &b))
                return 0;
            *out = !!b;
            return 1;
        }
        if (!size_fold(e->lhs, &a) || !size_fold(e->rhs, &b))
            return 0;
        switch (e->op) {
        case B_ADD: *out = a + b; return 1;
        case B_SUB: *out = a - b; return 1;
        case B_MUL: *out = a * b; return 1;
        case B_DIV: if (!b) return 0; *out = a / b; return 1;
        case B_MOD: if (!b) return 0; *out = a % b; return 1;
        case B_AND: *out = a & b; return 1;
        case B_OR:  *out = a | b; return 1;
        case B_XOR: *out = a ^ b; return 1;
        case B_SHL: *out = a << b; return 1;
        case B_SHR: *out = a >> b; return 1;
        case B_LT:  *out = a < b; return 1;
        case B_GT:  *out = a > b; return 1;
        case B_LE:  *out = a <= b; return 1;
        case B_GE:  *out = a >= b; return 1;
        case B_EQ:  *out = a == b; return 1;
        case B_NE:  *out = a != b; return 1;
        case B_LAND: *out = a && b; return 1;
        case B_LOR:  *out = a || b; return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* Shared by locals, globals, and members: trailing [N]([M]...) turns t
 * into (nested) array types. Sizes are constant expressions. */
static struct type *parse_array_dims(struct parser *ps, struct type *t)
{
    int dims[4];
    struct expr *dexpr[4];   /* each dimension's expression (NULL for []) */
    int dvar[4];             /* 1 = not a constant: a VLA dimension */
    int ndims = 0;
    while (cur(ps)->kind == TOK_LBRACKET) {
        advance(ps);
        int dim = 0; /* [] : legal for params (adjusts to a pointer);
                        elsewhere caught as an incomplete type */
        struct expr *de = NULL;
        int var = 0;
        if (cur(ps)->kind == TOK_STAR && ps->vla_ok) {
            /* `[*]`: a VLA of unspecified size, prototype scope only (C99
             * 6.7.5.2p4). Only its adjusted pointer type survives. */
            struct lexer save = ps->lx;
            advance(ps);
            if (cur(ps)->kind == TOK_RBRACKET) {
                de = new_expr(EXPR_NUM, cur(ps)->line, 0);
                de->num = 1;
                var = 1;
            } else {
                ps->lx = save;
            }
        }
        if (!de && cur(ps)->kind != TOK_RBRACKET) {
            int dline = cur(ps)->line;
            de = parse_cond(ps);
            long dv;
            if (!size_fold(de, &dv)) {
                if (!ps->vla_ok)
                    parse_error_line(ps, dline,
                               "array size must be a constant expression "
                               "here (a variable length array can only be "
                               "a local variable or a parameter)");
                var = 1;
            } else {
                if (dv < 0)
                    parse_error_line(ps, dline,
                               "array size cannot be negative");
                dim = (int)dv; /* 0 is the extern/flexible form */
            }
        }
        if (ndims >= 4)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "more than 4 array dimensions");
        dims[ndims] = dim;
        dexpr[ndims] = de;
        dvar[ndims] = var;
        ndims++;
        expect(ps, TOK_RBRACKET, "']'");
    }
    /* Innermost first. Once any dimension is variable, every dimension
     * outside it is too — `int a[3][n]` is 3 rows of a run-time size — so
     * each becomes a VLA node (with its constant as the length). */
    for (int i = ndims - 1; i >= 0; i--) {
        if (dvar[i] || (ty_is_vla(t) && dexpr[i]))
            t = ty_vla(t, dexpr[i]);
        else
            t = ty_array(t, dims[i]);
    }
    return t;
}

static struct type *parse_struct_body(struct parser *ps, struct type *t,
                                      const struct attrs *lead)
{
    /* A member's declarator has nowhere to carry an attribute found
     * after a `*`, so inside a struct body the refusal stands. */
    ps->attr_carry_on = 0;

    expect(ps, TOK_LBRACE, "'{'");
    int saved_vla_ok = ps->vla_ok;
    ps->vla_ok = 0;   /* a member cannot be variably modified (6.7.2.1p9) */
    struct member *ms = NULL;
    int n = 0, cap = 0;

    while (cur(ps)->kind != TOK_RBRACE) {
        /* a `_Static_assert` among the members: checked, contributes none */
        if (cur(ps)->kind == TOK_KW_STATIC_ASSERT) {
            parse_static_assert(ps);
            continue;
        }
        /* allow_body: nested struct/union definitions are legal C */
        struct type *spec = parse_type_spec(ps, 1);
        if (!spec)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected a member type before %s",
                       tok_describe(cur(ps)));
        for (;;) { /* declarators share the base: int a, *b, c[4]; */
            int mline = cur(ps)->line;
            const char *mname;
            struct type *mty = parse_declarator(ps, spec, &mname);
            /* A bitfield: `T name : width` or an anonymous `T : width`
             * (padding) / `T : 0` (a separator forcing the next field to a
             * storage-unit boundary). Only integer types may be bitfields. */
            int is_bf = 0, bit_width = 0;
            if (cur(ps)->kind == TOK_COLON) {
                advance(ps);
                if (!ty_is_integer(mty))
                    parse_error_line(ps, mline,
                               "a bitfield must have integer type, not %s",
                               ty_name(mty));
                struct expr *we = parse_cond(ps);
                long wv;
                if (!size_fold(we, &wv) || wv < 0)
                    parse_error_line(ps, mline,
                               "a bitfield width must be a constant >= 0");
                if (wv > 8 * (long)ty_size(mty))
                    parse_error_line(ps, mline,
                               "bitfield '%s' width %ld exceeds its type %s",
                               mname ? mname : "<anon>", wv, ty_name(mty));
                if (wv == 0 && mname)
                    parse_error_line(ps, mline,
                               "a named bitfield '%s' cannot have width 0",
                               mname);
                is_bf = 1;
                bit_width = (int)wv;
            }
            if (mty->kind == TY_VOID)
                parse_error_line(ps, mline,
                           "a member cannot have type void");
            if (mty->kind == TY_FUNC)
                parse_error_line(ps, mline,
                           "a member cannot be a function — use a "
                           "function pointer");
            /* An anonymous struct/union member (`struct { ... };` with no
             * declarator) is legal C11 — its members are reached as if they
             * belonged to the enclosing type. A nameless non-aggregate is
             * still an error. */
            if (!mname && !is_bf && mty->kind != TY_STRUCT)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a member name before %s",
                           tok_describe(cur(ps)));
            /* (an array of no elements — `T m[]`, C99's flexible array
             * member, or GNU's `T m[0]` — takes no room: its elements are
             * what follows the struct) */
            if (!is_bf && ty_size(mty) == 0 &&
                !(mty->kind == TY_ARRAY && mty->count == 0 && !mty->vla_len &&
                  ty_size(mty->pointee) > 0))
                parse_error_line(ps, mline,
                           "member '%s' has incomplete type %s",
                           mname, ty_name(mty));
            if (mname)
                for (int i = 0; i < n; i++)
                    if (ms[i].name && strcmp(ms[i].name, mname) == 0)
                        parse_error_line(ps, mline,
                                   "duplicate member '%s'", mname);
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                ms = xrealloc(ms, (size_t)cap * sizeof *ms);
            }
            struct attrs mat = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, NULL };
            parse_attributes(ps, &mat);  /* T buf[N] __attribute__((aligned(N))) */
            ms[n].name = mname;
            ms[n].ty = mty;
            ms[n].off = 0;
            ms[n].is_bitfield = is_bf;
            ms[n].bit_off = 0;
            ms[n].bit_width = bit_width;
            ms[n].user_align = mat.aligned > ps->alignas_out
                               ? mat.aligned : ps->alignas_out;
            ps->alignas_out = 0;
            n++;
            if (cur(ps)->kind == TOK_COMMA) {
                advance(ps);
                continue;
            }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
    }
    advance(ps); /* '}' */
    ps->vla_ok = saved_vla_ok;
    struct attrs at = *lead;     /* struct __attribute__((packed)) {...} */
    parse_attributes(ps, &at);   /* struct {...} __attribute__((packed)) */
    if (n == 0)
        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                   "a struct/union needs at least one member");
    ty_struct_layout(t, ms, n, at.packed, at.aligned);
    return t;
}

static void parse_enum_body(struct parser *ps)
{
    expect(ps, TOK_LBRACE, "'{'");
    long val = 0;

    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind != TOK_IDENT)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected an enumerator name before %s",
                       tok_describe(cur(ps)));
        const char *name = cur(ps)->text;
        int line = cur(ps)->line;
        advance(ps);
        if (cur(ps)->kind == TOK_ASSIGN) {
            advance(ps);
            int vline = cur(ps)->line, vcol = cur(ps)->col;
            struct expr *ve = parse_cond(ps);
            if (!size_fold(ve, &val))
                parse_error_at(ps, vline, vcol,
                        "an enumerator value must be an integer constant "
                        "expression");
        }
        for (struct econst *ec = ps->unit->econsts; ec; ec = ec->next)
            if (strcmp(ec->name, name) == 0)
                parse_error_line(ps, line,
                           "duplicate enumerator '%s'", name);
        struct econst *ec = xcalloc(1, sizeof *ec);
        ec->name = name;
        ec->val = val++;
        ec->seq = ps->seq;
        *ps->econst_tail = ec;
        ps->econst_tail = &ec->next;
        if (cur(ps)->kind != TOK_COMMA)
            break;
        advance(ps); /* trailing comma before '}' is fine, as in C99 */
    }
    expect(ps, TOK_RBRACE, "'}'");
}

/* ---- expressions ---- */

static struct expr *new_expr(enum expr_kind kind, int line, int col)
{
    struct expr *e = xcalloc(1, sizeof *e);
    e->kind = kind;
    e->line = line;
    e->col = col;
    e->desig_index = -1;      /* positional unless a [i] designator sets it */
    e->desig_index_hi = -1;
    return e;
}

static struct expr *parse_expr(struct parser *ps);
static struct expr *parse_comma(struct parser *ps);
static struct expr *parse_unary(struct parser *ps);
static struct expr *binop(enum binop op, struct expr *lhs,
                          struct expr *rhs);

static struct expr *parse_primary(struct parser *ps)
{
    struct token *t = cur(ps);
    struct expr *e;

    switch (t->kind) {
    case TOK_KW_GENERIC: {
        /* _Generic(controlling, T1: e1, ..., default: eN) — a compile-time
         * type-directed selection; sema picks the matching arm. */
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after _Generic");
        e = new_expr(EXPR_GENERIC, t->line, t->col);
        e->lhs = parse_expr(ps);          /* the controlling expression */
        int cap = 0;
        while (cur(ps)->kind == TOK_COMMA) {
            advance(ps);
            struct type *at = NULL;
            if (cur(ps)->kind == TOK_KW_DEFAULT)
                advance(ps);              /* the default association */
            else
                at = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_COLON, "':' in a _Generic association");
            struct expr *ae = parse_expr(ps);
            if (e->ngen == cap) {
                cap = cap ? cap * 2 : 4;
                e->gtypes = xrealloc(e->gtypes, (size_t)cap * sizeof *e->gtypes);
                e->gexprs = xrealloc(e->gexprs, (size_t)cap * sizeof *e->gexprs);
            }
            e->gtypes[e->ngen] = at;
            e->gexprs[e->ngen] = ae;
            e->ngen++;
        }
        expect(ps, TOK_RPAREN, "')' to close _Generic");
        return e;
    }
    case TOK_NUM:
        e = new_expr(EXPR_NUM, t->line, t->col);
        e->num = t->num;
        e->ty = ty_base(t->num_long ? TY_LONG : TY_INT, t->num_uns);
        advance(ps);
        return e;
    case TOK_FNUM:
        e = new_expr(EXPR_FNUM, t->line, t->col);
        e->fnum = t->fnum;
        e->ty = ty_base(t->fnum_is_float ? TY_FLOAT : TY_DOUBLE, 0);
        if (t->fnum_is_imag) {
            e->imag = 1;
            e->ty = ty_complex(ty_base(t->fnum_is_float ? TY_FLOAT
                                       : t->fnum_is_ld ? TY_LDOUBLE
                                                       : TY_DOUBLE, 0));
        }
        if (t->fnum_is_ld) {
            if (!t->fnum_is_imag)
                e->ty = ty_base(TY_LDOUBLE, 0);
            e->ldv = ldf_from_text(t->text, ldf_target_fmt());
            if (!e->ldv)
                parse_error_line(ps, t->line,
                           "malformed floating constant '%sL'", t->text);
        }
        advance(ps);
        return e;
    case TOK_STR: {
        /* Adjacent string literals concatenate (C translation phase 6):
         * "foo" "bar" is one literal "foobar". The DECODED elements are
         * joined and encoded once, because the result's width is the widest
         * prefix among the pieces — "a" L"b" is a wide literal, so the "a"
         * must be re-encoded as UTF-32, not copied as a byte. */
        e = new_expr(EXPR_STR, t->line, t->col);
        int line0 = t->line;   /* t is the current-token slot: it moves on */
        int width = t->str_width, prefix = t->str_prefix;
        size_t n = 0, cap = 0;
        struct litch *lc = NULL;
        while (cur(ps)->kind == TOK_STR) {
            struct token *st = cur(ps);
            if (st->str_prefix) {
                if (prefix && prefix != st->str_prefix)
                    parse_error_at(ps, st->line, st->col,
                            "concatenating %c\"\" and %c\"\" literals is not "
                            "supported", prefix, st->str_prefix);
                prefix = st->str_prefix;
                width = st->str_width;
            }
            if (n + (size_t)st->nlit > cap) {
                cap = (n + (size_t)st->nlit) * 2 + 8;
                lc = xrealloc(lc, cap * sizeof *lc);
            }
            memcpy(lc + n, st->lit, (size_t)st->nlit * sizeof *lc);
            n += (size_t)st->nlit;
            advance(ps);
        }
        e->str_width = width;
        e->str_prefix = (char)prefix;
        e->name = lit_encode(lc, (int)n, width, &e->num, ps->lx.file, line0);
        free(lc);
        return e;
    }
    case TOK_LPAREN:
        advance(ps);
        if (cur(ps)->kind == TOK_LBRACE) {
            /* GNU statement expression `({ ... })`: the block's value is its
             * last statement when that is an expression statement. */
            e = new_expr(EXPR_STMTEXPR, t->line, t->col);
            e->body = parse_block(ps);
            expect(ps, TOK_RPAREN, "')' to close a statement expression");
            return e;
        }
        e = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        return e;
    case TOK_IDENT: {
        /* va_arg(ap, type) -> __builtin_va_arg((ap), type): a special form,
         * because its second argument is a TYPE, not an expression. lhs
         * holds ap; cast_ty holds the type read. */
        if (strcmp(t->text, "__builtin_eh_typeid") == 0) {
            /* the selector value of a catch type (EXPR_EHTYPEID) */
            int line = t->line;
            advance(ps);
            expect(ps, TOK_LPAREN, "'(' after __builtin_eh_typeid");
            e = new_expr(EXPR_EHTYPEID, line, 0);
            if (cur(ps)->kind == TOK_IDENT)
                e->name = cur(ps)->text;
            else if (!(cur(ps)->kind == TOK_NUM && cur(ps)->num == 0))
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                        "expected a typeinfo object or 0");
            advance(ps);
            expect(ps, TOK_RPAREN, "')' to close __builtin_eh_typeid");
            return e;
        }
        if (strcmp(t->text, "__builtin_va_arg") == 0) {
            int line = t->line;
            advance(ps);
            expect(ps, TOK_LPAREN, "'(' after __builtin_va_arg");
            e = new_expr(EXPR_VA_ARG, line, 0);
            e->lhs = parse_expr(ps);
            expect(ps, TOK_COMMA, "',' before the va_arg type");
            e->cast_ty = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_RPAREN, "')' to close __builtin_va_arg");
            return e;
        }
        /* __builtin_offsetof(type, member-designator) — the byte offset of a
         * member, folded to a size_t constant right here so it is usable in an
         * integer-constant-expression (a _Static_assert, an array size). The
         * designator may descend through `.field` and `[index]`. This is what
         * <stddef.h>'s offsetof expands to. */
        if (strcmp(t->text, "__builtin_offsetof") == 0) {
            int line = t->line;
            advance(ps);
            expect(ps, TOK_LPAREN, "'(' after __builtin_offsetof");
            struct type *ty = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_COMMA, "',' before the member designator");
            long off = 0;
            for (;;) {
                if (ty->kind != TY_STRUCT || !ty->complete)
                    parse_error_line(ps, line,
                               "offsetof needs a complete struct/union type");
                if (cur(ps)->kind != TOK_IDENT)
                    parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                               "expected a member name in offsetof");
                struct member *m2 = ty_find_member(ty, cur(ps)->text);
                if (!m2)
                    parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                               "%s has no member '%s'", ty_name(ty),
                               cur(ps)->text);
                off += m2->off;
                ty = m2->ty;
                advance(ps);
                while (cur(ps)->kind == TOK_LBRACKET) {
                    advance(ps);
                    long iv;
                    if (!size_fold(parse_cond(ps), &iv))
                        parse_error_line(ps, line,
                                   "offsetof array index must be constant");
                    expect(ps, TOK_RBRACKET, "']'");
                    if (ty->kind != TY_ARRAY)
                        parse_error_line(ps, line,
                                   "offsetof indexed a non-array member");
                    off += iv * ty_size(ty->pointee);
                    ty = ty->pointee;
                }
                if (cur(ps)->kind == TOK_DOT) { advance(ps); continue; }
                break;
            }
            expect(ps, TOK_RPAREN, "')' to close __builtin_offsetof");
            e = new_expr(EXPR_NUM, line, 0);
            e->num = off;
            e->ty = ty_base(TY_LONG, 1);   /* size_t */
            return e;
        }
        reject_reserved(ps, t->text, t->line, t->col);
        e = new_expr(EXPR_VAR, t->line, t->col);
        e->name = t->text;
        advance(ps);
        return e;
    }
    default:
        parse_error_at(ps, t->line, t->col, "expected an expression, got %s",
                   tok_describe(t));
        return NULL;
    }
}

static struct expr *incdec(struct parser *ps, struct expr *target,
                           int line, int is_post, int delta)
{
    (void)ps;
    struct expr *e = new_expr(EXPR_INCDEC, line, 0);
    e->lhs = target;
    e->is_post = is_post;
    e->delta = delta;
    return e;
}

/* Applies postfix operators (call, [], ., ->, ++/--) to an already-parsed
 * primary/compound-literal seed. Split out so a compound literal can take
 * postfix too: `(struct P){...}.x`, `(int[]){1,2,3}[0]`. */
static struct expr *parse_postfix_ops(struct parser *ps, struct expr *e)
{
    for (;;) {
        if (cur(ps)->kind == TOK_LPAREN) {
            /* a call — through a name or any pointer-valued expression */
            int line = cur(ps)->line;
            advance(ps);
            struct expr *call = new_expr(EXPR_CALL, line, e->col);
            call->lhs = e;
            if (e->kind == EXPR_VAR)
                call->name = e->name;
            if (cur(ps)->kind != TOK_RPAREN) {
                for (;;) {
                    if (call->nargs >= MAX_PARAMS)
                        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                                   "more than %d call arguments",
                                   MAX_PARAMS);
                    call->args[call->nargs++] = parse_expr(ps);
                    if (cur(ps)->kind != TOK_COMMA)
                        break;
                    advance(ps);
                }
            }
            expect(ps, TOK_RPAREN, "')'");
            e = call;
        } else if (cur(ps)->kind == TOK_LBRACKET) {
            /* p[i] is sugar for *(p + i); the scaling by the pointee
             * size happens in irgen off the types. */
            int line = cur(ps)->line;
            advance(ps);
            struct expr *idx = parse_expr(ps);
            expect(ps, TOK_RBRACKET, "']'");
            struct expr *d = new_expr(EXPR_DEREF, line, e->col);
            d->rhs = binop(B_ADD, e, idx);
            e = d;
        } else if (cur(ps)->kind == TOK_DOT ||
                   cur(ps)->kind == TOK_ARROW) {
            int is_arrow = cur(ps)->kind == TOK_ARROW;
            int line = cur(ps)->line;
            advance(ps);
            if (cur(ps)->kind != TOK_IDENT)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a member name before %s",
                           tok_describe(cur(ps)));
            struct expr *m = new_expr(EXPR_MEMBER, line, e->col);
            m->lhs = e;
            m->name = cur(ps)->text;
            m->is_arrow = is_arrow;
            advance(ps);
            e = m;
        } else if (cur(ps)->kind == TOK_PLUSPLUS ||
                   cur(ps)->kind == TOK_MINUSMINUS) {
            int delta = cur(ps)->kind == TOK_PLUSPLUS ? 1 : -1;
            int line = cur(ps)->line;
            advance(ps);
            e = incdec(ps, e, line, 1, delta);
        } else {
            return e;
        }
    }
}

static struct expr *parse_postfix(struct parser *ps)
{
    return parse_postfix_ops(ps, parse_primary(ps));
}

static struct expr *parse_unary(struct parser *ps)
{
    struct token *t = cur(ps);
    struct expr *e;

    switch (t->kind) {
    case TOK_BANG:
        e = new_expr(EXPR_NOT, t->line, t->col);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_PLUS:
        /* unary plus: identity on an arithmetic operand (the surrounding
         * context applies the usual promotions). Just yield the operand. */
        advance(ps);
        return parse_unary(ps);
    case TOK_MINUS:
        e = new_expr(EXPR_NEG, t->line, t->col);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_TILDE:
        e = new_expr(EXPR_BNOT, t->line, t->col);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_STAR:
        e = new_expr(EXPR_DEREF, t->line, t->col);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_AMP:
        e = new_expr(EXPR_ADDR, t->line, t->col);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_ANDAND:
        /* GNU computed-goto label address: `&&label` yields a void* to that
         * label's code location, the target of a later `goto *expr`. */
        advance(ps);
        if (cur(ps)->kind != TOK_IDENT)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                    "expected a label name after '&&'");
        e = new_expr(EXPR_LABELADDR, t->line, t->col);
        e->name = cur(ps)->text;
        advance(ps);
        return e;
    case TOK_PLUSPLUS:
    case TOK_MINUSMINUS: {
        int delta = t->kind == TOK_PLUSPLUS ? 1 : -1;
        int line = t->line;
        advance(ps);
        return incdec(ps, parse_unary(ps), line, 0, delta);
    }
    case TOK_KW_REAL:
    case TOK_KW_IMAG: {
        /* GNU __real__ x / __imag__ x: a complex's parts (lvalues when x is
         * one); of a real x, x itself and 0 */
        int line = t->line;
        int imag = t->kind == TOK_KW_IMAG;
        advance(ps);
        e = new_expr(imag ? EXPR_IMAG : EXPR_REAL, line, 0);
        e->rhs = parse_unary(ps);
        return e;
    }
    case TOK_KW_SIZEOF: {
        int line = t->line;
        advance(ps);
        e = new_expr(EXPR_SIZEOF, line, 0);
        if (cur(ps)->kind == TOK_LPAREN) {
            struct lexer save = ps->lx;
            advance(ps);
            if (at_type_start(ps)) {
                e->cast_ty = parse_type_name(ps, parse_type_spec(ps, 0));
                expect(ps, TOK_RPAREN, "')'");
                return e;
            }
            ps->lx = save; /* sizeof (expr) */
        }
        e->rhs = parse_unary(ps);
        return e;
    }
    case TOK_KW_ALIGNOF: {
        /* _Alignof(type) — a type-name in parens (C11). The GNU __alignof__
         * also accepts an expression, so fall back to that like sizeof. */
        int line = t->line;
        advance(ps);
        e = new_expr(EXPR_ALIGNOF, line, 0);
        if (cur(ps)->kind == TOK_LPAREN) {
            struct lexer save = ps->lx;
            advance(ps);
            if (at_type_start(ps)) {
                e->cast_ty = parse_type_name(ps, parse_type_spec(ps, 0));
                expect(ps, TOK_RPAREN, "')'");
                return e;
            }
            ps->lx = save;
        }
        e->rhs = parse_unary(ps);
        return e;
    }
    case TOK_LPAREN: {
        /* a cast, or a parenthesized expression — peek one token */
        struct lexer save = ps->lx;
        advance(ps);
        if (at_type_start(ps)) {
            struct type *ct = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_RPAREN, "')'");
            /* `(type){ ... }` is a compound literal (an unnamed object),
             * not a cast — it can even take postfix operators. */
            if (cur(ps)->kind == TOK_LBRACE) {
                e = new_expr(EXPR_COMPLIT, t->line, t->col);
                e->cast_ty = ct;
                e->lhs = parse_initializer(ps);
                return parse_postfix_ops(ps, e);
            }
            e = new_expr(EXPR_CAST, t->line, t->col);
            e->cast_ty = ct;
            e->rhs = parse_unary(ps);
            return e;
        }
        ps->lx = save;
        return parse_postfix(ps);
    }
    default:
        return parse_postfix(ps);
    }
}

static struct expr *binop(enum binop op, struct expr *lhs, struct expr *rhs)
{
    struct expr *e = new_expr(EXPR_BINOP, lhs->line, lhs->col);
    e->op = op;
    e->lhs = lhs;
    e->rhs = rhs;
    return e;
}

/* One binary precedence level: while the current token maps to an op in
 * the table, consume it and parse the next-tighter level. */
struct oplevel {
    enum tok_kind tok;
    enum binop op;
};

static struct expr *parse_level(struct parser *ps,
                                const struct oplevel *ops, int nops,
                                struct expr *(*tighter)(struct parser *))
{
    struct expr *e = tighter(ps);
    for (;;) {
        int i;
        for (i = 0; i < nops; i++)
            if (cur(ps)->kind == ops[i].tok)
                break;
        if (i == nops)
            return e;
        advance(ps);
        e = binop(ops[i].op, e, tighter(ps));
    }
}

#define LEVEL(name, tighter, ...)                                        \
    static struct expr *name(struct parser *ps)                          \
    {                                                                    \
        static const struct oplevel ops[] = { __VA_ARGS__ };             \
        return parse_level(ps, ops,                                      \
                           (int)(sizeof ops / sizeof ops[0]), tighter);  \
    }

/* C's precedence ladder, loosest at the bottom. */
LEVEL(parse_mul, parse_unary, { TOK_STAR, B_MUL }, { TOK_SLASH, B_DIV },
      { TOK_PERCENT, B_MOD })
LEVEL(parse_add, parse_mul, { TOK_PLUS, B_ADD }, { TOK_MINUS, B_SUB })
LEVEL(parse_shift, parse_add, { TOK_SHL, B_SHL }, { TOK_SHR, B_SHR })
LEVEL(parse_rel, parse_shift, { TOK_LT, B_LT }, { TOK_LE, B_LE },
      { TOK_GT, B_GT }, { TOK_GE, B_GE })
LEVEL(parse_eq, parse_rel, { TOK_EQEQ, B_EQ }, { TOK_NEQ, B_NE })
LEVEL(parse_band, parse_eq, { TOK_AMP, B_AND })
LEVEL(parse_bxor, parse_band, { TOK_CARET, B_XOR })
LEVEL(parse_bor, parse_bxor, { TOK_PIPE, B_OR })
LEVEL(parse_land, parse_bor, { TOK_ANDAND, B_LAND })
LEVEL(parse_lor, parse_land, { TOK_OROR, B_LOR })

/* Assignment is right-associative; the target must be a variable or a
 * dereference. Compound forms desugar to 'a = a op (b)' and stay
 * variable-only: through a pointer the desugaring would evaluate the
 * address twice, which is observable once addresses have side effects. */
static const struct {
    enum tok_kind tok;
    enum binop op;
} compound_assign[] = {
    { TOK_PLUSEQ, B_ADD },   { TOK_MINUSEQ, B_SUB },
    { TOK_STAREQ, B_MUL },   { TOK_SLASHEQ, B_DIV },
    { TOK_PERCENTEQ, B_MOD },{ TOK_AMPEQ, B_AND },
    { TOK_PIPEEQ, B_OR },    { TOK_CARETEQ, B_XOR },
    { TOK_SHLEQ, B_SHL },    { TOK_SHREQ, B_SHR },
};

/* Full expressions (statements, parens, conditions) allow the comma
 * operator; argument lists and initializers use parse_expr, where a
 * comma separates. */
static struct expr *parse_comma(struct parser *ps)
{
    struct expr *e = parse_expr(ps);
    while (cur(ps)->kind == TOK_COMMA) {
        struct expr *c = new_expr(EXPR_COMMA, cur(ps)->line, cur(ps)->col);
        advance(ps);
        c->lhs = e;
        c->rhs = parse_expr(ps);
        e = c;
    }
    return e;
}

static struct expr *parse_cond(struct parser *ps)
{
    struct expr *e = parse_lor(ps);
    if (cur(ps)->kind != TOK_QUESTION)
        return e;
    struct expr *r = new_expr(EXPR_COND, cur(ps)->line, cur(ps)->col);
    advance(ps);
    r->args[0] = e;
    r->nargs = 1;
    r->lhs = parse_comma(ps); /* the then-branch is a FULL expression */
    expect(ps, TOK_COLON, "':'");
    r->rhs = parse_cond(ps); /* right-associative, as in C */
    return r;
}

static struct expr *parse_expr(struct parser *ps)
{
    struct expr *e = parse_cond(ps);
    enum tok_kind k = cur(ps)->kind;
    int line = cur(ps)->line;

    int comp = -1;
    for (size_t i = 0;
         i < sizeof compound_assign / sizeof compound_assign[0]; i++)
        if (k == compound_assign[i].tok)
            comp = (int)i;

    if (k != TOK_ASSIGN && comp < 0)
        return e;
    if (e->kind != EXPR_VAR && e->kind != EXPR_DEREF &&
        e->kind != EXPR_MEMBER && e->kind != EXPR_REAL &&
        e->kind != EXPR_IMAG)
        parse_error_line(ps, line,
                   "assignment target must be a variable, *pointer, or "
                   "member");
    advance(ps);

    if (comp < 0) {
        struct expr *a = new_expr(EXPR_ASSIGN, line, e->col);
        a->lhs = e;
        a->rhs = parse_expr(ps);
        return a;
    }
    /* `x op= y` keeps its own node rather than desugaring to
     * `x = x op y`: through a pointer or a member the address must be
     * evaluated ONCE, and the desugared form evaluates it twice. */
    struct expr *a = new_expr(EXPR_COMPOUND, line, e->col);
    a->op = compound_assign[comp].op;
    a->lhs = e;
    a->rhs = parse_expr(ps);
    return a;
}

/* ---- statements ---- */

static struct stmt *new_stmt(enum stmt_kind kind, int line, int col)
{
    struct stmt *s = xcalloc(1, sizeof *s);
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

static struct stmt *parse_stmt(struct parser *ps, int allow_decl);

/* The element count an initializer list implies for an unsized array:
 * the highest index reached, where a `[i] =` designator repositions the
 * running index and each element then advances it by one. */
int initlist_array_count(const struct expr *il)
{
    int idx = 0, max = 0;
    for (int i = 0; i < il->nelems; i++) {
        if (il->elems[i]->desig_index >= 0)
            idx = il->elems[i]->desig_index;
        if (il->elems[i]->desig_index_hi >= 0)  /* `[lo ... hi]` ends at hi */
            idx = il->elems[i]->desig_index_hi;
        idx++;
        if (idx > max)
            max = idx;
    }
    return max;
}

/* An initializer: either an ordinary expression or a brace list, which
 * may nest. Sema matches it against the target type. */
static struct expr *parse_initializer(struct parser *ps)
{
    if (cur(ps)->kind != TOK_LBRACE)
        return parse_expr(ps);

    struct expr *e = new_expr(EXPR_INITLIST, cur(ps)->line, cur(ps)->col);
    int cap = 0;
    advance(ps);
    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind == TOK_EOF)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "unterminated initializer");
        /* A designator: struct field `.name =` or array element `[i] =`.
         * One level only (no `[i].f =` chains — no EmbCC source needs it). */
        const char *field = NULL;
        long index = -1, index_hi = -1;
        if (cur(ps)->kind == TOK_LBRACKET) {
            int iline = cur(ps)->line;
            advance(ps);
            struct expr *ie = parse_cond(ps);
            if (!size_fold(ie, &index) || index < 0)
                parse_error_line(ps, iline,
                           "an array designator [index] must be a constant "
                           ">= 0");
            if (cur(ps)->kind == TOK_ELLIPSIS) {  /* GNU range `[lo ... hi]` */
                advance(ps);
                struct expr *he = parse_cond(ps);
                if (!size_fold(he, &index_hi) || index_hi < index)
                    parse_error_line(ps, iline,
                               "an array range [lo ... hi] must have "
                               "constant hi >= lo");
            }
            expect(ps, TOK_RBRACKET, "']'");
            expect(ps, TOK_ASSIGN, "'=' after an array designator");
        } else if (cur(ps)->kind == TOK_DOT) {
            advance(ps);
            if (cur(ps)->kind != TOK_IDENT)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a field name after '.'");
            field = cur(ps)->text;
            advance(ps);
            expect(ps, TOK_ASSIGN, "'=' after a field designator");
        }
        if (e->nelems == cap) {
            cap = cap ? cap * 2 : 8;
            e->elems = xrealloc(e->elems,
                                (size_t)cap * sizeof *e->elems);
        }
        struct expr *el = parse_initializer(ps);
        el->desig_field = field;
        el->desig_index = index >= 0 ? (int)index : -1;
        el->desig_index_hi = (int)index_hi;
        e->elems[e->nelems++] = el;
        if (cur(ps)->kind != TOK_COMMA)
            break;
        advance(ps); /* a trailing comma before '}' is legal C */
    }
    expect(ps, TOK_RBRACE, "'}'");
    return e;
}

/* The statement controlled by if/while/for: C99 does not allow a bare
 * declaration there, and neither do we — that keeps the subset strict. */
static struct stmt *parse_controlled(struct parser *ps)
{
    return parse_stmt(ps, 0);
}

static struct stmt *parse_block(struct parser *ps)
{
    struct stmt *s = new_stmt(STMT_BLOCK, cur(ps)->line, cur(ps)->col);
    expect(ps, TOK_LBRACE, "'{'");
    struct stmt **volatile tail = &s->body;
    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind == TOK_EOF) {
            /* Reported once: recovery cannot make more tokens, so the
             * block ends here whatever else was expected. */
            diag_error_at(ps->lx.file, cur(ps)->line, cur(ps)->col,
                          "unexpected end of file inside a block");
            ps->nerrors++;
            return s;
        }
        jmp_buf jb, *save = ps->recover;
        *tail = NULL;
        ps->recover = &jb;
        if (!setjmp(jb)) {
            *tail = parse_stmt(ps, 1);
            while (*tail) /* a declaration may be a chain: int a, b; */
                tail = &(*tail)->next;
        } else {
            *tail = NULL;          /* half a statement is no statement */
            resync(ps, 0);
        }
        ps->recover = save;
    }
    advance(ps); /* '}' */
    return s;
}

/* A string literal (a run of adjacent ones concatenated), as a plain
 * NUL-terminated C string — for asm templates, constraints, and clobbers. */
static char *parse_str_literal(struct parser *ps, const char *what)
{
    if (cur(ps)->kind != TOK_STR)
        parse_error_at(ps, cur(ps)->line, cur(ps)->col, "expected %s", what);
    struct token *t = cur(ps);
    size_t len = (size_t)t->num;            /* includes the NUL */
    char *bytes = xmalloc(len);
    memcpy(bytes, t->text, len);
    advance(ps);
    while (cur(ps)->kind == TOK_STR) {
        size_t add = (size_t)cur(ps)->num;
        char *nb = xmalloc(len - 1 + add);
        memcpy(nb, bytes, len - 1);
        memcpy(nb + len - 1, cur(ps)->text, add);
        bytes = nb;
        len = len - 1 + add;
        advance(ps);
    }
    return bytes;
}

/* A ':'-delimited operand list of `"constraint"(expr)` items, empty when the
 * next token is ':' or ')'. */
static void parse_asm_operands(struct parser *ps, struct asm_operand **out,
                               int *nout)
{
    int cap = 0;
    while (cur(ps)->kind != TOK_COLON && cur(ps)->kind != TOK_RPAREN) {
        if (*nout == cap) {
            cap = cap ? cap * 2 : 4;
            *out = xrealloc(*out, (size_t)cap * sizeof **out);
        }
        struct asm_operand *op = &(*out)[(*nout)++];
        op->name = NULL;
        if (cur(ps)->kind == TOK_LBRACKET) {  /* a `[name]` symbolic operand */
            advance(ps);
            if (cur(ps)->kind != TOK_IDENT)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a name in an asm `[name]` operand");
            op->name = cur(ps)->text;
            advance(ps);
            expect(ps, TOK_RBRACKET, "']' after an asm operand name");
        }
        op->constraint = parse_str_literal(ps, "an asm constraint");
        expect(ps, TOK_LPAREN, "'(' after an asm constraint");
        op->expr = parse_expr(ps);
        expect(ps, TOK_RPAREN, "')' after an asm operand");
        if (cur(ps)->kind != TOK_COMMA)
            break;
        advance(ps);
    }
}

/* GCC extended asm: asm [volatile] ( template
 *     [ : outputs [ : inputs [ : clobbers ] ] ] ) ;  */
static struct stmt *parse_asm_stmt(struct parser *ps)
{
    struct stmt *s = new_stmt(STMT_ASM, cur(ps)->line, cur(ps)->col);
    struct asm_stmt *a = xcalloc(1, sizeof *a);
    s->asm_s = a;
    advance(ps); /* 'asm' / '__asm__' */
    if (cur(ps)->kind == TOK_KW_VOLATILE) {
        a->is_volatile = 1;
        advance(ps);
    }
    expect(ps, TOK_LPAREN, "'(' after asm");
    a->tmpl = parse_str_literal(ps, "an asm template string");
    if (cur(ps)->kind == TOK_COLON) {
        advance(ps);
        parse_asm_operands(ps, &a->out, &a->nout);
    }
    if (cur(ps)->kind == TOK_COLON) {
        advance(ps);
        parse_asm_operands(ps, &a->in, &a->nin);
    }
    if (cur(ps)->kind == TOK_COLON) {
        /* clobbers: string literals. Kept (not discarded): a clobbered
         * register holds no live value ACROSS statements — EmbCC spills
         * everything — but WITHIN this asm the template destroys it, so the
         * operand allocator must not place an allocatable "r" operand there
         * (see irgen STMT_ASM). "cc"/"memory" are kept too and simply name
         * no GPR. */
        advance(ps);
        int cap = 0;
        while (cur(ps)->kind == TOK_STR) {
            const char *c = parse_str_literal(ps, "a clobber");
            if (a->nclob == cap) {
                cap = cap ? cap * 2 : 4;
                a->clob = xrealloc(a->clob, (size_t)cap * sizeof *a->clob);
            }
            a->clob[a->nclob++] = c;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')' to close asm");
    expect(ps, TOK_SEMI, "';'");
    return s;
}

/* __builtin_eh_region { body } __builtin_eh_landing (exc, sel, action...)
 * { pad } — EmbCC's own statement, written by the C++ lowering (D-013,
 * CX5): a call in body that throws lands in pad, the exception pointer
 * stored to exc and the selector to sel; actions are catch types
 * (typeinfo objects, or 0 for any) and __eh_cleanup. */
static struct stmt *parse_eh_region(struct parser *ps)
{
    struct token *t = cur(ps);
    struct stmt *s = new_stmt(STMT_EHREGION, t->line, t->col);
    advance(ps);
    if (cur(ps)->kind != TOK_LBRACE)
        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                "expected '{' after __builtin_eh_region");
    s->body = parse_stmt(ps, 1);
    t = cur(ps);
    if (t->kind != TOK_IDENT || strcmp(t->text, "__builtin_eh_landing") != 0)
        parse_error_at(ps, t->line, t->col,
                "expected __builtin_eh_landing after an exception region");
    advance(ps);
    expect(ps, TOK_LPAREN, "'(' after __builtin_eh_landing");
    s->expr = parse_expr(ps);
    expect(ps, TOK_COMMA, "',' after the exception pointer");
    s->cond = parse_expr(ps);
    int cap = 0;
    while (cur(ps)->kind == TOK_COMMA) {
        advance(ps);
        t = cur(ps);
        if (s->neh_acts == cap) {
            cap = cap ? cap * 2 : 4;
            s->eh_acts = xrealloc(s->eh_acts, (size_t)cap * sizeof *s->eh_acts);
        }
        struct eh_act *a = &s->eh_acts[s->neh_acts++];
        memset(a, 0, sizeof *a);
        if (t->kind == TOK_IDENT && strcmp(t->text, "__eh_cleanup") == 0)
            a->cleanup = 1;
        else if (t->kind == TOK_IDENT)
            a->name = t->text;
        else if (!(t->kind == TOK_NUM && t->num == 0))
            parse_error_at(ps, t->line, t->col,
                    "expected a typeinfo object, 0 or __eh_cleanup");
        advance(ps);
    }
    expect(ps, TOK_RPAREN, "')' to close __builtin_eh_landing");
    if (cur(ps)->kind != TOK_LBRACE)
        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                "expected '{' to begin the landing pad");
    s->thn = parse_stmt(ps, 1);
    return s;
}

static struct stmt *parse_stmt(struct parser *ps, int allow_decl)
{
    struct token *t = cur(ps);
    struct stmt *s;

    if (t->kind == TOK_KW_ASM)
        return parse_asm_stmt(ps);
    if (t->kind == TOK_IDENT && strcmp(t->text, "__builtin_eh_region") == 0)
        return parse_eh_region(ps);

    /* A block-scope `_Static_assert`: checked now, emits nothing. */
    if (t->kind == TOK_KW_STATIC_ASSERT) {
        parse_static_assert(ps);
        return new_stmt(STMT_BLOCK, t->line, t->col);
    }

    /* Block-scope `typedef` and `extern`: declarations that emit no code and
     * take no local storage. Handled here at the parser level -- a local
     * typedef registers a type name; a local extern registers the external
     * global/function the reference resolves to. Both are unit-visible (the
     * one flat namespace EmbCC keeps), which admits a superset of C's block
     * scoping -- fine, and the same trade-off tags/typedefs already make. */
    if (t->kind == TOK_KW_TYPEDEF || t->kind == TOK_KW_EXTERN) {
        if (!allow_decl)
            parse_error_at(ps, t->line, t->col,
                       "a declaration cannot be the body of if/while/for; "
                       "wrap it in braces");
        int is_td = t->kind == TOK_KW_TYPEDEF;
        advance(ps);
        struct type *base = parse_type_spec(ps, 1);
        if (!base)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected a type after '%s'", is_td ? "typedef" : "extern");
        /* extern declarators become STMT_DECL nodes with is_extern set; sema
         * registers the unit global/function (appending to those lists at
         * PARSE time would race parse_top's own list tails). typedef registers
         * a name right here (its list is a prepend-list -- no tail to race). */
        struct stmt *ehead = NULL, **etail = &ehead;
        for (;;) {
            if (is_td) {
                const char *tname;
                struct type *tt = parse_declarator(ps, base, &tname);
                if (!tname)
                    parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                               "typedef needs a name, got %s", tok_describe(cur(ps)));
                struct type *prev = find_typedef(ps, tname);
                if (prev && !ty_equal(prev, tt))
                    parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                               "redefinition of typedef '%s'", tname);
                struct typedefent *te = xcalloc(1, sizeof *te);
                te->name = tname; te->ty = tt;
                te->next = ps->typedefs; ps->typedefs = te;
            } else {
                ps->attr_carry_on = 0;
                ps->attr_carry_on = 0;
            struct type *dty = parse_stars(ps, base);
                if (cur(ps)->kind != TOK_IDENT)
                    parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                               "expected a name before %s", tok_describe(cur(ps)));
                const char *dname = cur(ps)->text;
                int dline = cur(ps)->line;
                advance(ps);
                /* a function type is `name(params)`, else it's a variable */
                struct type *ety = cur(ps)->kind == TOK_LPAREN
                                 ? parse_fn_params(ps, dty)
                                 : parse_array_dims(ps, dty);
                struct stmt *sd = new_stmt(STMT_DECL, dline, 0);
                sd->dty = ety; sd->name = dname; sd->is_extern = 1;
                *etail = sd; etail = &sd->next;
            }
            if (cur(ps)->kind == TOK_COMMA) { advance(ps); continue; }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
        if (ehead)
            return ehead;                    /* extern declarations (sema-handled) */
        s = new_stmt(STMT_BLOCK, t->line, t->col);   /* typedef only -> no code */
        return s;
    }

    /* A LEADING `__attribute__((...))` on a block-scope declaration:
     * `__attribute__((unused)) int y;`. Only the trailing spelling was
     * parsed before, so the leading one -- which is what a macro
     * writes, because it has to come before a type it does not know --
     * failed with "expected a statement". The attributes are merged
     * with any trailing ones at the declarator below. */
    struct attrs lead = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, NULL };
    if (t->kind == TOK_KW_ATTRIBUTE) {
        parse_attributes(ps, &lead);
        t = cur(ps);
    }

    /* `register` is otherwise an ignored storage hint, but it carries the
     * `register T x __asm__("r10")` binding EmbCC needs to place an asm 'r'
     * operand — so accept it as a qualifier before the type. */
    int is_register = t->kind == TOK_IDENT &&
                      strcmp(t->text, "register") == 0;
    if (t->kind == TOK_KW_STATIC || t->kind == TOK_KW_THREAD ||
        is_register || at_type_start(ps)) {
        int local_static = 0, local_tls = 0;
        if (is_register)
            advance(ps);
        /* `static __thread` and `__thread static` are the same
         * declaration, so they are consumed in either order rather than
         * one being spelled correctly and the other falling through to
         * "expected a type" -- which is what used to happen, and what
         * took the parser down a path that dereferenced nothing. */
        /* `_Alignas` is a declaration specifier too (C11 §6.7), and the
         * standard lets the specifiers come in any order -- so
         * `_Alignas(16) static char a[1];` is as valid as
         * `static _Alignas(16) char a[1];`. Only the second worked:
         * parse_type_spec consumes an `_Alignas` it meets, but it does
         * not know `static`, so when the alignment came FIRST it ate the
         * alignment, stopped at `static` having seen no type specifier,
         * and returned NULL -- which the caller dereferenced. A crash,
         * from a declaration the standard spells out. */
        while (cur(ps)->kind == TOK_KW_STATIC ||
               cur(ps)->kind == TOK_KW_THREAD ||
               cur(ps)->kind == TOK_KW_ALIGNAS) {
            if (cur(ps)->kind == TOK_KW_ALIGNAS) {
                consume_alignas(ps);       /* accumulates ps->alignas_out */
                continue;
            }
            if (cur(ps)->kind == TOK_KW_STATIC)
                local_static = 1;
            else
                local_tls = 1;
            advance(ps);
        }
        while (cur(ps)->kind == TOK_KW_INLINE ||   /* accepted, ignored */
               cur(ps)->kind == TOK_KW_NORETURN)   /* on a local prototype */
            advance(ps);
        if ((t->kind == TOK_KW_STATIC || t->kind == TOK_KW_THREAD ||
             is_register) && !at_type_start(ps))
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected a type after the storage specifier");
        /* C11 §6.7.1: at block scope _Thread_local must be accompanied
         * by static or extern. Without one it would name an object with
         * automatic storage that is also per-thread, which is two
         * answers to the same question -- an automatic object is
         * already private to the call. */
        if (local_tls && !local_static)
            parse_error_at(ps, t->line, t->col,
                       "a block-scope __thread object must also be "
                       "static: an automatic one is already private to "
                       "the call");
        if (!allow_decl)
            parse_error_at(ps, t->line, t->col,
                       "a declaration cannot be the body of if/while/for "
                       "(C99 forbids it too); wrap it in braces");
        /* allow_body=1: a block-scope struct/union/enum DEFINITION is legal C
         * (`union { double d; uint64_t u; } v;` inside a function). Tags share
         * the one flat tag namespace EmbCC keeps -- fine for the anonymous
         * types real code uses here; a same-named tag in two scopes is the
         * documented limitation, not a miscompile. */
        struct type *base = parse_type_spec(ps, 1);
        /* No type specifier at all. Implicit int is not C99 and guessing
         * one here would compile a declaration nobody wrote; the old
         * behaviour was worse still, since every use below dereferences
         * this. Say what is missing and stop. */
        if (!base)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected a type in this declaration, found %s",
                       tok_describe(cur(ps)));
        /* A declaration with no declarator: `enum { BAND = 64 };` or
         * `struct tag { ... };` inside a function declares only the tag or
         * the enumerators, which parse_type_spec has already registered. */
        if (cur(ps)->kind == TOK_SEMI) {
            advance(ps);
            return new_stmt(STMT_BLOCK, t->line, t->col);   /* does nothing */
        }
        struct stmt *head = NULL, **dtail = &head;
        for (;;) {
            s = new_stmt(STMT_DECL, t->line, t->col);
            const char *dname;
            s->dty = parse_declarator(ps, base, &dname);
            if (s->dty->kind == TY_VOID || s->dty->kind == TY_FUNC)
                parse_error_at(ps, t->line, t->col,
                           "a variable cannot have type %s",
                           ty_name(s->dty));
            if (!dname)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a variable name before %s",
                           tok_describe(cur(ps)));
            s->name = dname;
            s->is_static = local_static;
            s->is_tls = local_tls;
            /* An optional `__asm__("reg")` register binding follows the
             * declarator: `register long r10 __asm__("r10") = a4;`. */
            if (cur(ps)->kind == TOK_KW_ASM) {
                advance(ps);
                expect(ps, TOK_LPAREN, "'(' after __asm__ register binding");
                s->asm_reg = parse_str_literal(ps, "a register name");
                expect(ps, TOK_RPAREN, "')' after the register name");
            }
            /* `T x[16] __attribute__((aligned(16)))` — a trailing attribute
             * on a local declarator; aligned(N) raises the stack slot's
             * alignment (codegen rounds the frame offset). */
            {
                struct attrs lat = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, NULL };
                parse_attributes(ps, &lat);
                if (lat.section)
                    parse_error_line(ps, s->line,
                               "section attribute on block-scope '%s' is "
                               "not supported — declare it at file scope",
                               dname);
                if (lead.aligned > lat.aligned)
                    lat.aligned = lead.aligned;
                s->user_align = lat.aligned > ps->alignas_out
                                ? lat.aligned : ps->alignas_out;
                s->attr_unused = lat.unused || lead.unused;
                ps->alignas_out = 0;
            }
            int was_array = s->dty->kind == TY_ARRAY;
            (void)was_array;
            if (cur(ps)->kind == TOK_ASSIGN) {
                advance(ps);
                s->expr = parse_initializer(ps);
            }
            /* Record this local's type for a later `sizeof(name)`, inferring
             * an omitted array size from a string or brace initializer (as
             * sema does for codegen) so the size is available now. */
            {
                struct type *ft = s->dty;
                if (ft->kind == TY_ARRAY && ft->count == 0 && s->expr) {
                    if (s->expr->kind == EXPR_STR &&
                        ft->pointee->kind == TY_CHAR)
                        ft = ty_array(ft->pointee, (int)s->expr->num);
                    else if (s->expr->kind == EXPR_INITLIST)
                        ft = ty_array(ft->pointee,
                                      initlist_array_count(s->expr));
                }
                fold_local_add(dname, ft);
            }
            /* checked AFTER the initializer, because `char a[] = "..."`
             * takes its size from the literal */
            /* an omitted array size is filled in by sema from the
             * initializer, so only an UNINITIALIZED one is incomplete */
            if (ty_size(s->dty) == 0 && !s->expr && !ty_is_vla(s->dty)) {
                diag_error_at(ps->lx.file, s->line, 0,
                              "'%s' has incomplete type %s", s->name,
                              ty_name(s->dty));
                diag_set_id("E0005");
                ps->nerrors++;
                if (ps->recover)
                    longjmp(*ps->recover, 1);
                fatal_unwind();
            }
            *dtail = s;
            dtail = &s->next;
            if (cur(ps)->kind == TOK_COMMA) {
                advance(ps);
                continue;
            }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
        return head;
    }

    /* A label: `IDENT ':'` prefixes a statement (a goto target). Peek one
     * token past the identifier; if it is ':' this is a label, else restore
     * and fall through to the expression-statement path. */
    if (t->kind == TOK_IDENT) {
        const char *lname = t->text;
        int lline = t->line;
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind == TOK_COLON) {
            advance(ps);                       /* consume ':' */
            s = new_stmt(STMT_LABEL, lline, 0);
            s->name = lname;
            s->body = parse_stmt(ps, allow_decl);
            return s;
        }
        ps->lx = save;                         /* not a label */
    }

    switch (t->kind) {
    case TOK_SEMI:                             /* the null statement */
        s = new_stmt(STMT_BLOCK, t->line, t->col);     /* an empty block does nothing */
        advance(ps);
        return s;
    case TOK_KW_GOTO:
        s = new_stmt(STMT_GOTO, t->line, t->col);
        advance(ps);
        if (cur(ps)->kind == TOK_STAR) {
            /* GNU computed goto: `goto *expr` jumps to the label address in
             * expr (a void*). s->expr set, s->name NULL marks the indirect form. */
            advance(ps);
            s->expr = parse_expr(ps);
            expect(ps, TOK_SEMI, "';'");
            return s;
        }
        if (cur(ps)->kind != TOK_IDENT)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected a label name after 'goto'");
        s->name = cur(ps)->text;
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_LBRACE:
        return parse_block(ps);
    case TOK_KW_RETURN:
        s = new_stmt(STMT_RETURN, t->line, t->col);
        advance(ps);
        if (cur(ps)->kind != TOK_SEMI)
            s->expr = parse_expr(ps); /* NULL = bare return (void) */
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_IF:
        s = new_stmt(STMT_IF, t->line, t->col);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_comma(ps);       /* an expression: commas too */
        expect(ps, TOK_RPAREN, "')'");
        s->thn = parse_controlled(ps);
        if (cur(ps)->kind == TOK_KW_ELSE) {
            advance(ps);
            s->els = parse_controlled(ps);
        }
        return s;
    case TOK_KW_WHILE:
        s = new_stmt(STMT_WHILE, t->line, t->col);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_comma(ps);       /* an expression: commas too */
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_FOR:
        s = new_stmt(STMT_FOR, t->line, t->col);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        if (at_type_start(ps)) {
            /* `for (int i = 0; ...)`. One flat scope per function means
             * the variable simply outlives the loop — which ACCEPTS
             * fewer programs than C (a second loop reusing the name is
             * refused), never more. */
            s->initdecl = parse_stmt(ps, 1); /* consumes its own ';' */
        } else {
            if (cur(ps)->kind != TOK_SEMI)
                s->init = parse_comma(ps);
            expect(ps, TOK_SEMI, "';'");
        }
        if (cur(ps)->kind != TOK_SEMI) /* NULL cond = forever; break exits */
            s->cond = parse_comma(ps);
        expect(ps, TOK_SEMI, "';'");
        if (cur(ps)->kind != TOK_RPAREN)
            s->step = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_DO:
        s = new_stmt(STMT_DO, t->line, t->col);
        advance(ps);
        s->body = parse_controlled(ps);
        if (cur(ps)->kind != TOK_KW_WHILE)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected 'while' after a do-body, got %s",
                       tok_describe(cur(ps)));
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_SWITCH:
        s = new_stmt(STMT_SWITCH, t->line, t->col);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_CASE:
        /* a position MARKER in the switch body's list, not a wrapper —
         * C's fallthrough is what forces that shape */
        s = new_stmt(STMT_CASE, t->line, t->col);
        advance(ps);
        s->expr = parse_cond(ps); /* folded to a constant by sema */
        expect(ps, TOK_COLON, "':'");
        return s;
    case TOK_KW_DEFAULT:
        s = new_stmt(STMT_DEFAULT, t->line, t->col);
        advance(ps);
        expect(ps, TOK_COLON, "':'");
        return s;
    case TOK_KW_BREAK:
        s = new_stmt(STMT_BREAK, t->line, t->col);
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_CONTINUE:
        s = new_stmt(STMT_CONTINUE, t->line, t->col);
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_IDENT:
    case TOK_NUM:
    case TOK_FNUM:
    case TOK_LPAREN:
    case TOK_BANG:
    case TOK_MINUS:
    case TOK_TILDE:
    case TOK_STAR:  /* *p = ...; */
    case TOK_AMP:
    case TOK_PLUSPLUS:
    case TOK_MINUSMINUS:
    case TOK_KW_REAL:   /* __real__ z = ...; */
    case TOK_KW_IMAG:
        /* expression statement: assignment, call, or ++/-- */
        if (t->kind == TOK_IDENT)
            reject_reserved(ps, t->text, t->line, t->col);
        s = new_stmt(STMT_EXPR, t->line, t->col);
        s->expr = parse_comma(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    default:
        parse_error_at(ps, t->line, t->col,
                   "expected a statement, got %s", tok_describe(t));
        return NULL;
    }
}

/* ---- top level: functions and globals ---- */

/* A file-scope variable, after the declarator name has been consumed. */
static struct global *parse_global(struct parser *ps, struct type *ty,
                                   const char *name, int line,
                                   int is_static, int is_extern)
{
    struct global *g = xcalloc(1, sizeof *g);
    g->name = name;
    g->file = ps->lx.file;
    g->line = line;
    g->is_static = is_static;
    g->is_extern = is_extern;
    g->ty = ty;

    if (g->ty->kind == TY_VOID)
        parse_error_line(ps, line, "a variable cannot have type void");
    g->ty = parse_array_dims(ps, g->ty);
    int has_init = cur(ps)->kind == TOK_ASSIGN;
    /* `extern T x[];` is legal: the definition, and the size, live in
     * another translation unit. An omitted array size is legal when an
     * initializer follows — it supplies the count — so defer that check. */
    int size_from_init = g->ty->kind == TY_ARRAY && g->ty->count == 0 &&
                         has_init;
    if (ty_size(g->ty) == 0 && !is_extern && !size_from_init)
        parse_error_line(ps, line,
                   "'%s' has incomplete type %s", name, ty_name(g->ty));
    if (has_init) {
        advance(ps);
        if (is_extern)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "'extern' with an initializer");
        if (g->ty->kind == TY_ARRAY || g->ty->kind == TY_STRUCT) {
            /* Aggregates (and any string/relocation content) are lowered
             * by sema: it flattens the initializer against the type and
             * const-folds each leaf into the object's byte image. */
            g->init_expr = parse_initializer(ps);
            if (g->ty->kind == TY_ARRAY && g->ty->count == 0) {
                struct expr *ie = g->init_expr;
                if (ie->kind == EXPR_STR &&
                    ty_is_integer(g->ty->pointee) &&
                    ty_size(g->ty->pointee) == (ie->str_width ? ie->str_width : 1))
                    g->ty = ty_array(g->ty->pointee, (int)ie->num);
                else if (ie->kind == EXPR_INITLIST)
                    g->ty = ty_array(g->ty->pointee,
                                     initlist_array_count(ie));
                else
                    parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                               "'%s' needs a brace or string initializer "
                               "to supply its size", name);
            }
            g->has_init = 1;
        } else {
            /* A scalar/pointer global: a constant expression — an integer
             * literal, sizeof arithmetic, a string-literal address for a
             * pointer, 0 for a null pointer. sema flattens and folds it
             * through the same path as an aggregate leaf. */
            g->init_expr = parse_cond(ps);
            g->has_init = 1;
        }
    }
    return g; /* caller handles ',' and ';' */
}

/* Parses one top-level item into the unit: a function (prototype or
 * definition) or a global variable. */
/* `_Static_assert ( constant-expression , "message" ) ;` — evaluated now,
 * at parse time (like an array size). A false assertion is a fatal error
 * naming the message; a true one produces nothing. The message is optional
 * (C23 relaxed C11's requirement), which real headers rely on. Legal at
 * file scope, in a block, and in a struct/union body. */
static void parse_static_assert(struct parser *ps)
{
    int line = cur(ps)->line;
    advance(ps); /* _Static_assert */
    expect(ps, TOK_LPAREN, "'(' after _Static_assert");
    struct expr *ce = parse_cond(ps);
    long v;
    if (!size_fold(ce, &v))
        parse_error_line(ps, line,
                   "_Static_assert needs a constant integer expression");
    const char *msg = NULL;
    if (cur(ps)->kind == TOK_COMMA) {
        advance(ps);
        msg = parse_str_literal(ps, "a _Static_assert message string");
    }
    expect(ps, TOK_RPAREN, "')' to close _Static_assert");
    expect(ps, TOK_SEMI, "';'");
    if (v == 0)
        parse_error_line(ps, line, "static assertion failed: %s",
                   msg ? msg : "(no message)");
}

static void parse_top(struct parser *ps, struct unit *u,
                      struct func ***ftail, struct global ***gtail,
                      int seq)
{
    g_nfold_locals = 0;   /* a fresh local scope for sizeof(var) folding */
    if (cur(ps)->kind == TOK_KW_STATIC_ASSERT) {
        parse_static_assert(ps);
        return;
    }

    int is_static = 0, is_extern = 0, is_tls = 0;
    struct attrs at = { 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 0, NULL };
    ps->seq = seq;

    /* A file-scope `__asm__("...")` block (crt0's _start stub). Basic asm
     * only — a template, no operands — assembled later by topasm.c. */
    if (cur(ps)->kind == TOK_KW_ASM) {
        int line = cur(ps)->line;
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after a file-scope asm");
        struct topasm *ta = xcalloc(1, sizeof *ta);
        ta->tmpl = parse_str_literal(ps, "an asm template string");
        ta->file = ps->lx.file;
        ta->line = line;
        expect(ps, TOK_RPAREN, "')' to close the asm block");
        expect(ps, TOK_SEMI, "';'");
        struct topasm **t = &u->topasm;
        while (*t)
            t = &(*t)->next;
        *t = ta;
        return;
    }

    /* Storage/function specifiers in any order; `inline` is accepted and
     * ignored — EmbCC emits an inline function as an ordinary one. */
    for (;;) {
        if (cur(ps)->kind == TOK_KW_STATIC) {
            is_static = 1;
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_EXTERN) {
            is_extern = 1;
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_INLINE) {
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_NORETURN) {
            /* C11 §6.7.4: _Noreturn is a function specifier, and means
             * exactly what __attribute__((noreturn)) means — so it lands in
             * the same field and the flow analysis cannot tell them apart. */
            at.noreturn = 1;
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_THREAD) {
            /* A storage class, not a qualifier: it changes which SECTION
             * the object lands in and how its address is formed, not its
             * type. `static __thread` and `extern __thread` are both
             * legal and mean what they say. */
            is_tls = 1;
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_ATTRIBUTE) {
            parse_attributes(ps, &at); /* leading __attribute__((weak)) etc. */
        } else {
            break;
        }
    }
    if (cur(ps)->kind == TOK_KW_TYPEDEF) {
        if (is_static || is_extern)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "typedef cannot be static or extern");
        advance(ps);
        struct type *tbase = parse_type_spec(ps, 1);
        if (!tbase)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected a type after 'typedef'");
        for (;;) {
            const char *tname;
            struct type *tt = parse_declarator(ps, tbase, &tname);
            if (!tname)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "typedef needs a name, got %s",
                           tok_describe(cur(ps)));
            struct type *prev = find_typedef(ps, tname);
            if (prev && !ty_equal(prev, tt))
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "redefinition of typedef '%s'", tname);
            /* identical redefinition: headers do it; harmless */
            struct typedefent *te = xcalloc(1, sizeof *te);
            te->name = tname;
            te->ty = tt;
            te->next = ps->typedefs;
            ps->typedefs = te;
            if (cur(ps)->kind == TOK_COMMA) {
                advance(ps);
                continue;
            }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
        return;
    }
    if (cur(ps)->kind == TOK_IDENT)
        reject_reserved(ps, cur(ps)->text, cur(ps)->line, cur(ps)->col);
    struct type *base = parse_type_spec(ps, 1);
    if (!base)
        parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                   "expected a type before %s", tok_describe(cur(ps)));
    ps->alignas_out = 0;   /* a top-level object carries no over-alignment slot;
                            * drop any _Alignas so it can't leak into the next decl */
    if (cur(ps)->kind == TOK_SEMI) {
        /* bare declaration: 'struct X { ... };', 'enum { ... };' */
        advance(ps);
        return;
    }
    /* GCC lets an attribute sit between the declaration specifiers and
     * the declarator -- `extern int __attribute__((weak)) f(void);` --
     * and that position is where `weak`, `noreturn` and `section`
     * naturally belong, because they are about the entity being
     * declared rather than about a pointer in the middle of its type.
     * Real headers write it this way, so it is read here rather than
     * refused; parse_stars still refuses the ones that reach it after a
     * `*`, where there is nothing to carry them. */
    if (cur(ps)->kind == TOK_KW_ATTRIBUTE)
        parse_attributes(ps, &at);
    /* Open the slot for this declaration's declarators, including the
     * rewind below: an attribute after a `*` belongs to what is being
     * declared. */
    memset(&ps->attr_slot, 0, sizeof ps->attr_slot);
    ps->attr_carry_on = 1;
    struct lexer fork = ps->lx;
    struct type *ty = parse_stars(ps, base);
    take_carried(ps, &at);
    const char *name = NULL;
    int line = cur(ps)->line;
    struct func *f;
    int saved_vla_ok;
    if (cur(ps)->kind == TOK_IDENT) {
        name = cur(ps)->text;
        advance(ps);
    }
    if (!name || cur(ps)->kind != TOK_LPAREN) {
        /* not a function: rewind and parse global declarators */
        ps->lx = fork;
        for (int first = 1;; first = 0) {
            const char *gname;
            int gline = cur(ps)->line;
            struct type *gt = parse_declarator(ps, base, &gname);
            take_carried(ps, &at);
            if (!gname)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a name before %s",
                           tok_describe(cur(ps)));
            if (gt->kind == TY_FUNC && first) {
                /* a function whose declarator is parenthesized: it
                 * returns a pointer to a function or to an array
                 * (`void (*signal(int, void (*)(int)))(int)`) */
                f = xcalloc(1, sizeof *f);
                f->is_static = is_static;
                f->is_weak = at.weak;
                f->is_noreturn = at.noreturn;
    f->fmt_kind = at.fmt_kind;
    f->fmt_idx = at.fmt_idx;
    f->fmt_first = at.fmt_first;
                f->is_nothrow = at.nothrow;
    f->is_ctor = at.ctor;
    f->is_dtor = at.dtor;
    f->attr_used = at.used;
    f->attr_unused = at.unused;
    f->attr_always_inline = at.always_inline;
    f->attr_noinline = at.noinline;
    f->attr_deprecated = at.deprecated;
    f->attr_warn_unused_result = at.warn_unused_result;
    f->vis = at.vis;
                f->is_ctor = at.ctor;
                f->is_dtor = at.dtor;
                f->attr_used = at.used;
                f->attr_unused = at.unused;
                f->attr_always_inline = at.always_inline;
                f->attr_noinline = at.noinline;
                f->attr_deprecated = at.deprecated;
                f->attr_warn_unused_result = at.warn_unused_result;
                f->vis = at.vis;
                f->ret_ty = gt->ret;
                f->name = name = gname;
                f->file = ps->lx.file;
                f->line = line = gline;
                f->seq = seq;
                f->nparams = gt->nptypes;
                for (int i = 0; i < gt->nptypes; i++) {
                    f->param_tys[i] = gt->ptypes[i];
                    f->params[i] = ps->fn_pnames[i];
                    f->param_lines[i] = ps->fn_plines[i];
                    f->param_cols[i] = ps->fn_pcols[i];
                }
                f->is_varargs = gt->is_varargs;
                f->sret_first = gt->sret_first;
                saved_vla_ok = ps->vla_ok;
                ps->vla_ok = 1;
                goto fn_tail;
            }
            if (gt->kind == TY_FUNC)
                parse_error_line(ps, gline,
                           "a variable cannot have a function type — "
                           "did you mean a function pointer (*)?");
            parse_attributes(ps, &at); /* int x __attribute__((weak)) = ... */
            struct global *g = parse_global(ps, gt, gname, gline,
                                            is_static, is_extern);
            parse_attributes(ps, &at); /* trailing: T x[] __attribute__((weak)) */
            g->is_weak = at.weak;
            g->attr_used = at.used;
            g->attr_unused = at.unused;
            g->attr_deprecated = at.deprecated;
            g->user_align = at.aligned > ps->alignas_out
                            ? at.aligned : ps->alignas_out;
            g->vis = at.vis;
            g->is_tls = is_tls;
            g->section = at.section;
            g->seq = seq;
            g->def_seq = seq;
            **gtail = g;
            *gtail = &g->next;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
        expect(ps, TOK_SEMI, "';'");
        (void)u;
        return;
    }

    f = xcalloc(1, sizeof *f);
    /* 'extern' on a function is the default linkage — accept, ignore */
    f->is_static = is_static;
    f->is_weak = at.weak;   /* leading __attribute__((weak)) */
    f->is_noreturn = at.noreturn;
    f->fmt_kind = at.fmt_kind;
    f->fmt_idx = at.fmt_idx;
    f->fmt_first = at.fmt_first;
    f->is_nothrow = at.nothrow;
    f->is_ctor = at.ctor;
    f->is_dtor = at.dtor;
    f->attr_used = at.used;
    f->attr_unused = at.unused;
    f->attr_always_inline = at.always_inline;
    f->attr_noinline = at.noinline;
    f->attr_deprecated = at.deprecated;
    f->attr_warn_unused_result = at.warn_unused_result;
    f->vis = at.vis;
    f->ret_ty = ty;
    f->name = name;
    f->file = ps->lx.file;
    f->line = line;
    f->seq = seq;
    advance(ps); /* '(' */
    saved_vla_ok = ps->vla_ok;
    ps->vla_ok = 1;   /* parameters and body: VLAs allowed */

    if (cur(ps)->kind == TOK_KW_VOID) {
        /* "(void)" means no parameters; "(void *x)" is a parameter */
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind == TOK_RPAREN) {
            /* fall through to the closing paren below */
        } else {
            ps->lx = save;
        }
    }
    if (cur(ps)->kind != TOK_RPAREN) {
        for (;;) {
            if (cur(ps)->kind == TOK_ELLIPSIS) {
                /* (C23, and C++'s f(...): no named parameter before) */
                f->is_varargs = 1;
                advance(ps);
                break;
            }
            if (param_sret_attr(ps, f->nparams))
                f->sret_first = 1;
            if (cur(ps)->kind == TOK_IDENT)
                reject_reserved(ps, cur(ps)->text, cur(ps)->line, cur(ps)->col);
            struct type *spec = parse_type_spec(ps, 0);
            if (!spec)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a parameter type before %s",
                           tok_describe(cur(ps)));
            const char *pname;
            struct type *pt = parse_declarator(ps, spec, &pname);
            if (pt->kind == TY_ARRAY)
                pt = ty_ptr(pt->pointee); /* C's adjustment */
            if (pt->kind == TY_FUNC)
                pt = ty_ptr(pt);
            if (pt->kind == TY_VOID)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "a parameter cannot have type void");
            if (f->nparams >= MAX_PARAMS)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "more than %d parameters", MAX_PARAMS);
            f->param_tys[f->nparams] = pt;
            f->params[f->nparams] = pname; /* NULL fine in prototypes */
            f->param_lines[f->nparams] = pname ? ps->decl_name_line : 0;
            f->param_cols[f->nparams] = pname ? ps->decl_name_col : 0;
            f->nparams++;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')'");

fn_tail:
    /* trailing attributes: void f(void) __attribute__((noreturn/weak)) */
    parse_attributes(ps, &at);
    f->is_weak = at.weak;
    /* Trailing is GCC's usual spelling for these two -- `void die(void)
     * __attribute__((noreturn));` -- and they were parsed here and then
     * dropped, so only the leading form ever reached the func node. */
    f->is_noreturn = at.noreturn;
    f->fmt_kind = at.fmt_kind;
    f->fmt_idx = at.fmt_idx;
    f->fmt_first = at.fmt_first;
    f->is_nothrow = at.nothrow;
    f->is_ctor = at.ctor;
    f->is_dtor = at.dtor;
    f->attr_used = at.used;
    f->attr_unused = at.unused;
    f->attr_always_inline = at.always_inline;
    f->attr_noinline = at.noinline;
    f->attr_deprecated = at.deprecated;
    f->attr_warn_unused_result = at.warn_unused_result;
    f->vis = at.vis;
    if (at.section)
        parse_error_line(ps, line,
                   "section attribute on function '%s' is not supported — "
                   "every function is emitted into .text", name);

    if (cur(ps)->kind == TOK_SEMI) {
        advance(ps); /* prototype */
    } else if (cur(ps)->kind == TOK_COMMA) {
        /* The first declarator was a function PROTOTYPE and more declarators
         * share the same base type: `extern double f(double), g(double), x;`.
         * A comma can only follow a prototype, never a definition. */
        **ftail = f;
        *ftail = &f->next;
        while (cur(ps)->kind == TOK_COMMA) {
            advance(ps);
            struct type *dty = parse_stars(ps, base);
            if (cur(ps)->kind != TOK_IDENT)
                parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                           "expected a name before %s", tok_describe(cur(ps)));
            const char *dname = cur(ps)->text;
            int dline = cur(ps)->line;
            advance(ps);
            if (cur(ps)->kind == TOK_LPAREN) {
                /* a sibling function prototype: `g(double)` */
                struct type *fty = parse_fn_params(ps, dty);
                struct func *g = xcalloc(1, sizeof *g);
                g->is_static = is_static;
                g->ret_ty = fty->ret;
                g->name = dname;
                g->file = ps->lx.file;
                g->line = dline;
                g->seq = seq;
                g->nparams = fty->nptypes;
                for (int i = 0; i < fty->nptypes; i++) {
                    g->param_tys[i] = fty->ptypes[i];
                    g->params[i] = NULL;  /* unnamed prototype parameters */
                }
                g->is_varargs = fty->is_varargs;
                g->sret_first = fty->sret_first;
                **ftail = g;
                *ftail = &g->next;
            } else {
                /* a sibling variable: `x`, `*p`, `a[10]`, with optional init */
                struct type *vty = parse_array_dims(ps, dty);
                parse_attributes(ps, &at);
                struct global *g = parse_global(ps, vty, dname, dline,
                                                is_static, is_extern);
                parse_attributes(ps, &at);
                g->is_weak = at.weak;
                g->attr_used = at.used;
                g->attr_unused = at.unused;
                g->attr_deprecated = at.deprecated;
                g->user_align = at.aligned > ps->alignas_out
                                ? at.aligned : ps->alignas_out;
            g->user_align = at.aligned > ps->alignas_out
                            ? at.aligned : ps->alignas_out;
                g->vis = at.vis;
                g->is_tls = is_tls;
                g->section = at.section;
                g->seq = seq;
                g->def_seq = seq;
                **gtail = g;
                *gtail = &g->next;
            }
        }
        expect(ps, TOK_SEMI, "';'");
        ps->vla_ok = saved_vla_ok;
        (void)u;
        return;
    } else {
        if (cur(ps)->kind != TOK_LBRACE)
            parse_error_at(ps, cur(ps)->line, cur(ps)->col,
                       "expected '{' or ';' before %s",
                       tok_describe(cur(ps)));
        for (int i = 0; i < f->nparams; i++)
            if (!f->params[i])
                parse_error_line(ps, f->line,
                           "parameter %d of '%s' needs a name in a "
                           "definition", i + 1, f->name);
        struct stmt *blk = parse_block(ps);
        f->body = blk->body;
        f->defined = 1;
    }
    ps->vla_ok = saved_vla_ok;
    **ftail = f;
    *ftail = &f->next;
}

/* Syntax errors of the last parse_unit: the driver stops before semantic
 * analysis when there were any, since the tree is not whole. */
static int g_parse_errors;

int parse_error_count(void) { return g_parse_errors; }

struct unit *parse_unit(const char *file, const char *src)
{
    struct parser ps;
    struct unit *u = xcalloc(1, sizeof *u);
    u->file = file;

    ps.unit = u;
    g_fold_unit = u;          /* size_fold resolves this unit's enum constants */
    ps.recover = NULL;        /* no recovery point until a loop sets one */
    ps.nerrors = 0;
    ps.prev_line = 0;
    ps.prev_end_col = 0;
    ps.semi_line = ps.semi_col = -1;
    ps.tags = NULL;
    ps.typedefs = NULL;
    ps.econst_tail = &u->econsts;
    ps.seq = 0;
    ps.alignas_out = 0;
    ps.vla_ok = 0;            /* file scope */
    lex_init(&ps.lx, file, src);
    struct func **ftail = &u->funcs;
    struct global **gtail = &u->globals;
    volatile int seq = 0;      /* written between setjmp and longjmp */
    while (cur(&ps)->kind != TOK_EOF) {
        jmp_buf jb, *save = ps.recover;
        ps.recover = &jb;
        if (!setjmp(jb))
            parse_top(&ps, u, &ftail, &gtail, seq++);
        else
            resync(&ps, 1);
        ps.recover = save;
    }
    g_parse_errors = ps.nerrors;
    u->tags = ps.tags;            /* what a tool needs to answer questions */
    u->typedefs = ps.typedefs;
    /* A translation unit of only data (a table of globals, no functions) is
     * valid C — EmbCC's own predef macro table is exactly that. An entirely
     * empty unit is legal too (a header-only .c, a fully #if'd-out file);
     * it yields a valid, empty object. */
    return u;
}
