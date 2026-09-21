/* -Wformat: a printf or scanf format string checked against the
 * arguments beside it (see format.c for what is and is not reported). */
#ifndef EMBCC_FORMAT_H
#define EMBCC_FORMAT_H

#include "../parse/ast.h"

/* `call` is an EXPR_CALL whose arguments sema has already checked and
 * promoted; `fn` is its direct callee. Does nothing unless fn carries
 * __attribute__((format(printf|scanf, ...))) and the format argument is
 * a string literal. */
void format_check(const char *file, struct expr *call, const struct func *fn);

#endif
