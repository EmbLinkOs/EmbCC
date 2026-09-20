/* <ctype.h> — character classification, C11 §7.4.
 *
 * Defined for EOF and for values representable as unsigned char; anything
 * else is undefined, so the table is 257 wide and indexed from EOF. A
 * lookup rather than a chain of comparisons: it is one load, and it is what
 * makes the locale-independent answers constant-time. */
#ifndef _CTYPE_H
#define _CTYPE_H

extern const unsigned short __ctype_table[257];
extern const int __ctype_lower[257];
extern const int __ctype_upper[257];

#define _CT_UPPER 0x001
#define _CT_LOWER 0x002
#define _CT_DIGIT 0x004
#define _CT_SPACE 0x008
#define _CT_PUNCT 0x010
#define _CT_CNTRL 0x020
#define _CT_HEX   0x040
#define _CT_BLANK 0x080
#define _CT_GRAPH 0x100
#define _CT_PRINT 0x200

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

#endif
