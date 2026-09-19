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

#include <setjmp.h>
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

/* v, declared before, is defined here: its place in the unit's order of
 * initialization is its definition's (6.9.3.3), not its declaration's */
static void gvar_defined_here(struct cvar *v)
{
    for (int i = cx_ngvars - 1; i >= 0; i--)
        if (cx_gvars[i] == v) {
            memmove(&cx_gvars[i], &cx_gvars[i + 1],
                    (size_t)(cx_ngvars - 1 - i) * sizeof *cx_gvars);
            cx_gvars[cx_ngvars - 1] = v;
            return;
        }
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
    int no_unique_address;    /* [[no_unique_address]]: a member that may
                               * overlap others */
    long aligned;
    const char *section;
    const char *asm_name;
    const char **abi_tags;    /* __abi_tag__("...", ...), sorted */
    int nabi_tags;
};

/* a's ABI tags added to the set (v, *n) */
static void tags_from(const char ***v, int *n, const struct attrs *a)
{
    for (int i = 0; i < a->nabi_tags; i++)
        abi_tag_add(v, n, a->abi_tags[i]);
}

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
                    } else if (attr_is(n, "abi_tag")) {
                        cx_advance();
                        while (cx_kind() == TOK_STR) {
                            abi_tag_add(&a->abi_tags, &a->nabi_tags,
                                        cx_cur()->t.text);
                            cx_advance();
                            if (!cx_accept(TOK_COMMA))
                                break;
                        }
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
                if (cx_kind() == TOK_IDENT &&
                    attr_is(cx_cur()->t.text, "no_unique_address"))
                    a->no_unique_address = 1;
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
/* A GNU asm statement's string: adjacent literals are one. */
static const char *asm_string(const char *what)
{
    if (cx_kind() != TOK_STR)
        cx_error(cx_cur(), "expected a string for %s", what);
    const char *r = "";
    while (cx_kind() == TOK_STR) {
        r = cx_fmt("%s%s", r, cx_cur()->t.text);
        cx_advance();
    }
    return r;
}

/* [name] "constraint" (expression), ... — outputs lvalues */
static int asm_operands(struct casm_op **out, int output)
{
    int n = 0, cap = 0;
    *out = NULL;
    if (cx_kind() != TOK_STR && cx_kind() != TOK_LBRACKET)
        return 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            *out = xrealloc(*out, (size_t)cap * sizeof **out);
        }
        struct casm_op *o = &(*out)[n++];
        o->name = NULL;
        if (cx_accept(TOK_LBRACKET)) {
            if (cx_kind() != TOK_IDENT)
                cx_error(cx_cur(), "expected an operand name");
            o->name = cx_cur()->t.text;
            cx_advance();
            cx_expect(TOK_RBRACKET, "']'");
        }
        o->cons = asm_string("an operand's constraint");
        const struct ctok *at = cx_cur();
        cx_expect(TOK_LPAREN, "'(' before an asm operand");
        struct cexpr *e = expr_parse();
        cx_expect(TOK_RPAREN, "')' after an asm operand");
        if (e->t->k == CT_CLASS)
            cx_error(at, "an asm operand of class type '%s'",
                     ct_name(e->t));
        if (output && e->vc != VC_LVALUE)
            cx_error(at, "an asm output is not an lvalue");
        o->e = e;
        if (!cx_accept(TOK_COMMA))
            return n;
    }
}

/* asm [volatile] [inline] ( template [: outputs [: inputs [: clobbers]]] );
 * (a colon pair may be one `::` token) */
static struct cstmt *parse_asm_stmt(const struct ctok *at)
{
    struct cstmt *s = st_new(S_ASM);
    struct casm *a = xcalloc(1, sizeof *a);
    s->asm_ = a;
    s->line = at->t.line;
    s->file = at->file;
    cx_advance();
    for (;;) {
        if (cx_accept(TOK_KW_VOLATILE))
            a->is_volatile = 1;
        else if (cx_accept(TOK_KW_INLINE))
            a->is_inline = 1;
        else if (cx_kind() == TOK_KW_GOTO)
            cx_error(cx_cur(), "asm goto in C++ is not supported yet");
        else
            break;
    }
    cx_expect(TOK_LPAREN, "'(' after asm");
    a->tmpl = asm_string("the asm template");
    int part = 0;           /* the section the next colon opens */
    for (;;) {
        if (cx_accept(TOK_COLONCOLON))
            part += 2;
        else if (cx_accept(TOK_COLON))
            part += 1;
        else
            break;
        if (part > 3)
            cx_error(cx_cur(), "asm goto labels in C++ are not supported "
                               "yet");
        if (part == 1 && cx_kind() != TOK_COLON &&
            cx_kind() != TOK_COLONCOLON)
            a->nout = asm_operands(&a->outs, 1);
        else if (part == 2 && cx_kind() != TOK_COLON &&
                 cx_kind() != TOK_COLONCOLON)
            a->nin = asm_operands(&a->ins, 0);
        else if (part == 3)
            while (cx_kind() == TOK_STR) {
                a->clobs = xrealloc(a->clobs, (size_t)(a->nclob + 1) *
                                              sizeof *a->clobs);
                a->clobs[a->nclob++] = asm_string("a clobber");
                if (!cx_accept(TOK_COMMA))
                    break;
            }
    }
    cx_expect(TOK_RPAREN, "')' after the asm");
    cx_expect(TOK_SEMI, "';' after asm");
    if (!a->nout)
        a->is_volatile = 1;         /* (no outputs: implicitly volatile) */
    return s;
}

static void parse_asm_label(struct attrs *a)
{
    if (cx_kind() != TOK_KW_ASM || cx_kind_at(1) != TOK_LPAREN)
        return;
    cx_advance();
    cx_advance();
    if (cx_kind() != TOK_STR)
        cx_error(cx_cur(), "expected a string in an asm label");
    /* (adjacent literals are one: `"" "__xpg_strerror_r"`, newlib's
     * __ASMNAME with an empty __USER_LABEL_PREFIX__) */
    a->asm_name = "";
    while (cx_kind() == TOK_STR) {
        a->asm_name = cx_fmt("%s%s", a->asm_name, cx_cur()->t.text);
        cx_advance();
    }
    cx_expect(TOK_RPAREN, "')'");
}

/* ---- names ---- */

static struct cty *dep_member(struct cty *base, const char *name);
static int targ_dependent(const struct ctarg *a);
/* `typename` was just read: a dependent qualified name is a type */
static int dependent_type_ok;
/* peek_type_name's cursor state after the name: a template-id ending in
 * half of a `>>` leaves the other half (cx_skip_peek applies it) */
static int peek_half;

void cx_skip_peek(int n)
{
    cx_pos += n;
    cx_half_gt = peek_half;
    peek_half = 0;
}

/* The scope a namespace, class or enum name opens. */
static struct cscope *sym_scope(struct csym *y)
{
    if (!y)
        return NULL;
    switch (y->k) {
    case CS_NAMESPACE:
        return y->ns;
    case CS_CLASS: case CS_TYPEDEF: case CS_ENUM:
        if (y->type->k == CT_CLASS) {
            class_ensure(y->type->cls);     /* its members are needed */
            return y->type->cls->scope;
        }
        if (y->type->k == CT_ENUM)
            return y->type->en->scope;
        return NULL;
    default:
        return NULL;
    }
}

/* A template-id at the cursor (`name <`, name a class or alias template):
 * its type, the cursor moved past `>`; NULL (cursor unmoved) if it is not
 * one. In a pattern, dependent arguments make a CT_TID. */
/* A name that means a template before `<`: a template's, or an
 * instance's injected-class-name (13.8.2). */
static struct ctemplate *as_template(struct csym *y)
{
    if (!y)
        return NULL;
    if (y->k == CS_TEMPLATE)
        return y->tmpl;
    if ((y->k == CS_CLASS || y->k == CS_TYPEDEF) && y->type->k == CT_CLASS &&
        y->type->cls->tmpl && y->scope->k == SC_CLASS &&
        y->scope->cls == y->type->cls)
        return y->type->cls->tmpl;
    return NULL;
}

static struct cty *template_id_type(struct csym *y)
{
    struct ctemplate *t = as_template(y);
    if (!t || cx_kind_at(1) != TOK_LT)
        return NULL;
    if (t->kind != TK_CLASS && t->kind != TK_ALIAS)
        return NULL;
    const struct ctok *at = cx_cur();
    cx_advance();
    struct ctarg *args;
    int n = parse_template_args(t, &args);
    int dep = 0;
    for (int i = 0; i < n; i++)
        dep |= targ_dependent(&args[i]);
    if (dep || t->nparams < 0) {
        struct cty *d = xcalloc(1, sizeof *d);
        d->k = t->kind == TK_CLASS ? CT_TID : CT_DEP;
        d->tmpl = t;
        d->targs = args;
        d->ntargs = n;
        return d;
    }
    if (t->kind == TK_ALIAS)
        return alias_instance(t, args, n, at);
    return ct_class(class_instance(t, args, n, at));
}

struct qname peek_qname(void)
{
    struct qname q = { NULL, 0, 0, NULL };
    int save = cx_pos, save_half = cx_half_gt;
    if (cx_kind() == TOK_COLONCOLON) {
        q.scope = cx_global;
        cx_advance();
    }
    for (;;) {
        if (cx_kind() == TOK_CX_TEMPLATE && cx_kind_at(1) == TOK_IDENT)
            cx_advance();                   /* T::template X<...>:: */
        if (cx_kind() != TOK_IDENT)
            break;
        const char *n = cx_cur()->t.text;
        if (q.dep) {
            /* after a dependent qualifier: names known only later */
            if (cx_kind_at(1) == TOK_LT && cx_in_targs >= 0) {
                int p0 = cx_pos;
                cx_advance();
                skip_template_args();
                if (cx_kind() != TOK_COLONCOLON) {
                    cx_pos = p0;
                    break;
                }
                cx_advance();
                continue;
            }
            if (cx_kind_at(1) != TOK_COLONCOLON)
                break;
            q.dep = dep_member(q.dep, n);
            cx_advance();
            cx_advance();
            continue;
        }
        struct csym *y = q.scope ? lookup_in(q.scope, n) : lookup(cx_scope, n);
        if (cx_kind_at(1) == TOK_LT) {
            if (!as_template(y))
                break;
            int p0 = cx_pos;
            struct cty *t = template_id_type(y);
            if (!t || cx_kind() != TOK_COLONCOLON) {
                cx_pos = p0;
                cx_half_gt = 0;
                break;
            }
            cx_advance();
            if (t->k == CT_CLASS) {
                class_ensure(t->cls);
                q.scope = t->cls->scope;
            } else {
                q.dep = t;              /* a dependent template-id */
                q.scope = NULL;
            }
            continue;
        }
        if (cx_kind_at(1) != TOK_COLONCOLON)
            break;
        struct cscope *sc = sym_scope(y);
        if (!sc && y && (y->k == CS_TYPEDEF || y->k == CS_CLASS) &&
            ct_dependent(y->type)) {
            q.dep = y->type;            /* T:: in a pattern */
            q.scope = NULL;
            cx_advance();
            cx_advance();
            continue;
        }
        if (!sc) {
            y = q.scope ? scope_find_tag(q.scope, n) : lookup_tag(cx_scope, n);
            sc = sym_scope(y);
        }
        if (!sc) {
            q.bad = 1;
            break;
        }
        q.scope = sc;
        cx_advance();
        cx_advance();
    }
    q.fin = cx_pos - save;
    cx_pos = save;
    cx_half_gt = save_half;
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
    if (cx_kind() == TOK_IDENT &&
        (strcmp(cx_cur()->t.text, "__int128_t") == 0 ||
         strcmp(cx_cur()->t.text, "__uint128_t") == 0)) {
        /* (GCC's typedefs for the 128-bit integers) */
        *ntok = 1;
        return ct_basic(cx_cur()->t.text[2] == 'u' ? CT_UINT128 : CT_INT128);
    }
    if (cx_kind() != TOK_IDENT && cx_kind() != TOK_COLONCOLON &&
        cx_kind() != TOK_CX_TEMPLATE)
        return NULL;
    struct qname q = peek_qname();
    if (q.bad)
        return NULL;
    int at_template = cx_kind_at(q.fin) == TOK_CX_TEMPLATE;
    if (cx_kind_at(q.fin + at_template) != TOK_IDENT)
        return NULL;
    if (q.dep) {
        /* typename T::type (a pattern): known only at instantiation */
        if (!dependent_type_ok && !cx_pattern)
            return NULL;
        int save = cx_pos;
        cx_pos += q.fin + at_template;
        struct cty *d = dep_member(q.dep, cx_cur()->t.text);
        cx_advance();
        if (cx_kind() == TOK_LT)
            skip_template_args();
        *ntok = cx_pos - save;
        peek_half = cx_half_gt;
        cx_pos = save;
        cx_half_gt = 0;
        return d;
    }
    struct csym *y = find_final(q.scope, tok_text(q.fin));
    if (y && y->k == CS_TEMPLATE && y->tmpl->kind == TK_CLASS &&
        !y->tmpl->tparam && cx_kind_at(q.fin + 1) == TOK_IDENT &&
        !cx_pattern) {
        /* C x(args): a placeholder for a deduced class type (9.2.9.8),
         * C's arguments deduced from the initializer */
        struct cty *d = xcalloc(1, sizeof *d);
        d->k = CT_AUTO;
        d->tmpl = y->tmpl;
        *ntok = q.fin + 1;
        peek_half = 0;
        return d;
    }
    if (as_template(y) && cx_kind_at(q.fin + 1) == TOK_LT) {
        int save = cx_pos, save_half = cx_half_gt;
        cx_pos += q.fin;
        struct cty *t = template_id_type(y);
        *ntok = cx_pos - save;
        peek_half = cx_half_gt;
        cx_pos = save;
        cx_half_gt = save_half;
        return t;
    }
    if (!is_type_sym(y))
        return NULL;
    *ntok = q.fin + 1;
    peek_half = 0;
    return y->type;
}

/* At `X(` naming a constructor: `X` the class being defined, or `A::A`. */
static int at_ctor_declarator(void)
{
    struct cscope *cs = cx_scope;
    while (cs && cs->k == SC_TEMPLATE)      /* a member template's */
        cs = cs->parent;
    if (cs && cs->k == SC_CLASS && cx_kind() == TOK_IDENT &&
        cs->cls->name && cx_kind_at(1) == TOK_LPAREN &&
        strcmp(cx_cur()->t.text, cs->cls->name) == 0)
        return 1;
    struct qname q = peek_qname();
    if (q.bad || !q.scope || q.scope->k != SC_CLASS || q.fin == 0)
        return 0;
    if (cx_kind_at(q.fin) == TOK_TILDE)     /* X<T>::~X() */
        q.fin++;
    return cx_kind_at(q.fin) == TOK_IDENT && q.scope->cls->name &&
           strcmp(tok_text(q.fin), q.scope->cls->name) == 0 &&
           cx_kind_at(q.fin + 1) == TOK_LPAREN;
}

/* At `A::operator T` — or `X<T>::operator T`, outside the class: a
 * conversion function's declarator, with no decl-specifiers. */
static int at_qualified_operator(void)
{
    struct qname q = peek_qname();
    return !q.bad && q.scope && q.fin > 0 &&
           cx_kind_at(q.fin) == TOK_CX_OPERATOR;
}

static int is_decl_keyword(enum tok_kind k)
{
    switch (k) {
    case TOK_KW_INT: case TOK_KW_CHAR: case TOK_KW_SHORT: case TOK_KW_LONG:
    case TOK_KW_INT128:
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
    if (trait_at() && trait_is_type(cx_cur()->t.text))
        return 1;
    if (cx_kind() == TOK_IDENT || cx_kind() == TOK_COLONCOLON) {
        int ctn, cargs;                 /* C auto: a constrained placeholder */
        if (concept_at(&ctn, &cargs) &&
            (cx_kind_at(ctn) == TOK_CX_AUTO ||
             cx_kind_at(ctn) == TOK_CX_DECLTYPE ||
             (cx_kind_at(ctn) == TOK_IDENT &&
              !strncmp(cx_peek(ctn)->t.text, "__cx_abv", 8))))
            return 1;
    }
    if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET)
        return 1;
    int n;
    return peek_type_name(&n) != NULL;
}

/* ---- declaration specifiers ---- */

/* the last declaration specifiers read said `friend` */
static int dspec_friend;

enum { SK_NONE, SK_STATIC, SK_EXTERN, SK_TYPEDEF };

struct dspec {
    struct cty *type;
    int storage;
    int is_inline, is_constexpr, is_virtual, is_explicit, is_friend;
    int is_consteval;
    int is_mutable;
    struct attrs a;
    const struct ctok *at;
    struct cclass *cls_defined;   /* a class defined by the specifiers */
    struct cenum *enum_defined;
    int decl_only;                /* `struct X;`: declares X, nothing else */
};

static struct cty *parse_class_spec(struct dspec *ds);
static int member_class_only(void);
static struct cty *parse_enum_spec(struct dspec *ds);
static struct cty *parse_class_body(struct cclass *c, enum tok_kind kw,
                                    struct attrs *a, const struct ctok *at);

/* An anonymous member's members, named in scope s: each reached through
 * path (the anonymous members around it), nested ones as deep as they go. */
static void inject_anon_members(struct cscope *s, struct cclass *ac,
                                struct cfield **path, int np)
{
    for (int i = 0; i < ac->nfields; i++) {
        struct cfield *f = ac->fields[i];
        if (f->anon) {
            struct cfield **p2 = xmalloc((size_t)(np + 1) * sizeof *p2);
            memcpy(p2, path, (size_t)np * sizeof *p2);
            p2[np] = f;
            inject_anon_members(s, f->anon, p2, np + 1);
            continue;
        }
        if (!f->name)
            continue;
        struct csym *y = scope_add(s, CS_FIELD, f->name);
        y->field = f;
        y->fpath = xmalloc((size_t)np * sizeof *y->fpath);
        memcpy(y->fpath, path, (size_t)np * sizeof *y->fpath);
        y->nfpath = np;
    }
}

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
static int targ_mentions_param(void);
static const char *dep_expr_cond(void);

static struct cty *decltype_of(struct cexpr *e)
{
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

/* ... and decltype(auto): a placeholder its initializer (or a return)
 * gives decltype's type */
static struct cty *parse_decltype(void)
{
    cx_advance();
    cx_expect(TOK_LPAREN, "'(' after decltype");
    if (cx_accept(TOK_CX_AUTO)) {
        cx_expect(TOK_RPAREN, "')' after decltype(auto");
        struct cty *t = xcalloc(1, sizeof *t);
        t->k = CT_AUTO;
        t->dauto = 1;
        return t;
    }
    if (cx_pattern && targ_mentions_param()) {
        /* in a pattern, of an expression of the template's parameters:
         * a dependent type, known by the expression's mangling (when
         * EmbCC can spell it) */
        struct cty *d = xcalloc(1, sizeof *d);
        d->k = CT_DEP;
        int save = cx_pos;
        jmp_buf jb;
        void *saved = cx_sfinae;
        struct parse_state *st = parse_save();
        if (!setjmp(jb)) {
            cx_sfinae = &jb;
            const char *m = dep_expr_cond();
            cx_sfinae = saved;
            if (cx_kind() == TOK_RPAREN)
                d->dexpr = m;
        } else {
            parse_restore(st);
            cx_sfinae = saved;
        }
        if (!d->dexpr) {
            cx_pos = save - 1;          /* at the `(` */
            cx_skip_balanced();
        } else {
            cx_expect(TOK_RPAREN, "')'");
        }
        return d;
    }
    cx_unevaluated++;
    struct cexpr *e = expr_parse();
    cx_unevaluated--;
    cx_expect(TOK_RPAREN, "')'");
    return decltype_of(e);
}

static void parse_dspec(struct dspec *ds)
{
    memset(ds, 0, sizeof *ds);
    ds->at = cx_cur();
    int n_void = 0, n_bool = 0, n_char = 0, n_short = 0, n_int = 0;
    int n_long = 0, n_int128 = 0, n_signed = 0, n_unsigned = 0;
    int n_float = 0, n_double = 0;
    int n_complex = 0;
    enum cty_kind special = CT_VOID;
    int n_special = 0;
    unsigned cv = 0;
    struct cty *named = NULL;
    int after_typename = 0;
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
            if (cx_kind() == TOK_CX_CONSTEVAL)
                ds->is_consteval = 1;
            cx_advance();
            continue;
        case TOK_CX_CONSTINIT: cx_advance(); continue;
        case TOK_CX_VIRTUAL: ds->is_virtual = 1; cx_advance(); continue;
        case TOK_CX_EXPLICIT:
            ds->is_explicit = 1;
            cx_advance();
            if (cx_kind() == TOK_LPAREN && cx_pattern) {
                /* explicit(C) in a pattern: decided per specialization,
                 * when its declaration is read again */
                ds->is_explicit = 0;
                cx_skip_balanced();
            } else if (cx_kind() == TOK_LPAREN) {
                cx_advance();
                ds->is_explicit = expr_parse_const_as("explicit(...)", 1)
                                  != 0;
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
        case TOK_KW_INT128: n_int128++; cx_advance(); continue;
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
        case TOK_KW_COMPLEX:        /* __complex__ double (GNU) */
            n_complex++;
            cx_advance();
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
            /* decltype(e)::type: a member of that class */
            while (cx_kind() == TOK_COLONCOLON && cx_kind_at(1) == TOK_IDENT) {
                const char *mn = cx_peek(1)->t.text;
                const struct ctok *mat = cx_peek(1);
                cx_advance();
                cx_advance();
                if (named->k == CT_CLASS) {
                    class_ensure(named->cls);
                    struct csym *y = lookup_in(named->cls->scope, mn);
                    if (!y || (y->k != CS_TYPEDEF && y->k != CS_CLASS &&
                               y->k != CS_ENUM))
                        cx_error(mat, "'%s' names no type in '%s'", mn,
                                 ct_name(named));
                    named = y->type;
                } else if (ct_dependent(named)) {
                    named = dep_member(named, mn);
                } else {
                    cx_error(mat, "'%s' is not a class", ct_name(named));
                }
            }
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
            after_typename = 1;
            dependent_type_ok = 1;
            continue;
        case TOK_IDENT: case TOK_COLONCOLON: {
            if (named || builtin)
                break;
            if (trait_at() && trait_is_type(cx_cur()->t.text)) {
                named = parse_type_trait();     /* __underlying_type(T) */
                continue;
            }
            {
                /* C auto, C<A> decltype(auto): a constrained placeholder
                 * — the constraint is not checked yet, the type deduced
                 * as the placeholder's */
                int ctn, cargs;
                if (concept_at(&ctn, &cargs) &&
                    (cx_kind_at(ctn) == TOK_CX_AUTO ||
                     cx_kind_at(ctn) == TOK_CX_DECLTYPE ||
                     (cx_kind_at(ctn) == TOK_IDENT &&
                      !strncmp(cx_peek(ctn)->t.text, "__cx_abv", 8)))) {
                    cx_pos += ctn;          /* (C __cx_abvN: its type-
                                             * constraint, checked there) */
                    continue;
                }
            }
            if (at_ctor_declarator())
                break;
            int n;
            struct cty *t = peek_type_name(&n);
            if (!t)
                break;
            cx_skip_peek(n);
            named = t;
            continue;
        }
        default:
            break;
        }
        break;
    }

    dependent_type_ok = 0;
    dspec_friend = ds->is_friend;
    if (after_typename && !named)
        cx_error(cx_cur(), "'typename' before %s, which names no type",
                 tok_describe(&cx_cur()->t));
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
        else if (n_int128) k = n_unsigned ? CT_UINT128 : CT_INT128;
        else if (n_long >= 2) k = n_unsigned ? CT_ULLONG : CT_LLONG;
        else if (n_long) k = n_unsigned ? CT_ULONG : CT_LONG;
        else if (n_int || n_signed || n_unsigned)
            k = n_unsigned ? CT_UINT : CT_INT;
        else
            k = CT_AUTO;          /* no type at all */
        if (k == CT_AUTO && n_complex)
            k = CT_DOUBLE;        /* a bare __complex__: double's */
        if (k != CT_AUTO)
            t = ct_basic(k);
    }
    if (n_complex) {
        if (!t || !ct_is_float(t))
            cx_error(ds->at, "__complex__ of a non-floating type");
        t = ct_complex(t);
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
    case TOK_KW_INT128:
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
    struct ctarg *targs;      /* f<int> (an explicit specialization) */
    int ntargs;
    int has_targs;
    struct ctemplate *vtmpl;  /* ... v<int>: of this variable template */
    struct cscope *qual;      /* A::f: the scope named, or NULL */
    struct cscope *saved;     /* the scope to return to after a qualified
                               * declarator (its rest reads in `qual`) */
    struct cty *conv_type;    /* DN_CONV */
    const struct ctok *at;
    struct attrs a;
    int pack;                 /* `...` before the name: a parameter pack */
};

/* parse_declarator may take a `...` before the name (a parameter pack's
 * declarator: parse_params decided it is one) */
static int decl_pack_ok;

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
    case TOK_ARROW: return "->"; case TOK_CX_CO_AWAIT: return " co_await";
    default: return NULL;
    }
}

static struct cty *parse_declarator(struct cty *t, struct declarator *d,
                                    int mode);

/* After `->`: a trailing return type, the parameters of ft in scope
 * (-> decltype(a + b)). */
static struct cty *trailing_return_type(struct cty *ft)
{
    scope_push(SC_PARAMS, NULL);
    for (int i = 0; i < ft->np; i++) {
        if (!ft->pnames || !ft->pnames[i])
            continue;
        struct cvar *v = xcalloc(1, sizeof *v);
        v->name = v->cname = ft->pnames[i];
        v->type = ft->pdecl ? ft->pdecl[i] : ft->params[i];
        v->is_param = 1;
        v->fparam = i + 1;
        struct csym *y = scope_add(cx_scope, CS_VAR, v->name);
        y->var = v;
    }
    struct cty *ret = parse_type_id();
    scope_pop();
    return ret;
}

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
    if (k == TOK_STR) {
        /* operator""_x: a literal operator */
        const struct token *st = &cx_cur()->t;
        if (st->num != 1)
            cx_error(at, "a literal operator's name has an empty \"\"");
        cx_advance();
        const char *suffix = st->ud_suffix;
        if (!suffix) {
            if (cx_kind() != TOK_IDENT)
                cx_error(at, "expected a literal operator's suffix");
            suffix = cx_cur()->t.text;
            cx_advance();
        }
        return cx_fmt("operator\"\"%s", suffix);
    }
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

/* Scope `into` (a qualified declarator's) with the template parameters in
 * scope `from` still visible in front of it — a member template of a
 * class, defined outside it (template<class F> locale::locale(F *)):
 * the parameters' symbols copied into a scope over `into`. */
static struct cscope *with_template_params(struct cscope *into,
                                           struct cscope *from)
{
    struct cscope *tps[8];
    int n = 0;
    for (struct cscope *s = from; s && n < 8; s = s->parent) {
        if (s->k != SC_TEMPLATE)
            break;
        tps[n++] = s;
    }
    if (!n)
        return into;
    for (struct cscope *s = into; s; s = s->parent)
        if (s == tps[0])
            return into;               /* already visible from there */
    struct cscope *ts = scope_new(SC_TEMPLATE, NULL, into);
    for (int i = n - 1; i >= 0; i--) {  /* outermost first: inner hide */
        struct csym *list[256];
        int m = 0;
        for (struct csym *y = tps[i]->syms; y && m < 256; y = y->next)
            list[m++] = y;
        while (m-- > 0) {                /* in declaration order */
            struct csym *c = scope_add(ts, list[m]->k, list[m]->name);
            struct csym *hn = c->hnext, *nx = c->next;
            *c = *list[m];
            c->hnext = hn;
            c->next = nx;
            c->scope = ts;
        }
    }
    return ts;
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
        struct cscope *cs = cx_scope;
        while (cs && cs->k == SC_TEMPLATE)      /* a member template's */
            cs = cs->parent;
        struct cclass *c = q.scope && q.scope->k == SC_CLASS ? q.scope->cls
                          : !q.scope && cs && cs->k == SC_CLASS
                          ? cs->cls : NULL;
        cx_advance();
        if (cx_kind() == TOK_LT) {
            /* f<args>: a function template's specialization named */
            struct csym *y = find_final(q.scope, d->name);
            struct ctemplate *ft = NULL;
            int ntm = 0;
            if (y && y->k == CS_FUNC)
                for (struct cfunc *g = y->fns; g; g = g->next)
                    if (g->tmpl) {
                        if (!ft)
                            ft = g->tmpl;
                        ntm++;
                    }
            if (ft) {
                /* (several templates of the name: the arguments read as
                 * written, kinds from their spelling) */
                d->ntargs = parse_template_args(ntm == 1 ? ft : NULL,
                                                &d->targs);
                d->has_targs = 1;
            } else if (y && y->k == CS_TEMPLATE && y->tmpl->kind == TK_VAR) {
                /* v<args>: a variable template's specialization */
                d->vtmpl = y->tmpl;
                d->ntargs = parse_template_args(y->tmpl, &d->targs);
                d->has_targs = 1;
            }
        }
        if (c && c->name && strcmp(d->name, c->name) == 0 &&
            cx_kind() == TOK_LPAREN)
            d->kind = DN_CTOR;
    } else if (k == TOK_TILDE) {
        cx_advance();
        d->name = cx_fmt("~%s", cx_cur()->t.text);
        d->kind = DN_DTOR;
        cx_advance();
    } else {
        /* X::operator T: T is looked up in X too (X's member typedefs) */
        struct cscope *ss = cx_scope;
        if (q.scope && q.scope->k == SC_CLASS && q.scope != cx_scope)
            cx_scope = with_template_params(q.scope, cx_scope);
        d->name = parse_operator_name(&d->conv_type);
        cx_scope = ss;
        d->kind = d->conv_type ? DN_CONV : DN_OPERATOR;
    }
    (void)mode;
    d->qual = q.scope;
    if (q.scope && q.scope != cx_scope && !dspec_friend) {
        /* (a friend's qualified name names a declared function: its
         * parameters are read where the friend declaration is) */
        d->saved = cx_scope;
        cx_scope = with_template_params(q.scope, cx_scope);
    }
}

/* At `(` after a name: parameters follow (a function declarator), rather
 * than an initializer. `T x();` declares a function (the standard's rule). */
static struct cty *parse_params(void);

static int params_follow(void)
{
    enum tok_kind k = cx_kind_at(1);
    if (k == TOK_RPAREN || k == TOK_ELLIPSIS)
        return 1;
    int save = cx_pos;
    cx_advance();
    if (k == TOK_CX_THIS)       /* (this Self &&self): not (this) */
        cx_advance();
    int r = at_type_start();
    cx_pos = save;
    if (!r)
        return 0;
    /* a type first: parameters, if they parse as such — `T x(A(5))`
     * initializes x with a temporary (9.3.3's disambiguation) */
    struct parse_state *st = parse_save();
    jmp_buf jb;
    void *saved = cx_sfinae;
    if (setjmp(jb)) {
        cx_sfinae = saved;
        parse_restore(st);
        return 0;
    }
    cx_sfinae = &jb;
    parse_params();
    cx_sfinae = saved;
    parse_restore(st);
    return 1;
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
        if (!q.bad && (q.scope || (q.dep && cx_pattern)) && q.fin > 0 &&
            cx_kind_at(1 + q.fin) == TOK_STAR)
            return 1;                  /* (a dependent C: C::* in a pattern) */
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

/* In a pattern, an array bound that mentions a template parameter: skipped
 * to its `]`, *bparam the value parameter when the bound is one alone.
 * 0 (nothing consumed) when it is an ordinary constant. */
static int pattern_bound(int *bparam)
{
    int dep = 0;
    for (int i = cx_pos; i < cx_ntoks && cx_toks[i].t.kind != TOK_RBRACKET;
         i++) {
        if (cx_toks[i].t.kind != TOK_IDENT)
            continue;
        struct csym *y = lookup(cx_scope, cx_toks[i].t.text);
        if (y && ((y->k == CS_VAR && (y->var->is_tparam ||
                                      (y->var->fparam &&
                                       ct_dependent(y->var->type)))) ||
                  ((y->k == CS_TYPEDEF || y->k == CS_CLASS) &&
                   ct_dependent(y->type)) || y->k == CS_PACK))
            dep = 1;
    }
    if (!dep)
        return 0;
    struct csym *y = cx_kind() == TOK_IDENT
                     ? lookup(cx_scope, cx_cur()->t.text) : NULL;
    if (y && y->k == CS_VAR && y->var->is_tparam &&
        cx_kind_at(1) == TOK_RBRACKET)
        *bparam = y->var->tparam_index;
    while (cx_kind() != TOK_RBRACKET && cx_kind() != TOK_EOF) {
        if (cx_kind() == TOK_LPAREN || cx_kind() == TOK_LBRACKET)
            cx_skip_balanced();
        else
            cx_advance();
    }
    return 1;
}

static struct cty *parse_suffixes(struct cty *t, struct declarator *d,
                                  int mode, int after_id)
{
    if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) != TOK_LBRACKET) {
        const struct ctok *at = cx_cur();
        cx_advance();
        long n = -1;
        int bparam = -1;
        struct cexpr *vla = NULL;
        if (cx_kind() != TOK_RBRACKET) {
            if (mode & DK_NEW) {
                /* new T[n]: n is any expression; the caller reads it */
                cx_pos--;
                return t;
            }
            if (cx_pattern && pattern_bound(&bparam)) {
                n = bparam >= 0 ? -2 : -3;      /* [N], or unknown */
            } else {
                /* (a local's may be a run-time value: GNU's VLA) */
                int local = cx_curfn && cx_curblk && !cx_in_targs &&
                            (mode & DK_NAMED) && after_id;
                n = expr_parse_bound(local ? &vla : NULL);
                if (n < 0 && !vla)
                    cx_error(at, "array bound is negative");
            }
        }
        cx_expect(TOK_RBRACKET, "']'");
        struct cty *inner = parse_suffixes(t, d, mode, 0);
        if (inner->k == CT_FUNC || ct_is_ref(inner) || inner->k == CT_VOID)
            cx_error(at, "array of %s", ct_name(inner));
        if (vla && !ct_is_scalar(inner))
            cx_error(at, "a variable-length array of %s is not supported",
                     ct_name(inner));
        struct cty *at_ = ct_array(inner, n);
        at_->bparam = bparam;
        at_->vla = vla;
        return at_;
    }
    if (cx_kind() == TOK_LPAREN && !(mode & DK_NEW) &&
        (!after_id || !(mode & DK_INIT) || params_follow())) {
        struct cty *ft = parse_params();
        struct cty *ret = t;
        if (cx_kind() == TOK_ARROW) {
            cx_advance();
            ret = trailing_return_type(ft);
            if (t->k != CT_AUTO)
                cx_error(cx_cur(), "a trailing return type needs 'auto'");
        }
        ft->to = ret;
        if (after_id && cx_accept(TOK_CX_REQUIRES)) {
            /* f() requires C: checked where the declaration is known
             * with its arguments (a template's instance) */
            ft->treq = cx_pos;
            skip_constraint(0);
            ft->treq_end = cx_pos;
        }
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
            if (cx_pattern && q.dep && q.fin > 0 &&
                cx_kind_at(q.fin) == TOK_STAR) {
                /* T C::* with C a template parameter (in a pattern) */
                cx_pos += q.fin + 1;
                t = ct_mptr(NULL, t);
                t->mclass = q.dep;
                unsigned cq = parse_cv();
                if (cq)
                    t = ct_qual(t, cq);
                continue;
            }
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

/* where attributes after a declarator's parameter list go (`void f(int)
 * __attribute__((weak))`): the declarator being read's */
static struct attrs *attr_sink;

static struct cty *parse_declarator_(struct cty *t, struct declarator *d,
                                     int mode);

static struct cty *parse_declarator(struct cty *t, struct declarator *d,
                                    int mode)
{
    struct attrs *saved = attr_sink;
    attr_sink = &d->a;
    t = parse_declarator_(t, d, mode);
    attr_sink = saved;
    return t;
}

static struct cty *parse_declarator_(struct cty *t, struct declarator *d,
                                     int mode)
{
    parse_attrs(&d->a);
    t = ptr_ops(t, mode);
    if (decl_pack_ok && cx_kind() == TOK_ELLIPSIS) {
        cx_advance();
        d->pack = 1;
        decl_pack_ok = 0;
    }
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

/* At a parameter declaration: is it a pack, a `...` at its top level
 * after a type naming a pack (`Ts... args`, `const Ts&...`)? -1: no (a
 * bare or C-style `...` is not); else how many bound packs it names (0 in
 * a pattern). *end: where the declaration ends. */
static int param_expansion(struct csym **packs, int max, int *end)
{
    int sp = 0, ell = -1, i;
    for (i = cx_pos; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_EOF)
            return -1;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE ||
            k == TOK_LT) {
            sp++;
            continue;
        }
        if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE) {
            if (sp == 0)
                break;
            sp--;
            continue;
        }
        if (k == TOK_GT || k == TOK_SHR) {
            sp -= k == TOK_SHR && sp >= 2 ? 2 : sp > 0 ? 1 : 0;
            continue;
        }
        if (sp == 0 && (k == TOK_COMMA || k == TOK_ASSIGN || k == TOK_SEMI))
            break;
        if (sp == 0 && k == TOK_ELLIPSIS && ell < 0)
            ell = i;
    }
    *end = i;
    if (ell <= cx_pos)
        return -1;
    int np = 0, pattern = 0;
    for (int j = cx_pos; j < ell; j++) {
        if (cx_toks[j].t.kind != TOK_IDENT)
            continue;
        struct csym *y = lookup_raw(cx_scope, cx_toks[j].t.text);
        if (!y)
            continue;
        if (y->pack_param)
            pattern = 1;
        if (y->k != CS_PACK || pack_current(y) != y)
            continue;
        int have = 0;
        for (int k = 0; k < np; k++)
            have |= packs[k] == y;
        if (!have && np < max)
            packs[np++] = y;
    }
    return np ? np : pattern ? 0 : -1;
}

/* One parameter's type and name, `...` taken if it is a pack. */
static struct cty *parse_param(struct declarator *d, int pack)
{
    struct dspec ds;
    const struct ctok *at = cx_cur();
    parse_dspec(&ds);
    if (!ds.type)
        cx_error(at, "expected a parameter type before %s",
                 tok_describe(&cx_cur()->t));
    memset(d, 0, sizeof *d);
    decl_pack_ok = pack;
    struct cty *t = parse_declarator(ds.type, d, DK_NAMED | DK_ABSTRACT);
    decl_pack_ok = 0;
    if (d->saved)
        cx_scope = d->saved;
    if (pack && !d->pack)
        cx_error(at, "expected '...' in a parameter pack's declarator");
    if (t->k == CT_AUTO)
        cx_error(at, "an 'auto' parameter (an abbreviated "
                     "template, C++20) is not supported yet (CX7)");
    if (t->k == CT_VOID)
        cx_error(at, "a parameter of type void");
    if (t->k == CT_ARRAY)
        return ct_qual(ct_ptr(t->to), t->q);
    if (t->k == CT_FUNC)
        return ct_ptr(t);
    return t;
}

static struct cty *pack_marked(struct cty *t)
{
    struct cty *c = xmalloc(sizeof *c);
    *c = *t;
    c->pack_expansion = 1;
    return c;
}

/* ( parameter-declaration-clause ) cv ref noexcept: a function type with
 * no return type yet. A function parameter pack is one parameter in a
 * pattern (its type marked), its elements' parameters in an instance. */
static struct cty *parse_params(void);

struct cty *parse_param_list(void)
{
    return parse_params();
}

/* Past a default argument, unread: to the `,` or `)` that ends it (a
 * template-id's `<` ... `>` skipped as a unit). */
static void skip_default_arg(void)
{
    for (;;) {
        enum tok_kind k = cx_kind();
        if (k == TOK_COMMA || k == TOK_RPAREN || k == TOK_EOF ||
            k == TOK_SEMI)
            return;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            cx_skip_balanced();
            continue;
        }
        if (k == TOK_IDENT && cx_kind_at(1) == TOK_LT) {
            struct csym *y = lookup(cx_scope, cx_cur()->t.text);
            if (y && (y->k == CS_TEMPLATE ||
                      (y->k == CS_FUNC && y->fns && y->fns->tmpl) ||
                      as_template(y))) {
                cx_advance();
                skip_template_args();
                continue;
            }
        }
        cx_advance();
    }
}

static int class_depth;          /* inside class bodies being parsed */

static struct cty *parse_params(void)
{
    cx_expect(TOK_LPAREN, "'('");
    int cap = 8, np = 0, variadic = 0, anydef = 0;
    struct cty **params = xmalloc((size_t)cap * sizeof *params);
    struct cty **pdecl = xmalloc((size_t)cap * sizeof *pdecl);
    const char **names = xmalloc((size_t)cap * sizeof *names);
    struct cexpr **defs = xmalloc((size_t)cap * sizeof *defs);
    struct cpgroup *groups = NULL;
    int ngroups = 0;
    int saved_ok = decl_pack_ok;
    /* an explicit object parameter (C++23 `this Self &&self`): the object
     * a member function is called on, as its first parameter */
    int xobj = cx_accept(TOK_CX_THIS);
    if (cx_kind() == TOK_KW_VOID && cx_kind_at(1) == TOK_RPAREN) {
        cx_advance();
    } else {
        while (cx_kind() != TOK_RPAREN) {
            if (cx_accept(TOK_ELLIPSIS)) {
                variadic = 1;
                break;
            }
            struct csym *packs[8];
            int end;
            int ex = param_expansion(packs, 8, &end);
            int reps = ex > 0 ? expansion_length(packs, ex) : 1;
            int start = cx_pos, first = np;
            struct declarator d;
            memset(&d, 0, sizeof d);
            for (int e = 0; e < reps; e++) {
                cx_pos = start;
                for (int k = 0; k < ex; k++)
                    pack_push(packs[k], e);
                struct cty *adj = parse_param(&d, ex >= 0);
                if (ex > 0)
                    pack_pop(ex);
                if (np + 1 >= cap) {
                    cap *= 2;
                    params = xrealloc(params, (size_t)cap * sizeof *params);
                    pdecl = xrealloc(pdecl, (size_t)cap * sizeof *pdecl);
                    names = xrealloc(names, (size_t)cap * sizeof *names);
                    defs = xrealloc(defs, (size_t)cap * sizeof *defs);
                }
                defs[np] = NULL;
                params[np] = ct_unqual(adj);
                pdecl[np] = adj;
                if (ex == 0) {
                    params[np] = pack_marked(params[np]);
                    pdecl[np] = pack_marked(adj);
                }
                names[np] = d.name;
                np++;
            }
            if (ex > 0) {
                if (reps == 0)
                    cx_pos = end;
                groups = xrealloc(groups, (size_t)(ngroups + 1) *
                                          sizeof *groups);
                groups[ngroups].name = d.name;
                groups[ngroups].first = first;
                groups[ngroups].n = reps;
                ngroups++;
                if (reps == 0 && !d.name) {
                    /* the name, from the tokens: an empty pack's */
                    for (int j = start; j < end; j++)
                        if (cx_toks[j].t.kind == TOK_ELLIPSIS &&
                            cx_toks[j + 1].t.kind == TOK_IDENT)
                            groups[ngroups - 1].name = cx_toks[j + 1].t.text;
                }
            }
            if (ex < 0 && cx_accept(TOK_ASSIGN)) {
                if (cx_pattern) {
                    /* a pattern's default: only that there is one (an
                     * instance reads it with its arguments) */
                    skip_default_arg();
                    defs[np - 1] = ex_int(0, ct_basic(CT_INT));
                } else if (class_depth > 0 && cx_scope->k == SC_CLASS &&
                           !cx_scope->cls->complete) {
                    /* a member's, in its class: read when used, the
                     * class complete (`f(const path& = path())`) */
                    struct cexpr *d = ex_new(E_DEFARG, ct_basic(CT_INT),
                                             VC_PRVALUE);
                    d->ival = cx_pos;
                    d->dscope = cx_scope;
                    skip_default_arg();
                    defs[np - 1] = d;
                } else {
                    defs[np - 1] = cx_kind() == TOK_LBRACE
                                   ? parse_braced_list()
                                   : expr_parse_assign();
                }
                anydef = 1;
            }
            if (cx_accept(TOK_COMMA))
                continue;
            if (cx_accept(TOK_ELLIPSIS))
                variadic = 1;
            break;
        }
    }
    decl_pack_ok = saved_ok;
    cx_expect(TOK_RPAREN, "')' to close the parameters");
    struct cty *ft = ct_func(NULL, params, np, variadic);
    ft->xobj = xobj;
    ft->pdecl = pdecl;
    ft->pnames = names;
    ft->pgroups = groups;
    ft->npgroups = ngroups;
    ft->defargs = anydef ? defs : NULL;
    if (cx_kind() == TOK_KW_ATTRIBUTE)     /* (the declarator's) */
        parse_attrs(attr_sink);
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
            /* noexcept, noexcept(true): no exception leaves it; an
             * expression EmbCC cannot settle here counts as false (so an
             * exception passes rather than terminates) */
            ft->nothrow = 1;
            if (cx_kind() == TOK_LPAREN) {
                ft->nothrow = cx_kind_at(1) == TOK_CX_TRUE &&
                              cx_kind_at(2) == TOK_RPAREN;
                cx_skip_balanced();
            }
            continue;
        }
        if (cx_kind() == TOK_CX_THROW && cx_kind_at(1) == TOK_LPAREN) {
            /* a dynamic exception specification: throw() is noexcept */
            ft->nothrow = cx_kind_at(2) == TOK_RPAREN;
            cx_advance();
            cx_skip_balanced();
            continue;
        }
        if (cx_kind() == TOK_KW_ATTRIBUTE ||
            (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET)) {
            parse_attrs(attr_sink);
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

/* A scope under `parent` naming ft's parameters (a trailing
 * requires-clause's operands may: `requires requires { a.base(); }`) */
static struct cscope *with_params(struct cty *ft, struct cscope *parent)
{
    if (!ft->pnames)
        return parent;
    struct cscope *s = scope_new(SC_BLOCK, NULL, parent);
    for (int i = 0; i < ft->np; i++) {
        if (!ft->pnames[i])
            continue;
        struct cvar *v = xcalloc(1, sizeof *v);
        v->name = v->cname = ft->pnames[i];
        v->type = ft->pdecl ? ft->pdecl[i] : ft->params[i];
        v->is_param = v->is_local = 1;
        struct csym *y = scope_add(s, CS_VAR, v->name);
        y->var = v;
    }
    return s;
}

/* A special member's signature for class c: a default constructor, a
 * copy or move constructor (ctor), or a copy or move assignment. */
static int special_signature(struct cclass *c, int ctor, struct cty *ft)
{
    if (ctor && ft->np == 0)
        return 1;
    if (ft->np < 1)
        return 0;
    struct cty *p = ct_strip_ref(ft->params[0]);
    if (p->k != CT_CLASS || p->cls != c)
        return 0;
    return ctor ? ct_is_ref(ft->params[0]) : ft->np == 1;
}

/* The same trailing requires-clause (by its tokens), or both none? */
static int same_treq(const struct cty *a, const struct cty *b)
{
    if (!a->treq || !b->treq)
        return !a->treq && !b->treq;
    if (a->treq_end - a->treq != b->treq_end - b->treq)
        return 0;
    for (int i = 0; i < a->treq_end - a->treq; i++) {
        const struct token *x = &cx_toks[a->treq + i].t,
                           *y = &cx_toks[b->treq + i].t;
        if (x->kind != y->kind ||
            (x->text && y->text && strcmp(x->text, y->text) != 0))
            return 0;
    }
    return 1;
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
    /* a requires-clause: a special member's is decided with its class
     * (which one is eligible makes the class trivial or not); another's
     * when the function is first a candidate — its class, or one it
     * names, may be incomplete now (view_interface<D>'s, D's base) */
    int special = d->kind == DN_DTOR ||
                  (cls && (d->kind == DN_CTOR || !strcmp(d->name, "operator="))
                   && special_signature(cls, d->kind == DN_CTOR, ft));
    int unsat = ft->treq && !cx_pattern && special &&
                !constraint_satisfied(ft->treq, ft->treq_end,
                                      with_params(ft, cx_scope));
    for (struct cfunc *f = unsat ? NULL : *set; f; f = f->next) {
        if (f->unsat || f->tmpl)
            continue;       /* (a template of the same signature is
                             * another function: bitset's to_string) */
        if (!same_signature(f->type, ft) &&
            !(f->c_linkage && cx_extern_c))
            continue;
        if (d->kind == DN_CONV && !ct_same(f->type->to, ft->to))
            continue;       /* conversion functions differ by their type */
        if (!same_treq(f->type, ft))
            continue;       /* ... and functions by their constraints */
        if (f->inherited) {
            /* the class's own constructor hides the inherited one */
            f->unsat = 1;
            continue;
        }
        if (!ct_same(f->type->to, ft->to))
            cx_error(d->at, "'%s' redeclared with a different return type",
                     d->name);
        merge_defaults(f, ft);
        if (!ds->is_friend)
            f->hidden_friend = 0;   /* declared where lookup finds it */
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
    f->hidden_friend = ds->is_friend && !d->qual;
    /* (an explicit object member function has no `this`: C sees the
     * object as its first parameter) */
    f->is_static = ds->storage == SK_STATIC || ft->xobj;
    /* a class's operator new/delete are static members (11.12) */
    if (cls && (strncmp(d->name, "operator new", 12) == 0 ||
                strncmp(d->name, "operator delete", 15) == 0))
        f->is_static = 1;
    f->is_inline = ds->is_inline || ds->is_constexpr;
    f->is_constexpr = ds->is_constexpr;
    f->is_consteval = ds->is_consteval;
    f->is_explicit = ds->is_explicit;
    f->is_virtual = ds->is_virtual;
    f->c_linkage = cx_extern_c && !cls;
    f->weak = ds->a.weak || d->a.weak;
    f->noreturn = ds->a.noreturn || d->a.noreturn;
    tags_from(&f->abi_tags, &f->nabi_tags, &ds->a);
    tags_from(&f->abi_tags, &f->nabi_tags, &d->a);
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
    if (unsat) {
        /* its requires-clause is not satisfied (in this instance): no
         * candidate, and its body never read */
        f->unsat = 1;
        return f;
    }
    if (ft->treq && !cx_pattern && !special)
        f->treq_scope = with_params(ft, cx_scope);
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
    int fn_try = cx_accept(TOK_CX_TRY);    /* define_function sees it */
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
    while (fn_try && cx_accept(TOK_CX_CATCH)) {  /* the handlers */
        cx_skip_balanced();
        cx_skip_balanced();
    }
    f->body_end = cx_pos;
}

static int in_instance(struct cclass *c);

static void run_pending(void)
{
    /* default member initializers first: constructors use them — an
     * instance's only when used (a constructor that initializes the
     * member never reads `_Vp _M_base = _Vp();`) */
    for (int i = 0; i < npend; i++)
        if (pend[i].fl && !in_instance(pend[i].c))
            field_parse_default(pend[i].c, pend[i].fl);
    for (int i = 0; i < npend; i++) {
        if (!pend[i].f || pend[i].f->defined || func_unsat(pend[i].f))
            continue;               /* (read early: its return type) */
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

struct cvar *cx_new_local(const char *name, struct cty *t,
                          const struct ctok *at)
{
    return new_local(name, t, at);
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

/* At a mem-initializer: if it is `pattern...`, the packs the pattern
 * names and the `...`'s position; 0 if not (-1: names none). */
static int meminit_expansion(struct csym **packs, int max, int *ell)
{
    int i = cx_pos, ang = 0;
    for (; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_EOF)
            return 0;
        if (k == TOK_LT)
            ang++;
        else if (k == TOK_GT)
            ang--;
        else if (k == TOK_SHR)
            ang -= 2;
        else if (k == TOK_LPAREN || k == TOK_LBRACE || k == TOK_LBRACKET) {
            if (ang <= 0 && k != TOK_LBRACKET)
                break;
            int save = cx_pos;
            cx_pos = i;
            cx_skip_balanced();
            i = cx_pos - 1;
            cx_pos = save;
        }
    }
    int save = cx_pos;
    cx_pos = i;
    cx_skip_balanced();
    int after = cx_pos;
    cx_pos = save;
    if (cx_toks[after].t.kind != TOK_ELLIPSIS)
        return 0;
    *ell = after;
    int np = packs_in(cx_pos, after, packs, max);
    return np ? np : -1;
}

/* One mem-initializer: a member's name, or a type (a base, or the class
 * itself), then ( arguments ) or { list }. */
static void parse_meminit(struct meminit_raw *r)
{
    const struct ctok *mat = cx_cur();
    memset(r, 0, sizeof *r);
    r->at = mat;
    struct qname q = peek_qname();
    if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
        cx_error(mat, "expected a member name in a mem-initializer");
    struct csym *y = find_final(q.scope, cx_toks[cx_pos + q.fin].t.text);
    int is_type = y && (y->k == CS_CLASS || y->k == CS_TYPEDEF ||
                        (y->k == CS_TEMPLATE &&
                         cx_kind_at(q.fin + 1) == TOK_LT));
    if (is_type) {
        struct dspec ds;
        r->name = cx_toks[cx_pos + q.fin].t.text;
        parse_dspec(&ds);
        if (!ds.type || ds.type->k != CT_CLASS)
            cx_error(mat, "a mem-initializer's type is not a class");
        r->cls = ds.type->cls;
        r->name = r->cls->name ? r->cls->name : r->name;
    } else {
        cx_pos += q.fin;
        r->name = cx_cur()->t.text;
        cx_advance();
    }
    r->braced = cx_kind() == TOK_LBRACE;
    if (r->braced) {
        r->args = xmalloc(sizeof *r->args);
        r->args[0] = parse_braced_list();
        r->na = 1;
        return;
    }
    cx_expect(TOK_LPAREN, "'(' in a mem-initializer");
    r->na = expr_call_args_rest(&r->args);
}

/* Parse a function's definition at the cursor (at `{` or a constructor's
 * `:`), with parameter types from ft (the definition's declarator). */
static struct cstmt *parse_handlers(const struct ctok *at);
static void parse_range_for(struct cstmt *s, const struct ctok *at,
                            int colon);

static void define_function(struct cfunc *f, struct cty *ft)
{
    /* a function-try-block: `try` here, or just before a delayed body */
    int fn_try = cx_accept(TOK_CX_TRY) ||
                 (cx_pos > 0 && cx_toks[cx_pos - 1].t.kind == TOK_CX_TRY);
    if (fn_try && !cx_exceptions)
        cx_error(cx_cur(), "'try' with exceptions disabled (-fno-exceptions)");
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
        int in_pack = 0;
        for (int g = 0; g < ft->npgroups; g++)
            in_pack |= i >= ft->pgroups[g].first &&
                       i < ft->pgroups[g].first + ft->pgroups[g].n;
        if (in_pack && n)
            v->cname = cx_fmt("%s__%d", n, cx_uid());
        else if (n) {
            struct csym *y = scope_add(ps, CS_VAR, n);
            y->var = v;
        }
    }
    /* a function parameter pack: its name stands for its parameters */
    for (int g = 0; g < ft->npgroups; g++) {
        struct cpgroup *pg = &ft->pgroups[g];
        if (!pg->name)
            continue;
        struct csym *y = scope_add(ps, CS_PACK, pg->name);
        y->npack = pg->n;
        y->pvars = xcalloc((size_t)(pg->n ? pg->n : 1), sizeof *y->pvars);
        for (int k = 0; k < pg->n; k++)
            y->pvars[k] = f->params[pg->first + k];
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
            /* `Bases(b)...`: one per element of the packs named */
            struct csym *packs[8];
            int ell = -1;
            int ex = meminit_expansion(packs, 8, &ell);
            int reps = ex > 0 ? expansion_length(packs, ex) : 1;
            int start = cx_pos;
            for (int e = 0; e < reps; e++) {
                cx_pos = start;
                for (int k = 0; k < ex; k++)
                    pack_push(packs[k], e);
                if (nmi == capmi) {
                    capmi = capmi ? capmi * 2 : 8;
                    mi = xrealloc(mi, (size_t)capmi * sizeof *mi);
                }
                parse_meminit(&mi[nmi++]);
                if (ex > 0)
                    pack_pop(ex);
            }
            if (ex > 0)
                cx_pos = ell + 1;
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
    if (fn_try) {
        /* a function-try-block's handlers, the parameters in scope */
        f->fn_try = parse_handlers(at);
    }
    if (ct_has_auto(f->type->to))          /* no return with a value */
        f->type->to = ct_basic(CT_VOID);
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
    int na = 0;
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
        na = expr_call_args_rest(&args);    /* (packs expanded) */
    }
    *argsp = args;
    *nap = na;
    return na ? args[0] : NULL;
}

/* std::initializer_list<X> */
static struct cty *il_type(struct cty *X, const struct ctok *at)
{
    struct csym *ns = scope_find_here(cx_global, "std");
    struct csym *y = ns && ns->k == CS_NAMESPACE
                     ? scope_find_here(ns->ns, "initializer_list") : NULL;
    if (!y || y->k != CS_TEMPLATE || !is_std_il(y->tmpl))
        cx_error(at, "std::initializer_list is not declared (#include "
                     "<initializer_list>)");
    struct ctarg a;
    memset(&a, 0, sizeof a);
    a.kind = TP_TYPE;
    a.type = X;
    return ct_class(class_instance(y->tmpl, &a, 1, at));
}

static struct cstmt *decl_of(struct cvar *v, const struct ctok *at);

/* While a local std::initializer_list is initialized: the hidden
 * variables holding the backing arrays (a DECL chain its declaration puts
 * first — il_last_decls, after init_variable_with), so they live as long
 * as it (9.4.5). */
static struct cstmt *il_decls, *il_decls_tail, *il_last_decls;
static struct cfunc *il_hoist_fn;
static int il_collect;       /* the caller puts il_last_decls first */

struct cexpr *il_backing_var(struct cty *arr, struct cexpr *init,
                             const struct ctok *at)
{
    if (!il_hoist_fn || il_hoist_fn != cx_curfn)
        return NULL;
    struct cvar *v = new_local(cx_fmt("__cx_il%d", cx_uid()), arr, at);
    v->ctor = init;
    struct cstmt *d = decl_of(v, at);
    if (il_decls_tail)
        il_decls_tail->more = d;
    else
        il_decls = d;
    il_decls_tail = d;
    return expr_var(v);
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
/* ---- class template argument deduction (12.2.2.9) ---- */

static void skip_default_init_tokens(void);
static void skip_declaration(void);
static int parse_tparams(struct ctparam **out);
static int tparam_base;
static int abbrev_lparen(void);
static void parse_abbreviated_template(struct cclass *cls, int access);

/* A deduction candidate: a function template made of a constructor of
 * the primary template (C's own parameters first, then the constructor's
 * if it is a template: `template<class... P> C(params) -> C<T...>`), of a
 * deduction guide, or — for an aggregate — of its members' types. */
struct ctad_cand {
    struct ctemplate t;       /* the parameters, and pattern.type */
    struct cfunc pat;
    int nclass;               /* made of a constructor: C's parameters */
    int ptok;                 /* the parameter list's `(` (-1: aggregate) */
    int ret_tok;              /* a guide: its result type */
    struct cscope *pscope;    /* a constructor template's own parameters */
    int from_guide, is_explicit, il_first;
    struct cty **agg;         /* an aggregate's member types, in order */
    int nagg;
    struct ctad_cand *next;
};

/* C<T...> with C's own parameters as its arguments (the injected-class-
 * name, in a pattern) */
static struct cty *ctad_self(struct ctemplate *tm)
{
    struct cty *d = xcalloc(1, sizeof *d);
    d->k = CT_TID;
    d->tmpl = tm;
    d->ntargs = tm->nparams;
    d->targs = xcalloc((size_t)(tm->nparams ? tm->nparams : 1),
                       sizeof *d->targs);
    for (int i = 0; i < tm->nparams; i++) {
        struct ctarg *a = &d->targs[i];
        struct ctparam *p = &tm->params[i];
        struct ctarg one;
        memset(&one, 0, sizeof one);
        one.kind = p->kind;
        if (p->kind == TP_TYPE)
            one.type = ct_tparam(i, p->name);
        else if (p->kind == TP_VALUE)
            one.vtype = ct_tparam(i, p->name);
        if (p->pack) {
            /* T...: an expansion of the pack's pattern */
            struct cty *pt = one.type;
            if (pt) {
                pt = xmalloc(sizeof *pt);
                *pt = *one.type;
                pt->pack_expansion = 1;
                one.type = pt;
            }
            a->kind = p->kind;
            a->is_pack = a->expansion = 1;
            a->elems = xmalloc(sizeof *a->elems);
            a->elems[0] = one;
            a->nelems = 1;
        } else {
            *a = one;
        }
    }
    return d;
}

static int is_il_param(struct cty *t)
{
    t = ct_strip_ref(t);
    return (t->k == CT_TID && is_std_il(t->tmpl)) ||
           (t->k == CT_CLASS && t->cls->tmpl && is_std_il(t->cls->tmpl));
}

static struct ctad_cand *ctad_new(struct ctemplate *tm, struct ctparam *ps,
                                  int np, struct cty *ft, struct cscope *scope)
{
    struct ctad_cand *c = xcalloc(1, sizeof *c);
    c->t.kind = TK_FUNC;
    c->t.name = tm->name;
    c->t.scope = scope;
    c->t.params = ps;
    c->t.nparams = np;
    c->t.head_end = -1;
    c->t.pattern = &c->pat;
    c->pat.name = tm->name;
    c->pat.type = ft;
    c->pat.tmpl = &c->t;
    c->il_first = ft->np >= 1 && is_il_param(ft->params[0]);
    return c;
}

/* The candidates made of C's constructors: its body scanned for them (and
 * for the member typedefs their parameters may name), each parameter
 * list read as a pattern */
static void ctad_from_ctors(struct ctemplate *tm, struct ctad_cand **list)
{
    if (tm->head_end < 0 || !tm->pscope)
        return;
    struct parse_state *st = parse_save();
    int nclass = tm->nparams;
    struct cscope *cs = scope_new(SC_BLOCK, NULL, tm->pscope);
    struct csym *self = scope_add(cs, CS_TYPEDEF, tm->name);
    self->type = ctad_self(tm);
    cx_pattern = 1;
    cx_half_gt = 0;
    cx_in_targs = 0;
    cx_pos = tm->head_end;
    while (cx_kind() != TOK_LBRACE && cx_kind() != TOK_EOF) {
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        else
            cx_advance();
    }
    cx_advance();
    int nctors = 0, user = 0;
    struct cty **agg = NULL;
    int nagg = 0, capagg = 0;
    while (cx_kind() != TOK_RBRACE && cx_kind() != TOK_EOF) {
        int start = cx_pos;
        enum tok_kind k = cx_kind();
        if ((k == TOK_CX_PUBLIC || k == TOK_CX_PRIVATE ||
             k == TOK_CX_PROTECTED) && cx_kind_at(1) == TOK_COLON) {
            cx_pos += 2;
            continue;
        }
        if (k == TOK_SEMI) {
            cx_advance();
            continue;
        }
        jmp_buf jb;
        void *saved = cx_sfinae;
        struct parse_state *mst = parse_save();
        if (setjmp(jb)) {
            parse_restore(mst);
            cx_sfinae = saved;
            tparam_base = 0;
            cx_pos = start;
            cx_half_gt = 0;
            skip_declaration();
            continue;
        }
        cx_sfinae = &jb;
        cx_scope = cs;
        if (k == TOK_KW_TYPEDEF) {
            cx_advance();
            struct dspec ds;
            parse_dspec(&ds);
            for (;;) {
                struct declarator d;
                memset(&d, 0, sizeof d);
                struct cty *t = parse_declarator(ds.type, &d, DK_NAMED);
                if (d.name) {
                    struct csym *y = scope_add(cs, CS_TYPEDEF, d.name);
                    y->type = t;
                }
                if (!cx_accept(TOK_COMMA))
                    break;
            }
            cx_expect(TOK_SEMI, "';'");
        } else if (k == TOK_CX_USING && cx_kind_at(1) == TOK_IDENT &&
                   cx_kind_at(2) == TOK_ASSIGN) {
            const char *n = cx_peek(1)->t.text;
            cx_pos += 3;
            struct cty *t = parse_type_id();
            struct csym *y = scope_add(cs, CS_TYPEDEF, n);
            y->type = t;
            cx_expect(TOK_SEMI, "';'");
        } else {
            /* [template<...>] [explicit] [constexpr] ... C( : a
             * constructor; else another member (a data member's type is
             * kept, for an aggregate) */
            struct ctparam *tps = NULL;
            int ntps = 0;
            struct cscope *ts = NULL;
            if (k == TOK_CX_TEMPLATE && cx_kind_at(1) == TOK_LT) {
                cx_advance();
                cx_advance();
                ts = scope_push(SC_TEMPLATE, NULL);
                int saved_base = tparam_base;
                tparam_base = nclass;
                ntps = parse_tparams(&tps);
                tparam_base = saved_base;
                if (cx_accept(TOK_CX_REQUIRES))
                    skip_constraint(0);
            }
            int is_explicit = 0, is_static = 0, other = 0;
            for (;;) {
                enum tok_kind q = cx_kind();
                if (q == TOK_CX_EXPLICIT) {
                    is_explicit = 1;
                    cx_advance();
                    if (cx_kind() == TOK_LPAREN)
                        cx_skip_balanced();   /* (conditional: taken as
                                               * explicit) */
                } else if (q == TOK_CX_CONSTEXPR || q == TOK_KW_INLINE ||
                           q == TOK_CX_CONSTEVAL) {
                    cx_advance();
                } else if (q == TOK_LBRACKET && cx_kind_at(1) ==
                           TOK_LBRACKET) {
                    cx_skip_balanced();
                } else if (q == TOK_KW_ATTRIBUTE) {
                    cx_advance();
                    cx_skip_balanced();
                } else {
                    break;
                }
            }
            if (cx_kind() == TOK_IDENT && cx_kind_at(1) == TOK_LPAREN &&
                strcmp(cx_cur()->t.text, tm->name) == 0) {
                cx_advance();
                int ptok = cx_pos;
                struct cty *ft = parse_params();
                user = 1;
                /* not the copy or move constructor: that is the copy
                 * deduction candidate's */
                struct cty *p0 = ft->np == 1 ? ct_unqual(ct_strip_ref(
                                                   ft->params[0])) : NULL;
                if (!(p0 && p0->k == CT_TID && p0->tmpl == tm)) {
                    int np = nclass + ntps;
                    struct ctparam *ps = xcalloc((size_t)(np ? np : 1),
                                                 sizeof *ps);
                    memcpy(ps, tm->params, (size_t)nclass * sizeof *ps);
                    if (ntps)
                        memcpy(ps + nclass, tps, (size_t)ntps * sizeof *ps);
                    struct ctad_cand *c = ctad_new(tm, ps, np, ft,
                                                   tm->scope);
                    c->nclass = nclass;
                    c->ptok = ptok;
                    c->pscope = ts;
                    c->is_explicit = is_explicit;
                    c->next = *list;
                    *list = c;
                    nctors++;
                }
                cx_pos = start;
                cx_half_gt = 0;
                skip_declaration();
            } else if (!ts && (at_type_start() || cx_kind() == TOK_IDENT) &&
                       cx_kind() != TOK_CX_FRIEND &&
                       cx_kind() != TOK_KW_STATIC &&
                       cx_kind() != TOK_CX_USING &&
                       cx_kind() != TOK_KW_STATIC_ASSERT &&
                       cx_kind() != TOK_TILDE) {
                /* a data member: T a, b; (a function: not one) */
                struct dspec ds;
                parse_dspec(&ds);
                is_static = ds.storage == SK_STATIC;
                if (ds.type && !is_static && !ds.is_friend &&
                    !ds.cls_defined) {
                    for (;;) {
                        struct declarator d;
                        memset(&d, 0, sizeof d);
                        struct cty *t = parse_declarator(ds.type, &d,
                                                         DK_NAMED);
                        if (t->k == CT_FUNC || !d.name) {
                            other = 1;
                            break;
                        }
                        if (nagg == capagg) {
                            capagg = capagg ? capagg * 2 : 8;
                            agg = xrealloc(agg, (size_t)capagg *
                                                sizeof *agg);
                        }
                        agg[nagg++] = t;
                        if (cx_kind() == TOK_ASSIGN || cx_kind() ==
                            TOK_LBRACE)
                            skip_default_init_tokens();
                        if (!cx_accept(TOK_COMMA))
                            break;
                    }
                }
                cx_pos = start;
                cx_half_gt = 0;
                skip_declaration();
            } else {
                cx_pos = start;
                cx_half_gt = 0;
                skip_declaration();
            }
            (void)other;
        }
        cx_sfinae = saved;
        int end = cx_pos;
        parse_restore(mst);
        cx_pos = end;
        cx_half_gt = 0;
    }
    if (!user && nagg) {
        /* an aggregate (C++20): its members' types, in order */
        struct cty *ft = ct_func(ct_basic(CT_VOID), agg, nagg, 0);
        struct ctad_cand *c = ctad_new(tm, tm->params, nclass, ft,
                                       tm->scope);
        c->nclass = nclass;
        c->ptok = -1;
        c->agg = agg;
        c->nagg = nagg;
        c->next = *list;
        *list = c;
    }
    (void)nctors;
    parse_restore(st);
}

/* Past a default member initializer's tokens (= x or {x}) */
static void skip_default_init_tokens(void)
{
    if (cx_kind() == TOK_LBRACE) {
        cx_skip_balanced();
        return;
    }
    cx_advance();
    int depth = 0;
    while (cx_kind() != TOK_EOF) {
        enum tok_kind k = cx_kind();
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            cx_skip_balanced();
            continue;
        }
        if (k == TOK_LT)
            depth++;
        else if (k == TOK_GT && depth)
            depth--;
        else if (!depth && (k == TOK_SEMI || k == TOK_COMMA))
            return;
        cx_advance();
    }
}

/* The candidates made of C's deduction guides, in declaration order */
static void ctad_from_guides(struct ctemplate *tm, struct ctad_cand **list)
{
    struct cguide *gs[256];
    int n = 0;
    for (struct cguide *g = tm->guides; g && n < 256; g = g->next)
        gs[n++] = g;
    for (int i = 0; i < n; i++) {           /* (the list is newest first) */
        struct cguide *g = gs[i];
        jmp_buf jb;
        void *saved = cx_sfinae;
        struct parse_state *st = parse_save();
        if (setjmp(jb)) {
            parse_restore(st);
            cx_sfinae = saved;
            continue;
        }
        cx_sfinae = &jb;
        cx_scope = g->scope;
        cx_pos = g->tok + 1;
        cx_half_gt = 0;
        cx_in_targs = 0;
        cx_pattern = g->nparams > 0;
        int ptok = cx_pos;
        struct cty *ft = parse_params();
        cx_expect(TOK_ARROW, "'->' in a deduction guide");
        int ret = cx_pos;
        cx_sfinae = saved;
        parse_restore(st);
        struct cscope *home = g->nparams && g->scope->parent
                              ? g->scope->parent : g->scope;
        struct ctad_cand *c = ctad_new(tm, g->params, g->nparams, ft, home);
        c->from_guide = 1;
        c->ptok = ptok;
        c->ret_tok = ret;
        c->is_explicit = g->is_explicit;
        c->pscope = g->scope;
        c->next = *list;
        *list = c;
    }
}

/* A viable candidate: the class it deduces, and its parameters with the
 * arguments substituted (as a function overload resolution compares) */
struct ctad_try {
    struct ctad_cand *c;
    struct cty *cls;
    struct cfunc *fn;
};

static int ctad_try(struct ctad_cand *c, struct ctemplate *tm,
                    struct cexpr **args, int na, struct ctad_try *out,
                    const struct ctok *at)
{
    struct ctarg *targs;
    int nt;
    if (!deduce_call(&c->t, NULL, 0, args, na, &targs, &nt))
        return 0;
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        return 0;
    }
    cx_sfinae = &jb;
    struct cty *cls;
    struct cty *ft;
    cx_half_gt = 0;
    cx_in_targs = 0;
    cx_pattern = 0;
    if (c->from_guide) {
        struct cscope *b = tparam_scope(c->t.params, c->t.nparams, targs, nt,
                                        c->t.scope);
        cx_scope = b;
        cx_pos = c->ret_tok;
        cls = parse_type_id();
        cx_scope = b;
        cx_pos = c->ptok;
        ft = parse_params();
    } else {
        cls = ct_class(class_instance(tm, targs, c->nclass, at));
        class_ensure(cls->cls);
        if (c->ptok < 0) {
            /* an aggregate: its members' types, in this instance */
            struct cty **ps = xcalloc((size_t)c->nagg, sizeof *ps);
            int k = 0;
            for (int i = 0; i < cls->cls->nfields && k < c->nagg; i++)
                if (cls->cls->fields[i]->name)
                    ps[k++] = cls->cls->fields[i]->type;
            ft = ct_func(ct_basic(CT_VOID), ps, k, 0);
        } else {
            struct cscope *b = cls->cls->scope;
            if (c->t.nparams > c->nclass)
                b = tparam_scope(c->t.params + c->nclass,
                                 c->t.nparams - c->nclass, targs + c->nclass,
                                 nt - c->nclass, b);
            cx_scope = b;
            cx_pos = c->ptok;
            ft = parse_params();
        }
    }
    cx_sfinae = saved;
    parse_restore(st);
    struct cfunc *f = xcalloc(1, sizeof *f);
    f->name = tm->name;
    f->type = ft;
    f->pnames = ft->pnames;
    f->defargs = ft->defargs;
    f->is_explicit = c->is_explicit;
    f->vslot = -1;
    out->c = c;
    out->cls = cls;
    out->fn = f;
    return 1;
}

struct cty *ctad_deduce(struct ctemplate *tm, int form, struct cexpr **args,
                        int na, const struct ctok *at)
{
    int list = form == INIT_LIST || form == INIT_COPY_LIST;
    struct cexpr **xs = args;
    int nx = na;
    if (list) {
        xs = args[0]->a;
        nx = args[0]->na;
    }
    if (form == INIT_DEFAULT || form == INIT_VALUE)
        nx = 0;
    /* the copy deduction candidate: C(C<T...>) -> C<T...> */
    if (nx == 1 && xs[0]->t && xs[0]->t->k == CT_CLASS &&
        !(xs[0]->k == E_INITLIST && !xs[0]->t)) {
        struct cclass *k = xs[0]->t->cls;
        class_ensure(k);
        for (struct cclass *b = k; b; ) {
            if (b->tmpl == tm)
                return ct_class(b);
            b = b->nbases == 1 ? b->bases[0].cls : NULL;
        }
    }
    if (!tm->ctad_done) {
        tm->ctad_done = 1;
        struct ctad_cand *cands = NULL;
        ctad_from_guides(tm, &cands);
        ctad_from_ctors(tm, &cands);
        tm->ctad = cands;
    }
    int flags = form == INIT_COPY || form == INIT_COPY_LIST ? RS_NO_EXPLICIT
                                                           : 0;
    struct ctad_try tries[128];
    int nt = 0;
    /* a braced list: initializer-list candidates first, the list one
     * argument; then every candidate with the elements */
    for (int phase = list ? 0 : 1; phase < 2 && !nt; phase++) {
        for (struct ctad_cand *c = tm->ctad; c && nt < 128; c = c->next) {
            if (phase == 0 && !c->il_first)
                continue;
            if (ctad_try(c, tm, phase == 0 ? args : xs, phase == 0 ? 1 : nx,
                         &tries[nt], at))
                nt++;
        }
        if (nt) {
            struct cfunc *set = NULL, **tail = &set;
            for (int i = 0; i < nt; i++) {
                *tail = tries[i].fn;
                tail = &tries[i].fn->next;
            }
            struct cfunc *best = resolve_ex(set, NULL, phase == 0 ? args : xs,
                                            phase == 0 ? 1 : nx, NULL, NULL,
                                            flags);
            for (int i = 0; best && i < nt; i++)
                if (tries[i].fn == best)
                    return tries[i].cls;
            if (!best && expr_resolve_was_ambiguous()) {
                /* equally good: a guide over a constructor (12.2.4.3) */
                for (int i = 0; i < nt; i++)
                    if (tries[i].c->from_guide)
                        return tries[i].cls;
                return tries[0].cls;
            }
            nt = 0;                         /* none viable */
        }
    }
    cx_error(at, "cannot deduce the template arguments of '%s' from this "
                 "initializer", tm->name);
    return NULL;
}

static struct cty *deduce_auto(struct cty *P, enum init_form form,
                               struct cexpr **args, int na,
                               const struct ctok *at)
{
    if (P->k == CT_AUTO && P->tmpl)
        return ct_qual(ctad_deduce(P->tmpl, form, args, na, at), P->q);
    if (na != 1)
        cx_error(at, "cannot deduce 'auto' from this initializer");
    struct cexpr *e = args[0];
    if (P->k == CT_AUTO && P->dauto) {
        if (form == INIT_LIST || form == INIT_COPY_LIST)
            cx_error(at, "decltype(auto) from a braced list");
        if (e->k == E_OVL && !e->fn->next)
            return e->fn->type;
        return decltype_of(e);
    }
    if (form == INIT_COPY_LIST) {
        /* auto x = { a, b }: a std::initializer_list of their type */
        if (!e->na)
            cx_error(at, "cannot deduce 'auto' from an empty braced list");
        struct cty *X = NULL;
        for (int i = 0; i < e->na; i++) {
            struct cexpr *el = e->a[i];
            if (el->k == E_INITLIST && !el->t)
                cx_error(at, "cannot deduce 'auto' from a nested braced "
                             "list");
            struct cty *A = ct_unqual(ct_decay(el->k == E_OVL
                                               ? el->fn->type : el->t));
            if (X && !ct_same(X, A))
                cx_error(at, "'auto' deduced as both %s and %s", ct_name(X),
                         ct_name(A));
            X = A;
        }
        struct cty *il = il_type(X, at);
        if (ct_is_ref(P))
            return ct_ref(ct_qual(il, P->to->q), P->k == CT_RREF);
        return ct_qual(il, P->q);
    }
    if (form == INIT_LIST) {
        if (e->na != 1)
            cx_error(at, "'auto' from a braced list of %d values", e->na);
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
static void init_variable_with(struct cvar *v, enum init_form form,
                               struct cexpr **args, int na,
                               const struct ctok *at);

static void init_variable(struct cvar *v, const struct ctok *at)
{
    enum init_form form;
    struct cexpr **args;
    int na;
    parse_init_args(&form, &args, &na);
    init_variable_with(v, form, args, na, at);
}

/* v initialized as `form` from args (its `auto` deduced from them). */
static void init_variable_as(struct cvar *v, enum init_form form,
                             struct cexpr **args, int na,
                             const struct ctok *at);

static void init_variable_with(struct cvar *v, enum init_form form,
                               struct cexpr **args, int na,
                               const struct ctok *at)
{
    struct cty *t = v->type;
    if (t->k == CT_AUTO || (ct_is_ref(t) && t->to->k == CT_AUTO) ||
        (t->k == CT_PTR && t->to->k == CT_AUTO)) {
        t = deduce_auto(t, form, args, na, at);
        v->type = t;
    }
    int collect = il_collect;
    il_collect = 0;
    struct cstmt *sd = il_decls, *st = il_decls_tail;
    struct cfunc *sf = il_hoist_fn;
    il_decls = il_decls_tail = NULL;
    il_hoist_fn = collect && v->is_local && !v->is_static && cx_curfn &&
                  ct_il_elem(ct_strip_ref(t)) ? cx_curfn : NULL;
    init_variable_as(v, form, args, na, at);
    il_last_decls = il_decls;
    il_decls = sd;
    il_decls_tail = st;
    il_hoist_fn = sf;
}

/* v initialized as `form` from args, its type known. */
static void init_variable_as(struct cvar *v, enum init_form form,
                             struct cexpr **args, int na,
                             const struct ctok *at)
{
    struct cty *t = v->type;
    if (form == INIT_DEFAULT) {
        if (ct_is_ref(t) && !v->is_extern && !v->is_param)
            cx_error(at, "reference '%s' is not initialized", v->name);
        if (!v->is_extern && !ct_is_ref(t)) {
            if (t->k == CT_ARRAY && t->n < 0 && !t->vla)
                cx_error(at, "array '%s' has no size", v->name);
            if (t->vla && form == INIT_DEFAULT) {
                v->ctor = NULL;           /* (scalars: left as they are) */
                return;
            }
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
        if ((form == INIT_LIST || form == INIT_COPY_LIST) &&
            ct_il_elem(t->to)) {
            /* a reference to a std::initializer_list made from the list */
            e = il_make(ct_unqual(t->to), e, at);
        } else if (form == INIT_LIST || form == INIT_COPY_LIST) {
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
    int declared_before = y && y->k == CS_VAR;
    if (ds->is_constexpr)
        t = ct_qual(t, CQ_CONST);   /* (as the declaration may say const) */
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
        else if (((t->q & CQ_CONST) || ds->is_constexpr) &&
                 ds->storage != SK_EXTERN && !ds->is_inline && !cx_extern_c &&
                 !d->qual)
            v->is_static = 1;     /* (constexpr is const: 6.6/3.2) */
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
    if (ds->a.nabi_tags || d->a.nabi_tags) {
        tags_from(&v->abi_tags, &v->nabi_tags, &ds->a);
        tags_from(&v->abi_tags, &v->nabi_tags, &d->a);
        if (!v->c_linkage && !d->a.asm_name)
            v->cname = mangle_var(v, v->owner);
    }
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
        if (declared_before)
            gvar_defined_here(v);
        if ((v->init || v->ctor) && cx_kind() != TOK_ASSIGN &&
            cx_kind() != TOK_LBRACE && cx_kind() != TOK_LPAREN)
            return;           /* const T C::x; : initialized in the class */
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
    if (!name) {
        c->cname = cx_fmt("__cx_anon%d", cx_uid());
        c->unnamed_no = ++owner->nunnamed;
    } else if (c->local)
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

/* Is c a class template's instance, or a class inside one? */
static int in_instance(struct cclass *c)
{
    for (struct cscope *s = c->scope; s; s = s->parent)
        if (s->k == SC_CLASS && s->cls->tmpl)
            return 1;
    return 0;
}

/* Where an elaborated `struct X` that names nothing yet declares X: the
 * nearest enclosing namespace or block (not a class or parameter scope). */
static struct cscope *elaborated_home(void)
{
    struct cscope *s = cx_scope;
    while (s->k == SC_CLASS || s->k == SC_PARAMS || s->k == SC_ENUM)
        s = s->parent;
    return s;
}

static void add_base(struct cclass *c, const struct ctok *at, int virt,
                     int access, int *cap);

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
        /* `Bases...`: a base per element of the packs named */
        int i = cx_pos, depth = 0;
        for (; i < cx_ntoks; i++) {
            enum tok_kind k = cx_toks[i].t.kind;
            if (k == TOK_EOF || (depth == 0 && (k == TOK_COMMA ||
                                                k == TOK_LBRACE)))
                break;
            if (k == TOK_LT || k == TOK_LPAREN || k == TOK_LBRACKET)
                depth++;
            else if (k == TOK_GT || k == TOK_RPAREN || k == TOK_RBRACKET)
                depth--;
            else if (k == TOK_SHR)
                depth -= 2;
        }
        if (i > cx_pos && cx_toks[i - 1].t.kind == TOK_ELLIPSIS) {
            struct csym *packs[8];
            int ex = packs_in(cx_pos, i - 1, packs, 8);
            if (ex <= 0)
                cx_error(at, "'...' expands no parameter pack");
            int reps = expansion_length(packs, ex), start = cx_pos;
            for (int e = 0; e < reps; e++) {
                cx_pos = start;
                for (int k = 0; k < ex; k++)
                    pack_push(packs[k], e);
                add_base(c, at, virt, access, &cap);
                pack_pop(ex);
            }
            cx_pos = i;
            continue;
        }
        add_base(c, at, virt, access, &cap);
    } while (cx_accept(TOK_COMMA));
}

static void add_base(struct cclass *c, const struct ctok *at, int virt,
                     int access, int *cap)
{
    {
        struct cty *t;
        int n;
        if (cx_kind() == TOK_CX_DECLTYPE) {
            t = parse_decltype();
        } else {
            t = peek_type_name(&n);
            if (!t)
                cx_error(at, "expected a base class name before %s",
                         tok_describe(&cx_cur()->t));
            cx_skip_peek(n);
        }
        if (t->k != CT_CLASS)
            cx_error(at, "base '%s' is not a class", ct_name(t));
        struct cclass *b = t->cls;
        class_ensure(b);
        if (!b->complete)
            cx_error(at, "base class '%s' is incomplete", ct_name(t));
        if (b == c)
            cx_error(at, "a class cannot be its own base");
        if (b->is_union || c->is_union)
            cx_error(at, "a union cannot be a base or have bases");
        for (int i = 0; i < c->nbases; i++)
            if (c->bases[i].cls == b)
                cx_error(at, "'%s' is a direct base twice", b->name);
        if (c->nbases == *cap) {
            *cap = *cap ? *cap * 2 : 4;
            c->bases = xrealloc(c->bases, (size_t)*cap * sizeof *c->bases);
        }
        struct cbase *cb = &c->bases[c->nbases++];
        memset(cb, 0, sizeof *cb);
        cb->cls = b;
        cb->is_virtual = virt;
        cb->access = access;
    }
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
        if (cx_kind_at(q.fin + 1) == TOK_LT) {
            /* class X<args> (friend class map<K, T>;): the
             * specialization named */
            struct csym *ty = q.scope ? lookup_in(q.scope, tok_text(q.fin))
                                      : lookup(cx_scope, tok_text(q.fin));
            struct ctemplate *tt = as_template(ty);   /* (or injected) */
            if (tt && tt->kind == TK_CLASS) {
                cx_pos += q.fin;
                struct cty *t = template_id_type(ty);
                if (t) {
                    if (cx_kind() == TOK_SEMI)
                        ds->decl_only = 1;
                    return t;
                }
            }
        }
        qual = q.scope;
        cx_pos += q.fin;
        name = cx_cur()->t.text;
        cx_advance();
    }
    int is_final = 0;
    if (cx_kind() == TOK_IDENT && strcmp(cx_cur()->t.text, "final") == 0 &&
        (cx_kind_at(1) == TOK_LBRACE || cx_kind_at(1) == TOK_COLON)) {
        cx_advance();
        is_final = 1;
    }
    parse_attrs(&a);
    int is_def = cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON;
    struct cclass *c = NULL;
    struct csym *y = NULL;
    if (name) {
        if (qual) {
            y = qual->k == SC_NAMESPACE ? scope_find_tag_inline(qual, name)
                                        : scope_find_tag(qual, name);
            if (!y)
                cx_error(at, "no class '%s' in '%s'", name,
                         qual->name ? qual->name : "::");
        } else if (ds->is_friend && !is_def) {
            /* friend class X; : the X lookup finds (else one of the
             * enclosing namespace), never a new member class */
            y = lookup_tag(cx_scope, name);
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
        struct cscope *home = ds->is_friend && !is_def
                              ? enclosing_ns(cx_scope)
                              : is_def || cx_kind() == TOK_SEMI
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
    if (a.nabi_tags && !c->tmpl) {
        /* class __attribute__((abi_tag("cxx11"))) failure: in its name */
        tags_from(&c->abi_tags, &c->nabi_tags, &a);
        if (!c->local && c->name)
            c->cname = cx_fmt("_C%s", mangle_class_name(c));
    }
    if (!is_def) {
        if (cx_kind() == TOK_SEMI)
            ds->decl_only = 1;
        return ct_class(c);
    }
    if (c->complete)
        cx_error(at, "redefinition of '%s'", name);
    c->is_final = is_final;
    if (name && !qual && !ds->is_friend && cx_scope->k == SC_CLASS &&
        in_instance(cx_scope->cls) && member_class_only()) {
        /* a member class of a class template's instance: defined when it
         * is needed (13.9.2) — vector<T>'s helpers hold a T, and T may be
         * incomplete while vector<T> is */
        c->lazy_pos = cx_pos;
        c->lazy_key = kw;
        c->lazy_scope = cx_scope;
        while (cx_kind() != TOK_LBRACE && cx_kind() != TOK_EOF) {
            if (cx_kind() == TOK_LPAREN)
                cx_skip_balanced();
            else
                cx_advance();
        }
        cx_skip_balanced();
        parse_attrs(&a);
        return ct_class(c);
    }
    ds->cls_defined = c;
    if (qual) {
        /* struct N::X : B { }: B is looked up as from inside N */
        struct cscope *save = cx_scope;
        cx_scope = c->scope->parent;
        struct cty *t = parse_class_body(c, kw, &a, at);
        cx_scope = save;
        return t;
    }
    return parse_class_body(c, kw, &a, at);
}

/* At a member class's `:` or `{`: is it the whole member declaration
 * (`struct X { ... };`), with no declarator after the body? */
static int member_class_only(void)
{
    int save = cx_pos, save_half = cx_half_gt;
    while (cx_kind() != TOK_LBRACE && cx_kind() != TOK_EOF &&
           cx_kind() != TOK_SEMI) {
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        else
            cx_advance();
    }
    int r = 0;
    if (cx_kind() == TOK_LBRACE) {
        cx_skip_balanced();
        parse_attrs(NULL);
        r = cx_kind() == TOK_SEMI;
    }
    cx_pos = save;
    cx_half_gt = save_half;
    return r;
}

/* A class's definition from its base clause (or `{`) through `}` and its
 * trailing attributes: members, then layout, then — once the outermost
 * class being defined is complete — the member bodies that waited. */
static struct cty *parse_class_body(struct cclass *c, enum tok_kind kw,
                                    struct attrs *a, const struct ctok *at)
{
    c->is_union = kw == TOK_KW_UNION;
    c->is_struct = kw != TOK_CX_CLASS;
    c->packed = a->packed;
    c->align_attr = a->aligned;
    if (cx_accept(TOK_COLON))
        parse_bases(c);
    struct cscope *save = cx_scope;
    cx_scope = c->scope;
    c->defining = 1;
    /* the injected-class-name (an instance's names the instance) */
    if (c->name) {
        struct csym *inj = scope_add(c->scope, CS_CLASS, c->name);
        inj->type = ct_class(c);
    }
    class_depth++;
    cx_expect(TOK_LBRACE, "'{'");
    int access = c->is_struct ? CA_PUBLIC : CA_PRIVATE;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() == TOK_EOF)
            cx_error(at, "unterminated class '%s'", c->name ? c->name : "");
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
        } else {
            en->unnamed_no = ++cx_scope->nunnamed;
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
    /* a scoped enum's enumerators, named in the list's later values:
     * of the underlying type until the `}` (9.7.1) */
    struct cscope *inner = scoped ? scope_new(SC_BLOCK, NULL, cx_scope)
                                  : NULL;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected an enumerator");
        const char *en_name = cx_cur()->t.text;
        cx_advance();
        parse_attrs(NULL);
        if (cx_accept(TOK_ASSIGN)) {
            struct cscope *outer = cx_scope;
            if (inner)
                cx_scope = inner;
            next = expr_parse_const("an enumerator value");
            cx_scope = outer;
        }
        struct csym *y = scope_add(en->scope, CS_ENUMERATOR, en_name);
        y->type = et;
        y->value = next;
        if (inner) {
            y = scope_add(inner, CS_ENUMERATOR, en_name);
            y->type = en->underlying;
            y->value = next;
        }
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
        parse_template_decl(c, *access);
        return;
    default:
        break;
    }
    if (abbrev_lparen() >= 0) {
        parse_abbreviated_template(c, *access);  /* f(C auto x) */
        return;
    }
    struct dspec ds;
    parse_dspec(&ds);
    if (ds.is_friend && ds.type && cx_kind() == TOK_SEMI) {
        cx_advance();                           /* friend class X; */
        return;
    }
    if (cx_kind() == TOK_SEMI) {
        if (ds.cls_defined && ds.cls_defined->anon) {
            /* union { ... }; : a member of that type, its members named in
             * this class's scope (reached through it) */
            struct cclass *ac = ds.cls_defined;
            struct cfield *fl = xcalloc(1, sizeof *fl);
            fl->name = cx_fmt("__cx_anon%d", cx_uid());
            fl->type = ct_class(ac);
            fl->bitwidth = -1;
            fl->dflt_tok = -1;
            fl->access = *access;
            fl->anon = ac;
            add_field(c, fl);
            inject_anon_members(c->scope, ac, &fl, 1);
        }
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
        } else if (ds.is_friend && d.has_targs && cx_kind() != TOK_LBRACE) {
            /* friend R f<args>(...);: a specialization befriended — access
             * is not enforced, so nothing is declared */
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
                /* in a template's instance, a member's body is read only
                 * if it is used (13.9.2) */
                if (in_instance(c))
                    f->lazy = 1;
                else
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
                struct cty *vt = ct_unqual(v->type);
                int c128 = (vt->k == CT_INT128 || vt->k == CT_UINT128) &&
                           (v->type->q & CQ_CONST);
                if (!v->is_inline && !v->has_const && !c128)
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
            fl->nua = ds.a.no_unique_address || d.a.no_unique_address;
            fl->align_attr = ds.a.aligned > d.a.aligned ? ds.a.aligned
                                                        : d.a.aligned;
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
    /* the condition, contextually converted to bool (a true_type's
     * operator bool), then evaluated */
    const struct ctok *ct = cx_cur();
    struct cexpr *ce = expr_parse_cond();
    if (ce->t && ce->t->k == CT_CLASS)
        ce = convert_bool(ce, "static_assert");
    long v = 0;
    if (!ct_is_integer(ce->t) || !expr_const(ce, &v))
        cx_error(ct, "a static_assert condition is not an integral "
                     "constant expression");
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
/* ---- inheriting constructors ---- */

static struct { struct cfunc *bf; struct cclass *c; struct cfunc *f; }
    *inh;
static int ninh, capinh;

struct cfunc *inherited_ctor(struct cclass *c, struct cfunc *bf)
{
    for (int i = 0; i < ninh; i++)
        if (inh[i].bf == bf && inh[i].c == c)
            return inh[i].f;
    struct cfunc *f = xcalloc(1, sizeof *f);
    f->name = c->name;
    f->type = bf->type;
    f->pnames = bf->pnames;
    f->defargs = bf->defargs;
    f->owner = c->scope;
    f->cls = c;
    f->is_ctor = 1;
    f->is_explicit = bf->is_explicit;
    f->is_constexpr = bf->is_constexpr;
    f->is_consteval = bf->is_consteval;
    f->is_inline = 1;
    f->inherited = bf;
    f->access = CA_PUBLIC;
    f->vslot = -1;
    f->body_tok = f->mi_tok = -1;
    f->line = bf->line;
    f->file = bf->file;
    if (!bf->tmpl)
        func_register(f);
    if (ninh == capinh) {
        capinh = capinh ? capinh * 2 : 16;
        inh = xrealloc(inh, (size_t)capinh * sizeof *inh);
    }
    inh[ninh].bf = bf;
    inh[ninh].c = c;
    inh[ninh].f = f;
    ninh++;
    return f;
}

/* A specialization of a base's constructor template, chosen through the
 * entry that stands for it in a derived class: wrapped as that class's,
 * level by level when the base inherited it too */
struct cfunc *inherited_spec(struct cfunc *entry, struct cfunc *spec)
{
    struct cfunc *from = entry->inherited;
    if (from->inherited && from->tmpl)
        spec = inherited_spec(from, spec);
    return inherited_ctor(entry->cls, spec);
}

/* using B::B in class c: each of B's constructors but the copy and move
 * ones (and the default one) — unless c declares one like it */
static void inherit_ctors(struct cclass *c, struct cclass *b,
                          const struct ctok *at)
{
    int base = 0;
    for (int i = 0; i < c->nbases; i++)
        base |= c->bases[i].cls == b;
    if (!base)
        cx_error(at, "'%s' is not a direct base of '%s'", b->name,
                 c->name ? c->name : "class");
    class_ensure(b);
    for (struct cfunc *g = b->ctors; g; g = g->next) {
        struct cty *ft = g->type;
        /* (a default constructor too, since C++17: unless the class
         * declares its own, implicitly — class.c drops it then) */
        if (!g->tmpl && ft->np == 0 && (g->is_implicit || g->is_deleted))
            continue;
        if (ft->np == 1 && !g->tmpl) {
            struct cty *p = ct_unqual(ct_strip_ref(ft->params[0]));
            if (p->k == CT_CLASS && p->cls == b)
                continue;                   /* copy, move */
        }
        int hidden = 0;
        for (struct cfunc *h = c->ctors; h && !hidden; h = h->next)
            hidden = !h->inherited && !h->tmpl && !g->tmpl &&
                     same_signature(h->type, ft);
        if (hidden)
            continue;
        struct cfunc *f = inherited_ctor(c, g);
        if (g->tmpl)
            f->tmpl = g->tmpl;      /* each specialization wrapped */
        struct cfunc **tail = &c->ctors;
        while (*tail)
            tail = &(*tail)->next;
        *tail = f;
    }
}

/* An inherited constructor's definition: the base from its arguments
 * (forwarded as they came), the rest as a defaulted constructor would */
void define_inherited_ctor(struct cfunc *f)
{
    if (f->defined)
        return;
    struct cfunc *bf = f->inherited;
    const struct ctok *at = cx_cur();
    struct cty *ft = f->type;
    f->defined = 1;
    struct cfunc *savefn = cx_curfn;
    struct cstmt *saveblk = cx_curblk;
    int saved_uneval = cx_unevaluated;
    cx_unevaluated = 0;
    cx_curfn = f;
    f->params = xcalloc((size_t)(ft->np ? ft->np : 1), sizeof *f->params);
    struct cexpr **args = xcalloc((size_t)(ft->np ? ft->np : 1),
                                  sizeof *args);
    for (int i = 0; i < ft->np; i++) {
        struct cvar *v = new_local(NULL, ft->params[i], at);
        v->is_param = 1;
        f->params[i] = v;
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(v->type),
                                 ft->params[i]->k == CT_LREF ? VC_LVALUE
                                                             : VC_XVALUE);
        e->var = v;
        args[i] = e;
    }
    struct cvar *tv = new_local("this", NULL, at);
    tv->cname = "this";
    tv->type = ct_ptr(ct_class(f->cls));
    tv->is_param = 1;
    f->this_var = tv;
    struct meminit_raw mi;
    memset(&mi, 0, sizeof mi);
    mi.cls = bf->cls;
    mi.name = bf->cls->name;
    mi.args = args;
    mi.na = ft->np;
    mi.at = at;
    ctor_meminit(f, &mi, 1);
    f->body = st_new(S_BLOCK);
    cx_unevaluated = saved_uneval;
    cx_curfn = savefn;
    cx_curblk = saveblk;
}

static void using_declarator(const struct ctok *at);

static void parse_using(void)
{
    const struct ctok *at = cx_cur();
    cx_advance();
    if (cx_accept(TOK_KW_ENUM)) {
        /* using enum E; : E's enumerators named here too (C++20) */
        struct qname q = peek_qname();
        if (q.bad || cx_kind_at(q.fin) != TOK_IDENT)
            cx_error(at, "expected an enumeration's name");
        cx_pos += q.fin;
        struct csym *y = find_final(q.scope, cx_cur()->t.text);
        if (y && y->k != CS_ENUM && y->k != CS_TYPEDEF)
            y = q.scope ? scope_find_tag(q.scope, cx_cur()->t.text)
                        : lookup_tag(cx_scope, cx_cur()->t.text);
        if (!y || !y->type || y->type->k != CT_ENUM)
            cx_error(cx_cur(), "'%s' is not an enumeration",
                     cx_cur()->t.text);
        cx_advance();
        cx_expect(TOK_SEMI, "';'");
        struct cenum *en = y->type->en;
        for (struct csym *e = en->scope->syms; e; e = e->next) {
            if (e->k != CS_ENUMERATOR)
                continue;
            struct csym *n = scope_add(cx_scope, CS_ENUMERATOR, e->name);
            n->type = e->type;
            n->value = e->value;
        }
        return;
    }
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
    int alias_attrs = 0;
    if (cx_kind() == TOK_IDENT && (cx_kind_at(1) == TOK_KW_ATTRIBUTE ||
                                   (cx_kind_at(1) == TOK_LBRACKET &&
                                    cx_kind_at(2) == TOK_LBRACKET))) {
        /* using X [[attr]] = T; */
        int save = cx_pos;
        cx_advance();
        parse_attrs(NULL);
        alias_attrs = cx_kind() == TOK_ASSIGN;
        cx_pos = save;
    }
    if (cx_kind() == TOK_IDENT &&
        (cx_kind_at(1) == TOK_ASSIGN || alias_attrs)) {
        const char *name = cx_cur()->t.text;
        cx_advance();
        parse_attrs(NULL);
        cx_expect(TOK_ASSIGN, "'='");
        struct cty *t = parse_type_id();
        cx_expect(TOK_SEMI, "';'");
        struct csym *y = scope_add(cx_scope, CS_TYPEDEF, name);
        y->type = t;
        return;
    }
    /* using-declarators, `,`-separated, each maybe a pack expansion
     * (using Bases::f...;) */
    for (;;) {
        struct csym *packs[8];
        int ell = -1;
        int ex = expansion_at(0, packs, 8, &ell);
        if (ex < 0)
            cx_error(at, "'...' expands no parameter pack");
        int reps = ex > 0 ? expansion_length(packs, ex) : 1;
        int start = cx_pos;
        for (int e = 0; e < reps; e++) {
            cx_pos = start;
            for (int k = 0; k < ex; k++)
                pack_push(packs[k], e);
            using_declarator(at);
            if (ex > 0)
                pack_pop(ex);
        }
        if (ex > 0) {
            cx_pos = ell;
            cx_expect(TOK_ELLIPSIS, "'...'");
        }
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_SEMI, "';'");
}

/* One using-declarator: [typename] A::name, what it names brought here */
static void using_declarator(const struct ctok *at)
{
    cx_accept(TOK_CX_TYPENAME);
    struct qname q = peek_qname();
    if (q.bad || !q.scope)
        cx_error(at, "expected a qualified name after 'using'");
    /* the qualifier's last component (B in `using A::B::B;`, _Base in
     * `using _Base::_Base;`): repeated, it names the constructors */
    const char *last = NULL;
    for (int i = cx_pos + q.fin - 1; i > cx_pos; i--)
        if (cx_toks[i].t.kind == TOK_COLONCOLON) {
            int j = i - 1;
            if (cx_toks[j].t.kind == TOK_GT || cx_toks[j].t.kind == TOK_SHR) {
                int d = 0;                      /* back over <...> */
                for (; j > cx_pos; j--) {
                    enum tok_kind tk = cx_toks[j].t.kind;
                    d += tk == TOK_GT ? 1 : tk == TOK_SHR ? 2
                         : tk == TOK_LT ? -1 : 0;
                    if (d <= 0 && tk == TOK_LT)
                        break;
                }
                j--;
            }
            if (j >= cx_pos && cx_toks[j].t.kind == TOK_IDENT)
                last = cx_toks[j].t.text;
            break;
        }
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
    struct cclass *here = cx_scope->k == SC_CLASS ? cx_scope->cls : NULL;
    if (here && q.scope->k == SC_CLASS && q.scope->cls->name &&
        (strcmp(name, q.scope->cls->name) == 0 ||
         (last && strcmp(name, last) == 0)) && !cx_pattern) {
        /* using B::B; : B's constructors are this class's too (11.9.4) */
        inherit_ctors(here, q.scope->cls, at);
        return;
    }
    struct csym *src = lookup_in(q.scope, name);
    if (!src)
        cx_error(at, "'%s' is not declared in '%s'", name,
                 q.scope->name ? q.scope->name : "::");
    if (src->k == CS_FUNC) {
        /* the named overloads join this scope's: entries standing for
         * them (an overload set is one list, and they are in theirs) */
        struct csym *mine = scope_find_here(cx_scope, name);
        if (!mine || mine->k != CS_FUNC)
            mine = func_sym(cx_scope, name, at);
        for (struct cfunc *f = src->fns; f; f = f->next) {
            struct cfunc *orig = f->alias_of ? f->alias_of : f;
            int have = 0;
            for (struct cfunc *g = mine->fns; g; g = g->next)
                have |= g == orig || g->alias_of == orig;
            if (have)
                continue;
            struct cfunc *a = xmalloc(sizeof *a);
            *a = *orig;
            a->alias_of = orig;
            a->next = NULL;
            struct cfunc **tail = &mine->fns;
            while (*tail)
                tail = &(*tail)->next;
            *tail = a;
        }
        return;
    }
    struct csym *y = scope_add(cx_scope, src->k, name);
    struct csym *hn = y->hnext, *nx = y->next;
    struct cscope *sc = y->scope;
    *y = *src;
    y->hnext = hn;
    y->next = nx;
    y->scope = sc;
    if (src->k == CS_FIELD && !y->fcls)    /* using Base::m; */
        y->fcls = src->scope->cls;
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
static void parse_explicit_instantiation(int is_extern);
static void skip_declaration(void);
static struct cfunc *outdef_want;
static struct cguide *guide_at(void);

static void parse_namespace(void)
{
    const struct ctok *at = cx_cur();
    int is_inline = 0;
    if (cx_kind() == TOK_KW_INLINE) {
        is_inline = 1;
        cx_advance();
    }
    cx_advance();                               /* namespace */
    struct attrs na;
    memset(&na, 0, sizeof na);
    parse_attrs(&na);
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
    parse_attrs(&na);
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
        if (i == nn - 1 && y->ns->is_inline)   /* (else ignored, as g++) */
            tags_from(&y->ns->abi_tags, &y->ns->nabi_tags, &na);
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
static int binding_follows(void);
static struct cstmt *parse_structured_binding(struct cty *ds_type,
                                              struct cexpr *given,
                                              const struct ctok *at);
static struct cstmt *decl_of(struct cvar *v, const struct ctok *at);

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
        if (cx_kind_at(1) == TOK_CX_TEMPLATE && cx_kind_at(2) != TOK_LT) {
            /* GNU: `inline template class X<A>;` — the class's vtable
             * and RTTI here, not its members */
            cx_advance();
            cx_advance();
            parse_explicit_instantiation(2);
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
        if (cx_kind_at(1) == TOK_CX_TEMPLATE) {
            cx_advance();
            cx_advance();
            parse_explicit_instantiation(1);
            return;
        }
        break;
    case TOK_KW_STATIC_ASSERT:
        parse_static_assert();
        return;
    case TOK_CX_TEMPLATE:
        if (!toplevel)
            cx_error(at, "a template cannot be declared in a block");
        parse_template_decl(enclosing_class(cx_scope), CA_PUBLIC);
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
    if (toplevel && (cx_kind() == TOK_IDENT || cx_kind() == TOK_CX_EXPLICIT) &&
        guide_at()) {
        /* C(P...) -> C<A...>; : a deduction guide, not a template */
        skip_declaration();
        return;
    }
    if (toplevel && abbrev_lparen() >= 0) {
        /* void f(C auto x): a function template */
        parse_abbreviated_template(enclosing_class(cx_scope), CA_PUBLIC);
        return;
    }
    struct dspec ds;
    parse_dspec(&ds);
    if (!ds.type && !at_ctor_declarator() && cx_kind() != TOK_TILDE &&
        !(toplevel && cx_kind() == TOK_IDENT && cx_kind_at(1) == TOK_COLONCOLON) &&
        !at_qualified_operator())
        cx_error(cx_cur(), "expected a declaration before %s",
                 tok_describe(&cx_cur()->t));
    if (cx_kind() == TOK_SEMI) {
        cx_advance();
        return;
    }
    if (ds.type && ds.type->k == CT_AUTO && !ds.type->dauto &&
        binding_follows()) {
        if (toplevel)
            cx_error(at, "a structured binding at namespace scope is not "
                         "supported yet");
        struct cstmt *d = parse_structured_binding(ds.type, NULL, at);
        if (out)
            *out = d;
        cx_expect(TOK_SEMI, "';' after the declaration");
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
                    t->cls->anon = 0;     /* (it has linkage now) */
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
            if (outdef_want && outdef_want->cls &&
                d.qual != outdef_want->cls->scope) {
                /* replaying a definition for another class */
                skip_declaration();
                if (d.saved)
                    cx_scope = d.saved;
                return;
            }
            if (d.qual && d.kind != DN_CTOR && d.kind != DN_DTOR) {
                struct csym *y = scope_find_here(d.qual, d.name);
                if ((!y || y->k != CS_FUNC) && d.qual->k == SC_NAMESPACE) {
                    /* N::f() { } of an f an inline namespace of N
                     * declares: that namespace's */
                    struct cscope *in = inline_ns_declaring(d.qual, d.name);
                    if (in) {
                        target = in;
                        y = scope_find_here(in, d.name);
                    }
                }
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
            if (outdef_want && f != outdef_want &&
                (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON)) {
                /* replaying one member's definition: not this one */
                skip_declaration();
                if (d.saved)
                    cx_scope = d.saved;
                return;
            }
            if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON ||
                cx_kind() == TOK_CX_TRY) {
                if (!toplevel)
                    cx_error(cx_cur(), "a function definition is not "
                                       "allowed here");
                f->def_scope = cx_scope;
                define_function(f, t);
                if (d.saved)
                    cx_scope = d.saved;
                return;
            }
            if (cx_accept(TOK_ASSIGN)) {
                if (cx_accept(TOK_KW_DEFAULT)) {
                    f->is_defaulted = 1;
                    if (d.qual && f->cls && f->cls->complete)
                        class_default_outside(f);
                } else if (cx_accept(TOK_CX_DELETE)) {
                    f->is_deleted = 1;
                } else {
                    cx_error(cx_cur(), "expected 'default' or 'delete'");
                }
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
            il_collect = 1;
            il_last_decls = NULL;
            init_variable(v, d.at);
            il_collect = 0;
            struct cstmt *pre = il_last_decls;
            il_last_decls = NULL;
            if (pre && out) {
                /* the backing arrays first */
                if (tail)
                    tail->more = pre;
                else
                    *out = pre;
                tail = pre;
                while (tail->more)
                    tail = tail->more;
            }
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

/* ---- structured bindings ---- */

/* At `[` after `auto` (and a `&` or `&&` before it)? */
static int binding_follows(void)
{
    if (cx_kind() == TOK_LBRACKET)
        return cx_kind_at(1) == TOK_IDENT;
    return (cx_kind() == TOK_AMP || cx_kind() == TOK_ANDAND) &&
           cx_kind_at(1) == TOK_LBRACKET && cx_kind_at(2) == TOK_IDENT;
}

static struct ctemplate *std_template(const char *name)
{
    struct csym *ns = scope_find_here(cx_global, "std");
    if (!ns || ns->k != CS_NAMESPACE)
        return NULL;
    struct csym *y = scope_find_here(ns->ns, name);
    return y && y->k == CS_TEMPLATE && y->tmpl->kind == TK_CLASS ? y->tmpl
                                                                 : NULL;
}

static struct ctarg size_targ(long i)
{
    struct ctarg a;
    memset(&a, 0, sizeof a);
    a.kind = TP_VALUE;
    a.value = i;
    a.vtype = ct_size_t();
    return a;
}

/* std::tuple_size<E>::value, if E is tuple-like (-1 if not) */
static long tuple_size_of(struct cty *E, const struct ctok *at)
{
    struct ctemplate *ts = std_template("tuple_size");
    if (!ts)
        return -1;
    struct ctarg a;
    memset(&a, 0, sizeof a);
    a.kind = TP_TYPE;
    a.type = E;
    struct cclass *c = class_instance(ts, &a, 1, at);
    class_ensure(c);
    if (!c->complete)
        return -1;
    struct csym *y = class_member(c, "value");
    long v = 0;
    if (!y || y->k != CS_VAR || !expr_const(expr_var(y->var), &v))
        cx_error(at, "std::tuple_size<%s>::value is not a constant",
                 ct_name(E));
    return v;
}

/* auto [a, b, ...] = e; (9.6): a hidden variable holds the initializer
 * (or refers to it), and each name is a part of it — an array's
 * element, a class's data member, or (a tuple-like class:
 * std::tuple_size<E> is complete) a reference initialized from
 * get<i>. The cursor at the `&` or `[`; ds_type: `auto` with its cv;
 * given: the initializer when a range-based for gives it (else it
 * follows). The declarations made, as a DECL chain. */
static struct cstmt *parse_structured_binding(struct cty *ds_type,
                                              struct cexpr *given,
                                              const struct ctok *at)
{
    struct cty *declared = ds_type;
    if (cx_accept(TOK_AMP))
        declared = ct_ref(ds_type, 0);
    else if (cx_accept(TOK_ANDAND))
        declared = ct_ref(ds_type, 1);
    cx_expect(TOK_LBRACKET, "'['");
    const char *names[64];
    const struct ctok *ats[64];
    int n = 0;
    do {
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a name to bind");
        if (n == 64)
            cx_error(cx_cur(), "too many names in a structured binding");
        ats[n] = cx_cur();
        names[n++] = cx_cur()->t.text;
        cx_advance();
    } while (cx_accept(TOK_COMMA));
    cx_expect(TOK_RBRACKET, "']' after the names");
    parse_attrs(NULL);
    enum init_form form = INIT_COPY;
    struct cexpr **args = &given;
    int na = 1;
    if (!given) {
        if (cx_kind() != TOK_ASSIGN && cx_kind() != TOK_LBRACE &&
            cx_kind() != TOK_LPAREN)
            cx_error(cx_cur(), "a structured binding needs an initializer");
        parse_init_args(&form, &args, &na);
    }
    if (na != 1)
        cx_error(at, "a structured binding is initialized by one value");
    /* the hidden variable */
    struct cvar *e = new_local(cx_fmt("__cx_sb%d", cx_uid()), declared, at);
    struct cexpr *src = args[0];
    if ((form == INIT_LIST || form == INIT_COPY_LIST) && src->na == 1)
        src = src->a[0];
    if (!ct_is_ref(declared) && src->t->k == CT_ARRAY) {
        /* an array: copied, element by element */
        struct cty *el = src->t;
        while (el->k == CT_ARRAY)
            el = el->to;
        if (el->k == CT_CLASS && !el->cls->trivial_copy)
            cx_error(at, "copying an array of '%s' is not supported yet",
                     ct_name(el));
        e->type = ct_qual(src->t, declared->q);
        struct cexpr *cp = ex_new(E_CONSTRUCT, e->type, VC_PRVALUE);
        cp->a = xmalloc(sizeof *cp->a);
        cp->a[0] = src;
        cp->na = 1;
        e->ctor = cp;
    } else {
        init_variable_with(e, form, args, na, at);
    }
    struct cstmt *head = decl_of(e, at), *tail = head;
    struct cty *E = ct_strip_ref(e->type);
    long ts;
    if (E->k == CT_ARRAY) {
        if (E->n != n)
            cx_error(at, "%d names bind an array of %ld", n, E->n);
    } else if (E->k != CT_CLASS) {
        cx_error(at, "cannot bind names to a '%s'", ct_name(E));
    } else if ((ts = tuple_size_of(E, at)) >= 0) {
        /* tuple-like */
        if (ts != n)
            cx_error(at, "%d names bind a tuple-like '%s' of %ld", n,
                     ct_name(E), ts);
        struct ctemplate *te = std_template("tuple_element");
        if (!te)
            cx_error(at, "std::tuple_element is not declared");
        struct csym *mg = class_member(E->cls, "get");
        int member_get = 0;
        if (mg && mg->k == CS_FUNC)
            for (struct cfunc *g = mg->fns; g; g = g->next)
                if (g->tmpl)
                    member_get = 1;
        for (int i = 0; i < n; i++) {
            struct ctarg ta[2];
            ta[0] = size_targ(i);
            memset(&ta[1], 0, sizeof ta[1]);
            ta[1].kind = TP_TYPE;
            ta[1].type = E;
            struct cclass *tc = class_instance(te, ta, 2, at);
            class_ensure(tc);
            struct csym *ty = class_member(tc, "type");
            if (!ty || ty->k != CS_TYPEDEF)
                cx_error(at, "std::tuple_element<%d, %s>::type is not a "
                             "type", i, ct_name(E));
            /* e as an xvalue unless it is an lvalue reference */
            struct cexpr *obj = expr_var(e);
            if (e->type->k != CT_LREF) {
                struct cexpr *x = ex_new(E_CAST, obj->t, VC_XVALUE);
                x->a = xmalloc(sizeof *x->a);
                x->a[0] = obj;
                x->na = 1;
                x->lvcast = 1;
                obj = x;
            }
            struct ctarg *gi = xmalloc(sizeof *gi);
            *gi = size_targ(i);
            struct cexpr *g = member_get
                ? expr_call_named_targs(obj, "get", gi, 1, NULL, 0, ats[i])
                : expr_call_named_targs(NULL, "get", gi, 1, &obj, 1, ats[i]);
            struct cty *rt = ct_ref(ty->type, g->vc != VC_LVALUE);
            struct cvar *r = new_local(names[i], rt, ats[i]);
            init_variable_with(r, INIT_COPY, &g, 1, ats[i]);
            struct csym *y = scope_add(cx_scope, CS_VAR, names[i]);
            y->var = r;
            struct cstmt *d = decl_of(r, ats[i]);
            tail->more = d;
            tail = d;
        }
        return head;
    }
    /* the names: parts of e */
    struct cfield *fls[64];
    if (E->k == CT_CLASS) {
        struct cclass *c = E->cls;
        class_ensure(c);
        int k = 0;
        for (int i = 0; i < c->nfields; i++) {
            if (!c->fields[i]->name)
                continue;
            if (k < 64)
                fls[k] = c->fields[i];
            k++;
        }
        if (!k)
            for (int i = 0; i < c->nbases; i++)
                if (c->bases[i].cls->nfields)
                    cx_error(at, "binding the members of a base of '%s' is "
                                 "not supported yet", ct_name(E));
        if (k != n)
            cx_error(at, "%d names bind '%s', which has %d members", n,
                     ct_name(E), k);
    }
    for (int i = 0; i < n; i++) {
        struct cvar *b = new_local(names[i], E->k == CT_ARRAY ? E->to
                                                              : fls[i]->type,
                                   ats[i]);
        b->sb_var = e;
        if (E->k == CT_ARRAY)
            b->sb_index = i;
        else
            b->sb_field = fls[i];
        struct csym *y = scope_add(cx_scope, CS_VAR, names[i]);
        y->var = b;
    }
    return head;
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

/* A local's declaration statement, the variable made already. */
static struct cstmt *decl_of(struct cvar *v, const struct ctok *at)
{
    struct cstmt *s = st_new(S_DECL);
    s->line = at->t.line;
    s->file = at->file;
    s->var = v;
    s->blk = cx_curblk;
    struct cty *e = v->type;
    while (e->k == CT_ARRAY)
        e = e->to;
    if (e->k == CT_CLASS)
        s->dtor = class_dtor(e->cls) != NULL;
    return s;
}

/* for (decl : range) body, at the declaration (the `(` read), colon the
 * `:`'s position — as the standard defines it (8.6.5):
 *     { auto &&__range = range;
 *       auto __begin = begin-expr, __end = end-expr;
 *       for (; __begin != __end; ++__begin) { decl = *__begin; body } }
 * begin-expr being __range for an array, __range.begin() for a class
 * with begin or end members, else begin(__range) by argument-dependent
 * lookup (and end alike). s is the S_FOR. */
static void parse_range_for(struct cstmt *s, const struct ctok *at,
                            int colon)
{
    int decl_pos = cx_pos;
    int u = cx_uid();
    cx_pos = colon + 1;
    struct cvar *r;
    struct cstmt *pre = NULL;
    struct cty *ct;
    if (cx_kind() == TOK_LBRACE) {
        /* auto &&__range = { ... }: a std::initializer_list */
        struct cexpr *list = parse_braced_list();
        cx_expect(TOK_RPAREN, "')' after the range");
        r = new_local(cx_fmt("__for_range%d", u),
                      ct_ref(ct_basic(CT_AUTO), 1), at);
        il_collect = 1;
        il_last_decls = NULL;
        init_variable_with(r, INIT_COPY_LIST, &list, 1, at);
        il_collect = 0;
        pre = il_last_decls;
        il_last_decls = NULL;
        ct = ct_unqual(ct_strip_ref(r->type));
    } else {
        struct cexpr *re = expr_parse();
        cx_expect(TOK_RPAREN, "')' after the range");
        struct cty *rt = ct_ref(re->t, re->vc != VC_LVALUE);
        r = new_local(cx_fmt("__for_range%d", u), rt, at);
        r->init = bind_ref(re, rt, "a range-based for");
        ct = ct_unqual(re->t);
    }
    int body_pos = cx_pos;
    struct cexpr *b0, *e0;
    if (ct->k == CT_ARRAY) {
        if (ct->n < 0)
            cx_error(at, "a range-based for over an array of unknown size");
        b0 = rvalue(expr_var(r));
        e0 = expr_binary(TOK_PLUS, rvalue(expr_var(r)),
                         ex_int(ct->n, ct_basic(CT_LONG)));
    } else if (ct->k == CT_CLASS) {
        class_ensure(ct->cls);
        if (class_member(ct->cls, "begin") || class_member(ct->cls, "end")) {
            b0 = expr_call_named(expr_var(r), "begin", NULL, 0, at);
            e0 = expr_call_named(expr_var(r), "end", NULL, 0, at);
        } else {
            struct cexpr *a = expr_var(r);
            b0 = expr_call_named(NULL, "begin", &a, 1, at);
            a = expr_var(r);
            e0 = expr_call_named(NULL, "end", &a, 1, at);
        }
    } else {
        cx_error(at, "a range-based for over '%s', which has no begin/end",
                 ct_name(ct));
        return;
    }
    struct cvar *bv = new_local(cx_fmt("__for_begin%d", u),
                                ct_unqual(ct_decay(ct_strip_ref(b0->t))), at);
    init_variable_with(bv, INIT_COPY, &b0, 1, at);
    struct cvar *ev = new_local(cx_fmt("__for_end%d", u),
                                ct_unqual(ct_decay(ct_strip_ref(e0->t))), at);
    init_variable_with(ev, INIT_COPY, &e0, 1, at);
    struct cstmt *d1 = decl_of(r, at), *d2 = decl_of(bv, at),
                 *d3 = decl_of(ev, at);
    d1->more = d2;
    d2->more = d3;
    s->init = d1;
    if (pre) {                /* the backing array first */
        struct cstmt *p = pre;
        while (p->more)
            p = p->more;
        p->more = d1;
        s->init = pre;
    }
    s->e = convert_bool(expr_binary(TOK_NEQ, expr_var(bv), expr_var(ev)),
                        "a range-based for");
    s->e2 = expr_preinc(expr_var(bv));

    /* the body: the declaration, from *__begin, then the statement */
    struct cstmt *blk = st_new(S_BLOCK);
    struct cstmt *saveblk = cx_curblk;
    blk->blk = cx_curblk;
    cx_curblk = blk;
    scope_push(SC_BLOCK, NULL);
    cx_pos = decl_pos;
    struct dspec ds;
    parse_dspec(&ds);
    if (!ds.type)
        cx_error(cx_cur(), "expected a declaration in a range-based for");
    struct cstmt *dv;
    if (ds.type->k == CT_AUTO && !ds.type->dauto && binding_follows()) {
        struct cexpr *de = expr_deref(expr_var(bv));
        dv = parse_structured_binding(ds.type, de, at);
        if (cx_kind() != TOK_COLON)
            cx_error(cx_cur(), "expected ':' in a range-based for");
    } else {
        struct declarator d;
        memset(&d, 0, sizeof d);
        struct cty *t = parse_declarator(ds.type, &d, DK_NAMED);
        if (cx_kind() != TOK_COLON)
            cx_error(cx_cur(), "expected ':' in a range-based for");
        struct cvar *v = new_local(d.name, t, d.at);
        struct csym *y = scope_add(cx_scope, CS_VAR, d.name);
        y->var = v;
        struct cexpr *de = expr_deref(expr_var(bv));
        init_variable_with(v, INIT_COPY, &de, 1, d.at);
        dv = decl_of(v, d.at);
    }
    cx_pos = body_pos;
    dv->next = parse_stmt();
    blk->body = dv;
    scope_pop();
    cx_curblk = saveblk;
    s->body = blk;
}

/* A catch clause's parameter: a local of its handler, initialized from
 * the caught object — referring to it, copying it, or (a pointer) taking
 * the value __cxa_begin_catch adjusted. */
static struct cstmt *handler_param(struct cty *t, const char *name,
                                   const struct ctok *at)
{
    struct cvar *v = new_local(name, t, at);
    struct csym *y = scope_add(cx_scope, CS_VAR, name);
    y->var = v;
    struct cstmt *s = st_new(S_DECL);
    s->line = at->t.line;
    s->file = at->file;
    s->var = v;
    struct cty *ot = ct_is_ref(t) ? t->to : ct_unqual(t);
    struct cexpr *obj = ex_new(E_EXCOBJ, ot, VC_LVALUE);
    if (ct_is_ref(t)) {
        v->init = ex_addr(obj);
    } else if (ot->k == CT_PTR) {
        obj->vc = VC_PRVALUE;
        obj->is_array = 1;
        v->init = obj;
    } else if (ot->k == CT_CLASS) {
        v->ctor = construct(ot->cls, INIT_DIRECT, &obj, 1, at);
        s->dtor = class_dtor(ot->cls) != NULL;
    } else {
        v->init = rvalue(obj);
    }
    return s;
}

/* try { } catch (T x) { } ... */
static struct cstmt *parse_try(const struct ctok *at)
{
    if (!cx_exceptions)
        cx_error(at, "'try' with exceptions disabled (-fno-exceptions)");
    cx_advance();
    struct cstmt *body = parse_compound();
    struct cstmt *s = parse_handlers(at);
    s->body = body;
    return s;
}

/* catch (T x) { } ...: a try's handlers (S_TRY, no body yet). */
static struct cstmt *parse_handlers(const struct ctok *at)
{
    struct cstmt *s = st_new(S_TRY);
    s->line = at->t.line;
    s->file = at->file;
    s->blk = cx_curblk;
    int cap = 0;
    while (cx_kind() == TOK_CX_CATCH) {
        const struct ctok *hat = cx_cur();
        cx_advance();
        cx_expect(TOK_LPAREN, "'(' after catch");
        struct cscope *hs = scope_push(SC_BLOCK, NULL);
        struct chandler h;
        memset(&h, 0, sizeof h);
        struct cstmt *param = NULL;
        if (!cx_accept(TOK_ELLIPSIS)) {
            struct dspec ds;
            parse_dspec(&ds);
            if (!ds.type)
                cx_error(cx_cur(), "expected the type a handler catches");
            struct declarator d;
            memset(&d, 0, sizeof d);
            h.type = parse_declarator(ds.type, &d, DK_NAMED | DK_ABSTRACT);
            if (d.saved)
                cx_scope = d.saved;
            struct cty *ot = ct_is_ref(h.type) ? h.type->to : h.type;
            if (ot->k == CT_CLASS && !ct_is_complete(ot))
                cx_error(hat, "catching incomplete type '%s'", ct_name(ot));
            if (d.name)
                param = handler_param(h.type, d.name, d.at ? d.at : hat);
        }
        cx_expect(TOK_RPAREN, "')' after the handler's parameter");
        h.body = parse_block_body(hs);
        scope_pop();
        if (param) {
            param->blk = h.body;
            param->next = h.body->body;
            h.body->body = param;
        }
        if (s->nhandlers == cap) {
            cap = cap ? cap * 2 : 4;
            s->handlers = xrealloc(s->handlers,
                                   (size_t)cap * sizeof *s->handlers);
        }
        s->handlers[s->nhandlers++] = h;
    }
    if (!s->nhandlers)
        cx_error(at, "a try block needs a handler");
    return s;
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
        int n, ctn, cargs;
        if (concept_at(&ctn, &cargs) &&
            (cx_kind_at(ctn) == TOK_CX_AUTO ||
             cx_kind_at(ctn) == TOK_CX_DECLTYPE))
            return 1;                   /* C auto x = ... */
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
/* At a type the keywords spell, then `(` or `{`: a functional cast
 * (wchar_t('0') == L'0'), not a declaration. */
static int keyword_cast_at(void)
{
    int i = 0;
    while (is_decl_keyword(cx_kind_at(i)) &&
           cx_kind_at(i) != TOK_CX_TYPENAME && cx_kind_at(i) != TOK_CX_AUTO &&
           cx_kind_at(i) != TOK_KW_CONST && cx_kind_at(i) != TOK_KW_VOLATILE &&
           cx_kind_at(i) != TOK_KW_STATIC && cx_kind_at(i) != TOK_CX_CONSTEXPR &&
           cx_kind_at(i) != TOK_KW_STRUCT && cx_kind_at(i) != TOK_CX_CLASS &&
           cx_kind_at(i) != TOK_KW_UNION && cx_kind_at(i) != TOK_KW_ENUM &&
           cx_kind_at(i) != TOK_CX_DECLTYPE && cx_kind_at(i) != TOK_KW_TYPEOF &&
           cx_kind_at(i) != TOK_KW_ATTRIBUTE)
        i++;
    return i > 0 && (cx_kind_at(i) == TOK_LPAREN || cx_kind_at(i) == TOK_LBRACE)
           && !(cx_kind_at(i) == TOK_LPAREN && cx_kind_at(i + 1) == TOK_STAR);
}

static struct cexpr *parse_condition(struct cstmt **decl)
{
    *decl = NULL;
    if (at_decl_stmt() && cx_kind() != TOK_LPAREN && !keyword_cast_at()) {
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

/* Past a statement, unread (a discarded branch of `if constexpr`). */
static void skip_stmt(void)
{
    switch (cx_kind()) {
    case TOK_LBRACE:
        cx_skip_balanced();
        return;
    case TOK_KW_IF:
        cx_advance();
        cx_accept(TOK_CX_CONSTEXPR);
        if (cx_kind() == TOK_BANG)
            cx_advance();                 /* if !consteval */
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        skip_stmt();
        if (cx_accept(TOK_KW_ELSE))
            skip_stmt();
        return;
    case TOK_KW_WHILE: case TOK_KW_FOR: case TOK_KW_SWITCH:
        cx_advance();
        cx_skip_balanced();
        skip_stmt();
        return;
    case TOK_KW_DO:
        cx_advance();
        skip_stmt();
        cx_expect(TOK_KW_WHILE, "'while' after a do body");
        cx_skip_balanced();
        cx_expect(TOK_SEMI, "';' after do-while");
        return;
    case TOK_CX_TRY:
        cx_advance();
        cx_skip_balanced();
        while (cx_accept(TOK_CX_CATCH)) {
            cx_skip_balanced();
            cx_skip_balanced();
        }
        return;
    case TOK_KW_CASE: case TOK_KW_DEFAULT:
        while (cx_kind() != TOK_COLON && cx_kind() != TOK_EOF)
            cx_advance();
        cx_advance();
        skip_stmt();
        return;
    case TOK_IDENT:
        if (cx_kind_at(1) == TOK_COLON) {    /* a label */
            cx_advance();
            cx_advance();
            skip_stmt();
            return;
        }
        break;
    default:
        break;
    }
    while (cx_kind() != TOK_SEMI) {
        if (cx_kind() == TOK_EOF)
            cx_error(cx_cur(), "expected ';'");
        if (cx_kind() == TOK_LPAREN || cx_kind() == TOK_LBRACKET ||
            cx_kind() == TOK_LBRACE)
            cx_skip_balanced();
        else
            cx_advance();
    }
    cx_advance();
}

static struct cstmt *parse_stmt_or_none(void);

/* A statement — one that is nothing (a static_assert, a using-declaration,
 * a typedef) a null statement, so an if or a loop has a body */
static struct cstmt *parse_stmt(void)
{
    struct cstmt *s = parse_stmt_or_none();
    return s ? s : st_new(S_NULL);
}

static struct cstmt *parse_stmt_or_none(void)
{
    /* [[likely]], [[fallthrough]] ...: attributes of the statement */
    while (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_LBRACKET)
        parse_attrs(NULL);
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
        if (cx_kind() == TOK_CX_CONSTEVAL ||
            (cx_kind() == TOK_BANG && cx_kind_at(1) == TOK_CX_CONSTEVAL)) {
            /* C++23 if consteval { } else { }: the first in a constant
             * evaluation, the second at run time — as
             * if (__builtin_is_constant_evaluated()) (! : reversed) */
            int neg = cx_accept(TOK_BANG);
            cx_advance();
            struct cexpr *c = ex_new(E_BUILTIN, ct_basic(CT_BOOL),
                                     VC_PRVALUE);
            c->name = "__builtin_is_constant_evaluated";
            s->e = c;
            if (cx_kind() != TOK_LBRACE)
                cx_error(cx_cur(), "expected '{' after 'if consteval'");
            struct cstmt *a = parse_stmt(), *b = NULL;
            if (cx_accept(TOK_KW_ELSE))
                b = parse_stmt();
            s->body = neg ? (b ? b : st_new(S_NULL)) : a;
            s->els = neg ? a : b;
            return s;
        }
        int is_constexpr = cx_accept(TOK_CX_CONSTEXPR);
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
        if (is_constexpr) {
            /* the condition a constant; the other branch discarded —
             * never read, so what it says need not hold for these
             * template arguments (8.5.2) */
            long v;
            if (!expr_const(s->e, &v))
                cx_error(at, "the condition of 'if constexpr' is not a "
                             "constant expression");
            s->e = ex_int(v != 0, ct_basic(CT_BOOL));
            if (v)
                s->body = parse_stmt();
            else
                skip_stmt(), s->body = st_new(S_NULL);
            if (cx_accept(TOK_KW_ELSE)) {
                if (v)
                    skip_stmt();
                else
                    s->els = parse_stmt();
            }
            scope_pop();
            return s;
        }
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
                    parse_range_for(s, at, i);
                    scope_pop();
                    return s;
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
        if (e->t->k == CT_CLASS)          /* its one conversion to one */
            e = rvalue(convert(e, ct_basic(CT_LONG), "a switch"));
        if (!ct_is_integer(e->t))
            cx_error(at, "switch on a non-integral %s", ct_name(e->t));
        /* (an enumeration, scoped too, switches on its value) */
        s->e = e->t->k == CT_ENUM
               ? ex_cast(e, ct_promote(e->t->en->underlying))
               : convert(e, ct_promote(e->t), "a switch");
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
    case TOK_CX_CO_RETURN: {
        s = st_new(S_CORETURN);
        cx_advance();
        struct cexpr *e = NULL;
        int braced = cx_kind() == TOK_LBRACE;
        if (braced)
            e = parse_braced_list();
        else if (cx_kind() != TOK_SEMI)
            e = expr_parse();
        s->e = coro_return(e, braced, at);
        cx_expect(TOK_SEMI, "';' after co_return");
        return s;
    }
    case TOK_KW_RETURN: {
        if (cx_curfn->coro)
            cx_error(at, "a coroutine returns with co_return, not return");
        s = st_new(S_RETURN);
        cx_advance();
        struct cty *rt = cx_curfn->type->to;
        if (cx_kind() == TOK_SEMI && ct_has_auto(rt))
            rt = cx_curfn->type->to = ct_basic(CT_VOID);  /* deduced */
        if (cx_kind() != TOK_SEMI) {
            struct cexpr *e;
            enum init_form form = INIT_COPY;
            if (cx_kind() == TOK_LBRACE) {
                e = parse_braced_list();
                form = INIT_COPY_LIST;
            } else {
                e = expr_parse();
            }
            if (ct_has_auto(rt)) {
                /* the return type deduced, as an `auto` variable's */
                rt = e->t->k == CT_VOID && rt->k == CT_AUTO
                     ? ct_basic(CT_VOID) : deduce_auto(rt, form, &e, 1, at);
                cx_curfn->type->to = rt;
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
    case TOK_KW_ASM:
        return parse_asm_stmt(at);
    case TOK_CX_TRY:
        return parse_try(at);
    case TOK_CX_USING:
        parse_using();
        return NULL;
    case TOK_KW_STATIC_ASSERT:
        parse_static_assert();
        return NULL;
    case TOK_CX_NAMESPACE:
        if (cx_kind_at(1) == TOK_IDENT && cx_kind_at(2) == TOK_ASSIGN) {
            parse_namespace();          /* a block's namespace alias */
            return NULL;
        }
        break;
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

/* ---- templates ---- */

struct parse_state {
    int pos, half, extern_c, pattern, in_targs, class_depth, uneval;
    int npend, cappend, dependent_ok, packs, insts, no_user;
    struct pending *pend;
    struct cscope *scope;
    struct cfunc *curfn;
    struct cstmt *curblk;
};

struct parse_state *parse_save(void)
{
    struct parse_state *st = xmalloc(sizeof *st);
    st->pos = cx_pos;
    st->half = cx_half_gt;
    st->extern_c = cx_extern_c;
    st->pattern = cx_pattern;
    st->uneval = cx_unevaluated;
    st->in_targs = cx_in_targs;
    st->class_depth = class_depth;
    st->npend = npend;
    st->cappend = cappend;
    st->pend = pend;
    st->dependent_ok = dependent_type_ok;
    st->packs = pack_mark();
    st->insts = cx_inst_mark();
    st->no_user = expr_swap_no_user_conv(0);
    expr_swap_no_user_conv(st->no_user);
    st->scope = cx_scope;
    st->curfn = cx_curfn;
    st->curblk = cx_curblk;
    return st;
}

void parse_restore(struct parse_state *st)
{
    cx_inst_reset(st->insts);
    expr_swap_no_user_conv(st->no_user);
    cx_pos = st->pos;
    cx_half_gt = st->half;
    cx_extern_c = st->extern_c;
    cx_pattern = st->pattern;
    cx_unevaluated = st->uneval;
    cx_in_targs = st->in_targs;
    class_depth = st->class_depth;
    npend = st->npend;
    cappend = st->cappend;
    pend = st->pend;
    dependent_type_ok = st->dependent_ok;
    pack_reset(st->packs);
    cx_scope = st->scope;
    cx_curfn = st->curfn;
    cx_curblk = st->curblk;
}

/* At `<`: past its matching `>` — half of a closing `>>` counts. */
void skip_template_args(void)
{
    const struct ctok *at = cx_cur();
    int depth = 0;
    do {
        switch (cx_kind()) {
        case TOK_LT:
            depth++;
            cx_advance();
            break;
        case TOK_GT:
            depth--;
            cx_advance();
            break;
        case TOK_SHR:
            if (depth >= 2) {
                depth -= 2;
                cx_advance();
            } else {
                cx_close_angle();
                depth--;
            }
            break;
        case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE:
            cx_skip_balanced();
            break;
        case TOK_EOF: case TOK_SEMI: case TOK_RBRACE:
            cx_error(at, "unterminated template argument list");
            break;
        default:
            cx_advance();
        }
    } while (depth > 0);
}

static struct cty *dep_member(struct cty *base, const char *name)
{
    struct cty *d = xcalloc(1, sizeof *d);
    d->k = CT_DEP;
    int n = base && base->k == CT_DEP ? base->ndnames : 0;
    d->to = base && base->k == CT_DEP ? base->to : base;
    d->dnames = xmalloc((size_t)(n + 1) * sizeof *d->dnames);
    for (int i = 0; i < n; i++)
        d->dnames[i] = base->dnames[i];
    d->dnames[n] = name;
    d->ndnames = n + 1;
    return d;
}

int targ_is_dependent(const struct ctarg *a)
{
    return targ_dependent(a);
}

static int targ_dependent(const struct ctarg *a)
{
    if (a->is_pack) {
        for (int i = 0; i < a->nelems; i++)
            if (targ_dependent(&a->elems[i]))
                return 1;
        return 0;
    }
    switch (a->kind) {
    case TP_TYPE:
        return ct_dependent(a->type);
    case TP_VALUE:
        return a->vtype && (a->vtype->k == CT_TPARAM ||
                            a->vtype->k == CT_DEP);
    default:
        return a->tmpl && a->tmpl->nparams < 0;
    }
}

/* Past one template argument (a default's tokens): to its `,` or `>`. */
static void skip_targ(void)
{
    int depth = 0;
    for (;;) {
        enum tok_kind k = cx_kind();
        if (depth == 0 && (k == TOK_COMMA || k == TOK_GT || k == TOK_SHR))
            return;
        if (k == TOK_EOF || k == TOK_SEMI)
            cx_error(cx_cur(), "unterminated template argument");
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            cx_skip_balanced();
            continue;
        }
        if (k == TOK_LT && cx_pos > 0 &&
            cx_toks[cx_pos - 1].t.kind == TOK_IDENT) {
            /* after a name that is no template: less-than (bool = W <
             * sizeof(T) * 8, as <random> writes) */
            struct csym *y = lookup(cx_scope, cx_toks[cx_pos - 1].t.text);
            int value = y && (y->k == CS_VAR || y->k == CS_ENUMERATOR ||
                              y->k == CS_FIELD) &&
                        !(cx_pos > 1 &&
                          cx_toks[cx_pos - 2].t.kind == TOK_COLONCOLON);
            if (value) {
                cx_advance();
                continue;
            }
        }
        if (k == TOK_LT)
            depth++;
        else if (k == TOK_GT)
            depth--;
        else if (k == TOK_SHR && depth == 1) {
            /* X<Y>> : the first `>` closes X's arguments, the second
             * the list this argument is in — left for the caller */
            cx_half_gt = 1;
            return;
        } else if (k == TOK_SHR)
            depth -= 2;
        cx_advance();
    }
}

/* Does the template argument at the cursor (to its `,` or `>`) mention a
 * template parameter? */
static int targ_mentions_param(void)
{
    int depth = 0;
    for (int i = cx_pos; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_CX_OPERATOR && i + 1 < cx_ntoks) {
            /* operator> ... : the operator is a name's, not a bracket */
            enum tok_kind o = cx_toks[i + 1].t.kind;
            i += (o == TOK_LPAREN || o == TOK_LBRACKET) ? 2 : 1;
            continue;
        }
        if (depth == 0 && (k == TOK_COMMA || k == TOK_GT || k == TOK_SHR ||
                           k == TOK_SEMI))
            return 0;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE ||
            k == TOK_LT)
            depth++;
        else if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE ||
                 k == TOK_GT)
            depth--;
        else if (k == TOK_SHR)
            depth -= 2;
        if (depth < 0)
            return 0;
        if (k != TOK_IDENT)
            continue;
        struct csym *y = lookup(cx_scope, cx_toks[i].t.text);
        if (y && ((y->k == CS_VAR && (y->var->is_tparam ||
                                      (y->var->fparam &&
                                       ct_dependent(y->var->type)))) ||
                  ((y->k == CS_TYPEDEF || y->k == CS_CLASS) &&
                   ct_dependent(y->type)) || y->k == CS_PACK ||
                  (y->k == CS_TEMPLATE && y->tmpl->nparams < 0)))
            return 1;
    }
    return 0;
}

/* ---- dependent expressions (Itanium 5.1.6) ----
 * A pattern's value that mentions template parameters cannot be
 * evaluated; it is read into its mangling, which is also how two such
 * are told apart. Types inside are read as patterns and spelled without
 * substitutions. */

static const char *dep_expr_unary(void);
static const char *dep_expr_bin(int prec);

/* A type inside an expression's mangling: a reference (\1 index \1)
 * mangle.c resolves with its substitution table — the type's parts are
 * candidates like any others. */
static const char *type_ref(struct cty *t)
{
    return cx_fmt("\1%d\1", mangle_type_ref(t));
}

/* A template parameter named in an expression: T_ (no substitution
 * candidate there, unlike the parameter as a type). */
static const char *param_ref(int index)
{
    return cx_fmt("\2%d\2", index);
}

static const char *dep_type_mangle(void)
{
    return type_ref(parse_type_id());
}

static const char *dep_binop(enum tok_kind k, int *prec)
{
    switch (k) {
    case TOK_OROR: *prec = 1; return "oo";
    case TOK_ANDAND: *prec = 2; return "aa";
    case TOK_PIPE: *prec = 3; return "or";
    case TOK_CARET: *prec = 4; return "eo";
    case TOK_AMP: *prec = 5; return "an";
    case TOK_EQEQ: *prec = 6; return "eq";
    case TOK_NEQ: *prec = 6; return "ne";
    case TOK_LT: *prec = 7; return "lt";
    case TOK_GT: *prec = cx_in_targs > 0 ? 0 : 7; return "gt";
    case TOK_LE: *prec = 7; return "le";
    case TOK_GE: *prec = 7; return "ge";
    case TOK_SPACESHIP: *prec = 8; return "ss";
    case TOK_SHL: *prec = 9; return "ls";
    case TOK_SHR: *prec = cx_in_targs > 0 ? 0 : 9; return "rs";
    case TOK_PLUS: *prec = 10; return "pl";
    case TOK_MINUS: *prec = 10; return "mi";
    case TOK_STAR: *prec = 11; return "ml";
    case TOK_SLASH: *prec = 11; return "dv";
    case TOK_PERCENT: *prec = 11; return "rm";
    case TOK_ARROWSTAR: *prec = 12; return "pm";
    case TOK_DOTSTAR: *prec = 12; return "ds";
    default: *prec = 0; return NULL;
    }
}

/* A fold-expression's operator (C++17, 7.5.6): its mangled name */
static const char *fold_op(enum tok_kind k)
{
    int p;
    if (k == TOK_COMMA)
        return "cm";
    if (k == TOK_GT || k == TOK_SHR) /* (inside the fold's parentheses) */
        return k == TOK_GT ? "gt" : "rs";
    return dep_binop(k, &p);
}

/* After `(`: a fold-expression — `(e op ...)` fr, `(... op e)` fl,
 * `(e op ... op i)` fR, `(i op ... op e)` fL — to its `)`; NULL (the
 * cursor unmoved) if it is not one */
static const char *dep_fold(void)
{
    if (cx_kind() == TOK_ELLIPSIS) {
        const char *op = fold_op(cx_kind_at(1));
        if (!op)
            cx_error(cx_cur(), "expected an operator after '...' in a "
                               "fold-expression");
        cx_advance();
        cx_advance();
        return cx_fmt("fl%s%s", op, dep_expr_unary());
    }
    int start = cx_pos, save_half = cx_half_gt;
    jmp_buf jb;
    void *sv = cx_sfinae;
    struct parse_state *st = parse_save();
    const char *l = NULL;
    if (!setjmp(jb)) {
        cx_sfinae = &jb;
        l = dep_expr_unary();
        cx_sfinae = sv;
    } else {
        parse_restore(st);
        cx_sfinae = sv;
    }
    const char *op = l ? fold_op(cx_kind()) : NULL;
    if (!op || cx_kind_at(1) != TOK_ELLIPSIS) {
        cx_pos = start;
        cx_half_gt = save_half;
        return NULL;
    }
    cx_advance();
    cx_advance();
    if (cx_kind() == TOK_RPAREN)
        return cx_fmt("fr%s%s", op, l);
    const char *op2 = fold_op(cx_kind());
    if (!op2 || strcmp(op, op2) != 0)
        cx_error(cx_cur(), "a binary fold-expression's operators differ");
    cx_advance();
    return cx_fmt("fR%s%s%s", op, l, dep_expr_unary());
}

/* an assignment operator's mangled name (5.1.6), or NULL */
static const char *dep_assign_op(enum tok_kind k)
{
    switch (k) {
    case TOK_ASSIGN: return "aS";
    case TOK_PLUSEQ: return "pL";
    case TOK_MINUSEQ: return "mI";
    case TOK_STAREQ: return "mL";
    case TOK_SLASHEQ: return "dV";
    case TOK_PERCENTEQ: return "rM";
    case TOK_AMPEQ: return "aN";
    case TOK_PIPEEQ: return "oR";
    case TOK_CARETEQ: return "eO";
    case TOK_SHLEQ: return "lS";
    case TOK_SHREQ: return "rS";
    default: return NULL;
    }
}

static const char *dep_expr_cond(void)
{
    const char *c = dep_expr_bin(1);
    const char *aop = dep_assign_op(cx_kind());
    if (aop && !cx_in_targs) {
        /* (an assignment, right to left) */
        cx_advance();
        return cx_fmt("%s%s%s", aop, c, dep_expr_cond());
    }
    if (!cx_accept(TOK_QUESTION))
        return c;
    int saved = cx_in_targs;
    cx_in_targs = 0;
    const char *a = dep_expr_bin(1);
    cx_in_targs = saved;
    cx_expect(TOK_COLON, "':'");
    const char *b = dep_expr_cond();
    return cx_fmt("qu%s%s%s", c, a, b);
}

static const char *dep_expr_bin(int minprec)
{
    const char *l = dep_expr_unary();
    for (;;) {
        int p;
        const char *op = dep_binop(cx_kind(), &p);
        if (!op || !p || p < minprec)
            return l;
        cx_advance();
        const char *r = dep_expr_bin(p + 1);
        l = cx_fmt("%s%s%s", op, l, r);
    }
}

/* A name in an expression: a template parameter (T_), a member of a
 * dependent type (sr <type> <name>), or a plain name. */
static const char *dep_expr_name(void)
{
    const struct ctok *at = cx_cur();
    struct qname q = peek_qname();
    if (q.dep || (q.scope && q.scope->k == SC_CLASS)) {
        /* X<T>::value: sr and the qualifying type */
        struct cty *qt = q.dep;
        if (!qt && q.scope->k == SC_CLASS)
            qt = ct_class(q.scope->cls);
        cx_pos += q.fin;
        cx_half_gt = 0;
        int tkw = cx_accept(TOK_CX_TEMPLATE) ||
                  cx_toks[cx_pos - 1].t.kind == TOK_CX_TEMPLATE;
        if (cx_kind() != TOK_IDENT)
            cx_error(at, "expected a name after '::'");
        const char *n = cx_cur()->t.text;
        cx_advance();
        const char *r = cx_fmt("sr%s%zu%s", type_ref(qt), strlen(n), n);
        /* `<` after a dependent type's member: its template arguments
         * only after `template` (T::template f<X>); else less-than
         * ((L::value < R::value)) — a known class's member template
         * aside */
        struct csym *my = !q.dep && q.scope ? lookup_in(q.scope, n) : NULL;
        int targs = tkw || (my && (my->k == CS_TEMPLATE ||
                                   (my->k == CS_FUNC && my->fns &&
                                    my->fns->tmpl)));
        if (cx_kind() == TOK_LT && targs) {
            int p0 = cx_pos;
            skip_template_args();
            r = cx_fmt("%sI_%dE", r, p0);     /* template args: vendor */
        }
        return r;
    }
    cx_pos += q.fin;
    if (cx_kind() == TOK_CX_TEMPLATE && cx_kind_at(1) == TOK_IDENT)
        cx_advance();
    if (cx_kind() != TOK_IDENT)
        cx_error(at, "expected an expression in a template argument");
    const char *n = cx_cur()->t.text;
    struct csym *y = q.scope ? lookup_in(q.scope, n) : lookup(cx_scope, n);
    cx_advance();
    if (y && y->k == CS_VAR && y->var->is_tparam)
        return param_ref(y->var->tparam_index);
    if (y && y->k == CS_VAR && y->var->fparam)
        /* a function parameter: fp_ for the first, fp<n-2>_ after */
        return y->var->fparam == 1 ? "fp_"
               : cx_fmt("fp%d_", y->var->fparam - 2);
    if (y && y->k == CS_ENUMERATOR)
        return cx_fmt("L%s%ldE", "i", y->value);
    const char *r = cx_fmt("%zu%s", strlen(n), n);
    if (cx_kind() == TOK_LT && y && (y->k == CS_FUNC || y->k == CS_TEMPLATE)) {
        /* f<args>: the arguments (vendor-spelled, as sr's) */
        int p0 = cx_pos;
        skip_template_args();
        r = cx_fmt("%sI_%dE", r, p0);
    }
    return r;
}

static const char *dep_expr_postfix(const char *e)
{
    for (;;) {
        if (cx_kind() == TOK_LPAREN) {
            int saved = cx_in_targs;
            cx_in_targs = 0;
            cx_advance();
            char *r = cx_fmt("cl%s", e);
            while (cx_kind() != TOK_RPAREN) {
                r = cx_fmt("%s%s", r, dep_expr_cond());
                if (!cx_accept(TOK_COMMA))
                    break;
            }
            cx_expect(TOK_RPAREN, "')'");
            cx_in_targs = saved;
            e = cx_fmt("%sE", r);
            continue;
        }
        if ((cx_kind() == TOK_DOT || cx_kind() == TOK_ARROW) &&
            cx_kind_at(1) == TOK_IDENT) {
            const char *op = cx_kind() == TOK_DOT ? "dt" : "pt";
            cx_advance();
            const char *n = cx_cur()->t.text;
            cx_advance();
            e = cx_fmt("%s%s%zu%s", op, e, strlen(n), n);
            continue;
        }
        if (cx_kind() == TOK_PLUSPLUS || cx_kind() == TOK_MINUSMINUS) {
            e = cx_fmt("%s%s", cx_kind() == TOK_PLUSPLUS ? "pp" : "mm", e);
            cx_advance();                   /* (postfix) */
            continue;
        }
        return e;
    }
}

static const char *dep_expr_unary(void)
{
    enum tok_kind k = cx_kind();
    const char *op = k == TOK_BANG ? "nt" : k == TOK_MINUS ? "ng"
                   : k == TOK_PLUS ? "ps" : k == TOK_TILDE ? "co"
                   : k == TOK_AMP ? "ad" : k == TOK_STAR ? "de" : NULL;
    if (op) {
        cx_advance();
        return cx_fmt("%s%s", op, dep_expr_unary());
    }
    if (k == TOK_PLUSPLUS || k == TOK_MINUSMINUS) {
        /* prefix: pp_ / mm_ (5.1.6) */
        cx_advance();
        return cx_fmt("%s_%s", k == TOK_PLUSPLUS ? "pp" : "mm",
                      dep_expr_unary());
    }
    if (k == TOK_KW_SIZEOF || k == TOK_KW_ALIGNOF) {
        cx_advance();
        const char *ty = k == TOK_KW_SIZEOF ? "st" : "at";
        const char *ex = k == TOK_KW_SIZEOF ? "sz" : "az";
        if (cx_accept(TOK_ELLIPSIS)) {
            /* sizeof...(pack): sZ and the parameter */
            cx_expect(TOK_LPAREN, "'('");
            struct csym *y = cx_kind() == TOK_IDENT
                             ? lookup(cx_scope, cx_cur()->t.text) : NULL;
            const char *r;
            if (y && y->k == CS_TYPEDEF && y->type->k == CT_TPARAM) {
                r = cx_fmt("sZ%s", param_ref((int)y->type->n));
                cx_advance();
            } else if (y && y->k == CS_TEMPLATE && y->tmpl->tparam) {
                r = cx_fmt("sZ%s", param_ref(y->tmpl->tparam - 1));
                cx_advance();
            } else {
                r = cx_fmt("sZ%s", dep_expr_name());
            }
            cx_expect(TOK_RPAREN, "')'");
            return r;
        }
        if (cx_kind() == TOK_LPAREN) {
            cx_advance();
            int saved = cx_in_targs;
            cx_in_targs = 0;
            const char *r = at_type_start()
                            ? cx_fmt("%s%s", ty, dep_type_mangle())
                            : cx_fmt("%s%s", ex, dep_expr_cond());
            cx_in_targs = saved;
            cx_expect(TOK_RPAREN, "')'");
            return r;
        }
        return cx_fmt("%s%s", ex, dep_expr_unary());
    }
    if (k == TOK_CX_NOEXCEPT) {
        cx_advance();
        cx_expect(TOK_LPAREN, "'('");
        int saved = cx_in_targs;
        cx_in_targs = 0;
        const char *r = cx_fmt("nx%s", dep_expr_cond());
        cx_in_targs = saved;
        cx_expect(TOK_RPAREN, "')'");
        return r;
    }
    if (k == TOK_CX_STATIC_CAST || k == TOK_CX_CONST_CAST ||
        k == TOK_CX_REINTERPRET_CAST) {
        const char *op2 = k == TOK_CX_STATIC_CAST ? "sc"
                          : k == TOK_CX_CONST_CAST ? "cc" : "rc";
        cx_advance();
        cx_expect(TOK_LT, "'<'");
        int saved = cx_in_targs;
        cx_in_targs = 1;
        const char *ty = dep_type_mangle();
        cx_in_targs = saved;
        cx_close_angle();
        cx_expect(TOK_LPAREN, "'('");
        cx_in_targs = 0;
        const char *r = cx_fmt("%s%s%s", op2, ty, dep_expr_cond());
        cx_in_targs = saved;
        cx_expect(TOK_RPAREN, "')'");
        return dep_expr_postfix(r);
    }
    if (k == TOK_NUM) {
        long v = cx_cur()->t.num;
        const char *code = cx_cur()->t.char_lit ? "c"
                           : cx_cur()->t.num_long
                           ? (cx_cur()->t.num_uns ? "m" : "l")
                           : cx_cur()->t.num_uns ? "j" : "i";
        cx_advance();
        return cx_fmt("L%s%s%ldE", code, v < 0 ? "n" : "", v < 0 ? -v : v);
    }
    if (k == TOK_CX_TRUE || k == TOK_CX_FALSE) {
        cx_advance();
        return k == TOK_CX_TRUE ? "Lb1E" : "Lb0E";
    }
    if (k == TOK_CX_NULLPTR) {
        cx_advance();
        return "LDnE";
    }
    if (k == TOK_LPAREN) {
        cx_advance();
        int saved = cx_in_targs;
        cx_in_targs = 0;
        const char *r;
        /* (X<T>::value): a dependent qualified name without `typename`
         * names a value (13.8.1), not a type to cast to */
        struct qname dq = peek_qname();
        int dep_value = !dq.bad && dq.dep && dq.fin > 0 &&
                        cx_kind_at(dq.fin) == TOK_IDENT &&
                        cx_kind_at(dq.fin + 1) == TOK_RPAREN;
        if (!dep_value && at_type_start()) {
            /* (T)e — if what follows `(` is a type ending at `)`
             * (X<T>::value && ... is an expression, not a cast) */
            int start = cx_pos;
            const char *ty = NULL;
            jmp_buf jb;
            void *sv = cx_sfinae;
            struct parse_state *st = parse_save();
            if (!setjmp(jb)) {
                cx_sfinae = &jb;
                ty = dep_type_mangle();
                cx_sfinae = sv;
                if (cx_kind() != TOK_RPAREN) {
                    ty = NULL;
                    cx_pos = start;
                    cx_half_gt = 0;
                }
            } else {
                parse_restore(st);
                cx_sfinae = sv;
                cx_pos = start;
                ty = NULL;
            }
            if (ty) {
                cx_expect(TOK_RPAREN, "')'");
                cx_in_targs = saved;
                return cx_fmt("cv%s%s", ty, dep_expr_unary());
            }
        }
        r = dep_fold();
        if (!r)
            r = dep_expr_cond();
        cx_in_targs = saved;
        cx_expect(TOK_RPAREN, "')'");
        return dep_expr_postfix(r);
    }
    if (k == TOK_CX_REQUIRES) {
        /* a requires-expression in a pattern: known only for arguments —
         * spelled by where it is (vendor), as EmbCC cannot mangle it */
        int p0 = cx_pos;
        cx_advance();
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        if (cx_kind() != TOK_LBRACE)
            cx_error(cx_cur(), "expected '{' in a requires-expression");
        cx_skip_balanced();
        return cx_fmt("u8requiresI_%dE", p0);
    }
    if (trait_at()) {
        /* __is_constructible(T, Args...) in a pattern: u <name> <types> E */
        const char *tn = cx_cur()->t.text;
        cx_advance();
        cx_expect(TOK_LPAREN, "'('");
        int saved = cx_in_targs;
        cx_in_targs = 0;
        const char *r = cx_fmt("u%zu%s", strlen(tn), tn);
        while (cx_kind() != TOK_RPAREN) {
            r = cx_fmt("%s%s", r, dep_type_mangle());
            if (cx_accept(TOK_ELLIPSIS))
                r = cx_fmt("%sDp", r);
            if (!cx_accept(TOK_COMMA))
                break;
        }
        cx_expect(TOK_RPAREN, "')'");
        cx_in_targs = saved;
        return cx_fmt("%sE", r);
    }
    int n;
    const char *ty = NULL;
    struct cty *pt = cx_kind() == TOK_CX_TYPENAME ? NULL : peek_type_name(&n);
    if (pt && pt->k != CT_DEP &&     /* (X<T>::f(): a call, not a type) */
        (cx_kind_at(n) == TOK_LPAREN || cx_kind_at(n) == TOK_LBRACE)) {
        ty = dep_type_mangle();
    } else if (at_type_start() && cx_kind() != TOK_IDENT &&
               cx_kind() != TOK_COLONCOLON) {
        /* bool(e), unsigned long(e): a type the keywords spell */
        int save = cx_pos;
        struct dspec ds;
        parse_dspec(&ds);
        if (ds.type && (cx_kind() == TOK_LPAREN || cx_kind() == TOK_LBRACE))
            ty = type_ref(ds.type);
        else
            cx_pos = save;
    }
    if (ty) {
        /* T(args), T{}: a functional cast — cv <type> <expr> for one
         * argument, cv <type> _ <expr>* E otherwise, tl <type> <expr>* E
         * braced */
        int brace = cx_kind() == TOK_LBRACE;
        cx_advance();
        int saved = cx_in_targs;
        cx_in_targs = 0;
        const char *args = "";
        int na = 0;
        while (cx_kind() != (brace ? TOK_RBRACE : TOK_RPAREN)) {
            args = cx_fmt("%s%s", args, dep_expr_cond());
            na++;
            if (!cx_accept(TOK_COMMA))
                break;
        }
        cx_expect(brace ? TOK_RBRACE : TOK_RPAREN, "')'");
        cx_in_targs = saved;
        const char *r = brace ? cx_fmt("tl%s%sE", ty, args)
                        : na == 1 ? cx_fmt("cv%s%s", ty, args)
                        : cx_fmt("cv%s_%sE", ty, args);
        return dep_expr_postfix(r);
    }
    return dep_expr_postfix(dep_expr_name());
}

/* One template argument, for parameter p (NULL: its kind is guessed). */
static void parse_one_targ(struct ctemplate *t, struct ctparam *p, int n,
                           struct ctarg *r)
{
    memset(r, 0, sizeof *r);
    int kind = p ? p->kind : at_type_start() ? TP_TYPE : TP_VALUE;
    const struct ctok *at = cx_cur();
    r->kind = kind;
    if (kind == TP_TYPE) {
        if (!at_type_start())
            cx_error(at, "expected a type as template argument %d of '%s'",
                     n + 1, t ? t->name : "?");
        r->type = parse_type_id();
    } else if (kind == TP_TEMPLATE) {
        struct qname q = peek_qname();
        cx_pos += q.fin;
        struct csym *y = cx_kind() == TOK_IDENT
                         ? find_final(q.scope, cx_cur()->t.text) : NULL;
        if (!y || y->k != CS_TEMPLATE)
            cx_error(at, "expected a template as template argument");
        r->tmpl = y->tmpl;
        cx_advance();
    } else if (cx_pattern && targ_mentions_param()) {
        /* a value in a pattern: a parameter, or an expression of them —
         * kept as its mangling (which also tells two apart) */
        struct csym *y = cx_kind() == TOK_IDENT
                         ? lookup(cx_scope, cx_cur()->t.text) : NULL;
        if (y && y->k == CS_VAR && y->var->is_tparam &&
            (cx_kind_at(1) == TOK_COMMA || cx_kind_at(1) == TOK_GT ||
             cx_kind_at(1) == TOK_SHR || cx_kind_at(1) == TOK_ELLIPSIS)) {
            r->vtype = ct_tparam(y->var->tparam_index, y->name);
            cx_advance();
        } else {
            struct cty *d = xcalloc(1, sizeof *d);
            d->k = CT_DEP;
            r->vtype = d;
            r->mexpr = dep_expr_cond();
        }
    } else {
        struct cexpr *e = expr_parse_cond();
        long v;
        if (e->t && e->t->k == CT_CLASS) {
            /* a converted constant expression: a class constant through
             * its constexpr conversion (__and_<...>{} for a bool) */
            struct cty *to = p && p->vtype && !ct_dependent(p->vtype) &&
                             ct_is_integer(p->vtype) ? ct_unqual(p->vtype)
                                                     : ct_basic(CT_BOOL);
            e = to->k == CT_BOOL ? convert_bool(e, "a template argument")
                                 : convert(e, to, "a template argument");
        }
        if (!ct_is_integer(e->t) || !expr_const(e, &v))
            cx_error(at, "template argument %d of '%s' is not an integral "
                         "constant", n + 1, t ? t->name : "?");
        r->vtype = ct_unqual(e->t);
        if (p && p->vtype && !ct_dependent(p->vtype) &&
            ct_is_integer(p->vtype)) {
            r->vtype = ct_unqual(p->vtype);
            v = r->vtype->k == CT_BOOL ? v != 0 : v;
        }
        r->value = v;
    }
}

int parse_template_args(struct ctemplate *t, struct ctarg **out)
{
    cx_expect(TOK_LT, "'<'");
    int saved = cx_in_targs;
    cx_in_targs = 1;
    int n = 0, cap = 4;
    struct ctarg *a = xcalloc((size_t)cap, sizeof *a);
    while (cx_kind() != TOK_GT && !(cx_kind() == TOK_SHR)) {
        struct ctparam *p = t && t->nparams > 0
                            ? &t->params[n < t->nparams ? n : t->nparams - 1]
                            : NULL;
        if (p && n >= t->nparams && !p->pack)
            p = NULL;
        struct csym *packs[8];
        int ell;
        int ex = expansion_at(1, packs, 8, &ell);
        if (ex > 0) {
            /* Ts...: the pattern once per element */
            int len = expansion_length(packs, ex);
            int start = cx_pos;
            for (int e = 0; e < len; e++) {
                if (n + 1 >= cap) {
                    cap *= 2;
                    a = xrealloc(a, (size_t)cap * sizeof *a);
                }
                cx_pos = start;
                cx_half_gt = 0;
                for (int k = 0; k < ex; k++)
                    pack_push(packs[k], e);
                struct ctparam *pe = t && t->nparams > 0
                                     ? &t->params[n < t->nparams ? n
                                                  : t->nparams - 1] : NULL;
                if (pe && n >= t->nparams && !pe->pack)
                    pe = NULL;
                parse_one_targ(t, pe, n, &a[n]);
                n++;
                pack_pop(ex);
            }
            cx_pos = ell + 1;
            cx_half_gt = 0;
            if (!cx_accept(TOK_COMMA))
                break;
            continue;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            a = xrealloc(a, (size_t)cap * sizeof *a);
        }
        parse_one_targ(t, p, n, &a[n]);
        if (cx_kind() == TOK_ELLIPSIS) {
            /* in a pattern: an expansion kept to deduce a pack from */
            if (!cx_pattern)
                cx_error(cx_cur(), "'...' expands no parameter pack");
            cx_advance();
            struct ctarg *pat = xmalloc(sizeof *pat);
            *pat = a[n];
            memset(&a[n], 0, sizeof a[n]);
            a[n].kind = pat->kind;
            a[n].is_pack = 1;
            a[n].expansion = 1;
            a[n].elems = pat;
            a[n].nelems = 1;
        }
        n++;
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_in_targs = saved;
    cx_close_angle();
    *out = a;
    return n;
}

/* template < parameters > — at the first parameter, past the `>`. Each
 * is declared in cx_scope (an SC_TEMPLATE scope) as a pattern placeholder:
 * a type parameter a CT_TPARAM, a value parameter a variable marked
 * is_tparam, a template template parameter a template of no known
 * parameters. */
/* a value template parameter's type, read again from its tokens (in the
 * current scope, the parameters before it bound): its name dropped */
struct cty *parse_value_tparam_type(int tok)
{
    cx_pos = tok;
    struct dspec ds;
    parse_dspec(&ds);
    if (!ds.type)
        cx_error(cx_cur(), "expected a template parameter's type");
    cx_accept(TOK_ELLIPSIS);
    struct declarator d;
    memset(&d, 0, sizeof d);
    return parse_declarator(ds.type, &d, DK_NAMED | DK_ABSTRACT);
}

/* (tparam_base: parse_tparams's first index — a constructor template's
 * parameters follow its class's in a deduction guide made of it) */

static int parse_tparams(struct ctparam **out)
{
    int n = 0, cap = 4;
    struct ctparam *ps = xcalloc((size_t)cap, sizeof *ps);
    int saved = cx_in_targs;
    cx_in_targs = 1;
    cx_pattern++;
    while (cx_kind() != TOK_GT) {
        if (n == cap) {
            cap *= 2;
            ps = xrealloc(ps, (size_t)cap * sizeof *ps);
        }
        struct ctparam *p = &ps[n];
        memset(p, 0, sizeof *p);
        p->def_tok = p->vtype_tok = -1;
        p->tc_args = -1;
        const struct ctok *at = cx_cur();
        int ctn, cargs;
        struct ctemplate *tc = cx_kind() == TOK_IDENT ||
                               cx_kind() == TOK_COLONCOLON
                               ? concept_at(&ctn, &cargs) : NULL;
        if (tc) {
            /* C T, C<A> T: a type parameter the concept constrains */
            cx_pos += ctn;
            p->kind = TP_TYPE;
            p->tc = tc;
            p->tc_args = cargs;
            if (cx_accept(TOK_ELLIPSIS))
                p->pack = 1;
            if (cx_kind() == TOK_IDENT) {
                p->name = cx_cur()->t.text;
                cx_advance();
            }
            if (cx_accept(TOK_ASSIGN)) {
                p->def_tok = cx_pos;
                skip_targ();
            }
            struct csym *y = scope_add(cx_scope, CS_TYPEDEF,
                                       p->name ? p->name : "");
            y->type = ct_tparam(tparam_base + n, p->name);
            y->pack_param = p->pack;
            n++;
            if (!cx_accept(TOK_COMMA))
                break;
            continue;
        }
        enum tok_kind k1 = cx_kind_at(1);
        enum tok_kind k2 = cx_kind_at(2);
        if ((cx_kind() == TOK_CX_TYPENAME || cx_kind() == TOK_CX_CLASS) &&
            (k1 == TOK_ELLIPSIS || k1 == TOK_COMMA || k1 == TOK_GT ||
             k1 == TOK_SHR || k1 == TOK_ASSIGN ||
             (k1 == TOK_IDENT && (k2 == TOK_COMMA || k2 == TOK_GT ||
                                  k2 == TOK_SHR || k2 == TOK_ASSIGN)))) {
            /* typename T, class U = X: a type parameter (typename
             * A<T>::type V = x is a value one) */
            p->kind = TP_TYPE;
            cx_advance();
            if (cx_accept(TOK_ELLIPSIS))
                p->pack = 1;
            if (cx_kind() == TOK_IDENT) {
                p->name = cx_cur()->t.text;
                cx_advance();
            }
            if (cx_accept(TOK_ASSIGN)) {
                p->def_tok = cx_pos;
                skip_targ();
            }
            struct csym *y = scope_add(cx_scope, CS_TYPEDEF,
                                       p->name ? p->name : "");
            y->type = ct_tparam(tparam_base + n, p->name);
            y->pack_param = p->pack;
        } else if (cx_kind() == TOK_CX_TEMPLATE) {
            p->kind = TP_TEMPLATE;
            cx_advance();
            if (cx_kind() != TOK_LT)
                cx_error(at, "expected '<' after 'template'");
            /* its own parameters: which (if any) is a pack */
            int tt_pack = 0, tt_n = 1, depth = 0;
            for (int i = cx_pos + 1; i < cx_ntoks; i++) {
                enum tok_kind k = cx_toks[i].t.kind;
                if (k == TOK_EOF || (depth == 0 && k == TOK_GT))
                    break;
                if (k == TOK_LT || k == TOK_LPAREN)
                    depth++;
                else if (k == TOK_GT || k == TOK_RPAREN)
                    depth--;
                else if (depth == 0 && k == TOK_COMMA)
                    tt_n++;
                else if (depth == 0 && k == TOK_ELLIPSIS)
                    tt_pack = tt_n;
            }
            skip_template_args();
            if (cx_kind() != TOK_CX_CLASS && cx_kind() != TOK_CX_TYPENAME)
                cx_error(cx_cur(), "expected 'class' in a template template "
                                   "parameter");
            cx_advance();
            if (cx_accept(TOK_ELLIPSIS))
                p->pack = 1;
            if (cx_kind() == TOK_IDENT) {
                p->name = cx_cur()->t.text;
                cx_advance();
            }
            if (cx_accept(TOK_ASSIGN)) {
                p->def_tok = cx_pos;
                skip_targ();
            }
            struct ctemplate *ph = xcalloc(1, sizeof *ph);
            ph->kind = TK_CLASS;
            ph->name = p->name ? p->name : "";
            ph->nparams = -1;               /* a placeholder: dependent */
            ph->head_end = -1;
            ph->tparam = tparam_base + n + 1;
            ph->tt_pack = tt_pack;
            struct csym *y = scope_add(cx_scope, CS_TEMPLATE, ph->name);
            y->tmpl = ph;
            y->pack_param = p->pack;
        } else {
            /* a value parameter: a parameter declaration */
            p->kind = TP_VALUE;
            p->vtype_tok = cx_pos;
            struct dspec ds;
            parse_dspec(&ds);
            if (!ds.type)
                cx_error(at, "expected a template parameter before %s",
                         tok_describe(&cx_cur()->t));
            if (cx_accept(TOK_ELLIPSIS))
                p->pack = 1;
            struct declarator d;
            memset(&d, 0, sizeof d);
            p->vtype = parse_declarator(ds.type, &d, DK_NAMED | DK_ABSTRACT);
            if (d.saved)
                cx_scope = d.saved;
            p->name = d.name;
            if (cx_accept(TOK_ASSIGN)) {
                p->def_tok = cx_pos;
                skip_targ();
            }
            struct cvar *v = xcalloc(1, sizeof *v);
            v->name = v->cname = p->name ? p->name : "";
            v->type = p->vtype;
            v->is_tparam = 1;
            v->tparam_index = tparam_base + n;
            struct csym *y = scope_add(cx_scope, CS_VAR, v->name);
            y->var = v;
            y->pack_param = p->pack;
        }
        n++;
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_pattern--;
    cx_in_targs = saved;
    cx_close_angle();
    *out = ps;
    return n;
}

/* Past a declaration whose tokens were recorded: to its `;`, or the end of
 * a function body (and a constructor's mem-initializers before it). */
static void skip_declaration(void)
{
    int depth = 0, first = cx_pos;
    for (;;) {
        enum tok_kind k = cx_kind();
        if (k == TOK_EOF)
            cx_error(cx_cur(), "unterminated template declaration");
        if (k == TOK_LPAREN || k == TOK_LBRACKET) {
            cx_skip_balanced();
            continue;
        }
        if (k == TOK_COLON && depth == 0 && cx_pos > 0 &&
            (cx_pos == first ||
             cx_toks[cx_pos - 1].t.kind == TOK_RPAREN ||
             cx_toks[cx_pos - 1].t.kind == TOK_CX_NOEXCEPT)) {
            /* a constructor's mem-initializers (`: m{x}, B(y)`), then
             * its body — a braced initializer is not the body */
            int save = cx_pos;
            cx_advance();
            int ok = 1;
            for (;;) {
                while (cx_kind() != TOK_LPAREN && cx_kind() != TOK_LBRACE) {
                    if (cx_kind() == TOK_EOF || cx_kind() == TOK_SEMI) {
                        ok = 0;
                        break;
                    }
                    cx_advance();
                }
                if (!ok)
                    break;
                cx_skip_balanced();
                if (cx_kind() == TOK_ELLIPSIS)
                    cx_advance();
                if (!cx_accept(TOK_COMMA))
                    break;
            }
            if (ok && cx_kind() == TOK_LBRACE) {
                cx_skip_balanced();
                return;
            }
            cx_pos = save;
        }
        if (k == TOK_LBRACE) {
            cx_skip_balanced();
            /* a body ends a function definition; an initializer does not
             * (nor a lambda's body: `= []{ ... }();`) */
            enum tok_kind n = cx_kind();
            if (depth == 0 && n != TOK_SEMI && n != TOK_COMMA &&
                n != TOK_LPAREN && n != TOK_RPAREN)
                return;
            continue;
        }
        if (k == TOK_LT)
            depth++;
        else if (k == TOK_GT && depth > 0)
            depth--;
        else if (k == TOK_SHR && depth > 0)
            depth = depth > 2 ? depth - 2 : 0;    /* X<Y<T>> */
        cx_advance();
        if (k == TOK_SEMI)
            return;
    }
}

/* Past a class template's definition: its head's bases, its body, `;`. */
static void skip_class_def(void)
{
    while (cx_kind() != TOK_LBRACE) {
        if (cx_kind() == TOK_EOF || cx_kind() == TOK_SEMI)
            cx_error(cx_cur(), "expected a class body");
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        else
            cx_advance();
    }
    cx_skip_balanced();
    parse_attrs(NULL);
    cx_expect(TOK_SEMI, "';' after a class template");
}

static struct ctemplate *template_new(int kind, const char *name,
                                      struct cscope *home,
                                      struct ctparam *ps, int np)
{
    struct ctemplate *t = xcalloc(1, sizeof *t);
    t->kind = kind;
    t->name = name;
    t->scope = home;
    t->params = ps;
    t->nparams = np;
    t->head_end = -1;
    return t;
}

/* The class template `A` of `A<...>::` — the qualifier of an out-of-class
 * member definition — found by scanning the declaration: the last
 * template-id before its declarator's parameters or initializer. */
/* the `<` of the qualifier outdef_target last found (A<T, U>::f) */
static int outdef_qual_lt;

/* Are the template arguments at `<` (lt) the parameters ps themselves, in
 * order: the primary template's members, not a partial
 * specialization's (vector<bool, _Alloc>::f)? */
static int args_are_params(int lt, struct ctparam *ps, int np)
{
    int i = lt + 1;
    for (int k = 0; k < np; k++) {
        if (k > 0 && cx_toks[i++].t.kind != TOK_COMMA)
            return 0;
        if (cx_toks[i].t.kind != TOK_IDENT || !ps[k].name ||
            strcmp(cx_toks[i].t.text, ps[k].name) != 0)
            return 0;
        i++;
        if (ps[k].pack && cx_toks[i++].t.kind != TOK_ELLIPSIS)
            return 0;
    }
    return cx_toks[i].t.kind == TOK_GT || cx_toks[i].t.kind == TOK_SHR;
}

static struct ctemplate *outdef_target(const char **member)
{
    struct ctemplate *found = NULL;
    struct cscope *qs = NULL;
    int save = cx_pos;
    int depth = 0;
    for (int i = cx_pos; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (depth == 0 && ((k == TOK_KW_ATTRIBUTE &&
                            cx_toks[i + 1].t.kind == TOK_LPAREN) ||
                           (k == TOK_LBRACKET &&
                            cx_toks[i + 1].t.kind == TOK_LBRACKET))) {
            /* __attribute__((...)), [[...]]: not the declarator's `(` */
            cx_pos = k == TOK_KW_ATTRIBUTE ? i + 1 : i;
            cx_skip_balanced();
            i = cx_pos - 1;
            continue;
        }
        if (depth == 0 && (k == TOK_LPAREN || k == TOK_ASSIGN ||
                           k == TOK_SEMI || k == TOK_LBRACE))
            break;
        if (k == TOK_LPAREN || k == TOK_LBRACKET)
            depth++;
        else if (k == TOK_RPAREN || k == TOK_RBRACKET)
            depth--;
        if (!depth && k == TOK_IDENT &&
            cx_toks[i + 1].t.kind == TOK_COLONCOLON) {
            /* A::M<T>::f: the qualifier's scope, for the template */
            struct csym *q = qs ? lookup_in(qs, cx_toks[i].t.text)
                                : lookup(cx_scope, cx_toks[i].t.text);
            qs = q && q->k == CS_NAMESPACE ? q->ns
                 : q && (q->k == CS_CLASS || q->k == CS_TYPEDEF) &&
                   q->type->k == CT_CLASS ? q->type->cls->scope : NULL;
            i++;
            continue;
        }
        if (depth || k != TOK_IDENT || cx_toks[i + 1].t.kind != TOK_LT) {
            if (k != TOK_COLONCOLON)
                qs = NULL;
            continue;
        }
        struct csym *y = qs ? lookup_in(qs, cx_toks[i].t.text)
                            : lookup(cx_scope, cx_toks[i].t.text);
        qs = NULL;
        if (!y || y->k != CS_TEMPLATE || y->tmpl->kind != TK_CLASS)
            continue;
        cx_pos = i + 1;
        skip_template_args();
        if (cx_kind() == TOK_COLONCOLON) {
            /* the member's own name: the last component — a declarator
             * only if its parameters or initializer follow (not a type,
             * as in `typename A<T>::type f()`) */
            int j = cx_pos + 1;
            const char *m = NULL;
            int after = -1;
            for (;;) {
                if (cx_toks[j].t.kind == TOK_TILDE) {
                    m = cx_fmt("~%s", cx_toks[j + 1].t.text);
                    after = j + 2;
                    break;
                }
                if (cx_toks[j].t.kind == TOK_CX_OPERATOR) {
                    m = "operator";
                    after = -2;
                    break;
                }
                if (cx_toks[j].t.kind != TOK_IDENT)
                    break;
                m = cx_toks[j].t.text;
                if (cx_toks[j + 1].t.kind == TOK_COLONCOLON) {
                    j += 2;
                } else {
                    after = j + 1;
                    break;
                }
            }
            enum tok_kind ak = after >= 0 ? cx_toks[after].t.kind : TOK_EOF;
            if (after == -2 || ak == TOK_LPAREN || ak == TOK_ASSIGN ||
                ak == TOK_SEMI || ak == TOK_LBRACE || ak == TOK_LBRACKET) {
                found = y->tmpl;
                *member = m;
                outdef_qual_lt = i + 1;
            }
        }
        i = cx_pos - 1;
        cx_half_gt = 0;
    }
    cx_pos = save;
    return found;
}

static void parse_explicit_instantiation(int is_extern);
static void parse_explicit_specialization(struct cclass *cls, int access);
static struct cfunc *specialization_named(struct declarator *d,
                                          struct cty *ft,
                                          const struct ctok *at);
/* At a deduction guide — [explicit] C(P...) -> type; with C a class
 * template: recorded on C (the caller sets its parameters), the cursor
 * unmoved; NULL if this is not one */
static struct cguide *guide_at(void)
{
    int save = cx_pos, save_half = cx_half_gt;
    int is_explicit = 0;
    if (cx_kind() == TOK_CX_EXPLICIT) {
        is_explicit = 1;
        cx_advance();
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
    }
    struct cguide *g = NULL;
    if (cx_kind() == TOK_IDENT && cx_kind_at(1) == TOK_LPAREN) {
        struct csym *y = lookup(cx_scope, cx_cur()->t.text);
        int name = cx_pos;
        cx_advance();
        cx_skip_balanced();
        if (y && y->k == CS_TEMPLATE && y->tmpl->kind == TK_CLASS &&
            !y->tmpl->tparam && cx_kind() == TOK_ARROW) {
            g = xcalloc(1, sizeof *g);
            g->tok = name;
            g->is_explicit = is_explicit;
            g->scope = cx_scope;
            g->next = y->tmpl->guides;
            y->tmpl->guides = g;
        }
    }
    cx_pos = save;
    cx_half_gt = save_half;
    return g;
}

/* An abbreviated function template (C++20, 9.3.4.6): the declaration at
 * the cursor declares a function one of whose parameters' types is `auto`
 * (or `C auto`) — the `(` of that parameter list, else -1. (Its `auto`s
 * already named __cx_autoN count: a class template's body read again.) */
static int abbrev_lparen(void)
{
    int depth = 0;
    for (int i = cx_pos; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_EOF || (depth == 0 && (k == TOK_SEMI ||
                                            k == TOK_LBRACE ||
                                            k == TOK_ASSIGN)))
            return -1;
        if (k == TOK_LBRACKET || k == TOK_LBRACE) {
            depth++;
            continue;
        }
        if (k == TOK_RBRACKET || k == TOK_RBRACE || k == TOK_RPAREN) {
            depth--;
            continue;
        }
        if (k != TOK_LPAREN)
            continue;
        if (depth) {
            depth++;
            continue;
        }
        /* the declarator's parameters: after its name (or operator) */
        enum tok_kind b = i > 0 ? cx_toks[i - 1].t.kind : TOK_EOF;
        if (b != TOK_IDENT && b != TOK_GT && b != TOK_RPAREN) {
            if (b == TOK_CX_OPERATOR) {
                depth++;            /* operator() */
                continue;
            }
            int op = i > 1 && cx_toks[i - 2].t.kind == TOK_CX_OPERATOR;
            if (!op) {
                depth++;
                continue;
            }
        }
        int opcall = b == TOK_RPAREN && i > 2 &&
                     cx_toks[i - 2].t.kind == TOK_LPAREN &&
                     cx_toks[i - 3].t.kind == TOK_CX_OPERATOR;
        if (b == TOK_RPAREN && !opcall)
            return -1;
        /* auto at the parameters' top level, not in a default argument
         * nor decltype(auto) */
        int d = 0, in_def = 0;
        for (int j = i + 1; j < cx_ntoks; j++) {
            enum tok_kind kj = cx_toks[j].t.kind;
            if (kj == TOK_LPAREN || kj == TOK_LBRACKET || kj == TOK_LBRACE) {
                d++;
                continue;
            }
            if (kj == TOK_RPAREN || kj == TOK_RBRACKET || kj == TOK_RBRACE) {
                if (d-- == 0)
                    break;
                continue;
            }
            if (d)
                continue;
            if (kj == TOK_COMMA)
                in_def = 0;
            else if (kj == TOK_ASSIGN)
                in_def = 1;
            else if (!in_def && (kj == TOK_CX_AUTO ||
                                 (kj == TOK_IDENT &&
                                  !strncmp(cx_toks[j].t.text, "__cx_abv", 8))))
                return i;
            else if (kj == TOK_EOF)
                return -1;
        }
        return -1;
    }
    return -1;
}

/* ... its `auto`s: each an invented type parameter after the n declared
 * ones (__cx_abvN in the tokens from now on, declared in the template's
 * scope), a concept before one its type-constraint (`C auto`, `C<A>
 * auto`), `auto...` a pack. The parameters' number. */
static int invent_abbrev_params(int lparen, struct ctparam **ps, int n)
{
    int cap = n + 4, first = n;
    struct ctparam *v = xcalloc((size_t)cap, sizeof *v);
    if (n)
        memcpy(v, *ps, (size_t)n * sizeof *v);
    int depth = 0, in_def = 0, k0 = n;
    for (int i = lparen + 1; i < cx_ntoks; i++) {
        struct ctok *tk = &cx_toks[i];
        enum tok_kind k = tk->t.kind;
        if (k == TOK_EOF)
            break;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            depth++;
            continue;
        }
        if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE) {
            if (depth-- == 0)
                break;
            continue;
        }
        if (depth)
            continue;
        if (k == TOK_COMMA) {
            in_def = 0;
            first = n;
            continue;
        }
        if (k == TOK_ASSIGN) {
            in_def = 1;
            continue;
        }
        if (k == TOK_ELLIPSIS && !in_def) {
            for (int j = first; j < n; j++)
                v[j].pack = 1;
            continue;
        }
        if (in_def || !(k == TOK_CX_AUTO ||
                        (k == TOK_IDENT &&
                         !strncmp(tk->t.text, "__cx_abv", 8))))
            continue;
        if (n == cap) {
            cap *= 2;
            v = xrealloc(v, (size_t)cap * sizeof *v);
        }
        struct ctparam *p = &v[n];
        memset(p, 0, sizeof *p);
        p->kind = TP_TYPE;
        p->name = cx_fmt("__cx_abv%d", n - k0);
        p->def_tok = p->vtype_tok = -1;
        p->tc_args = -1;
        /* the concept before it: C, N::C, C<A> */
        int s0 = i - 1;
        if (s0 > lparen && cx_toks[s0].t.kind == TOK_GT) {
            int dd = 0;
            for (; s0 > lparen; s0--) {
                enum tok_kind kk = cx_toks[s0].t.kind;
                if (kk == TOK_GT)
                    dd++;
                else if (kk == TOK_SHR)
                    dd += 2;
                else if (kk == TOK_LT && --dd == 0)
                    break;
            }
            s0--;
        }
        while (s0 - 2 > lparen && cx_toks[s0].t.kind == TOK_IDENT &&
               cx_toks[s0 - 1].t.kind == TOK_COLONCOLON &&
               cx_toks[s0 - 2].t.kind == TOK_IDENT)
            s0 -= 2;
        if (s0 > lparen && cx_toks[s0].t.kind == TOK_IDENT) {
            int save = cx_pos, ctn, cargs;
            cx_pos = s0;
            struct ctemplate *tc = concept_at(&ctn, &cargs);
            cx_pos = save;
            if (tc && s0 + ctn == i) {
                p->tc = tc;
                p->tc_args = cargs;
            }
        }
        tk->t.kind = TOK_IDENT;
        tk->t.text = (char *)p->name;
        struct csym *y = scope_add(cx_scope, CS_TYPEDEF, p->name);
        y->type = ct_tparam(tparam_base + n, p->name);
        n++;
    }
    for (int j = k0; j < n; j++) {
        struct csym *y = scope_find_here(cx_scope, v[j].name);
        if (y)
            y->pack_param = v[j].pack;
    }
    *ps = v;
    return n;
}

static void template_decl_rest(struct cclass *cls, int access,
                               const struct ctok *at, struct cscope *home,
                               struct ctparam *ps, int np, int req_s,
                               int req_e);

/* void f(C auto x) { }: a function template of the invented parameters
 * alone */
static void parse_abbreviated_template(struct cclass *cls, int access)
{
    const struct ctok *at = cx_cur();
    struct cscope *home = cx_scope;
    scope_push(SC_TEMPLATE, NULL);
    struct ctparam *ps = NULL;
    int np = invent_abbrev_params(abbrev_lparen(), &ps, 0);
    template_decl_rest(cls, access, at, home, ps, np, 0, 0);
}

/* At a declarator: is it qualified by two template-ids, A<T>::B<U>::? */
static int nested_template_member(void)
{
    int ids = 0, depth = 0;
    for (int i = cx_pos; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_LT)
            depth++;
        else if (k == TOK_GT && depth > 0) {
            if (--depth == 0 && cx_toks[i + 1].t.kind == TOK_COLONCOLON)
                ids++;
        } else if (k == TOK_SHR && depth > 0) {
            depth = depth > 2 ? depth - 2 : 0;
            if (depth == 0 && cx_toks[i + 1].t.kind == TOK_COLONCOLON)
                ids++;
        } else if (depth == 0 && (k == TOK_LPAREN || k == TOK_SEMI ||
                                  k == TOK_LBRACE))
            break;
    }
    return ids >= 2;
}

void parse_template_decl(struct cclass *cls, int access)
{
    const struct ctok *at = cx_cur();
    cx_advance();                                   /* template */
    if (cx_kind() != TOK_LT) {
        parse_explicit_instantiation(0);
        return;
    }
    cx_advance();
    if (cx_kind() == TOK_GT) {
        cx_advance();
        parse_explicit_specialization(cls, access);
        return;
    }
    struct cscope *home = cx_scope;
    scope_push(SC_TEMPLATE, NULL);
    struct ctparam *ps;
    int np = parse_tparams(&ps);
    int req_s = 0, req_e = 0;
    if (cx_accept(TOK_CX_REQUIRES)) {
        /* template<...> requires C: kept, checked for each arguments */
        req_s = cx_pos;
        skip_constraint(0);
        req_e = cx_pos;
    }
    /* (an abbreviated template's `auto` parameters join them) */
    if (cx_kind() != TOK_CX_TEMPLATE) {
        int lp = abbrev_lparen();
        if (lp >= 0)
            np = invent_abbrev_params(lp, &ps, np);
    }
    template_decl_rest(cls, access, at, home, ps, np, req_s, req_e);
}

static void template_decl_rest(struct cclass *cls, int access,
                               const struct ctok *at, struct cscope *home,
                               struct ctparam *ps, int np, int req_s,
                               int req_e)
{
    int decl = cx_pos;
    if (cx_kind() == TOK_CX_TEMPLATE) {
        /* template<class T> template<class U> R A<T>::f(U): a member
         * template of a class template, defined outside it */
        const char *member = NULL;
        int inner = cx_pos;
        cx_advance();
        cx_expect(TOK_LT, "'<'");
        skip_template_args();
        struct ctemplate *t = outdef_target(&member);
        if (!t && nested_template_member()) {
            /* template<..> template<..> A<T>::B<U>::f(): a member of a
             * member class template — not kept yet (out_ptr_t's _Impl:
             * a use of it fails to link, not to compile) */
            skip_declaration();
            cx_scope = home;
            return;
        }
        if (!t)
            cx_error(at, "expected a member of a class template");
        struct coutdef *o = xcalloc(1, sizeof *o);
        o->params = ps;
        o->nparams = np;
        o->tok = inner;
        o->member = member;
        o->partial = !args_are_params(outdef_qual_lt, ps, np);
        o->next = t->outdefs;
        t->outdefs = o;
        skip_declaration();
        cx_scope = home;
        return;
    }
    if (cx_kind() == TOK_CX_USING) {
        /* template<...> using X = type; */
        cx_advance();
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected an alias template's name");
        const char *name = cx_cur()->t.text;
        cx_advance();
        parse_attrs(NULL);
        cx_expect(TOK_ASSIGN, "'=' in an alias template");
        struct ctemplate *t = template_new(TK_ALIAS, name, home, ps, np);
        t->decl_tok = cx_pos;
        t->pscope = cx_scope;
        t->req_start = req_s;       /* its constraints: checked per use */
        t->req_end = req_e;
        skip_declaration();
        struct csym *y = scope_add(home, CS_TEMPLATE, name);
        y->tmpl = t;
        cx_scope = home;
        return;
    }
    if (cx_kind() == TOK_CX_CONCEPT) {
        /* template<...> concept C = constraint; */
        cx_advance();
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a concept's name");
        const char *name = cx_cur()->t.text;
        cx_advance();
        cx_expect(TOK_ASSIGN, "'=' in a concept's definition");
        struct ctemplate *t = template_new(TK_CONCEPT, name, home, ps, np);
        t->decl_tok = t->req_start = cx_pos;
        skip_constraint(1);
        t->req_end = t->head_end = cx_pos;
        cx_expect(TOK_SEMI, "';' after a concept");
        struct csym *y = scope_add(home, CS_TEMPLATE, name);
        y->tmpl = t;
        cx_scope = home;
        return;
    }
    if (cx_kind() == TOK_CX_FRIEND &&
        (cx_kind_at(1) == TOK_CX_CLASS || cx_kind_at(1) == TOK_KW_STRUCT ||
         cx_kind_at(1) == TOK_KW_UNION)) {
        /* template<...> friend class X; : access is not enforced, so the
         * friendship itself is all it says (X is declared if it was not) */
        cx_advance();
        cx_advance();
        struct qname q = peek_qname();
        cx_pos += q.fin;
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a class template's name");
        const char *name = cx_cur()->t.text;
        cx_advance();
        cx_expect(TOK_SEMI, "';' after a friend declaration");
        struct cscope *ns = enclosing_ns(home);
        if (!q.scope && !lookup(ns, name)) {
            struct ctemplate *t = template_new(TK_CLASS, name, ns, ps, np);
            struct csym *y = scope_add(ns, CS_TEMPLATE, name);
            y->tmpl = t;
            t->key = TOK_CX_CLASS;
        }
        cx_scope = home;
        return;
    }
    struct cguide *g = guide_at();
    if (g) {
        /* template<...> C(P...) -> C<A...>; */
        g->params = ps;
        g->nparams = np;
        g->scope = cx_scope;
        skip_declaration();
        cx_scope = home;
        return;
    }
    enum tok_kind k = cx_kind();
    /* attributes may sit between the key and the name */
    int an = 1;
    for (;;) {
        if (cx_kind_at(an) == TOK_KW_ATTRIBUTE &&
            cx_kind_at(an + 1) == TOK_LPAREN) {
            int save = cx_pos;
            cx_pos += an + 1;
            cx_skip_balanced();
            an = cx_pos - save;
            cx_pos = save;
        } else if (cx_kind_at(an) == TOK_LBRACKET &&
                   cx_kind_at(an + 1) == TOK_LBRACKET) {
            int save = cx_pos;
            cx_pos += an;
            cx_skip_balanced();
            an = cx_pos - save;
            cx_pos = save;
        } else {
            break;
        }
    }
    /* a qualified name: class N::X (libstdc++'s _GLIBCXX_NAMESPACE_CXX11) */
    int qn = 0;
    struct cscope *chome = home;
    if ((k == TOK_KW_STRUCT || k == TOK_CX_CLASS || k == TOK_KW_UNION) &&
        cx_kind_at(an) == TOK_IDENT && cx_kind_at(an + 1) == TOK_COLONCOLON) {
        int save = cx_pos;
        cx_pos += an;
        struct qname q = peek_qname();
        cx_pos = save;
        if (!q.bad && q.scope && q.fin > 0 &&
            cx_kind_at(an + q.fin) == TOK_IDENT) {
            qn = q.fin;
            chome = q.scope;
        }
    }
    if ((k == TOK_KW_STRUCT || k == TOK_CX_CLASS || k == TOK_KW_UNION) &&
        cx_kind_at(an) == TOK_IDENT && cx_kind_at(an + 1) == TOK_LT &&
        !qn) {
        /* class A<T>::B { ... }: a member class of a class template,
         * defined outside it — kept for A's instances */
        int save = cx_pos;
        cx_pos += an;
        struct csym *ty = lookup(cx_scope, cx_cur()->t.text);
        if (ty && ty->k == CS_TEMPLATE && ty->tmpl->kind == TK_CLASS) {
            cx_advance();
            skip_template_args();
            if (cx_kind() == TOK_COLONCOLON && cx_kind_at(1) == TOK_IDENT &&
                (cx_kind_at(2) == TOK_LBRACE || cx_kind_at(2) == TOK_COLON)) {
                cx_advance();
                struct coutdef *o = xcalloc(1, sizeof *o);
                o->params = ps;
                o->nparams = np;
                o->tok = decl;
                o->member = cx_cur()->t.text;
                o->key = k;
                cx_advance();
                o->class_body = cx_pos;
                o->next = ty->tmpl->outdefs;
                ty->tmpl->outdefs = o;
                skip_class_def();
                cx_scope = home;
                return;
            }
        }
        cx_pos = save;
        cx_half_gt = 0;
    }
    if ((k == TOK_KW_STRUCT || k == TOK_CX_CLASS || k == TOK_KW_UNION) &&
        cx_kind_at(an + qn) == TOK_IDENT &&
        (cx_kind_at(an + qn + 1) == TOK_LBRACE ||
         cx_kind_at(an + qn + 1) == TOK_COLON ||
         cx_kind_at(an + qn + 1) == TOK_SEMI ||
         cx_kind_at(an + qn + 1) == TOK_LT ||
         (cx_kind_at(an + qn + 1) == TOK_IDENT &&
          strcmp(cx_peek(an + qn + 1)->t.text, "final") == 0))) {
        cx_advance();
        parse_attrs(NULL);
        cx_pos += qn;
        const char *name = cx_cur()->t.text;
        cx_advance();
        struct csym *y = scope_find_here(chome, name);
        if (y && y->k != CS_TEMPLATE)
            cx_error(at, "'%s' redeclared as a template", name);
        if (cx_kind() == TOK_LT) {
            /* a partial specialization: A<pattern> */
            if (!y)
                cx_error(at, "partial specialization of undeclared '%s'",
                         name);
            struct cpartial *p = xcalloc(1, sizeof *p);
            p->params = ps;
            p->nparams = np;
            p->req_start = req_s;
            p->req_end = req_e;
            p->pat_tok = cx_pos;
            cx_pattern++;
            p->npattern = parse_template_args(y->tmpl, &p->pattern);
            cx_pattern--;
            if (cx_kind() == TOK_IDENT &&
                strcmp(cx_cur()->t.text, "final") == 0) {
                cx_advance();
                p->is_final = 1;
            }
            if (cx_accept(TOK_SEMI)) {      /* declared only */
                cx_scope = home;
                return;
            }
            p->head_end = cx_pos;
            p->key = k;
            skip_class_def();
            struct cpartial **tail = &y->tmpl->partials;
            while (*tail)
                tail = &(*tail)->next;
            *tail = p;
            cx_scope = home;
            return;
        }
        struct ctemplate *t = y ? y->tmpl
                                : template_new(TK_CLASS, name, chome, ps, np);
        if (!y) {
            y = scope_add(chome, CS_TEMPLATE, name);
            y->tmpl = t;
            t->member_of = cls;
        }
        t->key = k;
        if (y && t->params != ps) {
            /* a redeclaration may add defaults (template<class C, class
             * T = traits<C>> class X; after one without) */
            for (int i = 0; i < np && i < t->nparams; i++)
                if (t->params[i].def_tok < 0 && ps[i].def_tok >= 0) {
                    t->params[i].def_tok = ps[i].def_tok;
                    if (!t->params[i].name)
                        t->params[i].name = ps[i].name;
                }
        }
        if (req_s) {
            t->req_start = req_s;
            t->req_end = req_e;
        }
        if (cx_kind() == TOK_IDENT && strcmp(cx_cur()->t.text, "final") == 0) {
            cx_advance();
            t->is_final = 1;
        }
        if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON) {
            if (t->head_end >= 0)
                cx_error(at, "redefinition of class template '%s'", name);
            /* the definition's names — the defaults an earlier
             * declaration gave, kept */
            for (int i = 0; y && i < np && i < t->nparams; i++)
                if (ps[i].def_tok < 0 && t->params[i].def_tok >= 0)
                    ps[i].def_tok = t->params[i].def_tok;
            t->params = ps;
            t->nparams = np;
            t->head_end = cx_pos;
            t->pscope = cx_scope;       /* (its parameters: for CTAD) */
            skip_class_def();
        } else {
            cx_expect(TOK_SEMI, "';'");
        }
        cx_scope = home;
        return;
    }
    const char *member = NULL;
    struct ctemplate *owner = outdef_target(&member);
    if (owner) {
        struct coutdef *o = xcalloc(1, sizeof *o);
        o->params = ps;
        o->nparams = np;
        o->tok = decl;
        o->member = member;
        o->partial = !args_are_params(outdef_qual_lt, ps, np);
        o->next = owner->outdefs;
        owner->outdefs = o;
        skip_declaration();
        cx_scope = home;
        return;
    }
    /* a function or variable template: read as a pattern */
    cx_pattern++;
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *base = ds.type ? ds.type : ct_basic(CT_VOID);
    struct cty *t = parse_declarator(base, &d, DK_NAMED);
    parse_asm_label(&d.a);
    parse_attrs(&d.a);
    cx_pattern--;
    if (d.saved)
        cx_scope = d.saved;
    if (!d.name)
        cx_error(at, "expected a template's name");
    if (t->k == CT_FUNC && ds.is_friend && d.qual &&
        cx_kind() != TOK_LBRACE) {
        /* friend N::f<...>(...); : names a template declared in N —
         * access is not enforced, so nothing to declare */
        skip_declaration();
        cx_scope = home;
        return;
    }
    if (t->k == CT_FUNC) {
        if (d.kind == DN_CONV)
            t->to = d.conv_type;
        /* defined outside its class (template<class F> C::f(F)): the
         * class the qualifier names */
        struct cclass *ocls = cls ? cls
                              : d.qual && d.qual->k == SC_CLASS
                              ? d.qual->cls : NULL;
        struct ctemplate *tm = template_new(TK_FUNC, d.name, home, ps, np);
        tm->decl_tok = decl;
        tm->member_of = ocls;
        tm->req_start = req_s;
        tm->req_end = req_e;
        struct cfunc *f = xcalloc(1, sizeof *f);
        f->name = d.name;
        f->type = t;
        f->tmpl = tm;
        f->owner = home;
        f->cls = ds.is_friend ? NULL : ocls;
        f->hidden_friend = ds.is_friend && !d.qual;
        f->is_ctor = d.kind == DN_CTOR;
        f->is_conv = d.kind == DN_CONV;
        f->is_static = ds.storage == SK_STATIC || t->xobj;
        f->is_explicit = ds.is_explicit;
        f->access = access;
        f->vslot = -1;
        f->body_tok = f->mi_tok = -1;
        f->line = d.at ? d.at->t.line : 0;
        f->file = d.at ? d.at->file : NULL;
        tags_from(&f->abi_tags, &f->nabi_tags, &ds.a);
        tags_from(&f->abi_tags, &f->nabi_tags, &d.a);
        tm->pattern = f;
        /* a redeclaration (the definition after a declaration): keep one
         * pattern, the one with the body */
        struct cfunc **set;
        if (f->is_ctor) {
            if (!ocls)
                cx_error(d.at, "a constructor template outside its class");
            set = &ocls->ctors;
        } else {
            struct cscope *target = ds.is_friend ? enclosing_ns(home)
                                    : d.qual ? d.qual : home;
            struct csym *y = func_sym(target, d.name, d.at);
            set = &y->fns;
            f->owner = target;
            /* (a friend is the namespace's, its declaration read in the
             * class: the class's names, an instance's arguments) */
            tm->scope = target == home || ds.is_friend ? home : target;
        }
        int has_body = cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON ||
                       cx_kind() == TOK_ASSIGN;
        tm->has_body = has_body;
        /* a declaration then its definition are one template; two
         * definitions of one signature are two (enable_if overloads
         * differ only where a pattern cannot compare them) */
        for (struct cfunc *g = *set; g; g = g->next)
            if (g->tmpl && g->tmpl->nparams == np &&
                same_signature(g->type, t) &&
                (!has_body || !g->tmpl->has_body)) {
                if (!ds.is_friend)
                    g->hidden_friend = 0;
                for (int i = 0; i < f->nabi_tags; i++)
                    abi_tag_add(&g->abi_tags, &g->nabi_tags, f->abi_tags[i]);
                if (has_body) {
                    if (!g->tmpl->decl0_tok)
                        g->tmpl->decl0_tok = g->tmpl->decl_tok + 1;
                    g->tmpl->decl_tok = decl;
                    /* the declaration's default template arguments stay
                     * too (read with the definition's names for the
                     * parameters: the same, as a rule) */
                    for (int i = 0; i < np; i++)
                        if (ps[i].def_tok < 0)
                            ps[i].def_tok = g->tmpl->params[i].def_tok;
                    g->tmpl->params = ps;
                    g->tmpl->has_body = 1;
                    g->tmpl->scope = tm->scope;  /* the definition's names */
                    g->tmpl->req_start = req_s;
                    g->tmpl->req_end = req_e;
                    /* (the declaration's default arguments stay: the
                     * definition may not repeat them) */
                    if (g->type->defargs && !t->defargs && t->np ==
                                                           g->type->np)
                        t->defargs = g->type->defargs;
                    g->type = t;
                }
                cx_scope = home;
                if (has_body)
                    skip_declaration();
                else
                    cx_expect(TOK_SEMI, "';'");
                return;
            }
        while (*set)
            set = &(*set)->next;
        *set = f;
        if (cx_kind() == TOK_SEMI)
            cx_advance();
        else
            skip_declaration();
        cx_scope = home;
        return;
    }
    if (d.vtmpl) {
        /* template<class T> constexpr bool v<T*> = ...: a partial
         * specialization, its declaration replayed for the arguments it
         * matches */
        struct cpartial *p = xcalloc(1, sizeof *p);
        p->params = ps;
        p->nparams = np;
        p->pattern = d.targs;
        p->npattern = d.ntargs;
        p->head_end = decl;
        p->req_start = req_s;
        p->req_end = req_e;
        struct cpartial **tail = &d.vtmpl->partials;
        while (*tail)
            tail = &(*tail)->next;
        *tail = p;
        skip_declaration();
        cx_scope = home;
        return;
    }
    /* a variable template: template<class T> constexpr T pi = T(3.14); */
    struct ctemplate *tm = template_new(TK_VAR, d.name, home, ps, np);
    tm->decl_tok = decl;
    tm->req_start = req_s;
    tm->req_end = req_e;
    tm->member_of = cls;
    struct csym *y = scope_add(home, CS_TEMPLATE, d.name);
    y->tmpl = tm;
    skip_declaration();
    cx_scope = home;
}

/* template class A<int>; / template void f<int>(int); (and with extern:
 * instantiated elsewhere) */
static void member_var_define(struct cvar *v, int explicit_inst);

static void parse_explicit_instantiation(int is_extern)
{
    const struct ctok *at = cx_cur();
    if (cx_kind() == TOK_KW_STRUCT || cx_kind() == TOK_CX_CLASS ||
        cx_kind() == TOK_KW_UNION) {
        cx_advance();
        int n;
        struct cty *t = peek_type_name(&n);
        if (!t || t->k != CT_CLASS || !t->cls->tmpl)
            cx_error(at, "expected a class template's specialization");
        cx_skip_peek(n);
        cx_expect(TOK_SEMI, "';'");
        struct cclass *c = t->cls;
        if (is_extern == 2) {
            class_ensure(c);
            c->explicit_inst = 1;
            return;
        }
        if (is_extern) {
            c->tmpl->is_extern = 1;
            c->extern_inst = 1;
            return;
        }
        c->extern_inst = 0;           /* (after an extern template) */
        c->explicit_inst = 1;
        class_ensure(c);
        /* every member it has a definition of is instantiated — in the
         * class, or outside it (template<class T> R A<T>::f() {...}) */
        for (struct csym *y = c->scope->syms; y; y = y->next)
            if (y->k == CS_VAR && y->var->is_member_static &&
                !y->var->defined) {
                member_var_define(y->var, 1);
                if (y->var->defined)
                    y->var->used = 1;
            }
        for (struct cfunc *f = cx_funcs; f; f = f->all_next)
            if (f->cls == c && !f->tmpl && !f->spec_of && !f->defined &&
                !f->is_implicit && !f->is_deleted && !func_unsat(f)) {
                func_ensure_body(f);
                if (f->defined)
                    f->explicit_inst = 1;
            }
        return;
    }
    /* a function: its declaration names the specialization */
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *ft = parse_declarator(ds.type ? ds.type : ct_basic(CT_VOID),
                                      &d, DK_NAMED);
    if (d.saved)
        cx_scope = d.saved;
    cx_expect(TOK_SEMI, "';'");
    if (ft->k != CT_FUNC) {
        /* template T A<X>::m; : a static data member of an instance —
         * defined here (its definition out of the class, replayed), or,
         * extern, not (another unit's) */
        struct csym *y = d.qual && d.qual->k == SC_CLASS && d.name
                         ? scope_find_here(d.qual, d.name) : NULL;
        if (!y || y->k != CS_VAR)
            cx_error(at, "explicit instantiation of a variable template is "
                         "not supported yet");
        struct cvar *v = y->var;
        if (is_extern) {
            v->extern_inst = 1;
        } else {
            v->extern_inst = 0;
            member_var_define(v, 1);
            v->used = 1;
        }
        return;
    }
    if (d.qual && d.qual->k == SC_CLASS && in_instance(d.qual->cls) &&
        !d.has_targs) {
        /* template R A<X>::f(...); : a member of a class template's
         * instance (or of a class in one), not a template itself */
        struct cclass *c = d.qual->cls;
        class_ensure(c);
        struct csym *y = d.kind == DN_CTOR || d.kind == DN_DTOR ? NULL
                         : scope_find_here(c->scope, d.name);
        struct cfunc *set = d.kind == DN_CTOR ? c->ctors
                            : d.kind == DN_DTOR ? c->dtor
                            : y && y->k == CS_FUNC ? y->fns : NULL;
        for (struct cfunc *f = set; f; f = f->next) {
            if (f->tmpl || !same_signature(f->type, ft))
                continue;
            if (is_extern) {
                f->extern_inst = 1;
            } else {
                func_ensure_body(f);
                f->explicit_inst = 1;
            }
            return;
        }
        /* (else a member template's specialization, below) */
    }
    struct cfunc *spec = specialization_named(&d, ft, at);
    if (!is_extern) {
        func_ensure_body(spec);
        spec->explicit_inst = 1;
    } else {
        spec->extern_inst = 1;
    }
}

/* The specialization a declarator names: f<args> or f deduced from the
 * declared parameter types (13.10.3.7). */
static struct cfunc *specialization_named(struct declarator *d,
                                          struct cty *ft,
                                          const struct ctok *at)
{
    struct cscope *where = d->qual ? d->qual : cx_scope;
    struct csym *y = d->qual ? lookup_in(where, d->name)
                             : lookup(where, d->name);
    struct cfunc *set = y && y->k == CS_FUNC ? y->fns : NULL;
    if (d->kind == DN_CTOR && d->qual && d->qual->k == SC_CLASS)
        set = d->qual->cls->ctors;
    for (struct cfunc *g = set; g; g = g->next) {
        if (!g->tmpl)
            continue;
        struct ctemplate *t = g->tmpl;
        struct ctarg *args;
        int nargs;
        if (d->has_targs) {
            if (!deduce_call(t, d->targs, d->ntargs, NULL, 0, &args,
                             &nargs))
                continue;
        } else {
            /* deduce from the declared types: fake arguments of them */
            struct cexpr **fa = xmalloc((size_t)(ft->np ? ft->np : 1) *
                                        sizeof *fa);
            for (int i = 0; i < ft->np; i++) {
                struct cty *pt = ft->params[i];
                fa[i] = ex_new(E_VAR, ct_strip_ref(pt),
                               pt->k == CT_RREF ? VC_XVALUE : VC_LVALUE);
            }
            if (!deduce_call(t, NULL, 0, fa, ft->np, &args, &nargs))
                continue;
        }
        struct cfunc *spec = func_instance(t, args, nargs);
        if (spec && same_signature(spec->type, ft))
            return spec;
    }
    cx_error(at, "'%s' matches no template's specialization", d->name);
    return NULL;
}

/* template<> ... : a specialization written out */
static void parse_explicit_specialization(struct cclass *cls, int access)
{
    const struct ctok *at = cx_cur();
    enum tok_kind k = cx_kind();
    struct qname q = { NULL, 0, 0, NULL };
    if (k == TOK_KW_STRUCT || k == TOK_CX_CLASS || k == TOK_KW_UNION) {
        cx_advance();
        q = peek_qname();             /* (template<> struct std::X<...>) */
        cx_pos--;
    }
    if ((k == TOK_KW_STRUCT || k == TOK_CX_CLASS || k == TOK_KW_UNION) &&
        !q.bad && cx_kind_at(1 + q.fin) == TOK_IDENT &&
        cx_kind_at(2 + q.fin) == TOK_LT) {
        cx_advance();
        cx_pos += q.fin;
        struct csym *y = q.scope ? lookup_in(q.scope, cx_cur()->t.text)
                                 : lookup(cx_scope, cx_cur()->t.text);
        if (!y || y->k != CS_TEMPLATE || y->tmpl->kind != TK_CLASS)
            cx_error(at, "'%s' is not a class template", cx_cur()->t.text);
        cx_advance();
        struct ctarg *args;
        int n = parse_template_args(y->tmpl, &args);
        struct cclass *c = class_instance(y->tmpl, args, n, at);
        if (cx_kind() == TOK_SEMI) {           /* declared: not the primary */
            if (c->complete && !c->explicit_spec)
                cx_error(at, "'%s' is specialized after its instantiation",
                         y->tmpl->name);
            c->inst_pending = 0;
            c->explicit_spec = 1;
            cx_advance();
            return;
        }
        if (c->complete || (!c->inst_pending && !c->explicit_spec))
            cx_error(at, "'%s' is specialized after its instantiation",
                     y->tmpl->name);
        c->explicit_spec = 1;
        c->inst_pending = 0;
        c->scope->parent = q.scope ? q.scope : cx_scope;
        struct attrs a;
        memset(&a, 0, sizeof a);
        if (cx_kind() == TOK_IDENT && strcmp(cx_cur()->t.text, "final") == 0)
            cx_advance();
        parse_class_body(c, k, &a, at);
        cx_expect(TOK_SEMI, "';' after a class specialization");
        return;
    }
    (void)cls;
    (void)access;
    /* a function (or a member of a specialization): its declarator names
     * which */
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *ft = parse_declarator(ds.type ? ds.type : ct_basic(CT_VOID),
                                      &d, DK_NAMED);
    parse_asm_label(&d.a);
    parse_attrs(&d.a);
    struct cscope *after = d.saved;
    if (ft->k != CT_FUNC && d.vtmpl) {
        /* template<> constexpr bool v<int> = true; */
        struct ctemplate *t = d.vtmpl;
        struct cvar *v = xcalloc(1, sizeof *v);
        v->name = t->name;
        v->type = ds.is_constexpr ? ct_qual(ft, CQ_CONST) : ft;
        v->owner = t->scope;
        v->is_inline = 1;
        v->is_constexpr = ds.is_constexpr;
        v->defined = 1;
        v->line = d.at ? d.at->t.line : 0;
        v->file = d.at ? d.at->file : NULL;
        var_explicit_spec(t, d.targs, d.ntargs, v, at);
        init_variable(v, d.at ? d.at : at);
        gvar_register(v);
        if (after)
            cx_scope = after;
        cx_expect(TOK_SEMI, "';' after a variable's specialization");
        return;
    }
    if (ft->k != CT_FUNC) {
        /* template<> int A<int>::count = 3; — a member of an instance,
         * its own (declared here: defined elsewhere, as libstdc++'s
         * __timepunct_cache<char>::_S_timezones is; or defined here) */
        if (after)
            cx_scope = after;
        struct csym *y = d.qual && d.qual->k == SC_CLASS
                         ? scope_find_here(d.qual, d.name) : NULL;
        if (!y || y->k != CS_VAR)
            cx_error(at, "'%s' is not a static data member of a class "
                         "template's instance", d.name ? d.name : "");
        struct cvar *v = y->var;
        v->explicit_spec = 1;
        if (cx_kind() == TOK_ASSIGN || cx_kind() == TOK_LBRACE ||
            cx_kind() == TOK_LPAREN) {
            v->type = ds.is_constexpr ? ct_qual(ft, CQ_CONST) : ft;
            v->is_extern = 0;
            v->is_inline = ds.is_inline || ds.is_constexpr;
            v->defined = 1;
            init_variable(v, d.at ? d.at : at);
        }
        cx_expect(TOK_SEMI, "';' after a member's specialization");
        return;
    }
    if (d.qual && d.qual->k == SC_CLASS && !d.has_targs) {
        /* a member of a class template's instance: an ordinary member */
        struct cfunc *f = NULL;
        struct csym *y = d.kind == DN_CTOR ? NULL
                         : scope_find_here(d.qual, d.name);
        struct cfunc *set = d.kind == DN_CTOR ? d.qual->cls->ctors
                            : d.kind == DN_DTOR ? d.qual->cls->dtor
                            : y && y->k == CS_FUNC ? y->fns : NULL;
        for (struct cfunc *g = set; g; g = g->next)
            if (!g->tmpl && same_signature(g->type, ft))
                f = g;
        if (f) {
            f->lazy = 0;
            f->def_scope = cx_scope;
            /* (its own definition: inline only as declared here) */
            f->is_inline = ds.is_inline || ds.is_constexpr;
            if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON)
                define_function(f, ft);
            else
                cx_expect(TOK_SEMI, "';'");
            if (after)
                cx_scope = after;
            return;
        }
    }
    struct cfunc *spec = specialization_named(&d, ft, at);
    if (spec->defined)
        cx_error(at, "'%s' is specialized after its instantiation", d.name);
    spec->lazy = 0;
    spec->pnames = ft->pnames;
    spec->type = ft;
    /* an ordinary function: inline only when declared so */
    spec->is_inline = ds.is_inline || ds.is_constexpr;
    if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON) {
        spec->def_scope = cx_scope;
        define_function(spec, ft);
    } else {
        cx_expect(TOK_SEMI, "';'");
        spec->body_tok = -1;
    }
    if (after)
        cx_scope = after;
}

void class_define_from(struct cclass *c, int pos, enum tok_kind key,
                       struct cscope *ps)
{
    int nuc = expr_swap_no_user_conv(0);
    struct parse_state *st = parse_save();
    cx_unevaluated = 0;
    cx_scope = ps;
    cx_pos = pos;
    cx_half_gt = 0;
    cx_curfn = NULL;
    cx_curblk = NULL;
    cx_extern_c = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    class_depth = 0;
    pend = NULL;
    npend = cappend = 0;
    c->scope->parent = ps;
    struct attrs a;
    memset(&a, 0, sizeof a);
    parse_class_body(c, key, &a, cx_cur());
    parse_restore(st);
    expr_swap_no_user_conv(nuc);
}

/* The declaration of a function template's specialization: its tokens
 * read with the parameters bound (ps), a function not entered in any
 * scope — overload resolution holds it — whose body is read when used. */
static struct cfunc *lambda_call_replay(struct ctemplate *t);

/* f, a specialization of t read from t's definition: the default
 * arguments t's earlier declaration gave (the definition repeats none) */
static void decl0_defaults(struct ctemplate *t, struct cfunc *f)
{
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        return;
    }
    cx_sfinae = &jb;
    cx_pos = t->decl0_tok - 1;
    cx_half_gt = 0;
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *ft = parse_declarator(ds.type ? ds.type : ct_basic(CT_VOID),
                                      &d, DK_NAMED);
    cx_sfinae = saved;
    parse_restore(st);
    if (ft->k != CT_FUNC || ft->np != f->type->np || !ft->defargs)
        return;
    if (!f->defargs)
        f->defargs = xcalloc((size_t)(ft->np ? ft->np : 1),
                             sizeof *f->defargs);
    for (int i = 0; i < ft->np; i++)
        if (!f->defargs[i])
            f->defargs[i] = ft->defargs[i];
}

struct cfunc *func_decl_replay(struct ctemplate *t, struct cscope *ps)
{
    cx_scope = ps;
    cx_pos = t->decl_tok;
    cx_half_gt = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    cx_curfn = NULL;
    cx_curblk = NULL;
    if (t->lambda)
        return lambda_call_replay(t);
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *ft = parse_declarator(ds.type ? ds.type : ct_basic(CT_VOID),
                                      &d, DK_NAMED);
    parse_asm_label(&d.a);
    parse_attrs(&d.a);
    if (ft->k != CT_FUNC)
        cx_error(cx_cur(), "a function template declares no function");
    if (ft->treq && !constraint_satisfied(ft->treq, ft->treq_end,
                                          with_params(ft, cx_scope)))
        cx_error(cx_cur(), "the constraints of '%s' are not satisfied",
                 t->name);
    if (d.kind == DN_CONV)
        ft->to = d.conv_type;
    struct cfunc *pat = t->pattern;
    struct cfunc *f = xcalloc(1, sizeof *f);
    f->name = pat->name;
    f->type = ft;
    f->pnames = ft->pnames;
    f->owner = pat->owner;
    f->cls = pat->cls;
    f->is_ctor = pat->is_ctor;
    f->is_conv = pat->is_conv;
    f->is_static = pat->is_static || ds.storage == SK_STATIC;
    f->is_explicit = pat->is_explicit || ds.is_explicit;
    f->is_inline = 1;
    f->is_constexpr = ds.is_constexpr;
    f->is_consteval = ds.is_consteval;
    f->access = pat->access;
    f->vslot = -1;
    f->line = pat->line;
    f->file = pat->file;
    f->body_tok = f->mi_tok = -1;
    if (ft->defargs) {
        f->defargs = xcalloc((size_t)(ft->np ? ft->np : 1),
                             sizeof *f->defargs);
        for (int i = 0; i < ft->np; i++)
            f->defargs[i] = ft->defargs[i];
    }
    if (t->decl0_tok)
        decl0_defaults(t, f);
    if (cx_accept(TOK_ASSIGN)) {
        if (cx_accept(TOK_CX_DELETE))
            f->is_deleted = 1;
        else if (cx_accept(TOK_KW_DEFAULT))
            f->is_defaulted = 1;
    } else if (cx_kind() == TOK_LBRACE || cx_kind() == TOK_COLON) {
        skip_body(f);
        f->lazy = 1;
        f->def_scope = cx_scope;
    }
    func_register(f);
    return f;
}

/* The body of a function whose tokens were kept (lazy): read now. A
 * member of a class template's instance with no body in the class has
 * it in an out-of-class definition (template<class T> R A<T>::f()). */

/* Is f an operator a class may default (11.10)? */
int is_defaultable_cmp(struct cfunc *f)
{
    static const char *const ops[] = {
        "operator==", "operator!=", "operator<=>", "operator<",
        "operator>", "operator<=", "operator>=",
    };
    for (size_t i = 0; f->name && i < sizeof ops / sizeof ops[0]; i++)
        if (strcmp(f->name, ops[i]) == 0)
            return 1;
    return 0;
}

/* std::X of <compare>'s category X, or NULL */
static struct cty *std_category(const char *name)
{
    struct csym *ns = scope_find_here(cx_global, "std");
    struct csym *y = ns && ns->k == CS_NAMESPACE
                     ? scope_find_here(ns->ns, name) : NULL;
    return y && (y->k == CS_CLASS || y->k == CS_TYPEDEF) &&
           y->type->k == CT_CLASS ? y->type : NULL;
}

/* 0 partial_ordering, 1 weak_ordering, 2 strong_ordering, -1 another */
static int category_rank(struct cty *t)
{
    t = ct_unqual(t);
    static const char *const names[] = {
        "partial_ordering", "weak_ordering", "strong_ordering",
    };
    for (int i = 0; i < 3; i++) {
        struct cty *c = std_category(names[i]);
        if (c && t->k == CT_CLASS && t->cls == c->cls)
            return i;
    }
    return -1;
}

static struct cexpr *param_ref_expr(struct cvar *v)
{
    struct cexpr *e = ex_new(E_VAR, ct_strip_ref(v->type), VC_LVALUE);
    e->var = v;
    return e;
}

/* A defaulted comparison operator (11.10), defined when first used: ==
 * compares the bases, then the members, in order; <=> compares them the
 * same way, the first unequal pair deciding (its `auto` result the
 * weakest category of theirs); a defaulted !=, <, >, <= or >= is its
 * rewritten form, through == or <=>. */
void define_defaulted_cmp(struct cfunc *f)
{
    if (f->defined)
        return;
    const struct ctok *at = cx_cur();
    struct cty *ft = f->type;
    int member = f->cls && !f->is_static;
    if (ft->np != (member ? 1 : 2))
        cx_error(at, "a defaulted '%s' compares two objects of its class",
                 f->name);
    struct cty *pt = ct_unqual(ct_strip_ref(ft->params[0]));
    if (pt->k != CT_CLASS)
        cx_error(at, "a defaulted '%s' compares objects of a class",
                 f->name);
    struct cclass *c = pt->cls;
    f->defined = 1;
    f->is_inline = 1;
    struct cfunc *savefn = cx_curfn;
    struct cstmt *saveblk = cx_curblk;
    int saved_uneval = cx_unevaluated;
    cx_unevaluated = 0;
    cx_curfn = f;
    f->params = xcalloc((size_t)ft->np, sizeof *f->params);
    for (int i = 0; i < ft->np; i++) {
        struct cvar *v = new_local(NULL, ft->params[i], at);
        v->is_param = 1;
        f->params[i] = v;
    }
    if (member) {
        struct cvar *tv = new_local("this", NULL, at);
        tv->cname = "this";
        tv->type = ct_ptr(ct_qual(ct_class(f->cls), ft->fq));
        tv->is_param = 1;
        f->this_var = tv;
    }
    struct cstmt *blk = st_new(S_BLOCK);
    f->body = blk;
    cx_curblk = blk;
    struct cstmt **tail = &blk->body;
    struct cexpr *x = member ? ex_deref(ex_this())
                             : param_ref_expr(f->params[0]);
    struct cexpr *y = param_ref_expr(f->params[member ? 0 : 1]);
    struct cty *rt = ft->to;
    const char *op = f->name + 8;
    struct cexpr *res = NULL;
    if (!strcmp(op, "==") || !strcmp(op, "<=>")) {
        int three = op[0] == '<';
        /* the operands: each base, then each member */
        int n = 0;
        struct cexpr **xs = xcalloc((size_t)(c->nbases + c->nfields + 1),
                                    sizeof *xs);
        struct cexpr **ys = xcalloc((size_t)(c->nbases + c->nfields + 1),
                                    sizeof *ys);
        for (int b = 0; b < c->nbases; b++) {
            xs[n] = to_base(x, c->bases[b].cls, 0);
            ys[n++] = to_base(y, c->bases[b].cls, 0);
        }
        for (int i = 0; i < c->nfields; i++) {
            struct cfield *fl = c->fields[i];
            if (!fl->name)
                continue;
            if (fl->type->k == CT_ARRAY)
                cx_error(at, "a defaulted '%s' of a class with an array "
                             "member is not supported yet", f->name);
            if (ct_is_ref(fl->type))
                cx_error(at, "a defaulted '%s' of a class with a reference "
                             "member is deleted", f->name);
            xs[n] = ex_member(x, fl);
            ys[n++] = ex_member(y, fl);
        }
        if (!three) {
            for (int i = 0; i < n; i++) {
                struct cexpr *e = convert_bool(expr_binary(TOK_EQEQ, xs[i],
                                                           ys[i]), "==");
                res = res ? expr_binary(TOK_ANDAND, res, e) : e;
            }
            if (!res)
                res = ex_int(1, ct_basic(CT_BOOL));
        } else {
            struct cexpr **cs = xcalloc((size_t)(n ? n : 1), sizeof *cs);
            int weakest = 2;
            for (int i = 0; i < n; i++) {
                cs[i] = expr_binary(TOK_SPACESHIP, xs[i], ys[i]);
                int r = category_rank(cs[i]->t);
                if (r < 0)
                    cx_error(at, "a defaulted '<=>': comparing a member "
                                 "gives '%s', not a comparison category",
                             ct_name(cs[i]->t));
                if (r < weakest)
                    weakest = r;
            }
            if (ct_has_auto(rt)) {
                static const char *const names[] = {
                    "partial_ordering", "weak_ordering", "strong_ordering",
                };
                rt = std_category(names[weakest]);
                if (!rt)
                    cx_error(at, "std::%s is not declared (#include "
                                 "<compare>)", names[weakest]);
                f->type->to = rt;
            }
            /* { R c = x.m <=> y.m; if (c != 0) return c; } ... */
            for (int i = 0; i < n; i++) {
                struct cvar *v = new_local(NULL, rt, at);
                init_variable_with(v, INIT_COPY, &cs[i], 1, at);
                struct cstmt *d = decl_of(v, at);
                *tail = d;
                tail = &d->next;
                struct cstmt *iff = st_new(S_IF);
                iff->e = convert_bool(expr_binary(TOK_NEQ, param_ref_expr(v),
                                                  ex_literal_zero()), "<=>");
                struct cstmt *r = st_new(S_RETURN);
                struct cexpr *rv = param_ref_expr(v);
                r->e = init_object(rt, INIT_COPY, &rv, 1, at);
                iff->body = r;
                *tail = iff;
                tail = &iff->next;
            }
            struct csym *eq = scope_find_here(rt->cls->scope, "equivalent");
            if (!eq || eq->k != CS_VAR)
                cx_error(at, "'%s' has no 'equivalent'", ct_name(rt));
            res = param_ref_expr(eq->var);
        }
    } else {
        /* x != y is !(x == y), x @ y is (x <=> y) @ 0 (11.10.4) — not
         * this operator again */
        if (!strcmp(op, "!=")) {
            res = ex_new(E_UNARY, ct_basic(CT_BOOL), VC_PRVALUE);
            res->a = xmalloc(sizeof *res->a);
            res->a[0] = convert_bool(expr_binary(TOK_EQEQ, x, y), "!=");
            res->na = 1;
            res->op = TOK_BANG;
        } else {
            enum tok_kind k = !strcmp(op, "<") ? TOK_LT : !strcmp(op, ">")
                              ? TOK_GT : !strcmp(op, "<=") ? TOK_LE : TOK_GE;
            res = expr_binary(k, expr_binary(TOK_SPACESHIP, x, y),
                              ex_literal_zero());
        }
    }
    struct cstmt *r = st_new(S_RETURN);
    r->e = rt->k == CT_BOOL ? convert_bool(res, f->name)
                            : init_object(rt, INIT_COPY, &res, 1, at);
    *tail = r;
    cx_unevaluated = saved_uneval;
    cx_curfn = savefn;
    cx_curblk = saveblk;
}

void func_define_from(struct cfunc *f)
{
    int nuc = expr_swap_no_user_conv(0);
    struct parse_state *st = parse_save();
    cx_unevaluated = 0;               /* a body is evaluated */
    cx_half_gt = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    cx_extern_c = 0;
    if (f->body_tok >= 0) {
        cx_scope = f->def_scope;
        cx_pos = f->mi_tok >= 0 ? f->mi_tok : f->body_tok;
        define_function(f, f->type);
        parse_restore(st);
        expr_swap_no_user_conv(nuc);
        return;
    }
    parse_restore(st);
    expr_swap_no_user_conv(nuc);
}

/* At `template<...> R A<T>::f(...) { ... }` (the class template's
 * parameters bound): f, a specialization of the instance's member
 * template f, defined by it — if this is that template's definition (its
 * type, the parameters bound to f's arguments, is f's). */
static void member_template_from_outdef(struct cfunc *f)
{
    struct ctemplate *mt = f->spec_of;
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        /* not this template's definition (another overload's) */
        parse_restore(st);
        cx_sfinae = saved;
        return;
    }
    cx_sfinae = &jb;
    cx_advance();                                   /* template */
    cx_expect(TOK_LT, "'<'");
    struct cscope *outer = cx_scope;
    scope_push(SC_TEMPLATE, NULL);
    struct ctparam *ps;
    int np = parse_tparams(&ps);
    if (np != mt->nparams || np != f->ntargs)
        cx_error(cx_cur(), "another member template");
    if (cx_accept(TOK_CX_REQUIRES))
        skip_constraint(0);
    cx_scope = tparam_scope(ps, np, f->targs, f->ntargs, outer);
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *ft = parse_declarator(ds.type ? ds.type : ct_basic(CT_VOID),
                                      &d, DK_NAMED);
    int same = ft->k == CT_FUNC && ft->np == f->type->np;
    for (int i = 0; same && i < ft->np; i++)
        same = ct_same(ft->params[i], f->type->params[i]);
    if (same && !f->is_ctor && !f->is_dtor)  /* (a constructor's is void) */
        same = ct_same(ft->to, f->type->to) && ft->fq == f->type->fq &&
               ft->refq == f->type->refq;
    if (!same || (cx_kind() != TOK_LBRACE && cx_kind() != TOK_COLON &&
                  cx_kind() != TOK_CX_TRY))
        cx_error(cx_cur(), "another member template");
    cx_sfinae = saved;
    f->def_scope = cx_scope;
    define_function(f, ft);
    parse_restore(st);
}

/* A class template instance's member that has no definition yet: its
 * out-of-class definition, if the template has one, replayed with the
 * instance's arguments (the qualified declarator then finds the member). */
int member_from_outdef(struct cfunc *f)
{
    struct cclass *c = f->cls;
    struct cclass *inst = c;
    while (inst && !inst->tmpl) {
        struct cclass *up = inst->owner && inst->owner->k == SC_CLASS
                            ? inst->owner->cls : NULL;
        if (!up && inst->scope->parent &&
            inst->scope->parent->k == SC_CLASS)
            up = inst->scope->parent->cls;
        inst = up;
    }
    if (!inst || (inst->extern_inst && !f->is_inline && !f->is_constexpr))
        return 0;         /* extern template: another unit's (not inline) */
    if (inst->explicit_spec)
        return 0;         /* its members are its own, not the template's */
    struct ctemplate *t = inst->tmpl;
    const char *want = f->is_conv ? "operator" : f->name;
    if (f->name && strncmp(f->name, "operator", 8) == 0)
        want = "operator";
    for (struct coutdef *o = t->outdefs; o && !f->defined; o = o->next) {
        if (!o->member || strcmp(o->member, want) != 0)
            continue;
        if (o->class_body)
            continue;       /* A<T>::B { }: a member class (B's ctor is
                             * named B too), not a function */
        struct ctarg *args = inst->targs;
        int na = inst->ntargs;
        if (o->partial && !inst->inst_partial)
            continue;       /* a partial specialization's member */
        if (inst->inst_partial) {
            /* a partial specialization's member: its own parameters
             * bound (a definition of another's fails its qualifier) */
            if (o->nparams != inst->inst_partial->nparams)
                continue;
            args = inst->inst_bound;
            na = inst->inst_partial->nparams;
        }
        struct parse_state *st = parse_save();
        jmp_buf jb;
        void *saved_sf = cx_sfinae;
        if (inst->inst_partial) {
            if (setjmp(jb)) {
                parse_restore(st);
                cx_sfinae = saved_sf;
                outdef_want = NULL;
                continue;
            }
            cx_sfinae = &jb;
        }
        cx_scope = tparam_scope(o->params, o->nparams, args, na, t->scope);
        cx_pos = o->tok;
        cx_half_gt = 0;
        cx_pattern = 0;
        cx_in_targs = 0;
        cx_curfn = NULL;
        cx_curblk = NULL;
        cx_extern_c = 0;
        if (cx_kind() == TOK_CX_TEMPLATE) {
            /* a member template's: for one of its specializations, with
             * the template's own parameters bound to its arguments */
            if (f->spec_of)
                member_template_from_outdef(f);
            parse_restore(st);
            continue;
        }
        outdef_want = f;
        parse_declaration(1, NULL);
        outdef_want = NULL;
        cx_sfinae = saved_sf;
        parse_restore(st);
    }
    if (f->defined)
        f->is_inline = 1;   /* each unit's instance: one function (weak) */
    return f->defined;
}

void member_class_from_outdef(struct cclass *c)
{
    struct cclass *inst = c->owner->cls;
    if (!inst || !inst->tmpl || inst->inst_partial)
        return;
    struct ctemplate *t = inst->tmpl;
    for (struct coutdef *o = t->outdefs; o; o = o->next) {
        if (!o->class_body || strcmp(o->member, c->name) != 0)
            continue;
        /* inside the instance, as a member — the definition's own names
         * for the parameters bound in front */
        struct cscope *ps = tparam_scope(o->params, o->nparams, inst->targs,
                                         inst->ntargs, inst->scope);
        cx_inst_push(cx_fmt("%s::%s", ct_name(ct_class(inst)), c->name),
                     cx_cur());
        class_define_from(c, o->class_body, o->key, ps);
        cx_inst_pop();
        return;
    }
}

/* A class template instance's static data member used: its definition
 * out of the class, replayed with the instance's arguments. */
static void member_var_define(struct cvar *v, int explicit_inst);

void member_var_from_outdef(struct cvar *v)
{
    member_var_define(v, 0);
}

/* (explicitly instantiated: though its class's instantiation is another
 * unit's) */
static void member_var_define(struct cvar *v, int explicit_inst)
{
    if (v->defined || v->explicit_spec || v->extern_inst || !v->owner ||
        v->owner->k != SC_CLASS)
        return;
    struct cclass *inst = v->owner->cls;
    while (inst && !inst->tmpl)
        inst = inst->owner && inst->owner->k == SC_CLASS ? inst->owner->cls
                                                         : NULL;
    if (!inst || (inst->extern_inst && !explicit_inst) ||
        inst->inst_partial || inst->explicit_spec)
        return;
    struct ctemplate *t = inst->tmpl;
    for (struct coutdef *o = t->outdefs; o && !v->defined; o = o->next) {
        if (!o->member || strcmp(o->member, v->name) != 0)
            continue;
        struct parse_state *st = parse_save();
        cx_scope = tparam_scope(o->params, o->nparams, inst->targs,
                                inst->ntargs, t->scope);
        cx_pos = o->tok;
        cx_half_gt = 0;
        cx_pattern = 0;
        cx_in_targs = 0;
        cx_curfn = NULL;
        cx_curblk = NULL;
        cx_extern_c = 0;
        parse_declaration(1, NULL);
        parse_restore(st);
        if (v->defined) {
            v->is_inline = 1;   /* each unit's instance: one object */
            v->explicit_inst = explicit_inst;
        }
    }
}

/* A member function's default argument read where it was written, now
 * its class is complete. */
struct cexpr *defarg_read(struct cexpr *d)
{
    if (d->k != E_DEFARG)
        return d;
    if (d->na)
        return d->a[0];
    struct parse_state *st = parse_save();
    cx_scope = d->dscope;
    cx_pos = (int)d->ival;
    cx_half_gt = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    cx_unevaluated = 0;
    cx_curfn = NULL;
    cx_curblk = NULL;
    struct cexpr *e = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                              : expr_parse_assign();
    if (cx_kind() != TOK_COMMA && cx_kind() != TOK_RPAREN)
        cx_error(cx_cur(), "expected ',' or ')' after a default argument");
    parse_restore(st);
    d->a = xmalloc(sizeof *d->a);
    d->a[0] = e;
    d->na = 1;
    return e;
}

struct cvar *var_define_from(struct ctemplate *t, int pos, struct ctarg *args,
                             struct cscope *ps)
{
    struct parse_state *st = parse_save();
    cx_unevaluated = 0;
    cx_scope = ps;
    cx_pos = pos;
    cx_half_gt = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    cx_curfn = NULL;
    cx_curblk = NULL;
    struct dspec ds;
    parse_dspec(&ds);
    struct declarator d;
    memset(&d, 0, sizeof d);
    struct cty *ty = parse_declarator(ds.type, &d, DK_NAMED);
    struct cvar *v = xcalloc(1, sizeof *v);
    v->name = t->name;
    v->type = ds.is_constexpr ? ct_qual(ty, CQ_CONST) : ty;
    v->owner = t->scope;
    v->is_inline = 1;
    v->is_constexpr = ds.is_constexpr;
    v->defined = 1;
    v->line = d.at ? d.at->t.line : 0;
    v->file = d.at ? d.at->file : NULL;
    v->targs = args;
    v->ntargs = t->nparams;
    v->cname = mangle_var(v, t->scope);
    init_variable(v, d.at ? d.at : cx_cur());
    gvar_register(v);
    parse_restore(st);
    return v;
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

/* template<template<class T, T...> class TT, class T, T N> using
 * __make_integer_seq = TT<T, 0, 1, ..., N - 1>; (template.c makes it) */
static void declare_builtin_templates(void)
{
    struct ctparam *ps = xcalloc(3, sizeof *ps);
    static const char *const names[3] = { "_Seq", "_Tp", "_Num" };
    static const int kinds[3] = { TP_TEMPLATE, TP_TYPE, TP_VALUE };
    for (int i = 0; i < 3; i++) {
        ps[i].kind = kinds[i];
        ps[i].name = names[i];
        ps[i].def_tok = ps[i].vtype_tok = ps[i].tc_args = -1;
    }
    ps[2].vtype = ct_tparam(1, "_Tp");
    struct ctemplate *t = template_new(TK_ALIAS, "__make_integer_seq",
                                       cx_global, ps, 3);
    t->builtin = BT_MAKE_INTEGER_SEQ;
    struct csym *y = scope_add(cx_global, CS_TEMPLATE, t->name);
    y->tmpl = t;
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
    declare_builtin_templates();
    while (cx_kind() != TOK_EOF)
        parse_declaration(1, NULL);
    /* members of instances called before their out-of-class definitions
     * were read: defined now (the point of instantiation is the unit's
     * end too, 13.8.4.1) */
    for (int progress = 1; progress;) {
        progress = 0;
        for (struct cfunc *f = cx_funcs; f; f = f->all_next)
            if (f->called && !f->defined && !f->is_deleted && !f->tmpl &&
                f->cls && !f->is_implicit && !f->is_defaulted &&
                !f->lazy && f->body_tok < 0) {
                func_ensure_body(f);
                progress |= f->defined;
            }
        /* a constructed class's vtables name its virtual functions: an
         * instance's are instantiated for them (a vtable is their only
         * use: _Formatting_scanner's _M_on_chars) */
        for (int ci = 0; ci < cx_nclasses; ci++) {
            struct cclass *c = cx_classes[ci];
            if (!c->dynamic || !c->complete || !in_instance(c))
                continue;
            int built = 0;
            for (struct cfunc *k = c->ctors; k && !built; k = k->next) {
                built = k->used;
                /* ... or a specialization of a constructor template
                 * (_Sp_counted_ptr_inplace's) */
                for (struct cinst *in = k->tmpl ? k->tmpl->insts : NULL;
                     in && !built; in = in->next)
                    built = in->fn && in->fn->used;
            }
            if (!built)
                continue;
            struct vgroup *g = vtable_group(c);
            for (int vi = 0; g && vi < g->n; vi++)
                for (int j = 0; j < g->v[vi].nfns; j++) {
                    struct cfunc *f = g->v[vi].fns[j].f;
                    if (!f || g->v[vi].fns[j].pure || f->defined ||
                        f->is_deleted || f->tmpl || f->is_implicit ||
                        f->is_defaulted)
                        continue;
                    func_ensure_body(f);
                    progress |= f->defined;
                }
        }
    }
}

/* ---- lambdas ---- */

static struct ccapture *new_capture(struct clambda *L)
{
    if (L->ncaps == L->capcaps) {
        L->capcaps = L->capcaps ? L->capcaps * 2 : 4;
        L->caps = xrealloc(L->caps, (size_t)L->capcaps * sizeof *L->caps);
    }
    struct ccapture *c = &L->caps[L->ncaps++];
    memset(c, 0, sizeof *c);
    return c;
}

/* The closure's member for capture c, of type t: added, and found by
 * name in the body (a member of the closure). */
static void capture_field(struct clambda *L, struct ccapture *c,
                          struct cty *t, const char *name)
{
    struct cfield *fl = xcalloc(1, sizeof *fl);
    fl->name = name;
    fl->type = t;
    fl->bitwidth = -1;
    fl->dflt_tok = -1;
    fl->access = CA_PUBLIC;
    add_field(L->cls, fl);
    c->field = fl;
    if (name) {
        struct csym *y = scope_add(L->cls->scope, CS_FIELD, name);
        y->field = fl;
    }
}

/* L's operator() being read: the function being defined, or the one
 * around it whose lambda L is (an enclosing lambda's, a lambda in its
 * body being made) — for a generic lambda, the instance being read. */
static struct cfunc *lambda_op(struct clambda *L)
{
    for (struct cfunc *f = cx_curfn; f; f = f->lambda ? f->lambda->outer
                                                       : NULL)
        if (f->lambda == L)
            return f;
    return L->call;
}

/* capture c's member, as an expression in L's operator() */
static struct cexpr *capture_use(struct clambda *L, struct ccapture *c)
{
    struct cexpr *self = ex_new(E_THIS, lambda_op(L)->this_var->type,
                                VC_PRVALUE);
    return ex_member(ex_deref(self), c->field);
}

static struct cexpr *capture_var_in(struct clambda *L, struct cvar *v,
                                    const struct ctok *at);

/* v's value where L is made: the enclosing function's own local, or its
 * own capture of it (an enclosing lambda). */
static struct cexpr *value_outside(struct clambda *L, struct cvar *v,
                                   const struct ctok *at)
{
    if (!L->outer || v->fn == L->outer || !L->parent)
        return expr_var(v);
    return capture_var_in(L->parent, v, at);
}

/* L's capture of v: found, or made (implicitly, by the capture-default) */
static struct ccapture *var_capture(struct clambda *L, struct cvar *v,
                                    const struct ctok *at)
{
    for (int i = 0; i < L->ncaps; i++)
        if (L->caps[i].var == v)
            return &L->caps[i];
    if (!L->dflt) {
        if (v->has_const)
            return NULL;          /* its value: not an odr-use (6.3) */
        cx_error(at, "'%s' is not captured by the lambda", v->name);
        return NULL;
    }
    if (L->closed)
        cx_error(at, "'%s' is captured too late (the closure is complete)",
                 v->name);
    int byref = L->dflt == '&';
    struct ccapture *c = new_capture(L);
    c->name = v->name;
    c->var = v;
    c->byref = byref;
    c->value = value_outside(L, v, at);
    /* as the name is where L is made: an enclosing lambda's copy is
     * const there (its operator() is) */
    struct cty *t = ct_strip_ref(c->value->t);
    capture_field(L, c, byref ? ct_ref(t, 0) : ct_unqual(t), v->name);
    return c;
}

static struct cexpr *capture_var_in(struct clambda *L, struct cvar *v,
                                    const struct ctok *at)
{
    struct ccapture *c = var_capture(L, v, at);
    if (!c)       /* a constant (constexpr int n = ...; [] { n; }) */
        return ex_int(v->const_val, ct_unqual(v->type));
    return capture_use(L, c);
}

/* L's capture of member fl of enclosing lambda Lk's closure (an
 * init-capture's, or an explicit capture's of Lk's own capture): found,
 * or made by the capture-default */
static struct ccapture *member_capture(struct clambda *L, struct clambda *Lk,
                                       struct cfield *fl,
                                       const struct ctok *at)
{
    for (int i = 0; i < L->ncaps; i++)
        if (L->caps[i].of == fl || L->caps[i].field == fl)
            return &L->caps[i];
    if (!L->dflt) {
        cx_error(at, "'%s' is not captured by the lambda", fl->name);
        return NULL;
    }
    if (L->closed)
        cx_error(at, "'%s' is captured too late (the closure is complete)",
                 fl->name);
    if (!L->parent)
        cx_error(at, "'%s' is not in an enclosing lambda", fl->name);
    struct cexpr *value;
    if (L->parent == Lk) {
        struct ccapture *k = NULL;
        for (int i = 0; i < Lk->ncaps; i++)
            if (Lk->caps[i].field == fl)
                k = &Lk->caps[i];
        value = capture_use(Lk, k);
    } else {
        value = capture_use(L->parent, member_capture(L->parent, Lk, fl, at));
    }
    int byref = L->dflt == '&';
    struct ccapture *c = new_capture(L);
    c->name = fl->name;
    c->of = fl;
    c->byref = byref;
    c->value = value;
    struct cty *t = ct_strip_ref(value->t);
    capture_field(L, c, byref ? ct_ref(t, 0) : ct_unqual(t), fl->name);
    return c;
}

/* member fl of enclosing lambda Lk's closure, seen by the lambda being
 * read or made (L): what it captured, captured by L */
static struct ccapture *enclosing_capture(struct clambda *L, struct clambda *Lk,
                                          struct cfield *fl,
                                          const struct ctok *at)
{
    for (int i = 0; i < Lk->ncaps; i++)
        if (Lk->caps[i].field == fl) {
            if (Lk->caps[i].var)
                return var_capture(L, Lk->caps[i].var, at);
            if (Lk->caps[i].is_this)
                return NULL;
            return member_capture(L, Lk, fl, at);
        }
    return NULL;
}

struct cexpr *lambda_capture_member(struct clambda *Lk, struct cfield *fl,
                                    const struct ctok *at)
{
    if (!cx_curfn || !cx_curfn->lambda || cx_curfn->lambda == Lk)
        return NULL;
    struct clambda *L = cx_curfn->lambda;
    struct clambda *up = L->parent;
    while (up && up != Lk)
        up = up->parent;
    if (!up)
        return NULL;
    struct ccapture *c = enclosing_capture(L, Lk, fl, at);
    return c ? capture_use(L, c) : NULL;
}

struct cexpr *lambda_capture(struct cvar *v, const struct ctok *at)
{
    if (!cx_curfn || !cx_curfn->lambda)
        return NULL;
    return capture_var_in(cx_curfn->lambda, v, at);
}

static struct cexpr *this_in(struct clambda *L, const struct ctok *at);

/* the enclosing object's `this` where L is made */
static struct cexpr *this_outside(struct clambda *L, const struct ctok *at)
{
    if (L->outer && L->outer->lambda)
        return this_in(L->outer->lambda, at);
    if (!L->outer || !L->outer->this_var)
        return NULL;
    return ex_new(E_THIS, L->outer->this_var->type, VC_PRVALUE);
}

/* L's capture of `this`: found, or made (the capture-default); NULL if
 * there is no `this` to capture */
static struct ccapture *this_capture(struct clambda *L, const struct ctok *at)
{
    for (int i = 0; i < L->ncaps; i++)
        if (L->caps[i].is_this)
            return &L->caps[i];
    if (!L->dflt)
        cx_error(at, "'this' is not captured by the lambda");
    struct cexpr *v = this_outside(L, at);
    if (!v)
        return NULL;
    if (L->closed)
        cx_error(at, "'this' is captured too late (the closure is "
                     "complete)");
    struct ccapture *c = new_capture(L);
    c->is_this = 1;
    c->value = v;
    capture_field(L, c, v->t, "__cx_this");
    return c;
}

static struct cexpr *this_in(struct clambda *L, const struct ctok *at)
{
    struct ccapture *c = this_capture(L, at);
    if (c && c->self_copy)              /* [*this]: the copy's address */
        return ex_addr(capture_use(L, c));
    return c ? rvalue(capture_use(L, c)) : NULL;
}

struct cexpr *lambda_this(const struct ctok *at)
{
    if (!cx_curfn || !cx_curfn->lambda)
        return NULL;
    return this_in(cx_curfn->lambda, at);
}

/* Can L capture `this` (quietly: without errors)? */
static int this_reachable(struct clambda *L)
{
    if (!L->outer)
        return 0;
    struct clambda *P = L->outer->lambda;
    if (!P)
        return L->outer->this_var != NULL;
    for (int i = 0; i < P->ncaps; i++)
        if (P->caps[i].is_this)
            return 1;
    return P->dflt && !P->closed && this_reachable(P);
}

/* A generic lambda's body is read only when its operator() is
 * instantiated — after the closure is complete — so what a
 * capture-default captures is decided before, from the body's tokens:
 * each enclosing function's local its names find (where the lambda is),
 * and `this` if it is named or a member is. (A name the body declares
 * again can capture what it hides — harmless: a copy, or a reference,
 * never used.) */
static void foresee_captures(struct clambda *L, int body)
{
    int want_this = 0;
    int end = body;
    int depth = 0;
    do {
        enum tok_kind k = cx_toks[end].t.kind;
        if (k == TOK_LBRACE || k == TOK_LPAREN || k == TOK_LBRACKET)
            depth++;
        else if (k == TOK_RBRACE || k == TOK_RPAREN || k == TOK_RBRACKET)
            depth--;
        else if (k == TOK_EOF)
            break;
        end++;
    } while (depth > 0);
    for (int i = body + 1; i < end; i++) {
        const struct ctok *tk = &cx_toks[i];
        if (tk->t.kind == TOK_CX_THIS) {
            want_this = 1;
            continue;
        }
        if (tk->t.kind != TOK_IDENT)
            continue;
        enum tok_kind before = cx_toks[i - 1].t.kind;
        if (before == TOK_DOT || before == TOK_ARROW ||
            before == TOK_COLONCOLON || cx_toks[i + 1].t.kind == TOK_COLONCOLON)
            continue;
        struct csym *y = lookup(cx_scope, tk->t.text);
        if (!y)
            continue;
        if (y->k == CS_FIELD && y->scope->cls->closure) {
            enclosing_capture(L, y->scope->cls->closure, y->field, tk);
            continue;
        }
        if (y->k == CS_FIELD ||
            (y->k == CS_FUNC && y->scope->k == SC_CLASS &&
             !y->fns->is_static)) {
            want_this = 1;
            continue;
        }
        if (y->k != CS_VAR)
            continue;
        struct cvar *v = y->var;
        if (!v->is_local || v->is_static || !v->fn)
            continue;
        int named = 0;           /* an init-capture of that name */
        for (int j = 0; j < L->ncaps; j++)
            if (!L->caps[j].var && L->caps[j].name &&
                strcmp(L->caps[j].name, v->name) == 0)
                named = 1;
        if (!named)
            var_capture(L, v, tk);
    }
    if (want_this && this_reachable(L))
        this_capture(L, cx_cur());
}

/* A generic lambda's `auto` parameters (the one at lparen): each an
 * invented template type parameter, which the tokens then name
 * (__cx_autoN) — rewritten once, so a lambda read again (in each of a
 * template's instances) finds them so named. Their number; *out: the
 * template's parameters (auto... a pack). */
static int invent_auto_params(int lparen, struct ctparam **out)
{
    int n = 0, cap = 0;
    struct ctparam *ps = NULL;
    int depth = 0, in_default = 0, first = 0;   /* first: this param's */
    for (int i = lparen + 1; i < cx_ntoks; i++) {
        struct ctok *tk = &cx_toks[i];
        enum tok_kind k = tk->t.kind;
        if (k == TOK_EOF)
            break;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            depth++;
            continue;
        }
        if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE) {
            if (depth-- == 0)
                break;
            continue;
        }
        if (depth)
            continue;
        if (k == TOK_COMMA) {
            in_default = 0;
            first = n;
        } else if (k == TOK_ASSIGN) {
            in_default = 1;
        } else if (k == TOK_ELLIPSIS && !in_default) {
            for (int j = first; j < n; j++)
                ps[j].pack = 1;
        } else if (!in_default &&
                   (k == TOK_CX_AUTO ||
                    (k == TOK_IDENT &&
                     strncmp(tk->t.text, "__cx_auto", 9) == 0))) {
            if (n == cap) {
                cap = cap ? cap * 2 : 4;
                ps = xrealloc(ps, (size_t)cap * sizeof *ps);
            }
            memset(&ps[n], 0, sizeof ps[n]);
            ps[n].kind = TP_TYPE;
            ps[n].name = cx_fmt("__cx_auto%d", n);
            ps[n].def_tok = -1;
            ps[n].vtype_tok = -1;
            tk->t.kind = TOK_IDENT;
            tk->t.text = (char *)ps[n].name;
            n++;
        }
    }
    *out = ps;
    return n;
}

/* After a lambda's parameters: its specifiers (mutable, constexpr,
 * attributes) and trailing return type (auto if none), into ft. */
static void lambda_declarator_rest(struct cty *ft)
{
    int is_mutable = 0;
    for (;;) {
        if (cx_kind() == TOK_CX_MUTABLE) {
            is_mutable = 1;
            cx_advance();
        } else if (cx_kind() == TOK_CX_CONSTEXPR ||
                   cx_kind() == TOK_CX_CONSTEVAL) {
            cx_advance();
        } else if (cx_kind() == TOK_KW_ATTRIBUTE ||
                   (cx_kind() == TOK_LBRACKET &&
                    cx_kind_at(1) == TOK_LBRACKET)) {
            parse_attrs(NULL);
        } else {
            break;
        }
    }
    struct cty *ret = ct_basic(CT_AUTO);
    if (cx_accept(TOK_ARROW))
        ret = trailing_return_type(ft);
    ft->to = ret;
    ft->fq = is_mutable ? 0 : CQ_CONST;
}

/* A generic lambda's operator() instance: its declaration (at the
 * parameters) read with the invented parameters bound, the body kept for
 * when it is used. */
static struct cfunc *lambda_call_replay(struct ctemplate *t)
{
    struct cty *ft = parse_params();
    lambda_declarator_rest(ft);
    struct cfunc *pat = t->pattern;
    struct cfunc *f = xcalloc(1, sizeof *f);
    f->name = pat->name;
    f->type = ft;
    f->pnames = ft->pnames;
    f->owner = pat->owner;
    f->cls = pat->cls;
    f->is_inline = 1;
    f->access = pat->access;
    f->vslot = -1;
    f->line = pat->line;
    f->file = pat->file;
    f->lambda = t->lambda;
    f->body_tok = f->mi_tok = -1;
    if (ft->defargs) {
        f->defargs = xcalloc((size_t)(ft->np ? ft->np : 1),
                             sizeof *f->defargs);
        for (int i = 0; i < ft->np; i++)
            f->defargs[i] = ft->defargs[i];
    }
    if (t->decl0_tok)
        decl0_defaults(t, f);
    skip_body(f);
    f->lazy = 1;
    f->def_scope = cx_scope;
    func_register(f);
    return f;
}

/* A function made here rather than read: f's parameters (as its type's
 * names say, or invented) and body. */
static void synth_define(struct cfunc *f, struct cstmt *body)
{
    struct cfunc *savefn = cx_curfn;
    cx_curfn = f;
    f->defined = 1;
    int np = f->type->np;
    f->params = xcalloc((size_t)(np ? np : 1), sizeof *f->params);
    for (int i = 0; i < np; i++) {
        struct cvar *v = new_local(cx_fmt("__cx_arg%d", i),
                                   f->type->params[i], NULL);
        v->is_param = 1;
        f->params[i] = v;
    }
    if (f->cls && !f->is_static) {
        struct cvar *tv = new_local("this", NULL, NULL);
        tv->cname = "this";
        tv->type = ct_ptr(ct_qual(ct_class(f->cls), f->type->fq));
        tv->is_param = 1;
        f->this_var = tv;
    }
    f->body = body ? body : st_new(S_BLOCK);
    cx_curfn = savefn;
}

/* A lambda that captures nothing converts to a pointer to a function:
 * a static member calling operator() on an (empty) closure, whose address
 * a conversion function gives. */
static void lambda_to_pointer(struct clambda *L, const struct ctok *at)
{
    struct cclass *c = L->cls;
    struct cfunc *call = L->call;
    struct cty *ft = ct_func(call->type->to, call->type->params,
                             call->type->np, call->type->variadic);
    struct dspec ds;
    memset(&ds, 0, sizeof ds);
    ds.storage = SK_STATIC;
    ds.is_inline = 1;
    struct declarator d;
    memset(&d, 0, sizeof d);
    d.kind = DN_IDENT;
    d.name = "__cx_invoke";
    d.at = at;
    struct cfunc *inv = declare_function(&ds, &d, ft, c->scope);
    synth_define(inv, NULL);
    struct cfunc *savefn = cx_curfn;
    cx_curfn = inv;
    struct cexpr **args = xcalloc((size_t)(ft->np ? ft->np : 1),
                                  sizeof *args);
    for (int i = 0; i < ft->np; i++)
        args[i] = expr_var(inv->params[i]);
    struct cexpr *none = ex_new(E_INITLIST, NULL, VC_PRVALUE);
    struct cexpr *obj = ex_materialize(init_aggregate(ct_class(c), none, at));
    struct cexpr *r = make_call(call, ex_addr(obj), args, ft->np, at);
    struct cstmt *st;
    if (ft->to->k == CT_VOID) {
        st = st_new(S_EXPR);
        st->e = r;
    } else {
        st = st_new(S_RETURN);
        st->e = ct_is_ref(ft->to) ? bind_ref(r, ft->to, "return")
                                  : init_object(ft->to, INIT_COPY, &r, 1, at);
    }
    inv->body->body = st;
    cx_curfn = savefn;

    struct cty *fpt = ct_ptr(ft);
    struct cty *cft = ct_func(fpt, NULL, 0, 0);
    cft->fq = CQ_CONST;
    memset(&ds, 0, sizeof ds);
    ds.is_inline = 1;
    memset(&d, 0, sizeof d);
    d.kind = DN_CONV;
    d.conv_type = fpt;
    d.name = cx_fmt("operator %s", ct_name(fpt));
    d.at = at;
    struct cfunc *cv = declare_function(&ds, &d, cft, c->scope);
    synth_define(cv, NULL);
    struct cstmt *ret = st_new(S_RETURN);
    struct cexpr *addr = ex_new(E_FUNC, fpt, VC_PRVALUE);
    addr->fn = inv;
    ret->e = addr;
    cv->body->body = ret;
}

/* [captures](params) specifiers -> ret { body }: a closure object — of
 * a class of its own whose operator() the body defines and whose members
 * are the captures (8.1.5). */
struct cexpr *parse_lambda(void)
{
    const struct ctok *at = cx_cur();
    struct clambda *L = xcalloc(1, sizeof *L);
    L->at = at;
    L->outer = cx_curfn;
    L->parent = cx_curfn ? cx_curfn->lambda : NULL;
    struct cclass *c = L->cls = class_new(NULL, cx_scope);
    c->closure = L;
    c->is_struct = 1;
    c->defining = 1;
    class_register(c);
    /* the captures */
    cx_expect(TOK_LBRACKET, "'['");
    while (cx_kind() != TOK_RBRACKET) {
        const struct ctok *cat = cx_cur();
        if (cx_kind() == TOK_ASSIGN && (cx_kind_at(1) == TOK_COMMA ||
                                        cx_kind_at(1) == TOK_RBRACKET)) {
            L->dflt = '=';
            cx_advance();
        } else if (cx_kind() == TOK_AMP && (cx_kind_at(1) == TOK_COMMA ||
                                            cx_kind_at(1) == TOK_RBRACKET)) {
            L->dflt = '&';
            cx_advance();
        } else if (cx_kind() == TOK_CX_THIS) {
            cx_advance();
            struct cexpr *v = this_outside(L, cat);
            if (!v)
                cx_error(cat, "'this' captured outside a member function");
            struct ccapture *k = new_capture(L);
            k->is_this = 1;
            k->value = v;
            capture_field(L, k, v->t, "__cx_this");
        } else if (cx_kind() == TOK_STAR && cx_kind_at(1) == TOK_CX_THIS) {
            /* [*this]: a copy of the object, which `this` then points at */
            cx_advance();
            cx_advance();
            struct cexpr *v = this_outside(L, cat);
            if (!v)
                cx_error(cat, "'*this' captured outside a member function");
            struct ccapture *k = new_capture(L);
            k->is_this = 1;
            k->self_copy = 1;
            k->value = ex_deref(v);
            capture_field(L, k, ct_unqual(v->t->to), "__cx_self");
        } else {
            int byref = cx_accept(TOK_AMP);
            if (cx_kind() != TOK_IDENT)
                cx_error(cx_cur(), "expected a capture");
            const char *name = cx_cur()->t.text;
            cx_advance();
            struct ccapture *k = new_capture(L);
            k->name = name;
            k->byref = byref;
            if (cx_kind() == TOK_ASSIGN || cx_kind() == TOK_LBRACE ||
                cx_kind() == TOK_LPAREN) {
                /* an init-capture: a new variable, of the initializer's
                 * type (or a reference to it) */
                struct cexpr *e;
                if (cx_accept(TOK_ASSIGN)) {
                    e = expr_parse_assign();
                } else if (cx_kind() == TOK_LBRACE) {
                    e = parse_braced_list();
                    if (e->na != 1)
                        cx_error(cat, "an init-capture takes one value");
                    e = e->a[0];
                } else {
                    cx_advance();
                    e = expr_parse_assign();
                    cx_expect(TOK_RPAREN, "')'");
                }
                k->value = e;
                capture_field(L, k, byref ? ct_ref(ct_strip_ref(e->t), 0)
                                          : ct_unqual(ct_decay(e->t)),
                              name);
            } else {
                struct csym *y = lookup(cx_scope, name);
                if (!y || (y->k != CS_VAR && y->k != CS_FIELD))
                    cx_error(cat, "'%s' is not a variable to capture", name);
                /* its value here: the variable, or an enclosing lambda's
                 * capture of it */
                k->value = expr_parse_name_value(y, name, cat);
                k->var = y->k == CS_VAR ? y->var : NULL;
                struct cty *t = k->value->t;
                capture_field(L, k, byref ? ct_ref(ct_strip_ref(t), 0)
                                          : ct_unqual(t), name);
            }
        }
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RBRACKET, "']' to close the captures");
    /* the declarator: [<template params>] (params), specifiers, -> ret */
    struct cty *ft;
    struct ctparam *tps = NULL, *eps = NULL;
    int neps = 0;
    struct cscope *tscope = NULL;
    if (cx_kind() == TOK_LT) {
        /* []<class T>(T x) { }: C++20, explicit template parameters */
        tscope = scope_push(SC_TEMPLATE, NULL);
        cx_advance();
        neps = parse_tparams(&eps);
        if (cx_accept(TOK_CX_REQUIRES))
            skip_constraint(0);
        scope_pop();
    }
    int lparen = cx_kind() == TOK_LPAREN ? cx_pos : -1;
    struct ctparam *autos = NULL;
    int nautos = lparen >= 0 ? invent_auto_params(lparen, &autos) : 0;
    int ntps = neps + nautos;
    if (ntps) {
        tps = xcalloc((size_t)ntps, sizeof *tps);
        for (int i = 0; i < neps; i++)
            tps[i] = eps[i];
        for (int i = 0; i < nautos; i++)
            tps[neps + i] = autos[i];
    }
    if (ntps) {
        /* generic: operator() is a member template, whose pattern the
         * declarator is read as */
        if (tscope) {
            tscope->parent = cx_scope;
            cx_scope = tscope;
        } else {
            scope_push(SC_TEMPLATE, NULL);
        }
        for (int i = neps; i < ntps; i++) {
            struct csym *y = scope_add(cx_scope, CS_TYPEDEF, tps[i].name);
            y->type = ct_tparam(i, tps[i].name);
            y->pack_param = tps[i].pack;
        }
        cx_pattern++;
        ft = parse_params();
        lambda_declarator_rest(ft);
        cx_pattern--;
        scope_pop();
    } else if (lparen >= 0) {
        ft = parse_params();
        lambda_declarator_rest(ft);
    } else {
        ft = ct_func(NULL, NULL, 0, 0);
        ft->pdecl = NULL;
        lambda_declarator_rest(ft);
    }
    if (cx_kind() != TOK_LBRACE)
        cx_error(cx_cur(), "expected a lambda's body");
    if (ntps) {
        struct ctemplate *tm = template_new(TK_FUNC, "operator()", c->scope,
                                            tps, ntps);
        tm->decl_tok = lparen;
        tm->member_of = c;
        tm->has_body = 1;
        tm->lambda = L;
        struct cfunc *f = xcalloc(1, sizeof *f);
        f->name = "operator()";
        f->type = ft;
        f->tmpl = tm;
        f->owner = c->scope;
        f->cls = c;
        f->access = CA_PUBLIC;
        f->vslot = -1;
        f->body_tok = f->mi_tok = -1;
        f->line = at->t.line;
        f->file = at->file;
        f->lambda = L;
        tm->pattern = f;
        L->call = f;
        struct csym *y = func_sym(c->scope, "operator()", at);
        struct cfunc **set = &y->fns;
        while (*set)
            set = &(*set)->next;
        *set = f;
        if (L->dflt)
            foresee_captures(L, cx_pos);
        cx_skip_balanced();
        c->defining = 0;
        class_complete(c);
        L->closed = 1;
    } else {
        /* operator(), defined by the body, read now in the closure's
         * scope (which sits in the enclosing one) */
        struct dspec ds;
        memset(&ds, 0, sizeof ds);
        ds.is_inline = 1;
        struct declarator d;
        memset(&d, 0, sizeof d);
        d.kind = DN_OPERATOR;
        d.name = "operator()";
        d.at = at;
        struct cfunc *f = declare_function(&ds, &d, ft, c->scope);
        f->lambda = L;
        L->call = f;
        struct cscope *saved = cx_scope;
        cx_scope = c->scope;
        f->def_scope = cx_scope;
        define_function(f, ft);
        cx_scope = saved;
        c->defining = 0;
        class_complete(c);
        L->closed = 1;
    }
    if (!L->ncaps && !L->dflt && !ntps)
        lambda_to_pointer(L, at);
    /* the closure object: each member from its capture's value */
    struct cexpr *list = ex_new(E_INITLIST, NULL, VC_PRVALUE);
    list->na = c->nfields;
    list->a = xcalloc((size_t)(c->nfields ? c->nfields : 1), sizeof *list->a);
    for (int i = 0; i < L->ncaps; i++)
        for (int k = 0; k < c->nfields; k++)
            if (c->fields[k] == L->caps[i].field)
                list->a[k] = L->caps[i].value;
    return init_aggregate(ct_class(c), list, at);
}
