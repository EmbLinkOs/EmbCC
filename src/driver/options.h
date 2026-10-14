/* EmbCC's options: the table in options.def, and what is printed from it. */
#ifndef EMBCC_DRIVER_OPTIONS_H
#define EMBCC_DRIVER_OPTIONS_H

#include <stdio.h>

/* Rewrite EmbCC's own spellings in argv into the ones the driver's
 * argument loop reads (GCC's, where GCC has one). argv is replaced only
 * when something was rewritten. -1 after an error message. */
int opts_normalize(int *argc, char ***argv);

void opts_print_help(FILE *out);      /* --help */
void opts_print_gcc(FILE *out);       /* --help=gcc */
void opts_print_markdown(FILE *out);  /* --help=markdown: the reference */
void opts_print_spellings(FILE *out); /* --help=spellings: for the tests */

#endif
