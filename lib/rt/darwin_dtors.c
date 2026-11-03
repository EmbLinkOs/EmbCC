/* __attribute__((destructor)) on Darwin. dyld runs __mod_init_func but no
 * longer __mod_term_func, so -- as clang does -- a destructor is
 * registered with __cxa_atexit when the image starts: EmbCC lists the
 * image's destructors in __DATA,__embcc_dtors and puts this function in
 * __mod_init_func after the constructors. It is linked into each image
 * (executable or dylib), so __dso_handle is that image's header, and
 * `done` is that image's. Registered in the list's order (by priority),
 * they run in the reverse. */
#if defined(__APPLE__)
struct mach_header_64;
extern const struct mach_header_64 __dso_handle;
unsigned char *getsectiondata(const struct mach_header_64 *mh,
                              const char *segname, const char *sectname,
                              unsigned long *size);
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

static int done;

void __embcc_register_dtors(void)
{
    if (done)
        return;
    done = 1;
    unsigned long size = 0;
    void (**d)(void) = (void (**)(void))getsectiondata(&__dso_handle, "__DATA",
                                                       "__embcc_dtors", &size);
    for (unsigned long k = 0; d && k < size / sizeof *d; k++)
        __cxa_atexit((void (*)(void *))d[k], 0, (void *)&__dso_handle);
}
#else
typedef int embcc_rt_darwin_dtors_empty;
#endif
