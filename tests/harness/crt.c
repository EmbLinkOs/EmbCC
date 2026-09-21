/* The start-up a program needs between the machine being ready and main,
 * shared by both harnesses: static constructors run first (a C++ global's
 * constructor, or a C __attribute__((constructor)), sits in .init_array or,
 * from x86_64-elf gcc, .ctors), and
 * main's value leaves through exit() — so atexit/__cxa_atexit handlers run
 * (every C++ global destructor is one) and stdio is flushed, as a real crt0
 * does. __dso_handle is the "this module" token __cxa_atexit is handed; in a
 * static image there is one module. */
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];
extern initfn __ctors_start[], __ctors_end[];
void *__dso_handle = &__dso_handle;

int main(int argc, char **argv);
void exit(int code) __attribute__((noreturn));

/* The unwind tables, for C++ exceptions: libgcc's unwinder finds a
 * frame's FDE among the objects registered with it. Weak, so an image
 * without the unwinder (no C++) does not pull it in. */
extern char __eh_frame_start[];
void __register_frame_info(const void *, void *) __attribute__((weak));
static long eh_object[16];            /* libgcc's struct object, and room */

void __harness_main(void)
{
    if (__register_frame_info)
        __register_frame_info(__eh_frame_start, eh_object);
    /* .ctors first (backward): x86 g++ code, libstdc++'s stream set-up
     * among it, which constructors in .init_array may already use */
    for (initfn *f = __ctors_end; f > __ctors_start; )
        (*--f)();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    exit(main(0, 0));
}

/* libstdc++'s random_device and newlib's arc4random ask for entropy. The
 * harness has no source of it, so it says so rather than fabricating any —
 * the answer EmbLinkOS itself gives on a CPU without RDRAND. */
#include <errno.h>
#include <stddef.h>
int getentropy(void *buf, size_t len)
{
    (void)buf;
    (void)len;
    errno = ENOSYS;
    return -1;
}

/* libstdc++'s file streams (pulled in with <iostream>) can open files; the
 * harness has no file system. */
int open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    errno = ENOSYS;
    return -1;
}
