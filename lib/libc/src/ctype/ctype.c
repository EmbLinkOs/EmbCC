/* <ctype.h>, C11 §7.4 — the "C" locale, which is the only one this library
 * has and the only one a freestanding target wants.
 *
 * One table, indexed from EOF so that `__ctype_table[c + 1]` is valid for
 * every value the standard defines these for. A caller passing something
 * else is undefined behaviour in C, but undefined behaviour that reads out
 * of bounds is worse than undefined behaviour that returns a wrong answer,
 * so the index is masked.
 */
#include <ctype.h>

#define U _CT_UPPER
#define L _CT_LOWER
#define D (_CT_DIGIT | _CT_HEX)
#define S _CT_SPACE
#define P _CT_PUNCT
#define C _CT_CNTRL
#define X _CT_HEX
#define B _CT_BLANK
#define G _CT_GRAPH
#define R _CT_PRINT

/* printable = graph + space; graph = alnum + punct */
#define PU (P | G | R)
#define DG (D | G | R)
#define UP (U | G | R)
#define LO (L | G | R)
#define UX (U | X | G | R)
#define LX (L | X | G | R)

const unsigned short __ctype_table[257] = {
    0,                                            /* EOF */
    C, C, C, C, C, C, C, C,                       /* 00-07 */
    C, C|S|B, C|S, C|S, C|S, C|S, C, C,           /* 08-0f: \t \n \v \f \r */
    C, C, C, C, C, C, C, C,
    C, C, C, C, C, C, C, C,                       /* 10-1f */
    S|B|R,                                        /* space */
    PU, PU, PU, PU, PU, PU, PU,                   /* ! " # $ % & ' */
    PU, PU, PU, PU, PU, PU, PU, PU,               /* ( ) * + , - . / */
    DG, DG, DG, DG, DG, DG, DG, DG, DG, DG,       /* 0-9 */
    PU, PU, PU, PU, PU, PU, PU,                   /* : ; < = > ? @ */
    UX, UX, UX, UX, UX, UX,                       /* A-F */
    UP, UP, UP, UP, UP, UP, UP, UP, UP, UP,       /* G-P */
    UP, UP, UP, UP, UP, UP, UP, UP, UP, UP,       /* Q-Z */
    PU, PU, PU, PU, PU, PU,                       /* [ \ ] ^ _ ` */
    LX, LX, LX, LX, LX, LX,                       /* a-f */
    LO, LO, LO, LO, LO, LO, LO, LO, LO, LO,       /* g-p */
    LO, LO, LO, LO, LO, LO, LO, LO, LO, LO,       /* q-z */
    PU, PU, PU, PU,                               /* { | } ~ */
    C,                                            /* 7f DEL */
    /* 80-ff: not members of any class in the "C" locale */
    0
};

static unsigned short cls(int c)
{
    return (c >= -1 && c < 256) ? __ctype_table[c + 1] : 0;
}

/* Each returns 1 or 0, not the class bit.
 *
 * C says only "nonzero", and newlib and glibc both hand back the internal
 * mask -- so `isdigit('5')` is 4 there and 1 here, and both conform. One is
 * chosen deliberately: a program that prints or compares the value is
 * relying on something the standard does not promise, and a library that
 * returns a tidy 1 makes that bug visible on the first run rather than
 * after a port. */
int isalnum(int c)  { return (cls(c) & (_CT_UPPER | _CT_LOWER | _CT_DIGIT)) != 0; }
int isalpha(int c)  { return (cls(c) & (_CT_UPPER | _CT_LOWER)) != 0; }
int isblank(int c)  { return (cls(c) & _CT_BLANK) != 0; }
int iscntrl(int c)  { return (cls(c) & _CT_CNTRL) != 0; }
int isdigit(int c)  { return (cls(c) & _CT_DIGIT) != 0; }
int isgraph(int c)  { return (cls(c) & _CT_GRAPH) != 0; }
int islower(int c)  { return (cls(c) & _CT_LOWER) != 0; }
int isprint(int c)  { return (cls(c) & _CT_PRINT) != 0; }
int ispunct(int c)  { return (cls(c) & _CT_PUNCT) != 0; }
int isspace(int c)  { return (cls(c) & _CT_SPACE) != 0; }
int isupper(int c)  { return (cls(c) & _CT_UPPER) != 0; }
int isxdigit(int c) { return (cls(c) & _CT_HEX) != 0; }

int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
