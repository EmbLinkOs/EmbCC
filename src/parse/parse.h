#ifndef EMBCC_PARSE_PARSE_H
#define EMBCC_PARSE_PARSE_H

#include "ast.h"

struct unit *parse_unit(const char *file, const char *src);

/* -fsingle-precision-constant: an unsuffixed floating constant has type
 * float, and the value it would have as a float (GCC's rule). */
void parse_set_single_precision_constant(int on);
void parse_set_short_enums(int on);

#endif
