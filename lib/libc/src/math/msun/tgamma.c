/* tgamma: 4.3BSD's, as FreeBSD's libm has it (bsdsrc/b_tgamma.c, with
 * the extra-precise log and exp it is built on, b_log.c and b_exp.c,
 * which it includes). Kept in bsdsrc/ under its own notice and compiled
 * from here: those two cannot be compiled alone. */
#include "bsdsrc/b_tgamma.c"
