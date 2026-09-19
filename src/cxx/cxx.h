/* The C++ front-end's internal interface (D-013; docs/CXX.md).
 *
 * src/cxx parses preprocessed C++ with semantic analysis interleaved — C++
 * cannot be parsed without knowing which names are types — and lowers it to
 * C text, which EmbCC's C front-end then compiles for either target:
 *
 *   tokens (tok.c) -> declarations and statements (parse.c), expressions
 *   (expr.c), classes (class.c) over types (type.c) and scopes (scope.c)
 *   -> typed trees -> C (emit.c), names mangled per the Itanium ABI
 *   (mangle.c)
 *
 * Its types are its own: C++ keeps apart what C does not (char / signed
 * char / unsigned char, long / long long, references, bool, nullptr_t,
 * classes with members and methods), and only emit.c turns them into C.
 */
#ifndef EMBCC_CXX_CXX_H
#define EMBCC_CXX_CXX_H

#include "../lex/lex.h"
#include "translate.h"

/* ---- tokens ---- */

struct ctok {
    struct token t;           /* the lexer's token (kind, text, number, ...) */
    const char *file;         /* the source file it came from (line markers) */
};

extern struct ctok *cx_toks;  /* the whole preprocessed unit */
extern int cx_ntoks;
extern int cx_pos;            /* the parser's cursor */

void cx_tokenize(const char *file, const char *src);
static inline struct ctok *cx_cur(void) { return &cx_toks[cx_pos]; }
static inline struct ctok *cx_peek(int k)
{
    int i = cx_pos + k;
    return &cx_toks[i < cx_ntoks ? i : cx_ntoks - 1];
}
extern int cx_half_gt;         /* a `>>` whose first `>` was consumed */
static inline enum tok_kind cx_kind(void)
{
    return cx_half_gt ? TOK_GT : cx_toks[cx_pos].t.kind;
}
static inline enum tok_kind cx_kind_at(int k)
{
    return k == 0 ? cx_kind() : cx_peek(k)->t.kind;
}
void cx_advance(void);
int cx_accept(enum tok_kind k);
void cx_expect(enum tok_kind k, const char *what);
int cx_is_ident(const char *name);   /* current token is this identifier */
void cx_skip_balanced(void);         /* past a ( [ { group and its close */

/* diagnostics at a token (exit) */
void cx_error(const struct ctok *at, const char *fmt, ...);
void cx_warn(const struct ctok *at, const char *fmt, ...);

/* ---- types ---- */

enum cty_kind {
    CT_VOID, CT_BOOL,
    CT_CHAR, CT_SCHAR, CT_UCHAR, CT_WCHAR, CT_CHAR8, CT_CHAR16, CT_CHAR32,
    CT_SHORT, CT_USHORT, CT_INT, CT_UINT, CT_LONG, CT_ULONG,
    CT_LLONG, CT_ULLONG,
    CT_FLOAT, CT_DOUBLE, CT_LDOUBLE,
    CT_NULLPTR, CT_VALIST,
    CT_PTR, CT_LREF, CT_RREF, CT_ARRAY, CT_FUNC, CT_CLASS, CT_ENUM,
    CT_MPTR,                  /* a pointer to a member of `cls`, of type `to`
                               * (a CT_FUNC for a member function) */
    CT_TPARAM,                /* a template's type parameter (n: its index),
                               * in a pattern being read for deduction */
    CT_TID,                   /* a template-id with dependent arguments:
                               * tmpl<targs> (a pattern) */
    CT_DEP,                   /* some other dependent type: `typename
                               * to::names` (to may be NULL: unknown) */
    CT_AUTO                   /* `auto`, until its initializer deduces it */
};

enum { CQ_CONST = 1, CQ_VOLATILE = 2 };

struct cclass;
struct cenum;
struct ctemplate;
struct ctarg;

struct cty {
    enum cty_kind k;
    unsigned q;               /* CQ_* on this type */
    struct cty *to;           /* PTR/REF/ARRAY: element; FUNC: return type */
    long n;                   /* ARRAY: element count, -1 when omitted */
    struct cty **params;      /* FUNC */
    int np;
    int variadic;             /* FUNC: `...` */
    unsigned fq;              /* FUNC: a member function's cv (CQ_*) */
    int refq;                 /* FUNC: a member function's & (1) or && (2) */
    int nothrow;              /* FUNC: noexcept (or throw()) */
    struct cclass *cls;       /* CLASS */
    struct cenum *en;         /* ENUM */
    /* FUNC, as a declarator wrote it: the parameters' declared types (top-
     * level cv kept, which params[] drops), names and default arguments */
    struct cty **pdecl;
    const char **pnames;
    struct cexpr **defargs;
    int *defarg_toks;         /* FUNC in a template pattern: default
                               * arguments as tokens, parsed per use */
    struct cpgroup *pgroups;  /* FUNC in an instance: the parameters
                               * expanded function parameter packs gave */
    int npgroups;
    /* CT_TPARAM: its name; CT_TID: the template and its arguments; CT_DEP:
     * the nested names after `to` */
    const char *tpname;
    struct ctemplate *tmpl;
    struct ctarg *targs;
    int ntargs;
    const char **dnames;
    int ndnames;
    int dauto;                /* CT_AUTO: decltype(auto) */
    struct cty *mclass;       /* CT_MPTR in a pattern: its class, a
                               * dependent type (cls then NULL) */
    const char *dexpr;        /* CT_DEP: decltype(e) of a dependent e — e's
                               * mangling (NULL: not known) */
    int treq, treq_end;       /* FUNC: a trailing requires-clause's tokens
                               * (0: none) */
    int pack_expansion;       /* CT_TPARAM etc.: `T...` in a pattern */
    int bparam;               /* ARRAY in a pattern with n -2: the value
                               * parameter its bound is */
};

struct cty *ct_basic(enum cty_kind k);
struct cty *ct_qual(struct cty *t, unsigned q);     /* t with q added */
struct cty *ct_unqual(struct cty *t);
struct cty *ct_ptr(struct cty *to);
struct cty *ct_ref(struct cty *to, int rvalue);
struct cty *ct_array(struct cty *elem, long n);
struct cty *ct_func(struct cty *ret, struct cty **params, int np, int variadic);
struct cty *ct_class(struct cclass *c);
struct cty *ct_enum(struct cenum *e);
struct cty *ct_mptr(struct cclass *c, struct cty *member);
int ct_is_pmf(const struct cty *t);          /* a pointer to member function */
struct cty *ct_size_t(void);                 /* unsigned long */
struct cty *ct_ptrdiff_t(void);              /* long */

int ct_same(const struct cty *a, const struct cty *b);          /* incl. quals */
int targ_same(const struct ctarg *a, const struct ctarg *b);
int ct_same_unqual(const struct cty *a, const struct cty *b);
int ct_is_integer(const struct cty *t);     /* incl. bool, chars, enums */
int ct_is_arith(const struct cty *t);
int ct_is_float(const struct cty *t);
int ct_is_scalar(const struct cty *t);
int ct_is_ref(const struct cty *t);
int ct_is_signed(const struct cty *t);
int ct_is_void(const struct cty *t);
int ct_is_complete(const struct cty *t);
long ct_size(const struct cty *t);
long ct_align(const struct cty *t);
struct cty *ct_promote(struct cty *t);      /* integral promotion */
struct cty *ct_arith_common(struct cty *a, struct cty *b);
struct cty *ct_decay(struct cty *t);        /* array -> ptr, func -> ptr */
struct cty *ct_strip_ref(struct cty *t);
int ct_has_auto(const struct cty *t);
/* std::initializer_list: the template (t is it?), and the element type of
 * an instance t (through references and cv; NULL if t is not one) */
int is_std_il(const struct ctemplate *t);
struct cty *ct_il_elem(struct cty *t);
/* ... or a pattern's parameter naming it (initializer_list<T>) */
int ct_il_param(struct cty *t);       /* auto, auto&, auto *... */
struct ctarg;
int ct_is_local(const struct cty *t);        /* a class with no linkage */
int targs_local(const struct ctarg *a, int n);
const char *ct_name(const struct cty *t);   /* for diagnostics */

/* ---- symbols and scopes ---- */

enum csym_kind {
    CS_VAR, CS_FUNC, CS_TYPEDEF, CS_CLASS, CS_ENUM, CS_ENUMERATOR,
    CS_NAMESPACE, CS_FIELD,
    CS_TEMPLATE,              /* a class, alias or variable template */
    CS_PACK                   /* a bound parameter pack (template.c) */
};

enum cscope_kind { SC_NAMESPACE, SC_CLASS, SC_ENUM, SC_BLOCK, SC_PARAMS,
                   SC_TEMPLATE /* a template's parameters, bound or not */ };

struct cscope;
struct cfunc;
struct cstmt;
struct cexpr;
struct cfield;

/* A variable: a global, a namespace-scope or class-static variable, a
 * local, or a parameter. `cname` is the C identifier emitted for it. */
struct cvar {
    const char *name;
    const char *cname;
    struct cty *type;         /* as declared (may be a reference) */
    struct cscope *owner;     /* the namespace or class declaring it */
    struct cfunc *fn;         /* a local's function */
    int is_local;             /* a block-scope variable or parameter */
    int is_param;
    int is_static;            /* static storage in a function, or internal
                               * linkage at namespace scope */
    int is_extern;            /* a declaration only */
    int is_member_static;     /* a static data member */
    int is_inline;            /* an inline variable (C++17) */
    int is_constexpr;
    int defined;              /* a namespace-scope definition was seen */
    int c_linkage;            /* extern "C" */
    int weak;                 /* __attribute__((weak)) */
    const char *section;      /* __attribute__((section)) */
    long align_attr;          /* __attribute__((aligned)) / alignas */
    struct cexpr *init;       /* scalar/reference initializer (a reference's
                               * is the pointer to store) */
    struct cexpr *ctor;       /* an object's initialization (construct,
                               * aggregate list), emitted into the variable */
    int nrvo;                 /* the named return value: built in the
                               * caller's return slot (emit.c) */
    int has_const;            /* a const integral with a constant value */
    long const_val;
    struct cvar *sb_var;      /* a structured binding's name: it names
                               * member sb_field of this hidden variable,
                               * or its element sb_index */
    struct cfield *sb_field;
    int sb_index;
    int ce_state;             /* consteval.c: its object evaluated (2), being
                               * (1), only its address known (3) */
    void *ce_obj;
    int used;
    int emitted;
    int refd, declared;       /* emit.c's bookkeeping */
    int is_tparam;            /* a value template parameter in a pattern */
    int fparam;               /* a function parameter seen in its
                               * declaration (a trailing return type): its
                               * index + 1 */
    int tparam_index;
    struct ctarg *targs;      /* a variable template's instance: its args */
    int ntargs;
    int disc;                 /* a local static's number among its
                               * function's same-named ones */
    int line;
    const char *file;
};

/* A function parameter pack as expanded: parameters first..first+n-1. */
struct cpgroup {
    const char *name;
    int first, n;
};

struct csym {
    enum csym_kind k;
    const char *name;
    struct cscope *scope;     /* where it is declared */
    struct cty *type;         /* TYPEDEF/CLASS/ENUM: the type; ENUMERATOR: its
                               * enum type */
    struct cvar *var;         /* VAR */
    struct cfunc *fns;        /* FUNC: the overload set (cfunc.next) */
    struct cscope *ns;        /* NAMESPACE: its scope */
    struct cfield *field;     /* FIELD: a non-static data member */
    struct cclass *fcls;      /* FIELD brought in by a using-declaration:
                               * the class declaring it */
    struct cfield **fpath;    /* FIELD of an anonymous union or struct
                               * member: those members, outermost first,
                               * the way to it */
    int nfpath;
    struct ctemplate *tmpl;   /* TEMPLATE */
    struct ctarg *pack;       /* PACK: its elements */
    int npack;
    struct cvar **pvars;      /* PACK of function parameters: the variables */
    int pack_param;           /* in a pattern: a template parameter pack */
    long value;               /* ENUMERATOR */
    int access;               /* in a class: CA_* */
    struct csym *next;        /* in the scope's list */
    struct csym *hnext;       /* in the lookup hash chain */
};

enum { CA_PUBLIC, CA_PROTECTED, CA_PRIVATE };

struct cscope {
    enum cscope_kind k;
    const char *name;         /* a namespace's or class's name */
    struct cscope *parent;
    struct csym *syms;        /* declaration order (most recent first) */
    struct cclass *cls;       /* SC_CLASS */
    struct cenum *en;         /* SC_ENUM */
    struct cfunc *fn;         /* SC_BLOCK/SC_PARAMS inside a function */
    struct cscope **usings;   /* using-directives: namespaces searched too */
    int nusings;
    int anon;                 /* an unnamed namespace */
    int is_inline;            /* an inline namespace */
    int nunnamed;             /* unnamed classes and enums declared here */
};

extern struct cscope *cx_global;   /* the global namespace */
extern struct cscope *cx_scope;    /* where the parser is */

void scope_init(void);
struct cscope *scope_new(enum cscope_kind k, const char *name,
                         struct cscope *parent);
struct cscope *scope_push(enum cscope_kind k, const char *name);
void scope_pop(void);
struct csym *scope_add(struct cscope *s, enum csym_kind k, const char *name);
struct csym *scope_find_here(struct cscope *s, const char *name);
/* The class or enum named `name` declared in s (hidden or not). */
struct csym *scope_find_tag(struct cscope *s, const char *name);
/* Unqualified lookup from `from` outward (through using-directives). */
struct csym *lookup(struct cscope *from, const char *name);
/* ... a parameter pack staying one (lookup gives the element being
 * expanded) */
struct csym *lookup_raw(struct cscope *from, const char *name);
/* ... only classes and enums (an elaborated type specifier) */
struct csym *lookup_tag(struct cscope *from, const char *name);
/* Qualified lookup: name as a member of namespace or class scope `in`. */
struct csym *lookup_in(struct cscope *in, const char *name);
struct cscope *enclosing_ns(struct cscope *s);
struct cclass *enclosing_class(struct cscope *s);

/* ---- templates (template.c) ---- */

enum { TP_TYPE, TP_VALUE, TP_TEMPLATE };

/* A template parameter. */
struct ctparam {
    int kind;                 /* TP_* */
    const char *name;
    struct ctemplate *tc;     /* a type-constraint: `C T` or `C<A> T` */
    int tc_args;              /* ... C's written arguments (their `<`), or
                               * -1 */
    int pack;                 /* a parameter pack: ...name */
    struct cty *vtype;        /* TP_VALUE: its type (a pattern) */
    int vtype_tok;            /* ... as tokens, for a dependent one */
    int def_tok;              /* its default argument's tokens, or -1 */
};

/* A template argument (or a pack of them). */
struct ctarg {
    int kind;                 /* TP_* */
    struct cty *type;         /* TP_TYPE */
    long value;               /* TP_VALUE: an integral value */
    struct cty *vtype;        /* TP_VALUE: its type */
    struct ctemplate *tmpl;   /* TP_TEMPLATE */
    int is_pack;
    struct ctarg *elems;      /* a pack's elements */
    int nelems;
    const char *mexpr;        /* TP_VALUE in a pattern, dependent: its
                               * Itanium expression encoding (in X...E) */
    int expansion;            /* in a pattern: `pattern...`, elems[0] the
                               * pattern (it deduces a pack) */
};

enum { TK_CLASS, TK_FUNC, TK_ALIAS, TK_VAR, TK_CONCEPT };

/* A partial specialization of a class template: its own parameters, the
 * argument patterns it matches, and its definition's tokens. */
struct cpartial {
    struct ctparam *params;
    int nparams;
    struct ctarg *pattern;
    int npattern;
    int head_end;             /* the definition: at `:` or `{` */
    enum tok_kind key;
    struct cpartial *next;
    int is_final;             /* its definition says `final` */
    int req_start, req_end;   /* its requires-clause (0: none) */
    int pat_tok;              /* the pattern's `<` (to substitute it) */
};

/* An out-of-class definition of a class template's member:
 * template<class T> R A<T>::f(...) { ... } */
struct coutdef {
    struct ctparam *params;
    int nparams;
    int tok;                  /* the declaration after template<...> */
    const char *member;       /* its (last) name, for the search */
    int class_body;           /* a member class's definition: at its `{`
                               * or `:` (0: a function's or variable's) */
    enum tok_kind key;        /* ... class, struct or union */
    struct coutdef *next;
};
/* A class template instance's member class declared, not defined, in the
 * class: defined from the template's out-of-class definition of it
 * (template<class T> class A<T>::B { ... };), if there is one. */
void member_class_from_outdef(struct cclass *c);

struct cinst {
    struct ctarg *args;
    int nargs;
    struct cclass *cls;       /* a class template's instance */
    struct cfunc *fn;         /* a function template's specialization (NULL
                               * with failed set: substitution failed) */
    int failed;
    struct cvar *var;         /* a variable template's */
    struct cty *type;         /* an alias template's */
    struct cinst *next;
};

struct ctemplate {
    int kind;                 /* TK_* */
    const char *name;
    struct cscope *scope;     /* where declared: the context names in its
                               * definition are looked up in */
    struct ctparam *params;
    int nparams;
    int decl_tok;             /* the declaration, after template<...> */
    int head_end;             /* TK_CLASS: the definition, at `:` or `{`
                               * (-1: declared only, so far) */
    enum tok_kind key;        /* TK_CLASS: class, struct or union */
    struct cfunc *pattern;    /* TK_FUNC: the declaration, its types
                               * patterns (for deduction and mangling) */
    struct cclass *member_of; /* a member template: of this class */
    struct cinst *insts;
    struct cpartial *partials;
    struct coutdef *outdefs;
    int is_extern;            /* `extern template class`: instantiated
                               * elsewhere */
    int has_body;             /* TK_FUNC: its declaration is a definition */
    int tparam;               /* a template template parameter's
                               * placeholder (nparams -1): its index + 1 */
    int tt_pack;              /* ... whose own parameter this (+ 1) is a
                               * pack */
    struct clambda *lambda;   /* a generic lambda's operator(): the lambda
                               * (its declaration is the lambda's) */
    int is_final;             /* TK_CLASS: its definition says `final` */
    int req_start, req_end;   /* its requires-clause's tokens (0: none);
                               * TK_CONCEPT: the constraint, decl_tok on */
    int builtin;              /* the compiler's own: BT_* */
    struct cguide *guides;    /* TK_CLASS: its deduction guides */
    struct cscope *pscope;    /* TK_ALIAS: its parameters (as patterns) */
    struct cty *alias_pat;    /* ... its type in terms of them (deduction
                               * sees through it), once read */
    int alias_pat_done;
};
/* An alias template's type with its own parameters unsubstituted (NULL
 * when it cannot be read so) */
struct cty *alias_pattern(struct ctemplate *t);
/* A deduction guide (13.7.2.3): `template<params> C(P...) -> C<A...>;`,
 * kept as its tokens (at the name) with its template parameters (none
 * for a guide that is not a template), read when C's arguments are
 * deduced from an initializer. */
struct cguide {
    struct ctparam *params;
    int nparams;
    int tok;                  /* the guide's name */
    int is_explicit;
    struct cscope *scope;     /* where it was declared (its parameters') */
    struct cguide *next;
};

/* Builtin templates: __make_integer_seq<TT, T, N> is TT<T, 0, ..., N-1>
 * (Clang's; libstdc++ asks __has_builtin and builds its index sequences
 * with it — the classes, and so the ABI, are the same as g++'s). */
enum { BT_NONE, BT_MAKE_INTEGER_SEQ };

/* Class template instance: the class for these arguments (made, not yet
 * defined: class_ensure defines it when needed). */
struct cclass *class_instance(struct ctemplate *t, struct ctarg *args,
                              int nargs, const struct ctok *at);
/* A class that must be complete here: define it if it is an instance. */
void class_ensure(struct cclass *c);
/* The specialization of function template t for args (deduced or given),
 * its declaration instantiated; NULL when substitution fails (SFINAE). */
struct cfunc *func_instance(struct ctemplate *t, struct ctarg *args,
                            int nargs);
/* Is function template a more specialized than b (for n arguments)? */
int more_specialized(struct ctemplate *a, struct ctemplate *b, int n);
/* Deduce t's arguments from a call's (explicit ones first): 1 on success. */
int deduce_call(struct ctemplate *t, struct ctarg *expl, int nexpl,
                struct cexpr **args, int na, struct ctarg **out, int *nout);
/* A function whose body is tokens not yet parsed (a member of a class
 * template's instance, a function template's specialization): parse it. */
void func_ensure_body(struct cfunc *f);
void func_deduce_return(struct cfunc *f, const struct ctok *at);
/* A class template instance's member defined out of its class: define it
 * from that definition (1 if one was found). */
int member_from_outdef(struct cfunc *f);
void member_var_from_outdef(struct cvar *v);
/* `< args >` after the name of template t (at `<`). */
int parse_template_args(struct ctemplate *t, struct ctarg **out);
/* template<...> declaration, at `template` (ctx: in class c, or NULL). */
void parse_template_decl(struct cclass *c, int access);
/* parse.c's side of instantiation: define class c from the tokens at pos
 * (its bases and body) in the parameter scope ps; replay a function
 * template's declaration; parse a lazy body; define a variable
 * template's instance. */
void class_define_from(struct cclass *c, int pos, enum tok_kind key,
                       struct cscope *ps);
struct cfunc *func_decl_replay(struct ctemplate *t, struct cscope *ps);
void func_define_from(struct cfunc *f);
/* A defaulted comparison operator: is f one, and its definition */
int is_defaultable_cmp(struct cfunc *f);
void define_defaulted_cmp(struct cfunc *f);
struct cvar *var_define_from(struct ctemplate *t, int pos, struct ctarg *args,
                             struct cscope *ps);
struct cscope *tparam_scope(struct ctparam *ps, int np, struct ctarg *args,
                            int na, struct cscope *parent);
struct cty *ct_tparam(int index, const char *name);
/* Does the type mention a template parameter (a pattern)? */
int ct_dependent(const struct cty *t);
/* Parameter packs (template.c): while an expansion's pattern is read for
 * element i, looking up a pack it expands finds that element. */
void pack_push(struct csym *pack, int index);
void pack_pop(int n);
struct csym *pack_current(struct csym *y);   /* the element, or y itself */
int pack_length(struct csym *y);
/* The expansion at the cursor, if the element up to its `,` or closer ends
 * in `...` (angles: `<` `>` nest, in template arguments): the packs its
 * pattern names (at most max) and where the `...` is. */
int expansion_at(int angles, struct csym **packs, int max, int *ellipsis);
int expansion_length(struct csym **packs, int n);
int packs_in(int from, int to, struct csym **packs, int max);
int pack_mark(void);                    /* the expansion stack's depth */
void pack_reset(int mark);
/* An alias or variable template's instance. */
struct cty *alias_instance(struct ctemplate *t, struct ctarg *args, int n,
                           const struct ctok *at);
struct cvar *var_instance(struct ctemplate *t, struct ctarg *args, int n,
                          const struct ctok *at);
/* Parser state that an instantiation, run in the middle of parsing
 * something else, saves and restores. */
struct parse_state;
struct parse_state *parse_save(void);
/* The instantiations under way (template.c), for an error's notes:
 * pushed as each starts (its use at `at`), popped as it ends. */
void cx_inst_push(const char *what, const struct ctok *at);
void cx_inst_pop(void);
int cx_inst_mark(void);
void cx_inst_reset(int mark);
void cx_inst_notes(void);
void parse_restore(struct parse_state *st);
extern int cx_pattern;         /* reading a pattern: dependent types allowed */
extern int cx_unevaluated;     /* inside decltype, sizeof, noexcept or a
                                * requires-expression: calls need no body */
extern int cx_exceptions;      /* exceptions on (the default; -fno-exceptions) */
extern int cx_in_targs;        /* inside < > of template arguments */
/* SFINAE: while set, an error unwinds to it instead of ending the run. */
extern void *cx_sfinae;
/* Close a template argument list: `>`, or half of `>>`. */
void cx_close_angle(void);

/* ---- functions ---- */

struct cfunc {
    const char *name;         /* unqualified; "operator+" etc. for operators */
    const char *cname;        /* C identifier: mangled, or the name for C */
    const char *cname2;       /* a constructor's or destructor's C2/D2 */
    const char *cname0;       /* a virtual destructor's deleting D0 */
    struct cty *type;         /* CT_FUNC (params and return) */
    const char **pnames;      /* parameter names (np of them) */
    struct cexpr **defargs;   /* default arguments, per parameter (or NULL) */
    struct cscope *owner;     /* namespace or class scope */
    struct cclass *cls;       /* a member function */
    int is_static;            /* static member, or internal linkage */
    int is_inline;            /* inline (incl. defined in a class body) */
    int is_ctor, is_dtor;
    int is_explicit, is_virtual, is_constexpr;
    int is_deleted, is_defaulted;
    int is_implicit;          /* declared by the compiler */
    int special;              /* an implicit one's kind: SP_* */
    int trivial;              /* ... and it runs no code (a C struct copy,
                               * nothing at all) */
    int is_conv;              /* a conversion function: operator T() */
    struct ctemplate *tmpl;   /* a function template (not callable itself:
                               * its specializations are) */
    struct ctemplate *spec_of;/* a specialization of this template */
    struct ctarg *targs;      /* ... for these arguments */
    int ntargs;
    int lazy;                 /* its body is tokens, parsed when used */
    int explicit_inst;        /* explicitly instantiated here */
    int extern_inst;          /* `extern template`: defined elsewhere */
    struct cscope *inst_scope;/* the bound parameters to parse it in */
    int is_pure;              /* `= 0` */
    int vslot;                /* a virtual function's slot in the vtable of
                               * vclass (-1: not virtual) */
    struct cclass *vclass;    /* the class whose primary vtable has it */
    struct cexpr **vbaseinit; /* ... of each virtual base (c->vbases), by the
                               * complete-object constructor */
    struct cexpr **baseinit;  /* a constructor's initialization of each
                               * direct base (class.c), or NULL */
    int is_builtin;           /* implicitly declared (operator new, ...) */
    int c_linkage;
    int weak;                 /* __attribute__((weak)) */
    int noreturn;
    const char *section;
    const char *asm_name;     /* `__asm__("name")`: the symbol instead */
    int access;
    int ctor_variant;         /* emit.c: 1 complete, 2 base object */
    int defined;              /* has a body */
    int used;                 /* odr-used: an inline one is emitted then */
    int emitted;
    int queued, declared;     /* emit.c's bookkeeping */
    int nlocal_statics;       /* numbering local statics for mangling */
    struct cstmt *body;
    struct cscope *pscope;    /* parameter scope of the definition */
    struct cvar **params;     /* the definition's parameter variables */
    struct cvar *this_var;    /* a non-static member's `this` */
    /* a constructor's initialization of each base and member, in order
     * (class.c); NULL for a delegating one, which has `delegate` */
    struct cexpr **meminit;
    struct cexpr *delegate;
    int body_tok, body_end;   /* a delayed in-class body: token range */
    int mi_tok;               /* ... and its mem-initializer list (or -1) */
    int deducing;             /* its body is being read for its return type */
    struct clambda *lambda;   /* a lambda's operator(): the lambda */
    int unsat;                /* its requires-clause is not satisfied: no
                               * candidate for overload resolution */
    struct cfunc *alias_of;   /* an overload set's entry a using-declaration
                               * made: this function, declared elsewhere */
    int local_inst;           /* an instance for a class with no linkage:
                               * internal, named as a local class's are */
    struct cstmt *fn_try;     /* a function-try-block: its handlers (S_TRY;
                               * its body is the function's) */
    struct cscope *def_scope; /* where a delayed body is parsed */
    int line;
    const char *file;
    struct cfunc *next;       /* the overload set */
    struct cfunc *all_next;   /* every function (emit order) */
};

extern struct cfunc *cx_funcs;       /* all functions, declaration order */
void func_register(struct cfunc *f); /* append to cx_funcs */

enum {
    SP_NONE, SP_DEFAULT, SP_COPY, SP_MOVE, SP_COPY_ASSIGN, SP_MOVE_ASSIGN,
    SP_DTOR
};

/* ---- classes ---- */

struct cfield {
    const char *name;         /* NULL for an unnamed bit-field */
    struct cty *type;
    long off;
    int bitwidth;             /* a bit-field's width, or -1 */
    int access;
    int is_mutable;
    struct cexpr *dflt;       /* default member initializer */
    int dflt_tok;             /* ... its delayed tokens (-1 if none) */
    int dflt_braced;          /* ... written { } rather than = */
    struct cclass *anon;      /* an anonymous union/struct member */
};

/* A base-specifier: the base, and where its subobject sits. */
struct cbase {
    struct cclass *cls;
    int is_virtual;
    int access;
    long off;                 /* offset in the object (a non-virtual base's;
                               * a virtual base's is in the complete object
                               * only: vbases) */
    long vbindex;             /* in vbases: where the primary vtable holds
                               * its offset (bytes from the address point) */
    int claimed;              /* ... a subobject's primary base: at its
                               * address, taking no space of its own */
};

/* A slot of a vtable: the final overrider it calls, and how to get there
 * from the vptr's class (a this-adjusting thunk when non-zero). */
struct vslot {
    struct cfunc *f;          /* the function (NULL: __cxa_pure_virtual) */
    int deleting;             /* a virtual destructor's second slot (D0) */
    long adjust;              /* bytes to subtract from `this` (a thunk) */
};

struct cclass {
    const char *name;
    const char *cname;        /* the C struct tag */
    struct clambda *closure;  /* a lambda's closure type: the lambda */
    int is_union, is_struct;  /* is_struct: declared `struct` (default public) */
    struct cscope *scope;     /* member scope */
    struct cscope *owner;     /* enclosing scope */
    struct cfield **fields;
    int nfields, capfields;
    struct cbase *bases;      /* direct bases, in declaration order */
    int nbases;
    struct cbase *vbases;     /* every virtual base, at its offset in a
                               * complete object, in inheritance graph
                               * order */
    int nvbases;
    struct cclass **vinit;    /* ... in the order they are constructed */
    int nvinit;
    void *vinfo;              /* vtable.c's tables */
    struct cfunc **vcfns;     /* as a virtual base: the functions with a */
    long *vcidx;              /* vcall offset, and where it is */
    int nvc, vc_done;
    long size, align;
    long nvsize, nvalign;     /* the object without its virtual bases (what
                               * a base subobject occupies, and later
                               * subobjects may use its tail padding) */
    long dsize;               /* data size: without tail padding */
    int empty;                /* no data, no vptr, empty bases only */
    int pod_layout;           /* POD for the purpose of layout: no tail
                               * padding reuse */
    int explicit_layout;      /* emit.c spells its offsets out (bases, a
                               * vptr): a packed C struct */
    int dynamic;              /* has a vptr at offset 0 */
    struct cclass *primary;   /* the primary base: shares the vptr */
    int primary_virt;         /* ... a nearly empty virtual base */
    int abstract, abstract_known;
    struct vslot *vtab;       /* the primary vtable's function slots */
    int nvtab;
    struct cfunc *key;        /* the key function: the vtable's home unit
                               * is the one defining it (NULL: every unit
                               * that needs the vtable emits it, weak) */
    struct ctemplate *tmpl;   /* an instance of this class template */
    struct ctarg *targs;      /* ... for these arguments */
    int ntargs;
    int inst_pending;         /* instance not yet defined (class_ensure) */
    int extern_inst;          /* `extern template class`: its members are
                               * another unit's */
    struct cpartial *inst_partial;   /* defined by this partial spec */
    int vtable_used;          /* emit.c: the vtable (and RTTI) is needed */
    int vtable_done, rtti_used, rtti_done;
    long align_attr;          /* __attribute__((aligned)) / alignas */
    int packed;
    int is_final;             /* declared `final` */
    int explicit_spec;        /* an instance given as template<> ... */
    int complete;
    int defining;             /* its body is being parsed */
    int local;                /* declared inside a function */
    int anon;                 /* no name */
    int unnamed_no;           /* ... its number in its scope (from 1: the
                               * ABI's Ut_, Ut0_, ...) */
    struct cfunc *ctors;      /* the constructors (overload set) */
    struct cfunc *implicit_ctor;  /* the implicit default constructor */
    struct cfunc *dtor;       /* declared, or the implicit one once needed */
    struct cfunc *copy_assign;    /* user-declared operator=(const T&) */
    int user_copy_ctor, user_move_ctor, user_copy_assign, user_move_assign;
    int user_dtor;
    int user_ctors;           /* any constructor the user declared */
    int has_default_ctor;     /* some constructor takes no arguments */
    int trivial_assign;       /* copy/move assignment is a struct copy */
    /* what copying, destroying and default-constructing entail */
    int trivial_dtor;         /* no code to destroy */
    int trivial_copy;         /* copy/move construction is memcpy */
    int trivial_default;      /* default construction runs no code */
    int trivial_for_calls;    /* passed and returned in registers, as C */
    int aggregate;
    int emitted;
};

struct cenum {
    const char *name;
    struct cscope *owner;     /* where it is declared (for its mangled name) */
    struct cscope *scope;     /* its enumerators */
    struct cty *underlying;
    int scoped;
    int fixed;                /* an underlying type was written */
    int complete;
    int unnamed_no;           /* no name: its number in its scope */
};

/* ---- expressions ---- */

enum cexpr_kind {
    E_INT, E_FLT, E_STR, E_NULLPTR,
    E_VAR,        /* a variable (a reference variable is the referent) */
    E_FUNC,       /* a function designator (for & or a call) */
    E_OVL,        /* an unresolved overload set (fn; obj for members) */
    E_CALL,       /* a direct call to fn: a (a member's first is `this`) */
    E_ICALL,      /* through a function pointer: a[0] the pointer */
    E_BUILTIN,    /* a __builtin_ function passed to C: name, a */
    E_UNARY,      /* op: '-', '+', '~', '!' */
    E_BINARY,     /* op: a binop (arith, compare, logical, shifts) */
    E_CMP3,       /* the built-in a[0] <=> a[1] (operands converted to one
                   * type): t the comparison category (std::strong_ordering,
                   * or std::partial_ordering with ival 1: may be unordered) */
    E_ASSIGN,     /* op: '=' or the compound operator's binop */
    E_INCDEC,     /* ival: +1/-1, post */
    E_COND, E_COMMA,
    E_CAST,       /* to e->t (a C conversion); lvcast: a glvalue result */
    E_ADDR, E_DEREF,
    E_MEMBER,     /* a[0].field (a[0] a glvalue or prvalue of class type) */
    E_THIS,
    E_CONSTRUCT,  /* a class prvalue: ctor fn on a (NULL fn: trivial —
                   * a[0] copied if na, else zero-initialized) */
    E_INITLIST,   /* { a... }: typed (aggregate/array/scalar) once t set */
    E_TEMP,       /* a[0] (a prvalue) materialized: a temporary */
    E_NEW, E_DELETE,
    E_STMTEXPR,   /* GNU ({ ... }) */
    E_VAARG,      /* __builtin_va_arg(a[0], t) */
    E_MEMPTR,     /* &C::m: field (its offset) or fn */
    E_BASE,       /* a[0] as its base subobject t: ival bytes in (a pointer
                   * when is_array is set: null stays null) */
    E_TYPEID,     /* typeid: of alloc_t, or of the polymorphic a[0]'s
                   * dynamic type */
    E_DYNCAST,    /* dynamic_cast of pointer a[0] (to its class alloc_t) to
                   * t: ival the offset hint; is_array: to void *; zero: a
                   * reference (failure calls __cxa_bad_cast) */
    E_PMEM,       /* a[0].*a[1]: the data member a pointer to member names */
    E_PMCALL,     /* (a[0]->*a[1])(a[2]...): a[0] the object's address */
    E_MPCONV,     /* a[0], a pointer to member of a base, as one of its
                   * derived class: ival added (a data member's offset,
                   * a member function's this adjustment) */
    E_THROW,      /* throw a[0] (na 0: rethrow); alloc_t the exception
                   * object's type, a[0] its initialization */
    E_EXCOBJ      /* in a handler: the caught object (an lvalue of type t);
                   * is_array: a caught pointer's value */
};

enum { VC_PRVALUE, VC_LVALUE, VC_XVALUE };

struct cexpr {
    enum cexpr_kind k;
    struct cty *t;            /* the expression's type: never a reference */
    int vc;                   /* VC_* */
    int op;                   /* E_UNARY/E_BINARY/E_ASSIGN: a TOK_ kind */
    long ival;                /* E_INT; E_INCDEC delta */
    long vbindex;             /* E_BASE through a virtual base: its offset is
                               * at vptr + vbindex (then ival) */
    int post;                 /* E_INCDEC */
    int lvcast;               /* E_CAST: to a reference type */
    int is_null_const;        /* an integer literal 0 (a null pointer) */
    struct cexpr **binit;     /* E_INITLIST of an aggregate with bases: each
                               * base subobject's initialization (a[] its
                               * members') */
    int paren;                /* written in parentheses (decltype) */
    double fval;              /* E_FLT */
    const char *text;         /* E_FLT: the spelling; E_STR: the bytes */
    long slen;                /* E_STR: element count incl. NUL */
    int swidth;               /* E_STR: bytes per element */
    const char *name;         /* E_BUILTIN */
    struct cvar *var;         /* E_VAR */
    struct cfunc *fn;         /* E_FUNC, E_OVL, E_CALL, E_CONSTRUCT; E_NEW:
                               * operator new; E_DELETE: operator delete */
    struct cexpr *obj;        /* E_OVL: the object pointer (members) */
    struct cfield *field;     /* E_MEMBER */
    struct cexpr **a;         /* operands / arguments */
    int na;
    struct cstmt *body;       /* E_STMTEXPR */
    struct cty *alloc_t;      /* E_NEW/E_DELETE: the object (element) type */
    struct cexpr *count;      /* E_NEW[]: the element count */
    struct cexpr *init;       /* E_NEW: the object's initialization */
    long cookie;              /* E_NEW/E_DELETE[]: array cookie bytes */
    int is_array;             /* E_NEW/E_DELETE: [] form */
    int zero;                 /* E_CONSTRUCT: zero-initialize first */
    int adl;                  /* E_OVL: an unqualified name — argument-
                               * dependent lookup adds candidates */
    int memptr;               /* E_OVL: written &C::f — to become a pointer
                               * to member function */
    int nonvirt;              /* E_CALL: qualified (C::f()), no virtual call */
    int baseobj;              /* E_CONSTRUCT, E_CALL of a destructor: the
                               * base-object variant (C2, D2) */
    struct ctarg *targs;      /* E_OVL: explicit template arguments */
    int ntargs;
    int has_targs;
    struct cfunc *dtor;       /* E_DELETE: the destructor to run first */
    int line;
    const char *file;
};

struct cexpr *ex_new(enum cexpr_kind k, struct cty *t, int vc);
struct cexpr *ex_int(long v, struct cty *t);
struct cexpr *ex_literal_zero(void);        /* 0, a null pointer constant */
struct cexpr *ex_cast(struct cexpr *e, struct cty *t);
struct cexpr *ex_addr(struct cexpr *e);    /* &e (e a glvalue) */
struct cexpr *ex_deref(struct cexpr *p);   /* *p */
struct cexpr *ex_this(void);               /* `this` in a member function */
struct cexpr *ex_materialize(struct cexpr *e);   /* a prvalue's temporary */
struct cexpr *ex_member(struct cexpr *obj, struct cfield *fl);
/* `l op r` (op a TOK_ assignment operator), overloaded or built-in. */
struct cexpr *expr_assign(int op, struct cexpr *l, struct cexpr *r);
/* A class that is passed and returned through memory (Itanium: not
 * trivially copyable or destructible): by reference to a temporary, into
 * a return slot. */
int class_indirect(const struct cty *t);
/* The arguments of a call to fn converted to its parameters (defaults
 * filled in); *out gets np (or more, for `...`) expressions. */
int convert_args(struct cfunc *fn, struct cexpr **args, int na,
                 const struct ctok *at, struct cexpr ***out);
struct cexpr *expr_parse(void);            /* a full expression (with ,) */
struct cexpr *expr_parse_assign(void);     /* an assignment-expression */
struct cexpr *expr_parse_cond(void);       /* a conditional-expression */
long expr_parse_const(const char *what);   /* an integral constant */
long expr_parse_const_as(const char *what, int as_bool);
/* overload resolution's "standard conversions only" state, swapped out
 * by nested work (a class or body instantiated meanwhile) */
int expr_swap_no_user_conv(int v);
int expr_const(struct cexpr *e, long *out); /* integer constant expression */
int cx_expr_nothrow(struct cexpr *e);   /* can it not throw? (noexcept) */
/* e (of another class) made a c by one of its conversion functions: the
 * call (NULL if it has none that fits) */
struct cexpr *class_conversion(struct cexpr *e, struct cclass *c);
/* An expression of the binary operators binding tighter than minprec
 * (3: none of && and ||, as a constraint's operand). */
struct cexpr *expr_parse_binary(int minprec);
/* Constraints (concepts.c): a requires-clause skipped (general: a
 * concept's whole expression, to `;`); satisfied, read from its tokens in
 * scope; a concept's value for arguments (template.c, cached); a
 * type-constraint C<A...> on t; a requires-expression at `requires`; a
 * concept named at the cursor (its tokens, its arguments' `<` or -1). */
void skip_constraint(int general);
int constraint_satisfied(int start, int end, struct cscope *scope);
int concept_satisfied(struct ctemplate *c, struct ctarg *args, int n,
                      const struct ctok *at);
int type_constraint_holds(struct ctemplate *c, int args_tok, struct cty *t,
                          const struct ctok *at);
struct cexpr *parse_requires_expr(void);
struct ctemplate *concept_at(int *ntok, int *args_tok);
/* the constraints of template parameters ps bound in scope (their
 * type-constraints) and of the requires-clause start..end */
int template_constraints(struct ctparam *ps, int np, int req_start,
                         int req_end, struct cscope *scope,
                         const struct ctok *at);
void skip_template_args(void);           /* at `<`: past the matching `>` */
int targ_is_dependent(const struct ctarg *a);
int deduce_func_type(struct ctemplate *t, struct ctarg *expl, int nexpl,
                     struct cty *A, struct ctarg **outp, int *nout);
/* template<> ... v<args> = ...: v's instance for args is the variable */
void var_explicit_spec(struct ctemplate *t, struct ctarg *args, int n,
                       struct cvar *v, const struct ctok *at);
struct cty *parse_param_list(void);      /* ( params ): a function type */
/* Type-trait intrinsics (traits.c): known by that name (a type or a
 * bool)? a type one? at one here (its name, then `(`, not declared)? */
int trait_known(const char *name);
int trait_is_type(const char *name);
int trait_at(void);
struct cexpr *parse_trait(void);          /* __is_class(T) ...: a bool */
struct cty *parse_type_trait(void);       /* __underlying_type(T) ... */
/* ... folded only: no calls evaluated (what emit.c may write as a constant
 * without changing what the program does) */
int expr_fold(struct cexpr *e, long *out);
/* Constant evaluation (consteval.c): e's value, calls of constexpr
 * functions and all; 0 if it is not a constant expression. */
int cx_consteval_int(struct cexpr *e, long *out);
/* Convert e to type t implicitly (copy-initialization); ctx for messages. */
struct cexpr *convert(struct cexpr *e, struct cty *t, const char *ctx);
struct cexpr *convert_bool(struct cexpr *e, const char *ctx);
struct cexpr *rvalue(struct cexpr *e);     /* lvalue-to-rvalue, decay */
/* Expressions built rather than parsed (lowerings), with the semantics
 * and overload resolution of the operators they stand for. */
struct cexpr *expr_var(struct cvar *v);
struct cexpr *expr_parse_name_value(struct csym *y, const char *name,
                                    const struct ctok *at);
struct cexpr *expr_binary(int op, struct cexpr *l, struct cexpr *r);
struct cexpr *expr_preinc(struct cexpr *e);
struct cexpr *expr_deref(struct cexpr *e);
struct cexpr *expr_call_named_targs(struct cexpr *obj, const char *name,
                                    struct ctarg *targs, int ntargs,
                                    struct cexpr **args, int na,
                                    const struct ctok *at);
struct cexpr *expr_call_named(struct cexpr *obj, const char *name,
                              struct cexpr **args, int na,
                              const struct ctok *at);
/* A reference of type `rt` (a CT_LREF/CT_RREF) bound to e: returns the
 * pointer-valued expression to store (e's address, or a temp's). */
struct cexpr *bind_ref(struct cexpr *e, struct cty *rt, const char *ctx);
/* Call `fn` with `args` (`obj` the object pointer for a member): converts
 * the arguments, fills defaults, binds reference parameters. */
struct cexpr *make_call(struct cfunc *fn, struct cexpr *obj,
                        struct cexpr **args, int na, const struct ctok *at);
/* Overload resolution: the best viable function in the set for args (obj
 * the object for member candidates, or NULL). With `at` NULL it probes:
 * NULL when nothing (or nothing unique) is viable, instead of an error. */
struct cfunc *resolve(struct cfunc *set, struct cexpr *obj,
                      struct cexpr **args, int na, const struct ctok *at,
                      const char *what);
enum {
    RS_NO_USER = 1,           /* only standard conversions (a constructor
                               * in copy-initialization, 12.2.2.5) */
    RS_NO_EXPLICIT = 2,       /* explicit constructors are not candidates */
    RS_IL_CTORS = 4           /* only initializer-list constructors (the
                               * first phase of list-initialization) */
};
struct cfunc *resolve_ex(struct cfunc *set, struct cexpr *obj,
                         struct cexpr **args, int na, const struct ctok *at,
                         const char *what, int flags);
/* The path from class d down to base b: 1 if b is d or a base of it,
 * with *ambiguous set when there are two such subobjects. */
int class_derives(struct cclass *d, struct cclass *b, int *ambiguous);
/* e (a glvalue of class type, or a pointer to one when ptr) converted to
 * its base class b. */
struct cexpr *to_base(struct cexpr *e, struct cclass *b, int ptr);
/* The member `name` of class c, its bases searched when c has none;
 * class_lookup_ambiguous is set when two bases have different ones. */
struct csym *class_member(struct cclass *c, const char *name);
extern int class_lookup_ambiguous;
/* ---- vtables (vtable.c) ---- */

/* A vtable's function entry: f (NULL: a null entry, a destructor's in a
 * construction vtable) reached through a thunk when delta or vcall. */
struct vfn {
    struct cfunc *f;
    int deleting;             /* the deleting destructor (D0) */
    int pure;                 /* __cxa_pure_virtual */
    long delta;               /* this += delta, */
    long vcall;               /* then += the vcall offset at vptr + vcall */
};
/* One vtable of a group: vcall and vbase offsets (lowest address first),
 * offset to top, typeinfo, functions; its vptr points at `point`. */
struct vtbl {
    long *pre;
    int npre;
    long ott;
    struct cclass *rtti;
    struct vfn *fns;
    int nfns;
    int point;                /* the address point, in words into the group */
    long off;                 /* its subobject, from the group's root */
};
struct vgroup {
    struct vtbl *v;
    void **of;                /* vtable.c's: each one's subobject */
    int n;
    int words;
};
/* A construction vtable group: base `base` at `off` in the class. */
struct ctorgrp {
    struct cclass *base;
    long off;
    struct vgroup *g;
};
/* A VTT entry: an address point in the main group (ctor -1) or a
 * construction group. */
struct vttent {
    int ctor;
    int point;
};
/* A vptr a constructor or destructor sets: at this + off, or when virt at
 * the virtual base whose offset is at vptr + vbindex, + off; to vtt[vtt]
 * (with virtual bases) or the group's address point. */
struct vstore {
    int virt;
    long vbindex;
    long off;
    int vtt;
    int point;
};
struct vgroup *vtable_group(struct cclass *c);
int class_ctor_groups(struct cclass *c, struct ctorgrp **out);
int class_vtt(struct cclass *c, struct vttent **out);
int class_vstores(struct cclass *c, struct vstore **out);
int class_subvtt(struct cclass *c, int base);    /* direct non-virtual base */
int class_vvtt(struct cclass *c, int vbase);     /* c->vbases[vbase] */
long class_vbindex(struct cclass *c, struct cclass *v);
int class_rtti_flags(struct cclass *c);
/* The base subobjects of class b in c: how many; for one, the last
 * virtual base on the path to it (NULL: none) and b's offset from it (or
 * from c). */
int class_base_path(struct cclass *c, struct cclass *b, struct cclass **vb,
                    long *off);
int class_abstract(struct cclass *c);   /* a slot's final overrider is pure */
int func_nothrow(struct cfunc *f);      /* a call of f cannot throw */

/* Aggregate initialization of t from a braced list. */
/* A std::initializer_list (of class type ilt) made from the braced list
 * `list`: its backing array built, then the object pointing at it. */
struct cexpr *il_make(struct cty *ilt, struct cexpr *list,
                      const struct ctok *at);
/* parse.c: the backing array made a hidden local — when a local variable
 * is initialized (its lifetime then is the variable's); NULL otherwise
 * (a temporary, as for an argument) */
struct cexpr *il_backing_var(struct cty *arr, struct cexpr *init,
                             const struct ctok *at);
struct cexpr *init_aggregate(struct cty *t, struct cexpr *list,
                             const struct ctok *at);
/* The operator delete (the class's own, else the global one) a deleting
 * destructor of c calls, for args {pointer, size}. */
struct cexpr *call_delete_op(struct cclass *c, struct cexpr **args);
/* The call to the operator (new, delete, ...) `name` in the global scope. */
struct cexpr *call_global_op(const char *name, struct cexpr **args, int na,
                             const struct ctok *at);

/* How an object is initialized (9.4). */
enum init_form {
    INIT_DEFAULT,             /* T x;        */
    INIT_VALUE,               /* T x{}; T()  */
    INIT_COPY,                /* T x = e;    */
    INIT_DIRECT,              /* T x(a, b);  */
    INIT_LIST,                /* T x{a, b};  */
    INIT_COPY_LIST            /* T x = {a, b}; */
};
/* The initialization of an object of type t (not a reference): a class
 * prvalue (E_CONSTRUCT, a call, ...), an E_INITLIST for an aggregate or an
 * array, a converted scalar, or NULL for nothing to do (default-init of a
 * trivial type). */
struct cexpr *init_object(struct cty *t, enum init_form form,
                          struct cexpr **args, int na, const struct ctok *at);
/* A braced list as written (E_INITLIST with t NULL). */
struct cexpr *parse_braced_list(void);
int expr_call_args_rest(struct cexpr ***out);   /* after `(` */

/* ---- statements ---- */

enum cstmt_kind {
    S_EXPR, S_DECL, S_BLOCK, S_IF, S_WHILE, S_DO, S_FOR, S_SWITCH, S_CASE,
    S_DEFAULT, S_BREAK, S_CONTINUE, S_RETURN, S_GOTO, S_LABEL, S_NULL,
    S_ASM, S_TRY
};

/* A catch clause: the type it catches (NULL: catch (...)), as written —
 * a reference, a pointer, or a value — and its block, whose first
 * statement declares the parameter when it is named. */
struct chandler {
    struct cty *type;
    struct cstmt *body;
};

struct cstmt {
    enum cstmt_kind k;
    struct cexpr *e;          /* EXPR, RETURN value, IF/WHILE/DO/SWITCH cond,
                               * FOR cond */
    struct cexpr *e2;         /* FOR step */
    struct cstmt *init;       /* FOR init; IF/WHILE/SWITCH: a condition's or
                               * init-statement's declaration */
    struct cstmt *body, *els; /* bodies; BLOCK: its list */
    struct cstmt *next;
    struct cstmt *blk;        /* BLOCK: the enclosing block; LABEL/GOTO/
                               * BREAK/CONTINUE/RETURN: the block it is in */
    struct cvar *var;         /* DECL */
    struct cstmt *more;       /* DECL: the next declarator's DECL */
    int dtor;                 /* DECL: the object needs destruction */
    const char *label;        /* GOTO/LABEL */
    long cval, cval2;         /* CASE (cval2: a GNU range's end) */
    int is_range;
    struct cvar *ret_var;     /* RETURN: the local object it returns by name
                               * (a named-return-value candidate) */
    const char *asm_text;     /* ASM: the statement, verbatim */
    struct chandler *handlers;    /* TRY: body, then these */
    int nhandlers;
    int line;
    const char *file;
};

struct cstmt *st_new(enum cstmt_kind k);

/* ---- lambdas (parse.c) ---- */

/* A capture: the entity (a variable, or `this` when var is NULL and
 * is_this), how, the closure's member for it, and its value where the
 * lambda is made. */
struct ccapture {
    const char *name;
    struct cvar *var;
    struct cfield *of;        /* or an enclosing lambda's member (one not
                               * standing for a variable: an init-capture) */
    int self_copy;            /* [*this]: is_this, a copy of the object */
    int is_this;
    int byref;
    struct cfield *field;
    struct cexpr *value;
};

struct clambda {
    struct cclass *cls;        /* the closure type */
    struct cfunc *call;        /* its operator() (a generic lambda's: the
                                * member template's pattern) */
    struct cfunc *outer;       /* the function the lambda is in (or NULL) */
    struct clambda *parent;    /* the lambda that function is, if one */
    int dflt;                  /* capture-default: 0, '=' or '&' */
    struct ccapture *caps;
    int ncaps, capcaps;
    int closed;                /* the closure is complete: no captures
                                * can be added */
    const struct ctok *at;
};

struct cexpr *parse_lambda(void);
/* In a lambda's body: variable v of an enclosing function, captured —
 * the closure member standing for it; NULL if not in a lambda. */
struct cexpr *lambda_capture(struct cvar *v, const struct ctok *at);
/* ... or member fl of enclosing lambda L's closure (a name that finds
 * it): L's capture, captured again (NULL if fl is not one). */
struct cexpr *lambda_capture_member(struct clambda *L, struct cfield *fl,
                                    const struct ctok *at);
/* In a lambda's body: the enclosing object's `this`, captured (NULL if
 * not in a lambda). */
struct cexpr *lambda_this(const struct ctok *at);

/* ---- the parser ---- */

void cx_parse_unit(void);
struct cty *parse_type_id(void);           /* e.g. inside sizeof(...) */
struct cty *parse_value_tparam_type(int tok);
struct cty *parse_new_type_id(void);       /* after `new` */
int at_simple_type_kw(void);               /* int(x): a keyword type */
struct cty *parse_simple_type_spec(void);
/* A nested-name-specifier at the cursor, not consumed: `::`, `A::`,
 * `A::B::`... `scope` is what it names (NULL if absent) and `fin` how many
 * tokens it spans — the final component starts at cx_peek(fin). */
struct qname {
    struct cscope *scope;
    int fin;
    int bad;                  /* a qualifier names no namespace or class */
    struct cty *dep;          /* in a pattern: a dependent qualifier (the
                               * names after it are unknown until
                               * instantiation) */
};
struct qname peek_qname(void);
/* A (qualified) type name at the cursor: its type and token count —
 * skipped with cx_skip_peek, which also keeps half of a closing `>>`. */
struct cty *peek_type_name(int *ntok);
void cx_skip_peek(int n);
/* After `operator`: "operator+" etc., or a conversion function's type. */
const char *parse_operator_name(struct cty **conv);
int at_type_start(void);                   /* a type can begin here */
struct cstmt *parse_compound(void);        /* { ... } with its scope */
/* The function being defined (for `this`, return type, labels). */
extern struct cfunc *cx_curfn;
extern struct cstmt *cx_curblk;            /* the innermost block */
extern int cx_extern_c;                    /* inside extern "C" */

extern struct cvar **cx_gvars;             /* namespace-scope and static
                                            * member variables, in order */
extern int cx_ngvars;
void gvar_register(struct cvar *v);
extern struct cclass **cx_classes;         /* in completion order */
extern int cx_nclasses;

/* ---- classes ---- */

struct cclass *class_new(const char *name, struct cscope *owner);
void class_complete(struct cclass *c);     /* layout + implicit members */
struct cfield *class_find_field(struct cclass *c, const char *name);
/* The initialization of an object of class c from args (form as for
 * init_object): a constructor call, trivial copy, aggregate list. */
struct cexpr *construct(struct cclass *c, enum init_form form,
                        struct cexpr **args, int na, const struct ctok *at);
/* The destructor to call for c (NULL when trivial); used. */
struct cfunc *class_dtor(struct cclass *c);
/* A constructor's mem-initializer as written: `name(args)` / `name{args}`. */
struct meminit_raw {
    const char *name;
    struct cclass *cls;       /* named by a type: a base, or the class
                               * itself (delegating) */
    struct cexpr **args;
    int na;
    int braced;
    const struct ctok *at;
};
/* Resolve a constructor's initialization of each member (and base): the
 * written mem-initializers, else default member initializers, else
 * default-initialization. */
void ctor_meminit(struct cfunc *f, struct meminit_raw *mi, int n);
/* Give an implicit, non-trivial special member its definition (once). */
void define_implicit(struct cfunc *f);
/* Set up a delayed default member initializer (parse.c calls back). */
void field_parse_default(struct cclass *c, struct cfield *fl);
/* The implicitly-defined special members' bodies, built on use. */

/* ---- mangling ---- */

const char *mangle_func(struct cfunc *f);
const char *mangle_var(struct cvar *v, struct cscope *owner);
const char *mangle_local_static(struct cvar *v, struct cfunc *fn, int disc);
const char *mangle_class_name(struct cclass *c);   /* the nested-name form */
const char *mangle_type_alone(struct cty *t);
/* A type referenced from an expression's mangling, and such a mangling's
 * substitution-free key (mangle.c). */
int mangle_type_ref(struct cty *t);
const char *mexpr_key(const char *x);

/* ---- emission ---- */

char *cx_emit_unit(void);

/* util */
char *cx_strdup(const char *s);
char *cx_fmt(const char *fmt, ...);
int cx_uid(void);
struct ctok *cx_tok_at(int pos);

#endif
