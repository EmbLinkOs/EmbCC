/* The stage dumps behind `embcc inspect` (vision §18). See inspect.c. */
#ifndef EMBCC_INSPECT_H
#define EMBCC_INSPECT_H

struct outbuf;
struct unit;

void inspect_tokens(struct outbuf *b, const char *file, const char *pp,
                    int cxx);
void inspect_ast(struct outbuf *b, const struct unit *u);
void inspect_symbols(struct outbuf *b, const struct unit *u);
void inspect_types(struct outbuf *b, const struct unit *u);

#endif
