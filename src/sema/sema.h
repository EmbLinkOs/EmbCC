#ifndef EMBCC_SEMA_SEMA_H
#define EMBCC_SEMA_SEMA_H

#include "../parse/ast.h"

/* Resolves names and checks the unit; exits with a diagnostic on the
 * first error. On success every EXPR_VAR has a var_index, every
 * EXPR_CALL a callee, and every func its nvars. */
void sema_check(struct unit *u);

/* Semantic errors the last sema_check reported: the driver stops before
 * generating code when there were any (docs/tools/diagnostics.md T2). */
int sema_error_count(void);

/* The statement list a switch dispatches over: its body, unwrapped when
 * it is the usual brace block. Shared with irgen so both agree on which
 * statements carry the case markers. */
struct stmt *switch_stmts(struct stmt *body);

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
 * operand width — 4 for the plain form, 8 for the l / ll forms. */
int builtin_bitop(const char *bn, int *width);

/* A call that never comes back -- __attribute__((noreturn)), or one of
 * the handful of library names that genuinely never return. Statements
 * after it are unreachable, which both the missing-return check and the
 * uninitialized analysis need to know. */
int is_noreturn_call(const struct expr *e);

#endif
