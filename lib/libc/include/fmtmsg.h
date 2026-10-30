/* <fmtmsg.h> -- XSI's formatted messages (src/stdlib/fmtmsg.c). */
#ifndef _FMTMSG_H
#define _FMTMSG_H

#ifdef __cplusplus
extern "C" {
#endif

#define MM_HARD    0x001
#define MM_SOFT    0x002
#define MM_FIRM    0x004
#define MM_APPL    0x008
#define MM_UTIL    0x010
#define MM_OPSYS   0x020
#define MM_RECOVER 0x040
#define MM_NRECOV  0x080
#define MM_PRINT   0x100
#define MM_CONSOLE 0x200
#define MM_NULLMC  0L

#define MM_NOSEV   0
#define MM_HALT    1
#define MM_ERROR   2
#define MM_WARNING 3
#define MM_INFO    4

#define MM_NULLLBL ((char *)0)
#define MM_NULLSEV 0
#define MM_NULLTXT ((char *)0)
#define MM_NULLACT ((char *)0)
#define MM_NULLTAG ((char *)0)

#define MM_OK     0
#define MM_NOTOK  (-1)
#define MM_NOMSG  1
#define MM_NOCON  4

int fmtmsg(long classification, const char *label, int severity,
           const char *text, const char *action, const char *tag);

#ifdef __cplusplus
}
#endif

#endif
