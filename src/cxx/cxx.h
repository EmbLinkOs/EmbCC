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
static inline enum tok_kind cx_kind(void) { return cx_toks[cx_pos].t.kind; }
static inline enum tok_kind cx_kind_at(int k) { return cx_peek(k)->t.kind; }
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
    CT_AUTO                   /* `auto`, until its initializer deduces it */
};

enum { CQ_CONST = 1, CQ_VOLATILE = 2 };

struct cclass;
struct cenum;

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
    struct cclass *cls;       /* CLASS */
    struct cenum *en;         /* ENUM */
    /* FUNC, as a declarator wrote it: the parameters' declared types (top-
     * level cv kept, which params[] drops), names and default arguments */
    struct cty **pdecl;
    const char **pnames;
    struct cexpr **defargs;
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
const char *ct_name(const struct cty *t);   /* for diagnostics */

/* ---- symbols and scopes ---- */

enum csym_kind {
    CS_VAR, CS_FUNC, CS_TYPEDEF, CS_CLASS, CS_ENUM, CS_ENUMERATOR,
    CS_NAMESPACE, CS_FIELD
};

enum cscope_kind { SC_NAMESPACE, SC_CLASS, SC_ENUM, SC_BLOCK, SC_PARAMS };

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
    int used;
    int emitted;
    int refd, declared;       /* emit.c's bookkeeping */
    int disc;                 /* a local static's number among its
                               * function's same-named ones */
    int line;
    const char *file;
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
/* ... only classes and enums (an elaborated type specifier) */
struct csym *lookup_tag(struct cscope *from, const char *name);
/* Qualified lookup: name as a member of namespace or class scope `in`. */
struct csym *lookup_in(struct cscope *in, const char *name);
struct cscope *enclosing_ns(struct cscope *s);
struct cclass *enclosing_class(struct cscope *s);

/* ---- functions ---- */

struct cfunc {
    const char *name;         /* unqualified; "operator+" etc. for operators */
    const char *cname;        /* C identifier: mangled, or the name for C */
    const char *cname2;       /* a constructor's or destructor's C2/D2 */
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

struct cclass {
    const char *name;
    const char *cname;        /* the C struct tag */
    int is_union, is_struct;  /* is_struct: declared `struct` (default public) */
    struct cscope *scope;     /* member scope */
    struct cscope *owner;     /* enclosing scope */
    struct cfield **fields;
    int nfields, capfields;
    long size, align;
    long align_attr;          /* __attribute__((aligned)) / alignas */
    int packed;
    int complete;
    int defining;             /* its body is being parsed */
    int local;                /* declared inside a function */
    int anon;                 /* no name */
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
    E_PMEM,       /* a[0].*a[1]: the data member a pointer to member names */
    E_PMCALL      /* (a[0]->*a[1])(a[2]...): a[0] the object's address */
};

enum { VC_PRVALUE, VC_LVALUE, VC_XVALUE };

struct cexpr {
    enum cexpr_kind k;
    struct cty *t;            /* the expression's type: never a reference */
    int vc;                   /* VC_* */
    int op;                   /* E_UNARY/E_BINARY/E_ASSIGN: a TOK_ kind */
    long ival;                /* E_INT; E_INCDEC delta */
    int post;                 /* E_INCDEC */
    int lvcast;               /* E_CAST: to a reference type */
    int is_null_const;        /* an integer literal 0 (a null pointer) */
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
    struct cfunc *dtor;       /* E_DELETE: the destructor to run first */
    int line;
    const char *file;
};

struct cexpr *ex_new(enum cexpr_kind k, struct cty *t, int vc);
struct cexpr *ex_int(long v, struct cty *t);
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
int expr_const(struct cexpr *e, long *out); /* integer constant expression */
/* Convert e to type t implicitly (copy-initialization); ctx for messages. */
struct cexpr *convert(struct cexpr *e, struct cty *t, const char *ctx);
struct cexpr *convert_bool(struct cexpr *e, const char *ctx);
struct cexpr *rvalue(struct cexpr *e);     /* lvalue-to-rvalue, decay */
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
    RS_NO_EXPLICIT = 2        /* explicit constructors are not candidates */
};
struct cfunc *resolve_ex(struct cfunc *set, struct cexpr *obj,
                         struct cexpr **args, int na, const struct ctok *at,
                         const char *what, int flags);
/* Aggregate initialization of t from a braced list. */
struct cexpr *init_aggregate(struct cty *t, struct cexpr *list,
                             const struct ctok *at);
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

/* ---- statements ---- */

enum cstmt_kind {
    S_EXPR, S_DECL, S_BLOCK, S_IF, S_WHILE, S_DO, S_FOR, S_SWITCH, S_CASE,
    S_DEFAULT, S_BREAK, S_CONTINUE, S_RETURN, S_GOTO, S_LABEL, S_NULL,
    S_ASM
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
    int line;
    const char *file;
};

struct cstmt *st_new(enum cstmt_kind k);

/* ---- the parser ---- */

void cx_parse_unit(void);
struct cty *parse_type_id(void);           /* e.g. inside sizeof(...) */
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
};
struct qname peek_qname(void);
/* A (qualified) type name at the cursor: its type and token count. */
struct cty *peek_type_name(int *ntok);
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

/* ---- emission ---- */

char *cx_emit_unit(void);

/* util */
char *cx_strdup(const char *s);
char *cx_fmt(const char *fmt, ...);
int cx_uid(void);
struct ctok *cx_tok_at(int pos);

#endif
