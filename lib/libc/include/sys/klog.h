/* <sys/klog.h> -- the kernel's message buffer (src/posix/linuxapi.c). */
#ifndef _SYS_KLOG_H
#define _SYS_KLOG_H
#ifdef __cplusplus
extern "C" {
#endif
int klogctl(int type, char *buf, int n);
#ifdef __cplusplus
}
#endif
#endif
