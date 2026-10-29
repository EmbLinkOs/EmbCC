#ifndef EMBCC_SEMA_SEMA_H
#define EMBCC_SEMA_SEMA_H

#include "../parse/ast.h"

/* Resolves names and checks the unit; exits with a diagnostic on the
 * first error. On success every EXPR_VAR has a var_index, every
 * EXPR_CALL a callee, and every func its nvars. */
void sema_check(struct unit *u);

/* GNU89 inline semantics (-fgnu89-inline, -std=gnu89): `inline` alone
 * is an external definition and `extern inline` an inline-only one --
 * the opposite of C99, which is the default. */
void sema_set_gnu89_inline(int on);

/* Semantic errors the last sema_check reported: the driver stops before
 * generating code when there were any (docs/manual/diagnostics.md T2). */
int sema_error_count(void);

/* The statement list a switch dispatches over: its body, unwrapped when
 * it is the usual brace block. */
struct stmt *switch_stmts(struct stmt *body);
/* Every case and default marker of a switch, in source order, wherever it
 * is in the body -- in a nested block, an if, a loop (Duff's device) --
 * but not a nested switch's, which are that switch's. Returns how many,
 * with the markers in *out (xmalloc'd, NULL when none; the caller frees).
 * Shared with irgen so both agree on which markers a switch dispatches
 * to. */
int switch_labels(struct stmt *body, struct stmt ***out);

/* The GCC atomic builtins — the __atomic_* family and the older __sync_*
 * one. Classified in ONE place so sema (which types a call) and irgen (which
 * lowers it) cannot disagree about what a name means. */
enum atomic_kind {
    AK_NONE,
    AK_LOAD_N, AK_STORE_N, AK_EXCHANGE_N, AK_CMPXCHG_N,
    AK_LOAD, AK_STORE, AK_EXCHANGE, AK_CMPXCHG,  /* values passed by pointer */
    AK_FETCH_OP,      /* read-modify-write, the OLD value back */
    AK_OP_FETCH,      /* read-modify-write, the NEW value back */
    AK_TEST_AND_SET, AK_CLEAR,
    AK_THREAD_FENCE, AK_SIGNAL_FENCE,
    AK_LOCK_FREE,     /* folded to a constant in sema */
    AK_SYNC_BOOL_CAS, AK_SYNC_VAL_CAS, AK_SYNC_LOCK_TAS, AK_SYNC_LOCK_RELEASE
};

/* The kind of the builtin `name`, or AK_NONE. For the read-modify-write
 * kinds *op receives '+', '-', '&', '|', '^', or 'n' (nand). */
enum atomic_kind atomic_builtin(const char *name, int *op);

/* The GCC bit builtins, by the name after "__builtin_": 1 ctz, 2 clz,
 * 3 popcount, 4 ffs, 5 parity, 6 clrsb, or 0. *width (if not NULL) gets the
 * operand's type as a rank -- 1 int, 2 long, 3 long long -- whose size is
 * the target's to say (irgen gen_bitop). */
int builtin_bitop(const char *bn, int *width);
int builtin_bitx(const char *bn, int *bits);
struct type *ty_uint_bits(int bits);

/* GCC's type-generic bit builtins (clzg and kin, GCC 14) and C23's
 * stdc_ ones (GCC 15), by the name after "__builtin_", or 0: each counts
 * at its operand's own width, whatever unsigned (or, for ffsg and clrsbg,
 * signed) integer type that is. */
enum {
    BG_CLZ = 1, BG_CTZ, BG_POPCOUNT, BG_PARITY, BG_FFS, BG_CLRSB,
    BG_LZ, BG_LO, BG_TZ, BG_TO, BG_FLZ, BG_FLO, BG_FTZ, BG_FTO,
    BG_CZ, BG_CO, BG_ONE, BG_BW, BG_FLOOR, BG_CEIL, BG_ROTL, BG_ROTR
};
int builtin_bitg(const char *bn);
/* A generic bit builtin's operand width in bits: its _BitInt width, or
 * its size's. */
int bitg_bits(const struct type *t);
/* What one computes on an operand of `bits` bits (at most 64), the value
 * as an unsigned of that width; `n` is clzg's or ctzg's value for 0 (when
 * has_n) or a rotate's amount. The answer is in the result's type, as
 * const_fold reads an integer: 0/1 for has_single_bit, the operand's
 * bits for the floor, ceiling and rotates, a count otherwise. */
unsigned long long bitg_eval(int kind, unsigned long long x, int bits,
                             int has_n, long long n);
/* A bit builtin of constant operands -- the bit family, the generic and
 * stdc_ ones, the byte swaps, the bit reversals and rotates -- by the name
 * after "__builtin_": `a` the first operand's value and `at` its type, `b`
 * the second's when nargs is 2. 1 with *out the value, still to be
 * brought to its type *rty; 0 when bn is none of them. sema's const_fold
 * and the parser's (_Static_assert, an array size) both fold through it. */
int bitbuiltin_fold(const char *bn, long a, const struct type *at, int nargs,
                    long b, long *out, struct type **rty);

/* Does the C front end implement this compiler-known name? This is the
 * answer `__has_builtin` gives, and it must not be able to drift from
 * what the compiler actually accepts.
 *
 * Most of it is DERIVED rather than restated: the bit family goes
 * through builtin_bitop and the atomics through atomic_builtin -- the
 * same predicates the dispatch itself calls -- so adding a member to
 * either family updates this automatically. The remainder is a list,
 * because sema lowers those through a chain of strcmps and there is
 * nothing else to ask. tests/golden/has-builtin.sh closes that gap from
 * both directions: every name here must be accepted by the compiler,
 * and every name the dispatch strcmps for must be here.
 *
 * (Clang generates both its dispatch and its __has_builtin from one
 * Builtins.td, which is the better shape and the one to move to when
 * the dispatch becomes table-driven. Until then the test is what makes
 * this honest rather than aspirational.) */
int sema_has_builtin(const char *name);

/* The listed (non-derived) half, for the golden test to walk. */
int sema_named_builtin_count(void);
const char *sema_named_builtin(int i);

/* A call that never comes back -- __attribute__((noreturn)), or one of
 * the handful of library names that genuinely never return. Statements
 * after it are unreachable, which both the missing-return check and the
 * uninitialized analysis need to know. */
int is_noreturn_call(const struct expr *e);

#endif
