/* The type representation — the single most load-bearing structure in
 * the compiler (every phase downstream takes its shape from it).
 *
 * Types I scope: void, the integer types char/short/int/long with
 * unsigned variants, and pointers. LP64 throughout (TARGET_ABI): long
 * and pointers are 8 bytes. Arrays, structs, enums, and function types
 * are later M2 increments — code that would need them must refuse.
 *
 * Value representation contract (codegen relies on it): expression
 * temporaries always hold PROMOTED values — 32-bit class for
 * char/short/int (C's integer promotions do this anyway) or 64-bit
 * class for long/pointers. Only variables in memory have the narrow
 * widths; loads extend, stores truncate.
 */
#ifndef EMBCC_SEMA_TYPE_H
#define EMBCC_SEMA_TYPE_H

/* The most parameters/arguments a function type, prototype, or call carries.
 * Defined here (not ast.h) because struct type's ptypes[] lives here and
 * ast.h includes this header — the two must agree or a >N-param function
 * type overflows ptypes[]. */
#define MAX_PARAMS 32

struct expr;

enum ty_kind { TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG,
               TY_FLOAT, TY_DOUBLE, TY_LDOUBLE, TY_INT128, TY_PTR,
               TY_ARRAY, TY_STRUCT, TY_FUNC };

struct member {
    const char *name;     /* NULL for an anonymous (padding) bitfield */
    struct type *ty;
    int off;              /* byte offset; for a bitfield, of its storage unit */
    int is_bitfield;
    int bit_off;          /* bitfield: bit position within the storage unit */
    int bit_width;        /* bitfield: width in bits (0 = zero-width separator) */
    int bf_bytes;         /* bitfield of a packed struct crossing its type's
                           * storage unit: off/bit_off are its first byte and
                           * bit there, and it is read and written a byte at a
                           * time over this many bytes (0: a unit access) */
    int user_align;       /* __attribute__((aligned(N))) on the member; 0 = none */
};

struct type {
    enum ty_kind kind;
    int is_unsigned;        /* integers only */
    int is_volatile;        /* `volatile`-qualified: every access must happen and
                             * must not be CSE'd/removed (MMIO). Set on the
                             * ACCESSED type — the pointee of a volatile pointer,
                             * or a volatile variable. Ignored by ty_equal. */
    struct type *canon;     /* a volatile COPY points at the unqualified original
                             * (structs compare by identity, so equality follows
                             * this); NULL on an original. */
    struct type *pointee;   /* TY_PTR: target; TY_ARRAY: element */
    int count;              /* TY_ARRAY: element count */
    /* TY_ARRAY of variable length (C99 VLA, or a fixed count of VLA
     * elements): count is 0 and the element count is vla_len's run-time
     * value. vla_size is the hidden local (sema) holding sizeof this
     * type in bytes, stored when its declaration is reached (irgen
     * vla_eval). ty_size() is 0 for one; a run-time size is read from
     * the slot. One node per declarator — never interned or shared. */
    struct expr *vla_len;
    int vla_size;
    /* TY_STRUCT (unions too — one type kind, is_union flag): */
    const char *tag;        /* NULL for anonymous */
    int is_union;
    int complete;           /* body seen; size/align/members valid */
    int is_complex;         /* a C99 `T _Complex`: laid out, passed and
                             * returned as struct { T __real__, __imag__; }
                             * — which is exactly its ABI on both targets —
                             * but an ARITHMETIC type, lowered by sema */
    struct type *celem;     /* is_complex: T */
    struct member *members;
    int nmembers;
    int size, align;        /* SysV layout, computed when completed */
    /* TY_FUNC (always behind a pointer in this subset): */
    struct type *ret;
    struct type *ptypes[MAX_PARAMS];
    int nptypes;
    int is_varargs;
    int sret_first;         /* the first parameter is the ABI's indirect-
                             * result pointer (__attribute__((embcc_sret)),
                             * which C++ lowering writes): aarch64 passes it
                             * in x8, not x0 — x86-64 already takes it
                             * first, in rdi */
};

/* Base types are interned singletons — pointer equality works for
 * them; ty_equal() works for everything. */
struct type *ty_base(enum ty_kind kind, int is_unsigned);
/* The types whose signedness the TARGET decides, each matching what that
 * target's predefined macros already tell the headers:
 *   plain `char` — signed on x86-64, UNSIGNED on aarch64 (AAPCS64;
 *                  __CHAR_UNSIGNED__ is defined there);
 *   wchar_t      — int on x86-64 ELF, unsigned int on aarch64
 *                  (__WCHAR_TYPE__).
 * EmbCC does not model plain char as a third type distinct from signed and
 * unsigned char: it IS one of the two, chosen per target. */
struct type *ty_plain_char(void);
struct type *ty_wchar(void);
/* A copy of `t` marked `volatile` (or t itself if already). Base types are
 * interned singletons, so this returns a fresh non-interned node — safe because
 * nothing compares types by pointer identity (ty_equal compares fields). */
struct type *ty_volatile(struct type *t);
struct type *ty_ptr(struct type *pointee);
struct type *ty_array(struct type *elem, int count);
/* SysV x86-64: a struct that is exactly one long double (X87 + X87UP) —
 * returned in st0, where every other struct EmbCC returns goes through a
 * hidden pointer or rax/rdx/xmm. EmbCC refuses to return one on x86-64. */
int ty_x87_struct(const struct type *t);
/* `elem _Complex` (elem float, double or long double): one interned node
 * per elem, so type identity is type equality. */
struct type *ty_complex(struct type *elem);
int ty_is_complex(const struct type *t);
/* SysV x86-64: how a struct comes back in x87 registers — 1 for exactly
 * one long double (X87, in st0), 2 for a long double _Complex (COMPLEX_X87,
 * real in st0 and imaginary in st1), 0 otherwise. */
int ty_x87_ret(const struct type *t);
/* A variable-length array of elem, `len` elements (a VLA). */
struct type *ty_vla(struct type *elem, struct expr *len);
/* t is an array whose size is only known at run time. */
int ty_is_vla(const struct type *t);

/* AAPCS64: is `t` a homogeneous floating-point aggregate? Returns the
 * element count (1..4) and sets *esz to the element size, or 0. And: is it
 * a composite passed as a pointer to a caller-made copy (stage B.3)? */
int ty_hfa(const struct type *t, int *esz);
int ty_aapcs64_byref(const struct type *t);
/* t is variably modified: a VLA, or a pointer/array/function return
 * reaching one (C99 6.7.5p3) — `int (*p)[n]` is VM but not a VLA. */
int ty_is_vm(const struct type *t);

/* A new, incomplete struct/union type (one node per tag — completed in
 * place by ty_struct_layout once its body is parsed). */
struct type *ty_struct(const char *tag, int is_union);
struct type *ty_func(struct type *ret, struct type **ptypes, int n,
                     int is_varargs);
/* Assigns member offsets and the struct's size/align per SysV, and
 * marks the type complete. Members must already have complete types. */
void ty_struct_layout(struct type *t, struct member *members, int n,
                      int packed, int user_align);
struct member *ty_find_member(struct type *t, const char *name);

int ty_size(const struct type *t);          /* bytes; void has none */
int ty_align(const struct type *t);
int ty_equal(const struct type *a, const struct type *b);
int ty_is_integer(const struct type *t);
int ty_is_float(const struct type *t);
int ty_is_arith(const struct type *t);   /* integer or floating */
int ty_is_scalar(const struct type *t);     /* integer or pointer */
int ty_wide(const struct type *t);          /* 1 = 64-bit value class */
int ty_signed_int(const struct type *t);    /* signed integer? */

/* ---- SysV AMD64 argument classification (the ABI's §3.2.3) ----
 *
 * Getting this wrong does not fail loudly: it produces an object that
 * links against gcc-built code and passes arguments in the wrong place.
 * So it is implemented in full rather than only for the case a given
 * program happens to need.
 *
 * An aggregate larger than two eightbytes is MEMORY (stack / hidden
 * return pointer). Otherwise each eightbyte is SSE when every scalar
 * overlapping it is floating, and INTEGER otherwise. */
enum arg_class { CLASS_INTEGER, CLASS_SSE, CLASS_MEMORY };

/* Fills classes[] with one entry per eightbyte and returns the count
 * (1 or 2); returns 0 when the type is MEMORY class. Non-aggregates
 * answer with their single natural class. */
int ty_classify(const struct type *t, enum arg_class *classes);

/* Diagnostic spelling, e.g. "unsigned char **". Static buffer. */
const char *ty_name(const struct type *t);

#endif
