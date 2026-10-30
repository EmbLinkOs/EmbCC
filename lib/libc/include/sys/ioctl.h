/* <sys/ioctl.h> -- device control. The requests are Linux's, and a
 * backend whose kernel has others translates those it supports. */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#define TIOCEXCL   0x540C
#define TIOCNXCL   0x540D
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCSTI    0x5412
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCINQ    FIONREAD
#define TIOCNOTTY  0x5422
#define FIONBIO    0x5421
#define TIOCGSID   0x5429
#define FIONCLEX   0x5450
#define FIOCLEX    0x5451
#define FIOASYNC   0x5452

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
