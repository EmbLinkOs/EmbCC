/* What a static object's destructor is registered with, where no crtbegin
 * is linked -- the bare-metal targets, tools/build-libcxx.sh.
 *
 * __cxa_atexit itself is in the C library (lib/libc/src/stdlib/exit.c),
 * on the same list as atexit: guard.cc says why.
 */
extern "C" {

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

/* The "DSO handle" the compiler passes as __cxa_atexit's third argument
 * (Itanium 3.3.5): it names which shared object a destructor belongs to,
 * for __cxa_finalize. crtbegin.o defines it where there is one; a
 * firmware image has no crtbegin and one module, so any unique address
 * does. Weak, so crtbegin's wins wherever both are linked. */
__attribute__((weak)) void *__dso_handle = &__dso_handle;

#ifdef __ARM_EABI__
/* The ARM C++ ABI's registration function (3.3.5.3): __cxa_atexit's with
 * the object first. GCC calls this one on ARM; clang calls __cxa_atexit
 * there too, which is why both exist. */
int __aeabi_atexit(void *obj, void (*fn)(void *), void *dso)
{
    return __cxa_atexit(fn, obj, dso);
}
#endif

}  // extern "C"
