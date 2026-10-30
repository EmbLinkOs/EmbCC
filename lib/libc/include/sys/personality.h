/* <sys/personality.h> -- Linux's execution domains (src/posix/linuxapi.c). */
#ifndef _SYS_PERSONALITY_H
#define _SYS_PERSONALITY_H

#ifdef __cplusplus
extern "C" {
#endif

#define PER_LINUX          0x0000
#define PER_LINUX32        0x0008
#define ADDR_NO_RANDOMIZE  0x0040000
#define READ_IMPLIES_EXEC  0x0400000

int personality(unsigned long persona);

#ifdef __cplusplus
}
#endif

#endif
