/* C++ in, C out (D-013): the unit is tokenized whole, parsed with its
 * semantics (parse.c, expr.c, class.c), and written as C (emit.c) for the
 * C front-end and either back end. */
#include "cxx.h"
#include "translate.h"

int cx_exceptions = 1;
int cx_rtti = 1;

void cxx_set_exceptions(int on)
{
    cx_exceptions = on;
}

void cxx_set_rtti(int on)
{
    cx_rtti = on;
}

char *cxx_translate(const char *file, const char *src)
{
    scope_init();
    cx_tokenize(file, src);
    cx_parse_unit();
    return cx_emit_unit();
}
