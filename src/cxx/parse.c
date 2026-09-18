/* Declarations and statements. C++ is parsed with its semantics: whether a
 * name is a type decides how the tokens after it read, so every declaration
 * is entered into its scope as soon as its declarator is seen, and every
 * name is looked up as it is met.
 *
 * Two things are parsed out of order, by replaying tokens (tok.c keeps the
 * whole unit): a member function defined inside its class, and a default
 * member initializer, are parsed once the outermost enclosing class is
 * complete — they may use members declared after them. */
#include "cxx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

struct cfunc *cx_curfn;
struct cstmt *cx_curblk;
int cx_extern_c;
struct cfunc *cx_funcs;
static struct cfunc *funcs_tail;
struct cvar **cx_gvars;
int cx_ngvars;
static int capgvars;
struct cclass **cx_classes;
int cx_nclasses;
static int capclasses;

void func_register(struct cfunc *f)
{
    f->all_next = NULL;
    if (funcs_tail)
        funcs_tail->all_next = f;
    else
        cx_funcs = f;
    funcs_tail = f;
}

void gvar_register(struct cvar *v)
{
    if (cx_ngvars == capgvars) {
        capgvars = capgvars ? capgvars * 2 : 64;
        cx_gvars = xrealloc(cx_gvars, (size_t)capgvars * sizeof *cx_gvars);
    }
    cx_gvars[cx_ngvars++] = v;
}

static void class_register(struct cclass *c)
{
    if (cx_nclasses == capclasses) {
        capclasses = capclasses ? capclasses * 2 : 64;
        cx_classes = xrealloc(cx_classes,
                              (size_t)capclasses * sizeof *cx_classes);
    }
    cx_classes[cx_nclasses++] = c;
}

struct cstmt *st_new(enum cstmt_kind k)
{
    struct cstmt *s = xcalloc(1, sizeof *s);
    s->k = k;
    s->line = cx_cur()->t.line;
    s->file = cx_cur()->file;
    s->blk = cx_curblk;
    return s;
}

static const char *tok_text(int k)
{
    return cx_peek(k)->t.text;
}

/* ---- attributes ---- */

struct attrs {
    int weak, used, noreturn, packed;
    long aligned;
    const char *section;
    const char *asm_name;
};

static int attr_is(const char *n, const char *w)
{
    size_t l = strlen(w);
    if (strcmp(n, w) == 0)
        return 1;
    return strncmp(n, "__", 2) == 0 && strncmp(n + 2, w, l) == 0 &&
           strcmp(n + 2 + l, "__") == 0;
}

/* GNU __attribute__((...)), C++11 [[...]] and alignas(...), any number of
 * them. The few that change code are recorded; the rest are accepted. */
static void parse_attrs(struct attrs *a)
{
    struct attrs dummy;
    if (!a) {
        memset(&dummy, 0, sizeof dummy);
        a = &dummy;
    }
    for (;;) {
        if (cx_kind() == TOK_KW_ATTRIBUTE) {
            cx_advance();
            cx_expect(TOK_LPAREN, "'(' after __attribute__");
            cx_expect(TOK_LPAREN, "'((' after __attribute__");
            while (cx_kind() != TOK_RPAREN) {
                if (cx_accept(TOK_COMMA))
                    continue;
                const char *n = cx_cur()->t.text ? cx_cur()->t.text : "";
                cx_advance();
                if (cx_kind() == TOK_LPAREN) {
                    if (attr_is(n, "aligned")) {
                        cx_advance();
                        a->aligned = expr_parse_const("an alignment");
                        cx_expect(TOK_RPAREN, "')'");
                    } else if (attr_is(n, "section") &&
                               cx_kind_at(1) == TOK_STR) {
                        cx_advance();
                        a->section = cx_cur()->t.text;
                        cx_advance();
                        cx_expect(TOK_RPAREN, "')'");
                    } else {
                        cx_skip_balanced();
                    }
                    continue;
                }
                if (attr_is(n, "aligned")) a->aligned = 16;
                else if (attr_is(n, "weak")) a->weak = 1;
                else if (attr_is(n, "used")) a->used = 1;
                else if (attr_is(n, "noreturn")) a->noreturn = 1;
                else if (attr_is(n, "packed")) a->packed = 1;
            }
            cx_expect(TOK_RPAREN, "')'");
            cx_expect(TOK_RPAREN, "'))' to close __attribute__");
            continue;
        }
        if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET) {
            cx_advance();
            cx_advance();
            while (!(cx_kind() == TOK_RBRACKET &&
                     cx_kind_at(1) == TOK_RBRACKET)) {
                if (cx_kind() == TOK_EOF)
                    cx_error(cx_cur(), "unterminated [[ attribute");
                if (cx_kind() == TOK_IDENT &&
                    strcmp(cx_cur()->t.text, "noreturn") == 0)
                    a->noreturn = 1;
                if (cx_kind() == TOK_LPAREN)
                    cx_skip_balanced();
                else
                    cx_advance();
            }
            cx_advance();
            cx_advance();
            continue;
        }
        if (cx_kind() == TOK_KW_ALIGNAS) {
            cx_advance();
            cx_expect(TOK_LPAREN, "'(' after alignas");
            if (at_type_start())
                a->aligned = ct_align(parse_type_id());
            else
                a->aligned = expr_parse_const("an alignment");
            cx_expect(TOK_RPAREN, "')'");
            continue;
        }
        break;
    }
}

/* `__asm__("symbol")` after a declarator: the name to use in the object. */
static void parse_asm_label(struct attrs *a)
{
    if (cx_kind() != TOK_KW_ASM || cx_kind_at(1) != TOK_LPAREN)
        return;
    cx_advance();
    cx_advance();
    if (cx_kind() != TOK_STR)
        cx_error(cx_cur(), "expected a string in an asm label");
    a->asm_name = cx_cur()->t.text;
    while (cx_kind() == TOK_STR)
        cx_advance();
    cx_expect(TOK_RPAREN, "')'");
}

/* ---- names ---- */

/* The scope a namespace, class or enum name opens. */
static struct cscope *sym_scope(struct csym *y)
{
    if (!y)
        return NULL;
    switch (y->k) {
    case CS_NAMESPACE:
        return y->ns;
    case CS_CLASS: case CS_TYPEDEF: case CS_ENUM:
        if (y->type->k == CT_CLASS)
            return y->type->cls->scope;
        if (y->type->k == CT_ENUM)
            return y->type->en->scope;
        return NULL;
    default:
        return NULL;
    }
}

struct qname peek_qname(void)
{
    struct qname q = { NULL, 0, 0 };
    int i = 0;
    if (cx_kind_at(0) == TOK_COLONCOLON) {
        q.scope = cx_global;
        i = 1;
    }
    while (cx_kind_at(i) == TOK_IDENT && cx_kind_at(i + 1) == TOK_COLONCOLON) {
        const char *n = tok_text(i);
        struct csym *y = q.scope ? lookup_in(q.scope, n) : lookup(cx_scope, n);
        struct cscope *s = sym_scope(y);
        if (!s) {
            y = q.scope ? scope_find_tag(q.scope, n) : lookup_tag(cx_scope, n);
            s = sym_scope(y);
        }
        if (!s) {
            q.bad = 1;
            q.fin = i;
            return q;
        }
        q.scope = s;
        i += 2;
    }
    q.fin = i;
    return q;
}

static struct csym *find_final(struct cscope *qual, const char *name)
{
    return qual ? lookup_in(qual, name) : lookup(cx_scope, name);
}

static int is_type_sym(const struct csym *y)
{
    return y && (y->k == CS_TYPEDEF || y->k == CS_CLASS || y->k == CS_ENUM);
}

struct cty *peek_type_name(int *ntok)
{
    if (cx_kind() == TOK_IDENT &&
        strcmp(cx_cur()->t.text, "__builtin_va_list") == 0) {
        *ntok = 1;
        return ct_basic(CT_VALIST);
    }
    if (cx_kind() != TOK_IDENT && cx_kind() != TOK_COLONCOLON)
        return NULL;
    struct qname q = peek_qname();
    if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
        return NULL;
    struct csym *y = find_final(q.scope, tok_text(q.fin));
    if (!is_type_sym(y))
        return NULL;
    *ntok = q.fin + 1;
    return y->type;
}

/* At `X(` naming a constructor: `X` the class being defined, or `A::A`. */
static int at_ctor_declarator(void)
{
    if (cx_scope->k == SC_CLASS && cx_kind() == TOK_IDENT &&
        cx_scope->cls->name && cx_kind_at(1) == TOK_LPAREN &&
        strcmp(cx_cur()->t.text, cx_scope->cls->name) == 0)
        return 1;
    struct qname q = peek_qname();
    if (q.bad || !q.scope || q.scope->k != SC_CLASS || q.fin == 0)
        return 0;
    return cx_kind_at(q.fin) == TOK_IDENT && q.scope->cls->name &&
           strcmp(tok_text(q.fin), q.scope->cls->name) == 0 &&
           cx_kind_at(q.fin + 1) == TOK_LPAREN;
}

static int is_decl_keyword(enum tok_kind k)
{
    switch (k) {
    case TOK_KW_INT: case TOK_KW_CHAR: case TOK_KW_SHORT: case TOK_KW_LONG:
    case TOK_KW_FLOAT: case TOK_KW_DOUBLE: case TOK_KW_BOOL: case TOK_CX_BOOL:
    case TOK_KW_UNSIGNED: case TOK_KW_SIGNED: case TOK_KW_VOID:
    case TOK_CX_WCHAR_T: case TOK_CX_CHAR8_T: case TOK_CX_CHAR16_T:
    case TOK_CX_CHAR32_T: case TOK_KW_COMPLEX:
    case TOK_KW_STRUCT: case TOK_KW_UNION: case TOK_CX_CLASS: case TOK_KW_ENUM:
    case TOK_KW_CONST: case TOK_KW_VOLATILE: case TOK_KW_RESTRICT:
    case TOK_KW_STATIC: case TOK_KW_EXTERN: case TOK_KW_TYPEDEF:
    case TOK_KW_INLINE: case TOK_CX_CONSTEXPR: case TOK_CX_CONSTEVAL:
    case TOK_CX_CONSTINIT: case TOK_CX_VIRTUAL: case TOK_CX_EXPLICIT:
    case TOK_CX_FRIEND: case TOK_CX_MUTABLE: case TOK_CX_REGISTER:
    case TOK_CX_THREAD_LOCAL: case TOK_CX_TYPENAME: case TOK_CX_DECLTYPE:
    case TOK_KW_TYPEOF: case TOK_CX_AUTO: case TOK_KW_ATTRIBUTE:
    case TOK_KW_ALIGNAS: case TOK_KW_ATOMIC:
        return 1;
    default:
        return 0;
    }
}

int at_type_start(void)
{
    if (is_decl_keyword(cx_kind()))
        return 1;
    if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET)
        return 1;
    int n;
    return peek_type_name(&n) != NULL;
}

/* ---- declaration specifiers ---- */

enum { SK_NONE, SK_STATIC, SK_EXTERN, SK_TYPEDEF };

struct dspec {
    struct cty *type;
    int storage;
    int is_inline, is_constexpr, is_virtual, is_explicit, is_friend;
    int is_mutable;
    struct attrs a;
    const struct ctok *at;
    struct cclass *cls_defined;   /* a class defined by the specifiers */
    struct cenum *enum_defined;
    int decl_only;                /* `struct X;`: declares X, nothing else */
};

static struct cty *parse_class_spec(struct dspec *ds);
static struct cty *parse_enum_spec(struct dspec *ds);

static unsigned parse_cv(void)
{
    unsigned q = 0;
    for (;;) {
        if (cx_accept(TOK_KW_CONST)) q |= CQ_CONST;
        else if (cx_accept(TOK_KW_VOLATILE)) q |= CQ_VOLATILE;
        else if (cx_accept(TOK_KW_RESTRICT)) ;
        else if (cx_kind() == TOK_KW_ATTRIBUTE) parse_attrs(NULL);
        else return q;
    }
}

/* decltype(e): the declared type of a named entity, else e's type made a
 * reference by its value category. */
static struct cty *parse_decltype(void)
{
    cx_advance();
    cx_expect(TOK_LPAREN, "'(' after decltype");
    if (cx_kind() == TOK_CX_AUTO)
        cx_error(cx_cur(), "decltype(auto) is not supported yet (CX6)");
    struct cexpr *e = expr_parse();
    cx_expect(TOK_RPAREN, "')'");
    if (!e->paren && e->k == E_VAR)
        return e->var->type;
    if (!e->paren && e->k == E_MEMBER)
        return e->field->type;
    if (e->vc == VC_LVALUE)
        return ct_ref(e->t, 0);
    if (e->vc == VC_XVALUE)
        return ct_ref(e->t, 1);
    return e->t;
}

static void parse_dspec(struct dspec *ds)
{
    memset(ds, 0, sizeof *ds);
    ds->at = cx_cur();
    int n_void = 0, n_bool = 0, n_char = 0, n_short = 0, n_int = 0;
    int n_long = 0, n_signed = 0, n_unsigned = 0, n_float = 0, n_double = 0;
    enum cty_kind special = CT_VOID;
    int n_special = 0;
    unsigned cv = 0;
    struct cty *named = NULL;
    for (;;) {
        enum tok_kind k = cx_kind();
        int builtin = n_void + n_bool + n_char + n_short + n_int + n_long +
                      n_signed + n_unsigned + n_float + n_double + n_special;
        switch (k) {
        case TOK_KW_TYPEDEF: ds->storage = SK_TYPEDEF; cx_advance(); continue;
        case TOK_KW_STATIC: ds->storage = SK_STATIC; cx_advance(); continue;
        case TOK_KW_EXTERN: ds->storage = SK_EXTERN; cx_advance(); continue;
        case TOK_CX_REGISTER: cx_advance(); continue;
        case TOK_CX_THREAD_LOCAL:
            cx_error(cx_cur(), "thread_local is not supported");
            continue;
        case TOK_KW_INLINE: ds->is_inline = 1; cx_advance(); continue;
        case TOK_CX_CONSTEXPR: case TOK_CX_CONSTEVAL:
            ds->is_constexpr = 1;
            cx_advance();
            continue;
        case TOK_CX_CONSTINIT: cx_advance(); continue;
        case TOK_CX_VIRTUAL: ds->is_virtual = 1; cx_advance(); continue;
        case TOK_CX_EXPLICIT:
            ds->is_explicit = 1;
            cx_advance();
            if (cx_kind() == TOK_LPAREN) {
                cx_advance();
                ds->is_explicit = expr_parse_const("explicit(...)") != 0;
                cx_expect(TOK_RPAREN, "')'");
            }
            continue;
        case TOK_CX_FRIEND: ds->is_friend = 1; cx_advance(); continue;
        case TOK_CX_MUTABLE: ds->is_mutable = 1; cx_advance(); continue;
        case TOK_KW_CONST: cv |= CQ_CONST; cx_advance(); continue;
        case TOK_KW_VOLATILE: cv |= CQ_VOLATILE; cx_advance(); continue;
        case TOK_KW_RESTRICT: cx_advance(); continue;
        case TOK_KW_ATOMIC:
            cx_error(cx_cur(), "_Atomic is not supported in C++");
            continue;
        case TOK_KW_ATTRIBUTE: case TOK_KW_ALIGNAS:
            parse_attrs(&ds->a);
            continue;
        case TOK_LBRACKET:
            if (cx_kind_at(1) == TOK_LBRACKET) {
                parse_attrs(&ds->a);
                continue;
            }
            break;
        case TOK_KW_VOID: n_void++; cx_advance(); continue;
        case TOK_KW_BOOL: case TOK_CX_BOOL: n_bool++; cx_advance(); continue;
        case TOK_KW_CHAR: n_char++; cx_advance(); continue;
        case TOK_KW_SHORT: n_short++; cx_advance(); continue;
        case TOK_KW_INT: n_int++; cx_advance(); continue;
        case TOK_KW_LONG: n_long++; cx_advance(); continue;
        case TOK_KW_SIGNED: n_signed++; cx_advance(); continue;
        case TOK_KW_UNSIGNED: n_unsigned++; cx_advance(); continue;
        case TOK_KW_FLOAT: n_float++; cx_advance(); continue;
        case TOK_KW_DOUBLE: n_double++; cx_advance(); continue;
        case TOK_CX_WCHAR_T: special = CT_WCHAR; n_special++; cx_advance();
            continue;
        case TOK_CX_CHAR8_T: special = CT_CHAR8; n_special++; cx_advance();
            continue;
        case TOK_CX_CHAR16_T: special = CT_CHAR16; n_special++; cx_advance();
            continue;
        case TOK_CX_CHAR32_T: special = CT_CHAR32; n_special++; cx_advance();
            continue;
        case TOK_CX_AUTO:
            if (named || builtin)
                break;
            named = ct_basic(CT_AUTO);
            cx_advance();
            continue;
        case TOK_KW_COMPLEX:
            cx_error(cx_cur(), "_Complex is not supported in C++ yet");
            continue;
        case TOK_KW_STRUCT: case TOK_KW_UNION: case TOK_CX_CLASS:
            if (named || builtin)
                break;
            named = parse_class_spec(ds);
            continue;
        case TOK_KW_ENUM:
            if (named || builtin)
                break;
            named = parse_enum_spec(ds);
            continue;
        case TOK_CX_DECLTYPE:
            if (named || builtin)
                break;
            named = parse_decltype();
            continue;
        case TOK_KW_TYPEOF: {
            if (named || builtin)
                break;
            cx_advance();
            cx_expect(TOK_LPAREN, "'(' after typeof");
            if (at_type_start())
                named = parse_type_id();
            else
                named = rvalue(expr_parse())->t;
            cx_expect(TOK_RPAREN, "')'");
            continue;
        }
        case TOK_CX_TYPENAME:
            cx_advance();
            continue;
        case TOK_IDENT: case TOK_COLONCOLON: {
            if (named || builtin)
                break;
            if (at_ctor_declarator())
                break;
            int n;
            struct cty *t = peek_type_name(&n);
            if (!t)
                break;
            cx_pos += n;
            named = t;
            continue;
        }
        default:
            break;
        }
        break;
    }

    struct cty *t = named;
    if (named && (n_void || n_bool || n_char || n_short || n_int || n_long ||
                  n_signed || n_unsigned || n_float || n_double || n_special))
        cx_error(ds->at, "two types in one declaration");
    if (!t) {
        enum cty_kind k;
        if (n_special) k = special;
        else if (n_void) k = CT_VOID;
        else if (n_bool) k = CT_BOOL;
        else if (n_char)
            k = n_unsigned ? CT_UCHAR : n_signed ? CT_SCHAR : CT_CHAR;
        else if (n_float) k = CT_FLOAT;
        else if (n_double) k = n_long ? CT_LDOUBLE : CT_DOUBLE;
        else if (n_short) k = n_unsigned ? CT_USHORT : CT_SHORT;
        else if (n_long >= 2) k = n_unsigned ? CT_ULLONG : CT_LLONG;
        else if (n_long) k = n_unsigned ? CT_ULONG : CT_LONG;
        else if (n_int || n_signed || n_unsigned)
            k = n_unsigned ? CT_UINT : CT_INT;
        else
            k = CT_AUTO;          /* no type at all */
        if (k != CT_AUTO)
            t = ct_basic(k);
    }
    if (t && cv)
        t = ct_qual(t, cv);
    ds->type = t;
}

/* int(x), unsigned(y): a functional cast's type named by keywords. */
int at_simple_type_kw(void)
{
    switch (cx_kind()) {
    case TOK_KW_INT: case TOK_KW_CHAR: case TOK_KW_SHORT: case TOK_KW_LONG:
    case TOK_KW_FLOAT: case TOK_KW_DOUBLE: case TOK_KW_BOOL: case TOK_CX_BOOL:
    case TOK_KW_UNSIGNED: case TOK_KW_SIGNED: case TOK_KW_VOID:
    case TOK_CX_WCHAR_T: case TOK_CX_CHAR8_T: case TOK_CX_CHAR16_T:
    case TOK_CX_CHAR32_T: case TOK_CX_DECLTYPE: case TOK_CX_TYPENAME:
        return 1;
    default:
        return 0;
    }
}

struct cty *parse_simple_type_spec(void)
{
    struct dspec ds;
    const struct ctok *at = cx_cur();
    parse_dspec(&ds);
    if (!ds.type || ds.storage)
        cx_error(at, "expected a type");
    return ds.type;
}

/* ---- declarators ---- */

enum {
    DK_NAMED = 1,             /* a name may be declared */
    DK_ABSTRACT = 2,          /* ... or omitted */
    DK_INIT = 4,              /* `(` after the name may start an initializer */
    DK_NEW = 8                /* a new-type-id: no function suffix */
};

enum { DN_NONE, DN_IDENT, DN_OPERATOR, DN_CONV, DN_DTOR, DN_CTOR };

struct declarator {
    int kind;
    const char *name;
    struct cscope *qual;      /* A::f: the scope named, or NULL */
    struct cscope *saved;     /* the scope to return to after a qualified
                               * declarator (its rest reads in `qual`) */
    struct cty *conv_type;    /* DN_CONV */
    const struct ctok *at;
    struct attrs a;
};

static const char *op_spelling(enum tok_kind k)
{
    switch (k) {
    case TOK_PLUS: return "+"; case TOK_MINUS: return "-";
    case TOK_STAR: return "*"; case TOK_SLASH: return "/";
    case TOK_PERCENT: return "%"; case TOK_AMP: return "&";
    case TOK_PIPE: return "|"; case TOK_CARET: return "^";
    case TOK_TILDE: return "~"; case TOK_BANG: return "!";
    case TOK_ASSIGN: return "="; case TOK_LT: return "<";
    case TOK_GT: return ">"; case TOK_PLUSEQ: return "+=";
    case TOK_MINUSEQ: return "-="; case TOK_STAREQ: return "*=";
    case TOK_SLASHEQ: return "/="; case TOK_PERCENTEQ: return "%=";
    case TOK_CARETEQ: return "^="; case TOK_AMPEQ: return "&=";
    case TOK_PIPEEQ: return "|="; case TOK_SHL: return "<<";
    case TOK_SHR: return ">>"; case TOK_SHLEQ: return "<<=";
    case TOK_SHREQ: return ">>="; case TOK_EQEQ: return "==";
    case TOK_NEQ: return "!="; case TOK_LE: return "<=";
    case TOK_GE: return ">="; case TOK_SPACESHIP: return "<=>";
    case TOK_ANDAND: return "&&"; case TOK_OROR: return "||";
    case TOK_PLUSPLUS: return "++"; case TOK_MINUSMINUS: return "--";
    case TOK_COMMA: return ","; case TOK_ARROWSTAR: return "->*";
    case TOK_ARROW: return "->";
    default: return NULL;
    }
}

static struct cty *parse_declarator(struct cty *t, struct declarator *d,
                                    int mode);

/* After `operator`: the function's name ("operator+", "operator new[]",
 * "operator()") or, for a conversion function, its type. */
const char *parse_operator_name(struct cty **conv)
{
    const struct ctok *at = cx_cur();
    cx_expect(TOK_CX_OPERATOR, "'operator'");
    enum tok_kind k = cx_kind();
    if (k == TOK_CX_NEW || k == TOK_CX_DELETE) {
        cx_advance();
        int arr = 0;
        if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_RBRACKET) {
            cx_advance();
            cx_advance();
            arr = 1;
        }
        return cx_fmt("operator %s%s", k == TOK_CX_NEW ? "new" : "delete",
                      arr ? "[]" : "");
    }
    if (k == TOK_LPAREN) {
        cx_advance();
        cx_expect(TOK_RPAREN, "')' in operator()");
        return "operator()";
    }
    if (k == TOK_LBRACKET) {
        cx_advance();
        cx_expect(TOK_RBRACKET, "']' in operator[]");
        return "operator[]";
    }
    const char *sp = op_spelling(k);
    if (sp) {
        cx_advance();
        return cx_fmt("operator%s", sp);
    }
    if (k == TOK_STR)
        cx_error(at, "user-defined literals are not supported yet (CX6)");
    /* a conversion function: operator T */
    struct dspec ds;
    parse_dspec(&ds);
    if (!ds.type)
        cx_error(at, "expected a type or an operator after 'operator'");
    struct cty *t = ds.type;
    for (;;) {
        if (cx_accept(TOK_STAR)) t = ct_qual(ct_ptr(t), parse_cv());
        else if (cx_accept(TOK_AMP)) t = ct_ref(t, 0);
        else if (cx_accept(TOK_ANDAND)) t = ct_ref(t, 1);
        else break;
    }
    *conv = t;
    return cx_fmt("operator %s", ct_name(t));
}

/* The name being declared: an identifier, `~X`, `operator@`, possibly
 * qualified. A qualified one moves the parser into its scope, where the
 * rest of the declarator (and a function's body) is read. */
static void parse_declarator_id(struct declarator *d, int mode)
{
    struct qname q = peek_qname();
    if (q.bad)
        cx_error(cx_peek(q.fin), "'%s' does not name a namespace or class",
                 tok_text(q.fin));
    enum tok_kind k = cx_kind_at(q.fin);
    if (k != TOK_IDENT && k != TOK_TILDE && k != TOK_CX_OPERATOR) {
        if (q.fin)
            cx_error(cx_peek(q.fin), "expected a name after '::'");
        return;               /* abstract */
    }
    if (k == TOK_TILDE && cx_kind_at(q.fin + 1) != TOK_IDENT)
        return;
    d->at = cx_peek(q.fin);
    cx_pos += q.fin;
    if (k == TOK_IDENT) {
        d->name = cx_cur()->t.text;
        d->kind = DN_IDENT;
        struct cclass *c = q.scope && q.scope->k == SC_CLASS ? q.scope->cls
                          : !q.scope && cx_scope->k == SC_CLASS
                          ? cx_scope->cls : NULL;
        cx_advance();
        if (c && c->name && strcmp(d->name, c->name) == 0 &&
            cx_kind() == TOK_LPAREN)
            d->kind = DN_CTOR;
    } else if (k == TOK_TILDE) {
        cx_advance();
        d->name = cx_fmt("~%s", cx_cur()->t.text);
        d->kind = DN_DTOR;
        cx_advance();
    } else {
        d->name = parse_operator_name(&d->conv_type);
        d->kind = d->conv_type ? DN_CONV : DN_OPERATOR;
    }
    (void)mode;
    d->qual = q.scope;
    if (q.scope && q.scope != cx_scope) {
        d->saved = cx_scope;
        cx_scope = q.scope;
    }
}

/* At `(` after a name: parameters follow (a function declarator), rather
 * than an initializer. `T x();` declares a function (the standard's rule). */
static int params_follow(void)
{
    enum tok_kind k = cx_kind_at(1);
    if (k == TOK_RPAREN || k == TOK_ELLIPSIS)
        return 1;
    int save = cx_pos;
    cx_advance();
    int r = at_type_start();
    cx_pos = save;
    return r;
}

/* At `(` before any name: a nested declarator `(*p)` rather than a
 * function's parameters. */
static int nested_follows(int mode)
{
    enum tok_kind k = cx_kind_at(1);
    if (k == TOK_STAR || k == TOK_AMP || k == TOK_ANDAND || k == TOK_CARET ||
        k == TOK_KW_ATTRIBUTE)
        return 1;
    if (k == TOK_IDENT || k == TOK_COLONCOLON) {
        /* (C::*name) */
        int save = cx_pos;
        cx_advance();
        struct qname q = peek_qname();
        cx_pos = save;
        if (!q.bad && q.scope && q.fin > 0 && cx_kind_at(1 + q.fin) == TOK_STAR)
            return 1;
    }
    if (k == TOK_LPAREN)
        return 1;
    if (k == TOK_RPAREN || k == TOK_ELLIPSIS)
        return 0;
    if ((mode & DK_NAMED) && (k == TOK_IDENT || k == TOK_COLONCOLON ||
                              k == TOK_TILDE || k == TOK_CX_OPERATOR)) {
        int save = cx_pos;
        cx_advance();
        int r = !at_type_start();
        cx_pos = save;
        return r;
    }
    return 0;
}

static struct cty *parse_params(void);

static struct cty *parse_suffixes(struct cty *t, struct declarator *d,
                                  int mode, int after_id)
{
    if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) != TOK_LBRACKET) {
        const struct ctok *at = cx_cur();
        cx_advance();
        long n = -1;
        if (cx_kind() != TOK_RBRACKET) {
            if (mode & DK_NEW) {
                /* new T[n]: n is any expression; the caller reads it */
                cx_pos--;
                return t;
            }
            n = expr_parse_const("an array bound");
            if (n < 0)
                cx_error(at, "array bound is negative");
        }
        cx_expect(TOK_RBRACKET, "']'");
        struct cty *inner = parse_suffixes(t, d, mode, 0);
        if (inner->k == CT_FUNC || ct_is_ref(inner) || inner->k == CT_VOID)
            cx_error(at, "array of %s", ct_name(inner));
        return ct_array(inner, n);
    }
    if (cx_kind() == TOK_LPAREN && !(mode & DK_NEW) &&
        (!after_id || !(mode & DK_INIT) || params_follow())) {
        struct cty *ft = parse_params();
        struct cty *ret = t;
        if (cx_kind() == TOK_ARROW) {
            cx_advance();
            ret = parse_type_id();
            if (t->k != CT_AUTO)
                cx_error(cx_cur(), "a trailing return type needs 'auto'");
        }
        ft->to = ret;
        if (ret->k == CT_ARRAY || ret->k == CT_FUNC)
            cx_error(cx_cur(), "function returning %s", ct_name(ret));
        return ft;
    }
    return t;
}

static struct cty *ptr_ops(struct cty *t, int mode)
{
    for (;;) {
        if (cx_kind() == TOK_STAR) {
            cx_advance();
            if (ct_is_ref(t))
                cx_error(cx_cur(), "pointer to a reference");
            t = ct_ptr(t);
            unsigned q = parse_cv();
            if (q)
                t = ct_qual(t, q);
            continue;
        }
        if ((cx_kind() == TOK_AMP || cx_kind() == TOK_ANDAND) &&
            !(mode & DK_NEW)) {
            int rv = cx_kind() == TOK_ANDAND;
            cx_advance();
            if (t->k == CT_VOID)
                cx_error(cx_cur(), "reference to void");
            t = ct_ref(t, rv);
            parse_attrs(NULL);
            continue;
        }
        if (cx_kind() == TOK_IDENT || cx_kind() == TOK_COLONCOLON) {
            /* C::* : a pointer to a member of C */
            struct qname q = peek_qname();
            if (!q.bad && q.scope && q.fin > 0 &&
                cx_kind_at(q.fin) == TOK_STAR) {
                if (q.scope->k != SC_CLASS)
                    cx_error(cx_cur(), "'::*' names a member of a class");
                if (ct_is_ref(t) || t->k == CT_VOID)
                    cx_error(cx_cur(), "pointer to member of type %s",
                             ct_name(t));
                cx_pos += q.fin + 1;
                t = ct_mptr(q.scope->cls, t);
                unsigned cq = parse_cv();
                if (cq)
                    t = ct_qual(t, cq);
                continue;
            }
        }
        return t;
    }
}

static struct cty *parse_declarator(struct cty *t, struct declarator *d,
                                    int mode)
{
    parse_attrs(&d->a);
    t = ptr_ops(t, mode);
    if (cx_kind() == TOK_LPAREN && !(mode & DK_NEW) && nested_follows(mode)) {
        int open = cx_pos;
        cx_skip_balanced();
        t = parse_suffixes(t, d, mode, 0);
        int end = cx_pos;
        cx_pos = open + 1;
        t = parse_declarator(t, d, mode & ~DK_INIT);
        cx_expect(TOK_RPAREN, "')' to close the declarator");
        cx_pos = end;
        return t;
    }
    if (mode & DK_NAMED)
        parse_declarator_id(d, mode);
    if (!d->name && !(mode & DK_ABSTRACT))
        cx_error(cx_cur(), "expected a name to declare before %s",
                 tok_describe(&cx_cur()->t));
    parse_attrs(&d->a);
    return parse_suffixes(t, d, mode, d->name != NULL);
}

/* ( parameter-declaration-clause ) cv ref noexcept: a function type with
 * no return type yet. */
static struct cty *parse_params(void)
{
    cx_expect(TOK_LPAREN, "'('");
    int cap = 8, np = 0, variadic = 0, anydef = 0;
    struct cty **params = xmalloc((size_t)cap * sizeof *params);
    struct cty **pdecl = xmalloc((size_t)cap * sizeof *pdecl);
    const char **names = xmalloc((size_t)cap * sizeof *names);
    struct cexpr **defs = xmalloc((size_t)cap * sizeof *defs);
    if (cx_kind() == TOK_KW_VOID && cx_kind_at(1) == TOK_RPAREN) {
        cx_advance();
    } else {
        while (cx_kind() != TOK_RPAREN) {
            if (cx_accept(TOK_ELLIPSIS)) {
                variadic = 1;
                break;
            }
            struct dspec ds;
            const struct ctok *at = cx_cur();
            parse_dspec(&ds);
            if (!ds.type)
                cx_error(at, "expected a parameter type before %s",
                         tok_describe(&cx_cur()->t));
            struct declarator d;
            memset(&d, 0, sizeof d);
            struct cty *t = parse_declarator(ds.type, &d,
                                             DK_NAMED | DK_ABSTRACT);
            if (d.saved)
                cx_scope = d.saved;
            if (t->k == CT_AUTO)
                cx_error(at, "an 'auto' parameter (an abbreviated "
                             "template) is not supported yet (CX4)");
            if (t->k == CT_VOID)
                cx_error(at, "a parameter of type void");
            struct cty *adj = t;
            if (t->k == CT_ARRAY)
                adj = ct_qual(ct_ptr(t->to), t->q);
            else if (t->k == CT_FUNC)
                adj = ct_ptr(t);
            if (np == cap) {
                cap *= 2;
                params = xrealloc(params, (size_t)cap * sizeof *params);
                pdecl = xrealloc(pdecl, (size_t)cap * sizeof *pdecl);
                names = xrealloc(names, (size_t)cap * sizeof *names);
                defs = xrealloc(defs, (size_t)cap * sizeof *defs);
            }
            defs[np] = NULL;
            if (cx_accept(TOK_ASSIGN)) {
                defs[np] = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                                   : expr_parse_assign();
                anydef = 1;
            }
            params[np] = ct_unqual(adj);
            pdecl[np] = adj;
            names[np] = d.name;
            np++;
            if (cx_accept(TOK_COMMA))
                continue;
            if (cx_accept(TOK_ELLIPSIS))
                variadic = 1;
            break;
        }
    }
    cx_expect(TOK_RPAREN, "')' to close the parameters");
    struct cty *ft = ct_func(NULL, params, np, variadic);
    ft->pdecl = pdecl;
    ft->pnames = names;
    ft->defargs = anydef ? defs : NULL;
    ft->fq = parse_cv();
    if (cx_kind() == TOK_AMP) {
        cx_advance();
        ft->refq = 1;
    } else if (cx_kind() == TOK_ANDAND) {
        cx_advance();
        ft->refq = 2;
    }
    for (;;) {
        if (cx_accept(TOK_CX_NOEXCEPT)) {
            if (cx_kind() == TOK_LPAREN)
                cx_skip_balanced();
            continue;
        }
        if (cx_kind() == TOK_CX_THROW && cx_kind_at(1) == TOK_LPAREN) {
            cx_advance();
            cx_skip_balanced();
            continue;
        }
        if (cx_kind() == TOK_KW_ATTRIBUTE ||
            (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET)) {
            parse_attrs(NULL);
            continue;
        }
        /* virt-specifiers: checked by overriding, not needed to emit */
        if (cx_kind() == TOK_IDENT && (strcmp(cx_cur()->t.text, "override")
                                       == 0 ||
                                       strcmp(cx_cur()->t.text, "final")
                                       == 0)) {
            cx_advance();
            continue;
        }
        break;
    }
    return ft;
}

struct cty *parse_type_id(void)
{
    struct dspec ds;
    const struct ctok *at = cx_cur();
    parse_dspec(&ds);
    if (!ds.type)
        cx_error(at, "expected a type before %s", tok_describe(&at->t));
    struct declarator d;
    memset(&d, 0, sizeof d);
    return parse_declarator(ds.type, &d, DK_ABSTRACT);
}

/* A new-expression's type: no function declarator, and `[` stops it (the
 * array bound is an expression, read by the caller). */
struct cty *parse_new_type_id(void)
{
    struct dspec ds;
    const struct ctok *at = cx_cur();
    parse_dspec(&ds);
    if (!ds.type)
        cx_error(at, "expected a type after 'new'");
    struct declarator d;
    memset(&d, 0, sizeof d);
    return parse_declarator(ds.type, &d, DK_ABSTRACT | DK_NEW);
}

/* ---- functions ---- */

static int same_signature(struct cty *a, struct cty *b)
{
    if (a->np != b->np || a->variadic != b->variadic || a->fq != b->fq ||
        a->refq != b->refq)
        return 0;
    for (int i = 0; i < a->np; i++)
        if (!ct_same_unqual(a->params[i], b->params[i]))
            return 0;
    return 1;
}

/* The overload set `name` declared in scope s (creating the symbol). */
static struct csym *func_sym(struct cscope *s, const char *name,
                             const struct ctok *at)
{
    struct csym *y = scope_find_here(s, name);
    if (y && y->k == CS_FUNC)
        return y;
    if (y && y->k != CS_CLASS && y->k != CS_ENUM)
        cx_error(at, "'%s' redeclared as a function", name);
    return scope_add(s, CS_FUNC, name);
}

static void merge_defaults(struct cfunc *f, struct cty *ft)
{
    if (!ft->defargs)
        return;
    if (!f->defargs)
        f->defargs = xcalloc((size_t)(ft->np ? ft->np : 1),
                             sizeof *f->defargs);
    for (int i = 0; i < ft->np; i++)
        if (ft->defargs[i])
            f->defargs[i] = ft->defargs[i];
}

/* Declare (or redeclare) the function d names, with type ft, in scope
 * `target` (a namespace, or the class for members). */
static struct cfunc *declare_function(struct dspec *ds, struct declarator *d,
                                      struct cty *ft, struct cscope *target)
{
    struct cclass *cls = target->k == SC_CLASS ? target->cls : NULL;
    if (ds->is_friend)
        cls = NULL;
    struct cfunc **set;
    struct csym *y = NULL;
    if (d->kind == DN_CTOR) {
        if (!cls)
            cx_error(d->at, "a constructor outside its class");
        set = &cls->ctors;
    } else if (d->kind == DN_DTOR) {
        if (!cls || !cls->name || strcmp(d->name + 1, cls->name) != 0)
            cx_error(d->at, "'%s' is not the destructor of its class",
                     d->name);
        set = &cls->dtor;
    } else {
        y = func_sym(target, d->name, d->at);
        set = &y->fns;
    }
    if (ft->to->k == CT_AUTO)
        cx_error(d->at, "a deduced return type is not supported yet (CX6)");
    for (struct cfunc *f = *set; f; f = f->next) {
        if (!same_signature(f->type, ft) &&
            !(f->c_linkage && cx_extern_c))
            continue;
        if (!ct_same(f->type->to, ft->to))
            cx_error(d->at, "'%s' redeclared with a different return type",
                     d->name);
        merge_defaults(f, ft);
        if (ds->is_inline || ds->is_constexpr)
            f->is_inline = 1;
        if (ds->a.weak)
            f->weak = 1;
        if (ds->a.noreturn || d->a.noreturn)
            f->noreturn = 1;
        return f;
    }
    struct cfunc *f = xcalloc(1, sizeof *f);
    f->name = d->name;
    f->type = ft;
    f->owner = target;
    f->cls = cls;
    f->line = d->at ? d->at->t.line : 0;
    f->file = d->at ? d->at->file : NULL;
    f->is_ctor = d->kind == DN_CTOR;
    f->is_dtor = d->kind == DN_DTOR;
    f->is_conv = d->kind == DN_CONV;
    f->is_static = ds->storage == SK_STATIC;
    /* a class's operator new/delete are static members (11.12) */
    if (cls && (strncmp(d->name, "operator new", 12) == 0 ||
                strncmp(d->name, "operator delete", 15) == 0))
        f->is_static = 1;
    f->is_inline = ds->is_inline || ds->is_constexpr;
    f->is_constexpr = ds->is_constexpr;
    f->is_explicit = ds->is_explicit;
    f->is_virtual = ds->is_virtual;
    f->c_linkage = cx_extern_c && !cls;
    f->weak = ds->a.weak || d->a.weak;
    f->noreturn = ds->a.noreturn || d->a.noreturn;
    f->section = ds->a.section ? ds->a.section : d->a.section;
    f->asm_name = d->a.asm_name;
    f->body_tok = -1;
    f->mi_tok = -1;
    f->pnames = ft->pnames;
    merge_defaults(f, ft);
    f->vslot = -1;
    if (f->is_virtual && !cls)
        cx_error(d->at, "only a member function can be virtual");
    /* internal linkage: an unnamed namespace, or static at namespace scope */
    for (struct cscope *s = target; s; s = s->parent)
        if (s->k == SC_NAMESPACE && s->anon)
            f->is_static = cls ? f->is_static : 1;
    struct cfunc **tail = set;
    while (*tail)
        tail = &(*tail)->next;
    *tail = f;
    func_register(f);
    return f;
}

/* ---- delayed bodies ---- */

struct pending {
    struct cfunc *f;          /* a member function's body, or */
    struct cclass *c;         /* ... a default member initializer */
    struct cfield *fl;
};
static struct pending *pend;
static int npend, cappend;
static int class_depth;

static void add_pending(struct cfunc *f, struct cclass *c, struct cfield *fl)
{
    if (npend == cappend) {
        cappend = cappend ? cappend * 2 : 32;
        pend = xrealloc(pend, (size_t)cappend * sizeof *pend);
    }
    pend[npend].f = f;
    pend[npend].c = c;
    pend[npend].fl = fl;
    npend++;
}

static void define_function(struct cfunc *f, struct cty *ft);

/* Skip a function body — and a constructor's mem-initializers before it —
 * recording where it is. */
static void skip_body(struct cfunc *f)
{
    f->mi_tok = -1;
    if (cx_kind() == TOK_CX_TRY)
        cx_error(cx_cur(), "function-try-blocks are not supported yet (CX5)");
    if (cx_kind() == TOK_COLON) {
        f->mi_tok = cx_pos;
        cx_advance();
        for (;;) {
            while (cx_kind() != TOK_LPAREN && cx_kind() != TOK_LBRACE) {
                if (cx_kind() == TOK_EOF || cx_kind() == TOK_SEMI)
                    cx_error(cx_cur(), "malformed mem-initializer list");
                cx_advance();
            }
            cx_skip_balanced();
            if (cx_kind() == TOK_ELLIPSIS)
                cx_advance();
            if (!cx_accept(TOK_COMMA))
                break;
        }
    }
    if (cx_kind() != TOK_LBRACE)
        cx_error(cx_cur(), "expected a function body");
    f->body_tok = cx_pos;
    cx_skip_balanced();
    f->body_end = cx_pos;
}

static void run_pending(void)
{
    /* default member initializers first: constructors use them */
    for (int i = 0; i < npend; i++)
        if (pend[i].fl)
            field_parse_default(pend[i].c, pend[i].fl);
    for (int i = 0; i < npend; i++) {
        if (!pend[i].f)
            continue;
        struct cfunc *f = pend[i].f;
        int save = cx_pos;
        struct cscope *ss = cx_scope;
        cx_scope = f->def_scope;
        cx_pos = f->mi_tok >= 0 ? f->mi_tok : f->body_tok;
        define_function(f, f->type);
        cx_pos = save;
        cx_scope = ss;
    }
    npend = 0;
}

/* The names a local may not take in C: C's keywords the C++ source may
 * use as identifiers, and C-linkage entities it could hide. */
static const char *local_cname(const char *name)
{
    static const char *const ckw[] = {
        "restrict", "typeof", "_Bool", "_Complex", "_Generic", "_Atomic",
        "_Noreturn", "_Alignas", "_Alignof", "_Static_assert",
        "_Thread_local", "_Imaginary",
    };
    for (size_t i = 0; i < sizeof ckw / sizeof ckw[0]; i++)
        if (strcmp(name, ckw[i]) == 0)
            return cx_fmt("%s__%d", name, cx_uid());
    struct csym *g = lookup_in(cx_global, name);
    if (g && (g->k == CS_VAR || (g->k == CS_FUNC && g->fns &&
                                 g->fns->c_linkage)))
        return cx_fmt("%s__%d", name, cx_uid());
    if (strncmp(name, "__cx", 4) == 0)
        return cx_fmt("%s__%d", name, cx_uid());
    return name;
}

static struct cvar *new_local(const char *name, struct cty *t,
                              const struct ctok *at)
{
    struct cvar *v = xcalloc(1, sizeof *v);
    v->name = name;
    v->cname = name ? local_cname(name) : cx_fmt("__cx_anon%d", cx_uid());
    v->type = t;
    v->is_local = 1;
    v->fn = cx_curfn;
    v->line = at ? at->t.line : 0;
    v->file = at ? at->file : NULL;
    return v;
}

static struct cstmt *parse_stmt(void);
static struct cstmt *parse_block_body(struct cscope *s);
static void mark_nrvo(struct cfunc *f);

/* How many local statics named `name` function fn declared before: the
 * discriminator that tells them apart in their mangled names. */
static int static_disc(struct cfunc *fn, const char *name)
{
    static struct { struct cfunc *fn; const char *name; } *seen;
    static int nseen, capseen;
    int n = 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i].fn == fn && strcmp(seen[i].name, name) == 0)
            n++;
    if (nseen == capseen) {
        capseen = capseen ? capseen * 2 : 16;
        seen = xrealloc(seen, (size_t)capseen * sizeof *seen);
    }
    seen[nseen].fn = fn;
    seen[nseen].name = name;
    nseen++;
    return n;
}

/* Parse a function's definition at the cursor (at `{` or a constructor's
 * `:`), with parameter types from ft (the definition's declarator). */
static void define_function(struct cfunc *f, struct cty *ft)
{
    const struct ctok *at = cx_cur();
    if (f->defined)
        cx_error(at, "redefinition of '%s'", f->name);
    f->defined = 1;
    if (ft->pnames)
        f->pnames = ft->pnames;
    struct cfunc *savefn = cx_curfn;
    struct cstmt *saveblk = cx_curblk;
    cx_curfn = f;
    cx_curblk = NULL;
    struct cscope *ps = scope_push(SC_PARAMS, NULL);
    ps->fn = f;
    f->pscope = ps;
    int np = f->type->np;
    f->params = xcalloc((size_t)(np ? np : 1), sizeof *f->params);
    for (int i = 0; i < np; i++) {
        const char *n = ft->pnames ? ft->pnames[i] : NULL;
        struct cty *pt = ft->pdecl ? ft->pdecl[i] : f->type->params[i];
        struct cvar *v = new_local(n, pt, at);
        v->is_param = 1;
        f->params[i] = v;
        if (n) {
            struct csym *y = scope_add(ps, CS_VAR, n);
            y->var = v;
        }
    }
    if (f->cls && !f->is_static) {
        struct cvar *tv = new_local("this", NULL, at);
        tv->cname = "this";
        tv->type = ct_ptr(ct_qual(ct_class(f->cls), f->type->fq));
        tv->is_param = 1;
        f->this_var = tv;
    }
    struct meminit_raw *mi = NULL;
    int nmi = 0, capmi = 0;
    if (cx_kind() == TOK_COLON) {
        if (!f->is_ctor)
            cx_error(at, "only a constructor has mem-initializers");
        cx_advance();
        for (;;) {
            const struct ctok *mat = cx_cur();
            /* the name: a member, or the class itself (delegating) */
            struct qname q = peek_qname();
            if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
                cx_error(mat, "expected a member name in a mem-initializer");
            cx_pos += q.fin;
            const char *name = cx_cur()->t.text;
            cx_advance();
            if (nmi == capmi) {
                capmi = capmi ? capmi * 2 : 8;
                mi = xrealloc(mi, (size_t)capmi * sizeof *mi);
            }
            struct meminit_raw *r = &mi[nmi++];
            r->name = name;
            r->at = mat;
            r->braced = cx_kind() == TOK_LBRACE;
            r->args = NULL;
            r->na = 0;
            if (r->braced) {
                r->args = xmalloc(sizeof *r->args);
                r->args[0] = parse_braced_list();
                r->na = 1;
            } else {
                cx_expect(TOK_LPAREN, "'(' in a mem-initializer");
                int cap = 4;
                r->args = xmalloc((size_t)cap * sizeof *r->args);
                while (cx_kind() != TOK_RPAREN) {
                    if (r->na == cap) {
                        cap *= 2;
                        r->args = xrealloc(r->args,
                                           (size_t)cap * sizeof *r->args);
                    }
                    r->args[r->na++] = cx_kind() == TOK_LBRACE
                                       ? parse_braced_list()
                                       : expr_parse_assign();
                    if (!cx_accept(TOK_COMMA))
                        break;
                }
                cx_expect(TOK_RPAREN, "')'");
            }
            if (!cx_accept(TOK_COMMA))
                break;
        }
    }
    if (f->is_ctor)
        ctor_meminit(f, mi, nmi);
    if (cx_kind() != TOK_LBRACE)
        cx_error(cx_cur(), "expected '{' to begin the body of '%s'", f->name);
    struct cscope *bs = scope_push(SC_BLOCK, NULL);
    f->body = parse_block_body(bs);
    scope_pop();
    scope_pop();
    mark_nrvo(f);
    cx_curfn = savefn;
    cx_curblk = saveblk;
}

/* ---- variables ---- */

static struct cexpr *parse_init_args(enum init_form *form,
                                     struct cexpr ***argsp, int *nap)
{
    struct cexpr **args = NULL;
    int na = 0, cap = 0;
    *form = INIT_DEFAULT;
    if (cx_kind() == TOK_ASSIGN) {
        cx_advance();
        args = xmalloc(sizeof *args);
        if (cx_kind() == TOK_LBRACE) {
            *form = INIT_COPY_LIST;
            args[0] = parse_braced_list();
        } else {
            *form = INIT_COPY;
            args[0] = expr_parse_assign();
        }
        na = 1;
    } else if (cx_kind() == TOK_LBRACE) {
        *form = INIT_LIST;
        args = xmalloc(sizeof *args);
        args[0] = parse_braced_list();
        na = 1;
    } else if (cx_kind() == TOK_LPAREN) {
        *form = INIT_DIRECT;
        cx_advance();
        while (cx_kind() != TOK_RPAREN) {
            if (na == cap) {
                cap = cap ? cap * 2 : 4;
                args = xrealloc(args, (size_t)cap * sizeof *args);
            }
            args[na++] = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                                 : expr_parse_assign();
            if (!cx_accept(TOK_COMMA))
                break;
        }
        cx_expect(TOK_RPAREN, "')' to close the initializer");
    }
    *argsp = args;
    *nap = na;
    return na ? args[0] : NULL;
}

/* Deduce `auto` in P from A (template argument deduction's rules, for
 * the forms a declaration uses: cv auto, auto *, auto &, auto &&). */
static struct cty *match_auto(struct cty *P, struct cty *A,
                              const struct ctok *at)
{
    if (P->k == CT_AUTO)
        return ct_qual(A, P->q);
    if (P->k == CT_PTR) {
        if (A->k != CT_PTR)
            cx_error(at, "'%s' deduced from a non-pointer %s", ct_name(P),
                     ct_name(A));
        return ct_qual(ct_ptr(match_auto(P->to, A->to, at)), P->q);
    }
    cx_error(at, "cannot deduce '%s'", ct_name(P));
    return NULL;
}

/* A declared type P holding `auto`, deduced from its initializer. */
static struct cty *deduce_auto(struct cty *P, enum init_form form,
                               struct cexpr **args, int na,
                               const struct ctok *at)
{
    if (na != 1)
        cx_error(at, "cannot deduce 'auto' from this initializer");
    struct cexpr *e = args[0];
    if (form == INIT_LIST || form == INIT_COPY_LIST) {
        if (form == INIT_COPY_LIST || e->na != 1)
            cx_error(at, "'auto' from a braced list (std::initializer_list) "
                         "is not supported yet (CX6)");
        e = e->a[0];
    }
    struct cty *A = e->t;
    if (e->k == E_OVL) {
        if (e->fn->next)
            cx_error(at, "cannot deduce 'auto' from an overloaded function");
        A = e->fn->type;
    }
    if (ct_is_ref(P)) {
        struct cty *inner = P->to;
        if (P->k == CT_RREF && inner->k == CT_AUTO && !inner->q)
            return ct_ref(A, e->vc != VC_LVALUE);   /* forwarding */
        return ct_ref(match_auto(inner, A, at), P->k == CT_RREF);
    }
    return match_auto(P, ct_unqual(ct_decay(A)), at);
}

static int is_const_integral(struct cty *t)
{
    return (t->q & CQ_CONST) && !(t->q & CQ_VOLATILE) && ct_is_integer(t);
}

/* Initialize v (whose type may still be `auto`) from the initializer at
 * the cursor. */
static void init_variable(struct cvar *v, const struct ctok *at)
{
    enum init_form form;
    struct cexpr **args;
    int na;
    parse_init_args(&form, &args, &na);
    struct cty *t = v->type;
    if (t->k == CT_AUTO || (ct_is_ref(t) && t->to->k == CT_AUTO) ||
        (t->k == CT_PTR && t->to->k == CT_AUTO)) {
        t = deduce_auto(t, form, args, na, at);
        v->type = t;
    }
    if (form == INIT_DEFAULT) {
        if (ct_is_ref(t) && !v->is_extern && !v->is_param)
            cx_error(at, "reference '%s' is not initialized", v->name);
        if (!v->is_extern && !ct_is_ref(t)) {
            if (t->k == CT_ARRAY && t->n < 0)
                cx_error(at, "array '%s' has no size", v->name);
            if (!ct_is_complete(t))
                cx_error(at, "'%s' has incomplete type %s", v->name,
                         ct_name(t));
            v->ctor = init_object(t, INIT_DEFAULT, NULL, 0, at);
            if ((t->q & CQ_CONST) && !v->ctor && t->k != CT_CLASS &&
                !v->is_extern)
                cx_error(at, "const '%s' is not initialized", v->name);
        }
        return;
    }
    if (ct_is_ref(t)) {
        struct cexpr *e = args[0];
        if (form == INIT_LIST || form == INIT_COPY_LIST) {
            if (e->na != 1)
                cx_error(at, "a reference is initialized by one value");
            e = e->a[0];
        } else if (na != 1) {
            cx_error(at, "a reference is initialized by one value");
        }
        v->init = bind_ref(e, t, "initialization");
        return;
    }
    struct cexpr *ini = init_object(t, form, args, na, at);
    if (t->k == CT_ARRAY && t->n < 0 && ini)
        v->type = t = ini->t;
    if (t->k == CT_CLASS || t->k == CT_ARRAY)
        v->ctor = ini;
    else
        v->init = ini;
    long cv;
    if (v->init && is_const_integral(t) && expr_const(v->init, &cv)) {
        v->has_const = 1;
        v->const_val = cv;
    }
}

/* A variable at namespace scope, or a static data member's definition. */
static void declare_global_var(struct dspec *ds, struct declarator *d,
                               struct cty *t)
{
    struct cscope *target = d->qual ? d->qual : cx_scope;
    struct csym *y = scope_find_here(target, d->name);
    struct cvar *v = NULL;
    if (y && y->k == CS_VAR) {
        v = y->var;
        if (!ct_same(ct_unqual(v->type), ct_unqual(t)) &&
            !(v->type->k == CT_ARRAY && t->k == CT_ARRAY &&
              ct_same(v->type->to, t->to)))
            cx_error(d->at, "'%s' redeclared with a different type",
                     d->name);
        if (t->k == CT_ARRAY && t->n >= 0)
            v->type = t;
    } else {
        if (d->qual && d->qual != cx_scope)
            cx_error(d->at, "'%s' is not a member of '%s'", d->name,
                     d->qual->name ? d->qual->name : "::");
        if (y && y->k != CS_CLASS && y->k != CS_ENUM)
            cx_error(d->at, "'%s' redeclared as a different kind of entity",
                     d->name);
        v = xcalloc(1, sizeof *v);
        v->name = d->name;
        v->type = t;
        v->owner = target;
        v->c_linkage = cx_extern_c;
        v->line = d->at->t.line;
        v->file = d->at->file;
        v->is_extern = 1;
        /* internal linkage: static, const without extern, unnamed ns */
        if (ds->storage == SK_STATIC)
            v->is_static = 1;
        else if ((t->q & CQ_CONST) && ds->storage != SK_EXTERN &&
                 !ds->is_inline && !cx_extern_c)
            v->is_static = 1;
        for (struct cscope *s = target; s; s = s->parent)
            if (s->k == SC_NAMESPACE && s->anon)
                v->is_static = 1;
        v->cname = d->a.asm_name ? d->a.asm_name
                   : v->is_static && target == cx_global ? v->name
                   : mangle_var(v, target);
        y = scope_add(target, CS_VAR, d->name);
        y->var = v;
        gvar_register(v);
    }
    if (ds->a.weak || d->a.weak)
        v->weak = 1;
    if (ds->a.section || d->a.section)
        v->section = ds->a.section ? ds->a.section : d->a.section;
    if (ds->a.aligned || d->a.aligned)
        v->align_attr = ds->a.aligned ? ds->a.aligned : d->a.aligned;
    if (ds->is_inline || ds->is_constexpr)
        v->is_inline = ds->is_inline || v->is_member_static;
    if (ds->is_constexpr) {
        v->is_constexpr = 1;
        v->type = t = ct_qual(t, CQ_CONST);
    }
    /* `extern "C" int x;` (the linkage-specification's one declaration)
     * declares, as if written extern; inside extern "C" { } it defines */
    int is_def = (ds->storage != SK_EXTERN && cx_extern_c != 1) ||
                 cx_kind() == TOK_ASSIGN || cx_kind() == TOK_LBRACE;
    if (is_def) {
        if (v->defined && (cx_kind() == TOK_ASSIGN || cx_kind() == TOK_LBRACE
                           || cx_kind() == TOK_LPAREN))
            cx_error(d->at, "redefinition of '%s'", d->name);
        v->defined = 1;
        v->is_extern = 0;
        struct cfunc *sf = cx_curfn;
        cx_curfn = NULL;
        init_variable(v, d->at);
        cx_curfn = sf;
    }
}

/* ---- classes ---- */

struct cclass *class_new(const char *name, struct cscope *owner)
{
    struct cclass *c = xcalloc(1, sizeof *c);
    c->name = name;
    c->owner = owner;
    c->anon = name == NULL;
    c->scope = scope_new(SC_CLASS, name, owner);
    c->scope->cls = c;
    c->scope->fn = NULL;
    for (struct cscope *s = owner; s; s = s->parent)
        if (s->k == SC_BLOCK || s->k == SC_PARAMS)
            c->local = 1;
    if (!name)
        c->cname = cx_fmt("__cx_anon%d", cx_uid());
    else if (c->local)
        c->cname = cx_fmt("__cx_%s_%d", name, cx_uid());
    else
        c->cname = cx_fmt("_C%s", mangle_class_name(c));
    return c;
}

static void add_field(struct cclass *c, struct cfield *fl)
{
    if (c->nfields == c->capfields) {
        c->capfields = c->capfields ? c->capfields * 2 : 8;
        c->fields = xrealloc(c->fields, (size_t)c->capfields *
                                        sizeof *c->fields);
    }
    c->fields[c->nfields++] = fl;
}

static void parse_member(struct cclass *c, int *access);

/* Where an elaborated `struct X` that names nothing yet declares X: the
 * nearest enclosing namespace or block (not a class or parameter scope). */
static struct cscope *elaborated_home(void)
{
    struct cscope *s = cx_scope;
    while (s->k == SC_CLASS || s->k == SC_PARAMS || s->k == SC_ENUM)
        s = s->parent;
    return s;
}

/* base-specifier-list: [virtual] [access] [virtual] class-name, ... */
static void parse_bases(struct cclass *c)
{
    int cap = 0;
    do {
        const struct ctok *at = cx_cur();
        parse_attrs(NULL);
        int virt = 0, access = c->is_struct ? CA_PUBLIC : CA_PRIVATE;
        for (;;) {
            if (cx_accept(TOK_CX_VIRTUAL)) virt = 1;
            else if (cx_accept(TOK_CX_PUBLIC)) access = CA_PUBLIC;
            else if (cx_accept(TOK_CX_PROTECTED)) access = CA_PROTECTED;
            else if (cx_accept(TOK_CX_PRIVATE)) access = CA_PRIVATE;
            else break;
        }
        struct cty *t;
        int n;
        if (cx_kind() == TOK_CX_DECLTYPE) {
            t = parse_decltype();
        } else {
            t = peek_type_name(&n);
            if (!t)
                cx_error(at, "expected a base class name before %s",
                         tok_describe(&cx_cur()->t));
            cx_pos += n;
        }
        if (t->k != CT_CLASS)
            cx_error(at, "base '%s' is not a class", ct_name(t));
        struct cclass *b = t->cls;
        if (!b->complete)
            cx_error(at, "base class '%s' is incomplete", ct_name(t));
        if (b == c)
            cx_error(at, "a class cannot be its own base");
        if (b->is_union || c->is_union)
            cx_error(at, "a union cannot be a base or have bases");
        for (int i = 0; i < c->nbases; i++)
            if (c->bases[i].cls == b)
                cx_error(at, "'%s' is a direct base twice", b->name);
        if (virt)
            cx_error(at, "virtual base classes are not supported yet "
                         "(CX3b: vbase and vcall offsets, VTTs)");
        if (cx_accept(TOK_ELLIPSIS))
            cx_error(at, "pack expansions are not supported yet (CX4)");
        if (c->nbases == cap) {
            cap = cap ? cap * 2 : 4;
            c->bases = xrealloc(c->bases, (size_t)cap * sizeof *c->bases);
        }
        struct cbase *cb = &c->bases[c->nbases++];
        memset(cb, 0, sizeof *cb);
        cb->cls = b;
        cb->is_virtual = virt;
        cb->access = access;
    } while (cx_accept(TOK_COMMA));
}

static struct cty *parse_class_spec(struct dspec *ds)
{
    const struct ctok *at = cx_cur();
    enum tok_kind kw = cx_kind();
    cx_advance();
    struct attrs a;
    memset(&a, 0, sizeof a);
    parse_attrs(&a);
    const char *name = NULL;
    struct cscope *qual = NULL;
    if (cx_kind() == TOK_IDENT || cx_kind() == TOK_COLONCOLON) {
        struct qname q = peek_qname();
        if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
            cx_error(cx_cur(), "expected a class name");
        qual = q.scope;
        cx_pos += q.fin;
        name = cx_cur()->t.text;
        cx_advance();
    }
    if (cx_kind() == TOK_IDENT && strcmp(cx_cur()->t.text, "final") == 0 &&
        (cx_kind_at(1) == TOK_LBRACE || cx_kind_at(1) == TOK_COLON))
        cx_advance();
    parse_attrs(&a);
    int is_def = cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON;
    struct cclass *c = NULL;
    struct csym *y = NULL;
    if (name) {
        if (qual) {
            y = scope_find_tag(qual, name);
            if (!y)
                cx_error(at, "no class '%s' in '%s'", name,
                         qual->name ? qual->name : "::");
        } else if (is_def || cx_kind() == TOK_SEMI) {
            y = scope_find_tag(cx_scope->k == SC_PARAMS ? cx_scope->parent
                                                        : cx_scope, name);
        } else {
            y = lookup_tag(cx_scope, name);
        }
        if (y && y->k == CS_ENUM)
            cx_error(at, "'%s' is an enum, not a class", name);
        if (y && (y->k == CS_CLASS || y->k == CS_TYPEDEF) &&
            y->type->k == CT_CLASS)
            c = y->type->cls;
    }
    if (!c) {
        struct cscope *home = is_def || cx_kind() == TOK_SEMI
                              ? (cx_scope->k == SC_PARAMS ? cx_scope->parent
                                                          : cx_scope)
                              : elaborated_home();
        c = class_new(name, home);
        c->is_union = kw == TOK_KW_UNION;
        c->is_struct = kw != TOK_CX_CLASS;
        if (name) {
            y = scope_add(home, CS_CLASS, name);
            y->type = ct_class(c);
        }
    }
    if (!is_def) {
        if (cx_kind() == TOK_SEMI)
            ds->decl_only = 1;
        return ct_class(c);
    }
    if (c->complete)
        cx_error(at, "redefinition of '%s'", name);
    c->is_union = kw == TOK_KW_UNION;
    c->is_struct = kw != TOK_CX_CLASS;
    c->packed = a.packed;
    c->align_attr = a.aligned;
    if (cx_accept(TOK_COLON))
        parse_bases(c);
    ds->cls_defined = c;
    struct cscope *save = cx_scope;
    cx_scope = c->scope;
    c->defining = 1;
    /* the injected-class-name */
    if (name) {
        struct csym *inj = scope_add(c->scope, CS_CLASS, name);
        inj->type = ct_class(c);
    }
    class_depth++;
    cx_expect(TOK_LBRACE, "'{'");
    int access = c->is_struct ? CA_PUBLIC : CA_PRIVATE;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() == TOK_EOF)
            cx_error(at, "unterminated class '%s'", name ? name : "");
        parse_member(c, &access);
    }
    cx_advance();
    struct attrs ta;
    memset(&ta, 0, sizeof ta);
    parse_attrs(&ta);
    if (ta.packed)
        c->packed = 1;
    if (ta.aligned)
        c->align_attr = ta.aligned;
    c->defining = 0;
    cx_scope = save;
    class_complete(c);
    class_register(c);
    class_depth--;
    if (class_depth == 0)
        run_pending();
    return ct_class(c);
}

static struct cty *parse_enum_spec(struct dspec *ds)
{
    const struct ctok *at = cx_cur();
    cx_advance();
    int scoped = 0;
    if (cx_kind() == TOK_CX_CLASS || cx_kind() == TOK_KW_STRUCT) {
        scoped = 1;
        cx_advance();
    }
    parse_attrs(NULL);
    const char *name = NULL;
    if (cx_kind() == TOK_IDENT) {
        name = cx_cur()->t.text;
        cx_advance();
    }
    struct cty *fixed = NULL;
    if (cx_kind() == TOK_COLON) {
        cx_advance();
        fixed = ct_unqual(parse_type_id());
        if (!ct_is_integer(fixed) || fixed->k == CT_ENUM)
            cx_error(at, "an enum's underlying type must be integral");
    }
    int is_def = cx_kind() == TOK_LBRACE;
    struct cenum *en = NULL;
    if (name) {
        struct csym *y = (is_def || fixed || cx_kind() == TOK_SEMI)
                         ? scope_find_tag(cx_scope, name)
                         : lookup_tag(cx_scope, name);
        if (y && y->type->k == CT_ENUM)
            en = y->type->en;
        else if (y)
            cx_error(at, "'%s' is not an enum", name);
    }
    if (!en) {
        if (!is_def && !fixed && !scoped)
            cx_error(at, "enum '%s' is not declared", name ? name : "");
        en = xcalloc(1, sizeof *en);
        en->name = name;
        en->owner = cx_scope;
        en->scoped = scoped;
        en->scope = scope_new(SC_ENUM, name, cx_scope);
        en->scope->en = en;
        en->underlying = fixed ? fixed : ct_basic(CT_INT);
        en->fixed = fixed || scoped;
        if (name) {
            struct csym *y = scope_add(cx_scope, CS_ENUM, name);
            y->type = ct_enum(en);
        }
    }
    struct cty *et = ct_enum(en);
    if (!is_def) {
        if (cx_kind() == TOK_SEMI)
            ds->decl_only = 1;
        return et;
    }
    if (en->complete)
        cx_error(at, "redefinition of enum '%s'", name);
    ds->enum_defined = en;
    cx_advance();
    long next = 0, minv = 0, maxv = 0;
    int first = 1;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected an enumerator");
        const char *en_name = cx_cur()->t.text;
        cx_advance();
        parse_attrs(NULL);
        if (cx_accept(TOK_ASSIGN))
            next = expr_parse_const("an enumerator value");
        struct csym *y = scope_add(en->scope, CS_ENUMERATOR, en_name);
        y->type = et;
        y->value = next;
        if (!scoped) {
            y = scope_add(cx_scope, CS_ENUMERATOR, en_name);
            y->type = et;
            y->value = next;
        }
        if (first || next < minv) minv = next;
        if (first || next > maxv) maxv = next;
        first = 0;
        next++;
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RBRACE, "'}' to close the enum");
    if (!en->fixed) {
        if (minv >= -2147483648L && maxv <= 2147483647L)
            en->underlying = ct_basic(CT_INT);
        else if (minv >= 0 && maxv <= 4294967295L)
            en->underlying = ct_basic(CT_UINT);
        else if (minv >= 0)
            en->underlying = ct_basic(CT_ULONG);
        else
            en->underlying = ct_basic(CT_LONG);
    }
    en->complete = 1;
    return et;
}

/* Skip a default member initializer, recording where it starts. */
static void skip_default_init(struct cfield *fl)
{
    fl->dflt_tok = cx_pos;
    fl->dflt_braced = cx_kind() == TOK_LBRACE;
    if (fl->dflt_braced) {
        cx_skip_balanced();
        return;
    }
    cx_advance();                               /* '=' */
    while (cx_kind() != TOK_COMMA && cx_kind() != TOK_SEMI) {
        if (cx_kind() == TOK_EOF)
            cx_error(cx_cur(), "unterminated member initializer");
        if (cx_kind() == TOK_LPAREN || cx_kind() == TOK_LBRACKET ||
            cx_kind() == TOK_LBRACE)
            cx_skip_balanced();
        else
            cx_advance();
    }
}

void field_parse_default(struct cclass *c, struct cfield *fl)
{
    if (fl->dflt)
        return;
    int save = cx_pos;
    struct cscope *ss = cx_scope;
    struct cfunc *sf = cx_curfn;
    /* parsed as if in a constructor: `this` is the object being built */
    static struct cfunc pseudo;
    memset(&pseudo, 0, sizeof pseudo);
    pseudo.name = "<default member initializer>";
    pseudo.cls = c;
    struct cvar *tv = xcalloc(1, sizeof *tv);
    tv->name = tv->cname = "this";
    tv->type = ct_ptr(ct_class(c));
    tv->is_local = tv->is_param = 1;
    pseudo.this_var = tv;
    cx_curfn = &pseudo;
    cx_scope = c->scope;
    cx_pos = fl->dflt_tok;
    const struct ctok *at = cx_cur();
    struct cexpr *arg;
    enum init_form form;
    if (fl->dflt_braced) {
        arg = parse_braced_list();
        form = INIT_LIST;
    } else {
        cx_advance();
        if (cx_kind() == TOK_LBRACE) {
            arg = parse_braced_list();
            form = INIT_COPY_LIST;
        } else {
            arg = expr_parse_assign();
            form = INIT_COPY;
        }
    }
    if (ct_is_ref(fl->type))
        fl->dflt = bind_ref(form == INIT_COPY ? arg : arg->a[0], fl->type,
                            "a member initializer");
    else
        fl->dflt = init_object(fl->type, form, &arg, 1, at);
    cx_pos = save;
    cx_scope = ss;
    cx_curfn = sf;
}

static void parse_using(void);
static void parse_static_assert(void);

static void parse_member(struct cclass *c, int *access)
{
    const struct ctok *at = cx_cur();
    switch (cx_kind()) {
    case TOK_CX_PUBLIC: case TOK_CX_PRIVATE: case TOK_CX_PROTECTED:
        *access = cx_kind() == TOK_CX_PUBLIC ? CA_PUBLIC
                : cx_kind() == TOK_CX_PRIVATE ? CA_PRIVATE : CA_PROTECTED;
        cx_advance();
        cx_expect(TOK_COLON, "':' after an access specifier");
        return;
    case TOK_SEMI:
        cx_advance();
        return;
    case TOK_CX_USING:
        parse_using();
        return;
    case TOK_KW_STATIC_ASSERT:
        parse_static_assert();
        return;
    case TOK_CX_TEMPLATE:
        cx_error(at, "member templates are not supported yet (CX4)");
        return;
    default:
        break;
    }
    struct dspec ds;
    parse_dspec(&ds);
    if (ds.is_friend && ds.type && cx_kind() == TOK_SEMI) {
        cx_advance();                           /* friend class X; */
        return;
    }
    if (cx_kind() == TOK_SEMI) {
        if (ds.cls_defined && ds.cls_defined->anon)
            cx_error(at, "anonymous struct/union members are not "
                         "supported yet");
        cx_advance();
        return;
    }
    for (;;) {
        struct declarator d;
        memset(&d, 0, sizeof d);
        struct cty *t;
        if (cx_kind() == TOK_COLON) {
            t = ds.type;                    /* an unnamed bit-field */
        } else {
            struct cty *base = ds.type;
            if (!base) {
                if (!at_ctor_declarator() && cx_kind() != TOK_TILDE &&
                    cx_kind() != TOK_CX_OPERATOR)
                    cx_error(cx_cur(), "expected a member declaration "
                                       "before %s",
                             tok_describe(&cx_cur()->t));
                base = ct_basic(CT_VOID);
            }
            t = parse_declarator(base, &d, DK_NAMED);
            parse_asm_label(&d.a);
            parse_attrs(&d.a);
        }
        if (d.saved)
            cx_scope = d.saved;
        if (ds.storage == SK_TYPEDEF) {
            struct csym *y = scope_add(c->scope, CS_TYPEDEF, d.name);
            y->type = t;
        } else if (t->k == CT_FUNC || ds.is_friend) {
            if (t->k != CT_FUNC)
                cx_error(d.at, "a friend declaration names a function or "
                               "class");
            if ((d.kind == DN_CTOR || d.kind == DN_DTOR) && ds.type)
                cx_error(d.at, "a constructor or destructor has no type");
            if (d.kind == DN_CONV)
                t->to = d.conv_type;
            struct cscope *target = c->scope;
            if (ds.is_friend)
                target = d.qual ? d.qual : enclosing_ns(c->scope);
            struct cfunc *f = declare_function(&ds, &d, t, target);
            f->access = *access;
            if (d.kind == DN_DTOR)
                c->user_dtor = 1;
            if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON ||
                cx_kind() == TOK_CX_TRY) {
                f->is_inline = 1;
                f->type = t;
                if (t->pnames)
                    f->pnames = t->pnames;
                f->def_scope = c->scope;
                skip_body(f);
                add_pending(f, NULL, NULL);
                cx_accept(TOK_SEMI);
                return;
            }
            if (cx_accept(TOK_ASSIGN)) {
                if (cx_accept(TOK_KW_DEFAULT)) {
                    f->is_defaulted = 1;
                } else if (cx_accept(TOK_CX_DELETE)) {
                    f->is_deleted = 1;
                } else if (cx_kind() == TOK_NUM && cx_cur()->t.num == 0) {
                    if (!f->is_virtual)
                        cx_error(cx_cur(), "only a virtual function can be "
                                           "pure");
                    f->is_pure = 1;
                    cx_advance();
                } else {
                    cx_error(cx_cur(), "expected 'default', 'delete' or 0");
                }
            }
        } else if (ds.storage == SK_STATIC) {
            struct cvar *v = xcalloc(1, sizeof *v);
            v->name = d.name;
            v->type = ds.is_constexpr ? ct_qual(t, CQ_CONST) : t;
            v->owner = c->scope;
            v->is_member_static = 1;
            v->is_extern = 1;
            v->is_constexpr = ds.is_constexpr;
            v->is_inline = ds.is_inline || ds.is_constexpr;
            v->line = d.at->t.line;
            v->file = d.at->file;
            v->cname = mangle_var(v, c->scope);
            struct csym *y = scope_add(c->scope, CS_VAR, d.name);
            y->var = v;
            y->access = *access;
            gvar_register(v);
            if (cx_kind() == TOK_ASSIGN || cx_kind() == TOK_LBRACE) {
                if (v->is_inline) {
                    v->defined = 1;
                    v->is_extern = 0;
                }
                struct cfunc *sf = cx_curfn;
                cx_curfn = NULL;
                init_variable(v, d.at);
                cx_curfn = sf;
                if (!v->is_inline && !v->has_const)
                    cx_error(d.at, "a static data member initialized in "
                                   "its class must be const integral or "
                                   "inline");
            }
        } else {
            struct cfield *fl = xcalloc(1, sizeof *fl);
            fl->name = d.name;
            fl->type = t;
            fl->access = *access;
            fl->is_mutable = ds.is_mutable;
            fl->bitwidth = -1;
            fl->dflt_tok = -1;
            if (t->k == CT_AUTO)
                cx_error(at, "a member cannot be 'auto'");
            if (!ct_is_complete(t) && !(t->k == CT_ARRAY && t->n < 0 &&
                                        ct_is_complete(t->to)))
                cx_error(d.at ? d.at : at, "field '%s' has incomplete type %s",
                         d.name ? d.name : "", ct_name(t));
            if (cx_accept(TOK_COLON)) {
                fl->bitwidth = (int)expr_parse_const("a bit-field width");
                if (!ct_is_integer(t))
                    cx_error(at, "bit-field '%s' has non-integral type",
                             d.name ? d.name : "");
            }
            add_field(c, fl);
            if (d.name) {
                struct csym *y = scope_add(c->scope, CS_FIELD, d.name);
                y->field = fl;
                y->type = t;
                y->access = *access;
            }
            if (cx_kind() == TOK_ASSIGN || cx_kind() == TOK_LBRACE) {
                skip_default_init(fl);
                add_pending(NULL, c, fl);
            }
        }
        if (cx_accept(TOK_COMMA))
            continue;
        cx_expect(TOK_SEMI, "';' after a member declaration");
        return;
    }
}

/* ---- namespace-scope declarations ---- */

static void parse_static_assert(void)
{
    const struct ctok *at = cx_cur();
    cx_advance();
    cx_expect(TOK_LPAREN, "'(' after static_assert");
    long v = expr_parse_const("a static_assert condition");
    const char *msg = NULL;
    if (cx_accept(TOK_COMMA)) {
        if (cx_kind() == TOK_STR)
            msg = cx_cur()->t.text;
        while (cx_kind() == TOK_STR)
            cx_advance();
    }
    cx_expect(TOK_RPAREN, "')'");
    cx_expect(TOK_SEMI, "';'");
    if (!v)
        cx_error(at, "static assertion failed%s%s", msg ? ": " : "",
                 msg ? msg : "");
}

/* using namespace N; / using N::x; / using T = type; */
static void parse_using(void)
{
    const struct ctok *at = cx_cur();
    cx_advance();
    if (cx_accept(TOK_CX_NAMESPACE)) {
        struct qname q = peek_qname();
        if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
            cx_error(at, "expected a namespace name");
        cx_pos += q.fin;
        struct csym *y = find_final(q.scope, cx_cur()->t.text);
        if (!y || y->k != CS_NAMESPACE)
            cx_error(cx_cur(), "'%s' is not a namespace", cx_cur()->t.text);
        cx_advance();
        cx_expect(TOK_SEMI, "';'");
        struct cscope *s = cx_scope;
        s->usings = xrealloc(s->usings, (size_t)(s->nusings + 1) *
                                        sizeof *s->usings);
        s->usings[s->nusings++] = y->ns;
        return;
    }
    if (cx_kind() == TOK_IDENT && cx_kind_at(1) == TOK_ASSIGN) {
        const char *name = cx_cur()->t.text;
        cx_advance();
        cx_advance();
        struct cty *t = parse_type_id();
        cx_expect(TOK_SEMI, "';'");
        struct csym *y = scope_add(cx_scope, CS_TYPEDEF, name);
        y->type = t;
        return;
    }
    cx_accept(TOK_CX_TYPENAME);
    struct qname q = peek_qname();
    if (q.bad || !q.scope)
        cx_error(at, "expected a qualified name after 'using'");
    cx_pos += q.fin;
    const char *name;
    if (cx_kind() == TOK_CX_OPERATOR) {
        struct cty *conv = NULL;
        name = parse_operator_name(&conv);
    } else {
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a name after '::'");
        name = cx_cur()->t.text;
        cx_advance();
    }
    cx_expect(TOK_SEMI, "';'");
    struct csym *src = lookup_in(q.scope, name);
    if (!src)
        cx_error(at, "'%s' is not declared in '%s'", name,
                 q.scope->name ? q.scope->name : "::");
    if (src->k == CS_FUNC) {
        struct csym *mine = scope_find_here(cx_scope, name);
        if (mine && mine->k == CS_FUNC) {
            /* add the named overloads to what is declared here */
            for (struct cfunc *f = src->fns; f; f = f->next) {
                int have = 0;
                for (struct cfunc *g = mine->fns; g; g = g->next)
                    have |= g == f;
                if (!have)
                    cx_error(at, "a using-declaration beside local "
                                 "overloads of '%s' is not supported yet",
                             name);
            }
            return;
        }
    }
    struct csym *y = scope_add(cx_scope, src->k, name);
    struct csym *hn = y->hnext, *nx = y->next;
    struct cscope *sc = y->scope;
    *y = *src;
    y->hnext = hn;
    y->next = nx;
    y->scope = sc;
    /* a class brought in also keeps its tag */
    if (src->k != CS_CLASS && src->k != CS_ENUM) {
        struct csym *tag = scope_find_tag(q.scope, name);
        if (tag) {
            struct csym *t2 = scope_add(cx_scope, tag->k, name);
            t2->type = tag->type;
        }
    }
}

static void parse_declaration(int toplevel, struct cstmt **out);

static void parse_namespace(void)
{
    const struct ctok *at = cx_cur();
    int is_inline = 0;
    if (cx_kind() == TOK_KW_INLINE) {
        is_inline = 1;
        cx_advance();
    }
    cx_advance();                               /* namespace */
    parse_attrs(NULL);
    const char *names[16];
    int nn = 0;
    if (cx_kind() == TOK_IDENT) {
        names[nn++] = cx_cur()->t.text;
        cx_advance();
        while (cx_accept(TOK_COLONCOLON)) {
            cx_accept(TOK_KW_INLINE);
            if (cx_kind() != TOK_IDENT || nn == 16)
                cx_error(cx_cur(), "expected a namespace name");
            names[nn++] = cx_cur()->t.text;
            cx_advance();
        }
    }
    parse_attrs(NULL);
    if (nn == 1 && cx_kind() == TOK_ASSIGN) {       /* namespace alias */
        cx_advance();
        struct qname q = peek_qname();
        if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
            cx_error(at, "expected a namespace name");
        cx_pos += q.fin;
        struct csym *y = find_final(q.scope, cx_cur()->t.text);
        if (!y || y->k != CS_NAMESPACE)
            cx_error(cx_cur(), "'%s' is not a namespace", cx_cur()->t.text);
        cx_advance();
        cx_expect(TOK_SEMI, "';'");
        struct csym *a = scope_add(cx_scope, CS_NAMESPACE, names[0]);
        a->ns = y->ns;
        return;
    }
    struct cscope *save = cx_scope;
    int opened = 0;
    if (nn == 0) {
        /* one unnamed namespace per scope, reopened each time */
        struct csym *y = scope_find_here(cx_scope, "(anonymous)");
        if (!y) {
            y = scope_add(cx_scope, CS_NAMESPACE, "(anonymous)");
            y->ns = scope_new(SC_NAMESPACE, NULL, cx_scope);
            y->ns->anon = 1;
            cx_scope->usings = xrealloc(cx_scope->usings,
                                        (size_t)(cx_scope->nusings + 1) *
                                        sizeof *cx_scope->usings);
            cx_scope->usings[cx_scope->nusings++] = y->ns;
        }
        cx_scope = y->ns;
        opened = 1;
    }
    for (int i = 0; i < nn; i++) {
        struct csym *y = scope_find_here(cx_scope, names[i]);
        if (y && y->k != CS_NAMESPACE)
            cx_error(at, "'%s' is not a namespace", names[i]);
        if (!y) {
            y = scope_add(cx_scope, CS_NAMESPACE, names[i]);
            y->ns = scope_new(SC_NAMESPACE, names[i], cx_scope);
            if (is_inline && i == nn - 1) {
                y->ns->is_inline = 1;
                cx_scope->usings = xrealloc(cx_scope->usings,
                                            (size_t)(cx_scope->nusings + 1) *
                                            sizeof *cx_scope->usings);
                cx_scope->usings[cx_scope->nusings++] = y->ns;
            }
        }
        cx_scope = y->ns;
        opened = 1;
    }
    (void)opened;
    cx_expect(TOK_LBRACE, "'{' after the namespace name");
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() == TOK_EOF)
            cx_error(at, "unterminated namespace");
        parse_declaration(1, NULL);
    }
    cx_advance();
    cx_scope = save;
}

static void parse_linkage_spec(void)
{
    const struct ctok *at = cx_cur();
    cx_advance();                               /* extern */
    const char *lang = cx_cur()->t.text;
    while (cx_kind() == TOK_STR)
        cx_advance();
    int c = strcmp(lang, "C") == 0;
    if (!c && strcmp(lang, "C++") != 0)
        cx_error(at, "unknown language linkage \"%s\"", lang);
    int save = cx_extern_c;
    if (cx_kind() == TOK_LBRACE) {
        cx_advance();
        cx_extern_c = c ? 2 : 0;
        while (cx_kind() != TOK_RBRACE) {
            if (cx_kind() == TOK_EOF)
                cx_error(at, "unterminated extern \"%s\" block", lang);
            parse_declaration(1, NULL);
        }
        cx_advance();
    } else {
        /* extern "C" int x; is a declaration, not a definition */
        cx_extern_c = c ? 1 : 0;
        parse_declaration(1, NULL);
    }
    cx_extern_c = save;
}

/* A declaration in namespace scope (toplevel) or block scope: in a block,
 * each variable's S_DECL is appended to *out. */
static void parse_declaration(int toplevel, struct cstmt **out)
{
    const struct ctok *at = cx_cur();
    switch (cx_kind()) {
    case TOK_SEMI:
        cx_advance();
        return;
    case TOK_CX_NAMESPACE:
        if (!toplevel)
            break;
        parse_namespace();
        return;
    case TOK_KW_INLINE:
        if (toplevel && cx_kind_at(1) == TOK_CX_NAMESPACE) {
            parse_namespace();
            return;
        }
        break;
    case TOK_CX_USING:
        parse_using();
        return;
    case TOK_KW_EXTERN:
        if (cx_kind_at(1) == TOK_STR) {
            parse_linkage_spec();
            return;
        }
        break;
    case TOK_KW_STATIC_ASSERT:
        parse_static_assert();
        return;
    case TOK_CX_TEMPLATE:
        cx_error(at, "templates are not supported yet (CX4)");
        return;
    case TOK_CX_EXPORT:
        cx_error(at, "modules are not supported");
        return;
    case TOK_KW_ASM:
        if (toplevel)
            cx_error(at, "file-scope asm in C++ is not supported yet");
        break;
    default:
        break;
    }
    struct dspec ds;
    parse_dspec(&ds);
    if (!ds.type && !at_ctor_declarator() && cx_kind() != TOK_TILDE &&
        !(toplevel && cx_kind() == TOK_IDENT && cx_kind_at(1) == TOK_COLONCOLON))
        cx_error(cx_cur(), "expected a declaration before %s",
                 tok_describe(&cx_cur()->t));
    if (cx_kind() == TOK_SEMI) {
        cx_advance();
        return;
    }
    struct cstmt *tail = NULL;
    for (;;) {
        struct declarator d;
        memset(&d, 0, sizeof d);
        struct cscope *here = cx_scope;
        struct cty *base = ds.type ? ds.type : ct_basic(CT_VOID);
        struct cty *t = parse_declarator(base, &d,
                                         DK_NAMED | (ds.storage == SK_TYPEDEF
                                                     ? 0 : DK_INIT));
        parse_asm_label(&d.a);
        parse_attrs(&d.a);
        if (!ds.type && d.kind != DN_CTOR && d.kind != DN_DTOR &&
            d.kind != DN_CONV)
            cx_error(d.at, "'%s' has no type", d.name);
        if (ds.storage == SK_TYPEDEF) {
            struct csym *old = scope_find_here(cx_scope, d.name);
            if (old && old->k == CS_TYPEDEF && ct_same(old->type, t))
                ;                                   /* a repeat typedef */
            else if (old && old->k != CS_CLASS && old->k != CS_ENUM)
                cx_error(d.at, "'%s' redeclared as a typedef", d.name);
            else {
                struct csym *y = scope_add(cx_scope, CS_TYPEDEF, d.name);
                y->type = t;
                /* an unnamed class takes its first typedef name for
                 * linkage (typedef struct { } T;) */
                if (t->k == CT_CLASS && t->cls->anon && !t->cls->name) {
                    t->cls->name = d.name;
                    t->cls->scope->name = d.name;
                    if (!t->cls->local)
                        t->cls->cname = cx_fmt("_C%s",
                                               mangle_class_name(t->cls));
                }
                if (t->k == CT_ENUM && !t->en->name)
                    t->en->name = d.name;
            }
        } else if (t->k == CT_FUNC) {
            struct cscope *target = d.qual ? d.qual : here;
            if (!toplevel && !d.qual)
                target = enclosing_ns(here);
            if (d.qual && d.kind != DN_CTOR && d.kind != DN_DTOR) {
                struct csym *y = scope_find_here(d.qual, d.name);
                if (!y || y->k != CS_FUNC)
                    cx_error(d.at, "no function '%s' declared in '%s'",
                             d.name, d.qual->name ? d.qual->name : "::");
            }
            if (d.kind == DN_CONV)
                t->to = d.conv_type;
            struct cfunc *f = declare_function(&ds, &d, t, target);
            if (!toplevel && !d.qual && target != here) {
                /* a block-scope function declaration: visible here */
                struct csym *y = func_sym(here, d.name, d.at);
                if (!y->fns)
                    y->fns = f;
            }
            if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON ||
                cx_kind() == TOK_CX_TRY) {
                if (!toplevel)
                    cx_error(cx_cur(), "a function definition is not "
                                       "allowed here");
                if (cx_kind() == TOK_CX_TRY)
                    cx_error(cx_cur(), "function-try-blocks are not "
                                       "supported yet (CX5)");
                f->def_scope = cx_scope;
                define_function(f, t);
                if (d.saved)
                    cx_scope = d.saved;
                return;
            }
            if (cx_accept(TOK_ASSIGN)) {
                if (cx_accept(TOK_KW_DEFAULT))
                    f->is_defaulted = 1;
                else if (cx_accept(TOK_CX_DELETE))
                    f->is_deleted = 1;
                else
                    cx_error(cx_cur(), "expected 'default' or 'delete'");
            }
        } else if (toplevel || ds.storage == SK_EXTERN) {
            if (t->k == CT_VOID)
                cx_error(d.at, "variable '%s' declared void", d.name);
            if (toplevel) {
                declare_global_var(&ds, &d, t);
            } else {
                /* extern in a block: the namespace-scope variable */
                struct cscope *ns = enclosing_ns(cx_scope);
                struct csym *g = scope_find_here(ns, d.name);
                struct cvar *v;
                if (g && g->k == CS_VAR) {
                    v = g->var;
                } else {
                    v = xcalloc(1, sizeof *v);
                    v->name = d.name;
                    v->type = t;
                    v->owner = ns;
                    v->is_extern = 1;
                    v->c_linkage = cx_extern_c;
                    v->cname = ns == cx_global ? v->name : mangle_var(v, ns);
                    g = scope_add(ns, CS_VAR, d.name);
                    g->var = v;
                    gvar_register(v);
                }
                struct csym *y = scope_add(cx_scope, CS_VAR, d.name);
                y->var = v;
            }
        } else {
            /* a local variable */
            if (t->k == CT_VOID)
                cx_error(d.at, "variable '%s' declared void", d.name);
            struct cvar *v = new_local(d.name, t, d.at);
            v->is_static = ds.storage == SK_STATIC;
            if (v->is_static)
                v->disc = static_disc(cx_curfn, d.name);
            if (ds.is_constexpr) {
                v->is_constexpr = 1;
                v->type = ct_qual(v->type, CQ_CONST);
            }
            if (ds.a.aligned || d.a.aligned)
                v->align_attr = ds.a.aligned ? ds.a.aligned : d.a.aligned;
            struct cstmt *s = st_new(S_DECL);
            s->line = d.at->t.line;
            s->file = d.at->file;
            s->var = v;
            struct csym *old = scope_find_here(cx_scope, d.name);
            if (old && (old->k == CS_VAR) &&
                (cx_scope->k == SC_BLOCK || cx_scope->k == SC_PARAMS))
                cx_error(d.at, "redeclaration of '%s'", d.name);
            struct csym *y = scope_add(cx_scope, CS_VAR, d.name);
            y->var = v;
            init_variable(v, d.at);
            if (v->type->k == CT_CLASS && !v->is_static)
                s->dtor = class_dtor(v->type->cls) != NULL;
            if (v->type->k == CT_ARRAY && !v->is_static) {
                struct cty *e = v->type;
                while (e->k == CT_ARRAY)
                    e = e->to;
                if (e->k == CT_CLASS)
                    s->dtor = class_dtor(e->cls) != NULL;
            }
            if (out) {
                if (tail)
                    tail->more = s;
                else
                    *out = s;
                tail = s;
            }
        }
        if (d.saved)
            cx_scope = d.saved;
        if (cx_accept(TOK_COMMA))
            continue;
        cx_expect(TOK_SEMI, "';' after the declaration");
        return;
    }
}

/* ---- statements ---- */

static struct cstmt *parse_block_body(struct cscope *s)
{
    struct cstmt *b = st_new(S_BLOCK);
    struct cstmt *saveblk = cx_curblk;
    b->blk = cx_curblk;
    cx_curblk = b;
    (void)s;
    cx_expect(TOK_LBRACE, "'{'");
    struct cstmt **tail = &b->body;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() == TOK_EOF)
            cx_error(cx_cur(), "unterminated block");
        struct cstmt *st = parse_stmt();
        if (!st)
            continue;
        *tail = st;
        while (st->next)
            st = st->next;
        tail = &st->next;
    }
    cx_advance();
    cx_curblk = saveblk;
    return b;
}

struct cstmt *parse_compound(void)
{
    struct cscope *s = scope_push(SC_BLOCK, NULL);
    struct cstmt *b = parse_block_body(s);
    scope_pop();
    return b;
}

/* A declaration or expression statement? A statement that can be read as
 * a declaration is one (the standard's rule), which `T(x);` exercises. */
static int at_decl_stmt(void)
{
    enum tok_kind k = cx_kind();
    if (k == TOK_IDENT || k == TOK_COLONCOLON) {
        int n;
        if (!peek_type_name(&n))
            return 0;
        enum tok_kind nk = cx_kind_at(n);
        if (nk == TOK_LPAREN) {
            /* T(x) ... : a declarator in parentheses, or a functional
             * cast. A declarator: (name) or (*...) or (&...). */
            enum tok_kind a = cx_kind_at(n + 1);
            if (a == TOK_STAR || a == TOK_AMP || a == TOK_ANDAND)
                return 1;
            if (a == TOK_IDENT && cx_kind_at(n + 2) == TOK_RPAREN) {
                int save = cx_pos;
                cx_pos += n + 1;
                int is_t = at_type_start();
                cx_pos = save;
                enum tok_kind after = cx_kind_at(n + 3);
                return !is_t && (after == TOK_SEMI || after == TOK_ASSIGN ||
                                 after == TOK_COMMA || after == TOK_LBRACKET ||
                                 after == TOK_LPAREN);
            }
            return 0;
        }
        if (nk == TOK_LBRACE || nk == TOK_DOT || nk == TOK_ARROW ||
            nk == TOK_COLONCOLON)
            return 0;
        return 1;
    }
    if (k == TOK_KW_TYPEOF || k == TOK_CX_DECLTYPE) {
        int save = cx_pos;
        cx_advance();
        cx_skip_balanced();
        enum tok_kind nk = cx_kind();
        cx_pos = save;
        return nk != TOK_DOT && nk != TOK_ARROW && nk != TOK_LPAREN;
    }
    if (k == TOK_KW_ATTRIBUTE)
        return 1;
    if (k == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET)
        return 1;
    return is_decl_keyword(k) && k != TOK_KW_ASM;
}

static struct cstmt *expr_stmt(void)
{
    struct cstmt *s = st_new(S_EXPR);
    s->e = expr_parse();
    cx_expect(TOK_SEMI, "';' after the expression");
    return s;
}

/* A condition: an expression, or a declaration `T x = e` whose value it
 * tests. The declaration goes in *decl. */
static struct cexpr *parse_condition(struct cstmt **decl)
{
    *decl = NULL;
    if (at_decl_stmt() && cx_kind() != TOK_LPAREN) {
        struct dspec ds;
        const struct ctok *at = cx_cur();
        parse_dspec(&ds);
        struct declarator d;
        memset(&d, 0, sizeof d);
        struct cty *t = parse_declarator(ds.type, &d, DK_NAMED);
        struct cvar *v = new_local(d.name, t, at);
        struct cstmt *s = st_new(S_DECL);
        s->var = v;
        struct csym *y = scope_add(cx_scope, CS_VAR, d.name);
        y->var = v;
        if (cx_kind() != TOK_ASSIGN && cx_kind() != TOK_LBRACE)
            cx_error(cx_cur(), "a condition's declaration needs an "
                               "initializer");
        init_variable(v, at);
        if (v->type->k == CT_CLASS)
            s->dtor = class_dtor(v->type->cls) != NULL;
        *decl = s;
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(v->type), VC_LVALUE);
        e->var = v;
        return e;
    }
    return expr_parse();
}

/* `return x;` naming a local object (or by-value parameter) of the
 * function's class return type: a candidate for the named return value
 * optimization, and moved from rather than copied (11.9.6, 7.5.4.2). */
static struct cvar *returned_local(struct cexpr *e, struct cty *rt)
{
    if (e->k != E_VAR || rt->k != CT_CLASS)
        return NULL;
    struct cvar *v = e->var;
    if (!v->is_local || v->is_static || ct_is_ref(v->type) ||
        v->fn != cx_curfn || !ct_same_unqual(v->type, rt) ||
        (v->type->q & CQ_VOLATILE))
        return NULL;
    return v;
}

/* The local, as an xvalue — if its class has a constructor that takes
 * one (else it is copied, as before C++11). */
static struct cexpr *implicit_move(struct cexpr *e, struct cty *rt)
{
    struct cexpr *x = ex_new(E_CAST, e->t, VC_XVALUE);
    x->a = xmalloc(sizeof *x->a);
    x->a[0] = e;
    x->na = 1;
    x->lvcast = 1;
    if (resolve_ex(rt->cls->ctors, NULL, &x, 1, NULL, NULL, 0))
        return x;
    return e;
}

/* Every return of f returns the same local by name: that local is built
 * in the caller's return slot (NRVO), when the class goes through one. */
static void collect_returns(struct cstmt *s, struct cvar **v, int *ok,
                            int *n)
{
    for (; s; s = s->next) {
        if (s->k == S_RETURN) {
            (*n)++;
            if (!s->ret_var || (*v && *v != s->ret_var))
                *ok = 0;
            *v = s->ret_var;
        }
        collect_returns(s->body, v, ok, n);
        collect_returns(s->els, v, ok, n);
        if (s->k == S_IF || s->k == S_WHILE || s->k == S_FOR ||
            s->k == S_SWITCH)
            collect_returns(s->init, v, ok, n);
    }
}

static void mark_nrvo(struct cfunc *f)
{
    struct cty *rt = f->type->to;
    if (!class_indirect(rt) || !f->body)
        return;
    struct cvar *v = NULL;
    int ok = 1, n = 0;
    collect_returns(f->body->body, &v, &ok, &n);
    if (ok && n && v && !v->is_param)
        v->nrvo = 1;
}

static struct cstmt *parse_stmt(void)
{
    const struct ctok *at = cx_cur();
    struct cstmt *s;
    switch (cx_kind()) {
    case TOK_LBRACE:
        return parse_compound();
    case TOK_SEMI:
        cx_advance();
        return st_new(S_NULL);
    case TOK_KW_IF: {
        s = st_new(S_IF);
        cx_advance();
        if (cx_accept(TOK_CX_CONSTEXPR))
            ;                                   /* folded like any if */
        cx_expect(TOK_LPAREN, "'(' after if");
        scope_push(SC_BLOCK, NULL);
        struct cstmt *decl = NULL;
        /* C++17 init-statement: if (init; cond) */
        int save = cx_pos;
        int has_init = 0;
        {
            int depth = 0;
            for (int i = cx_pos; i < cx_ntoks; i++) {
                enum tok_kind k = cx_toks[i].t.kind;
                if (k == TOK_LPAREN || k == TOK_LBRACE || k == TOK_LBRACKET)
                    depth++;
                else if (k == TOK_RPAREN || k == TOK_RBRACE ||
                         k == TOK_RBRACKET) {
                    if (depth-- == 0)
                        break;
                } else if (k == TOK_SEMI && depth == 0) {
                    has_init = 1;
                    break;
                }
            }
        }
        cx_pos = save;
        struct cstmt *init = NULL;
        if (has_init) {
            if (at_decl_stmt())
                parse_declaration(0, &init);
            else
                init = expr_stmt();
        }
        s->e = convert_bool(parse_condition(&decl), "an if condition");
        if (decl) {
            if (init) {
                struct cstmt *l = init;
                while (l->more)
                    l = l->more;
                l->more = decl;
            } else {
                init = decl;
            }
        }
        s->init = init;
        cx_expect(TOK_RPAREN, "')' after the condition");
        s->body = parse_stmt();
        if (cx_accept(TOK_KW_ELSE))
            s->els = parse_stmt();
        scope_pop();
        return s;
    }
    case TOK_KW_WHILE: {
        s = st_new(S_WHILE);
        cx_advance();
        cx_expect(TOK_LPAREN, "'(' after while");
        scope_push(SC_BLOCK, NULL);
        s->e = convert_bool(parse_condition(&s->init), "a while condition");
        cx_expect(TOK_RPAREN, "')' after the condition");
        s->body = parse_stmt();
        scope_pop();
        return s;
    }
    case TOK_KW_DO:
        s = st_new(S_DO);
        cx_advance();
        s->body = parse_stmt();
        cx_expect(TOK_KW_WHILE, "'while' after a do body");
        cx_expect(TOK_LPAREN, "'('");
        s->e = convert_bool(expr_parse(), "a do-while condition");
        cx_expect(TOK_RPAREN, "')'");
        cx_expect(TOK_SEMI, "';' after do-while");
        return s;
    case TOK_KW_FOR: {
        s = st_new(S_FOR);
        cx_advance();
        cx_expect(TOK_LPAREN, "'(' after for");
        scope_push(SC_BLOCK, NULL);
        if (cx_kind() == TOK_SEMI) {
            cx_advance();
        } else if (at_decl_stmt()) {
            /* range-for: `for (decl : range)` */
            int depth = 0;
            for (int i = cx_pos; i < cx_ntoks; i++) {
                enum tok_kind k = cx_toks[i].t.kind;
                if (k == TOK_LPAREN || k == TOK_LBRACE || k == TOK_LBRACKET)
                    depth++;
                else if (k == TOK_RPAREN || k == TOK_RBRACE ||
                         k == TOK_RBRACKET) {
                    if (depth-- == 0)
                        break;
                } else if (depth == 0 && k == TOK_SEMI) {
                    break;
                } else if (depth == 0 && k == TOK_COLON) {
                    cx_error(at, "range-based for is not supported yet "
                                 "(CX6)");
                }
            }
            parse_declaration(0, &s->init);
        } else {
            s->init = expr_stmt();
        }
        if (cx_kind() != TOK_SEMI)
            s->e = convert_bool(expr_parse(), "a for condition");
        cx_expect(TOK_SEMI, "';' in for");
        if (cx_kind() != TOK_RPAREN)
            s->e2 = expr_parse();
        cx_expect(TOK_RPAREN, "')' after for");
        s->body = parse_stmt();
        scope_pop();
        return s;
    }
    case TOK_KW_SWITCH: {
        s = st_new(S_SWITCH);
        cx_advance();
        cx_expect(TOK_LPAREN, "'(' after switch");
        scope_push(SC_BLOCK, NULL);
        struct cexpr *e = rvalue(parse_condition(&s->init));
        if (!ct_is_integer(e->t))
            cx_error(at, "switch on a non-integral %s", ct_name(e->t));
        s->e = convert(e, ct_promote(e->t), "a switch");
        cx_expect(TOK_RPAREN, "')'");
        s->body = parse_stmt();
        scope_pop();
        return s;
    }
    case TOK_KW_CASE:
        s = st_new(S_CASE);
        cx_advance();
        s->cval = expr_parse_const("a case label");
        if (cx_accept(TOK_ELLIPSIS)) {
            s->is_range = 1;
            s->cval2 = expr_parse_const("a case range's end");
        }
        cx_expect(TOK_COLON, "':' after case");
        s->body = cx_kind() == TOK_RBRACE ? st_new(S_NULL) : parse_stmt();
        return s;
    case TOK_KW_DEFAULT:
        s = st_new(S_DEFAULT);
        cx_advance();
        cx_expect(TOK_COLON, "':' after default");
        s->body = cx_kind() == TOK_RBRACE ? st_new(S_NULL) : parse_stmt();
        return s;
    case TOK_KW_BREAK:
        s = st_new(S_BREAK);
        cx_advance();
        cx_expect(TOK_SEMI, "';' after break");
        return s;
    case TOK_KW_CONTINUE:
        s = st_new(S_CONTINUE);
        cx_advance();
        cx_expect(TOK_SEMI, "';' after continue");
        return s;
    case TOK_KW_GOTO:
        s = st_new(S_GOTO);
        cx_advance();
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "computed goto is not supported in C++");
        s->label = cx_cur()->t.text;
        cx_advance();
        cx_expect(TOK_SEMI, "';' after goto");
        return s;
    case TOK_KW_RETURN: {
        s = st_new(S_RETURN);
        cx_advance();
        struct cty *rt = cx_curfn->type->to;
        if (cx_kind() != TOK_SEMI) {
            struct cexpr *e;
            enum init_form form = INIT_COPY;
            if (cx_kind() == TOK_LBRACE) {
                e = parse_braced_list();
                form = INIT_COPY_LIST;
            } else {
                e = expr_parse();
            }
            if (rt->k == CT_VOID) {
                if (form != INIT_COPY || e->t->k != CT_VOID)
                    cx_error(at, "a void function returns no value");
                s->e = e;
            } else if (ct_is_ref(rt)) {
                s->e = bind_ref(e, rt, "return");
            } else {
                struct cvar *v = form == INIT_COPY ? returned_local(e, rt)
                                                   : NULL;
                if (v) {
                    s->ret_var = v;
                    e = implicit_move(e, rt);
                }
                s->e = init_object(rt, form, &e, 1, at);
            }
        } else if (rt->k != CT_VOID && !cx_curfn->is_ctor &&
                   !cx_curfn->is_dtor) {
            cx_error(at, "a non-void function returns a value");
        }
        cx_expect(TOK_SEMI, "';' after return");
        return s;
    }
    case TOK_KW_ASM: {
        /* a GNU asm statement: passed to C as written */
        s = st_new(S_ASM);
        int start = cx_pos;
        cx_advance();
        while (cx_kind() == TOK_KW_VOLATILE || cx_kind() == TOK_KW_INLINE ||
               cx_kind() == TOK_KW_GOTO)
            cx_advance();
        if (cx_kind() != TOK_LPAREN)
            cx_error(cx_cur(), "expected '(' after asm");
        cx_skip_balanced();
        cx_expect(TOK_SEMI, "';' after asm");
        (void)start;
        cx_error(at, "asm statements in C++ are not supported yet");
        return s;
    }
    case TOK_CX_TRY:
        cx_error(at, "exceptions are not supported yet (CX5)");
        return NULL;
    case TOK_CX_USING:
        parse_using();
        return NULL;
    case TOK_KW_STATIC_ASSERT:
        parse_static_assert();
        return NULL;
    case TOK_IDENT:
        if (cx_kind_at(1) == TOK_COLON) {
            s = st_new(S_LABEL);
            s->label = cx_cur()->t.text;
            cx_advance();
            cx_advance();
            parse_attrs(NULL);
            s->body = cx_kind() == TOK_RBRACE ? st_new(S_NULL) : parse_stmt();
            return s;
        }
        break;
    default:
        break;
    }
    if (at_decl_stmt()) {
        struct cstmt *first = NULL;
        parse_declaration(0, &first);
        if (!first)
            return NULL;
        /* chain the declarators as statements */
        for (struct cstmt *d = first; d; d = d->more)
            d->next = d->more;
        return first;
    }
    return expr_stmt();
}

/* ---- the unit ---- */

/* The global operator new/delete are declared in every unit (6.7.5.5.2). */
static void declare_builtin_ops(void)
{
    static const struct {
        const char *name;
        int ret_void;
        int second;               /* 0 none, 1 size_t */
    } ops[] = {
        { "operator new", 0, 0 }, { "operator new[]", 0, 0 },
        { "operator delete", 1, 0 }, { "operator delete[]", 1, 0 },
        { "operator delete", 1, 1 }, { "operator delete[]", 1, 1 },
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        struct cty *p[2];
        int np = 0;
        if (ops[i].ret_void)
            p[np++] = ct_ptr(ct_basic(CT_VOID));
        else
            p[np++] = ct_size_t();
        if (ops[i].second)
            p[np++] = ct_size_t();
        struct cty *ft = ct_func(ops[i].ret_void ? ct_basic(CT_VOID)
                                                 : ct_ptr(ct_basic(CT_VOID)),
                                 p, np, 0);
        struct dspec ds;
        memset(&ds, 0, sizeof ds);
        struct declarator d;
        memset(&d, 0, sizeof d);
        d.kind = DN_OPERATOR;
        d.name = ops[i].name;
        struct cfunc *f = declare_function(&ds, &d, ft, cx_global);
        f->is_builtin = 1;
    }
}

void cx_parse_unit(void)
{
    cx_funcs = NULL;
    funcs_tail = NULL;
    cx_gvars = NULL;
    cx_ngvars = capgvars = 0;
    cx_classes = NULL;
    cx_nclasses = capclasses = 0;
    npend = 0;
    class_depth = 0;
    cx_curfn = NULL;
    cx_curblk = NULL;
    cx_extern_c = 0;
    declare_builtin_ops();
    while (cx_kind() != TOK_EOF)
        parse_declaration(1, NULL);
}
