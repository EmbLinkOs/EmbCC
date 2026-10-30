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

/* the socket interface requests (struct ifreq, <net/if.h>) */
#define SIOCGIFNAME    0x8910
#define SIOCSIFLINK    0x8911
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCSIFDSTADDR 0x8918
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMETRIC  0x891d
#define SIOCSIFMETRIC  0x891e
#define SIOCGIFMTU     0x8921
#define SIOCSIFMTU     0x8922
#define SIOCSIFNAME    0x8923
#define SIOCSIFHWADDR  0x8924
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCGIFTXQLEN  0x8942
#define SIOCSIFTXQLEN  0x8943

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
