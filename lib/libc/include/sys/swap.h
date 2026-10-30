/* <sys/swap.h> -- swap areas, on Linux (src/posix/linuxapi.c). */
#ifndef _SYS_SWAP_H
#define _SYS_SWAP_H
#ifdef __cplusplus
extern "C" {
#endif
#define SWAP_FLAG_PREFER      0x8000
#define SWAP_FLAG_PRIO_MASK   0x7fff
#define SWAP_FLAG_PRIO_SHIFT  0
#define SWAP_FLAG_DISCARD     0x10000
int swapon(const char *path, int flags);
int swapoff(const char *path);
#ifdef __cplusplus
}
#endif
#endif
