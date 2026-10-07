/* operator new and operator delete — C++ [new.delete].
 *
 * Twenty functions that are almost all the same function, which is the
 * point: a program may replace any of them, and the ones it does not
 * replace must agree with the ones it does. So exactly two of them
 * actually do anything -- the aligned and unaligned allocating forms --
 * and every other spelling forwards. Replacing `operator new(size_t)`
 * then really does change where `new T[n]` gets its memory, which is what
 * a user replacing it expects and what a copy-pasted implementation
 * quietly breaks.
 */
#include <new>

#include <stdlib.h>

namespace std {

static new_handler g_new_handler = nullptr;

new_handler set_new_handler(new_handler h) noexcept
{
    new_handler old = g_new_handler;
    g_new_handler = h;
    return old;
}

new_handler get_new_handler() noexcept { return g_new_handler; }

const nothrow_t nothrow{};

#if __cpp_exceptions
/* The exception classes only where there are exceptions: built with
 * -fno-exceptions (tools/build-libcxx.sh, the embedded targets) nothing
 * can throw or catch one, and their vtables would pull std::exception's
 * key function, which that build does not have. */
bad_alloc::bad_alloc() noexcept {}
bad_alloc::~bad_alloc() noexcept {}
const char *bad_alloc::what() const noexcept { return "std::bad_alloc"; }

bad_array_new_length::bad_array_new_length() noexcept {}
bad_array_new_length::~bad_array_new_length() noexcept {}
const char *bad_array_new_length::what() const noexcept
{ return "std::bad_array_new_length"; }
#endif

}  // namespace std

/* Every allocation and deallocation function is weak: a program may
 * replace any one of them ([replacement.functions]) and keep the rest,
 * and these all sit in one archive member -- so a program defining
 * operator new[] alone, and calling operator new too, pulled this member
 * in for the one and collided with it on the other. */
#define REPLACEABLE __attribute__((weak))

/* The loop [new.delete.single]/3 describes: call the new-handler and try
 * again, until either the allocation succeeds or there is no handler, in
 * which case throw. A handler that returns without freeing anything spins
 * forever -- and that is specified behaviour, not a bug here: the contract
 * is that a handler must free memory, install a different handler, or not
 * return. */
/* (`nothrow`: give back null where there is no handler, instead of
 * failing; only for the build without exceptions, below) */
static void *allocate(std::size_t n, std::size_t align, bool nothrow = false)
{
    if (n == 0)
        n = 1;                      /* distinct pointers for zero-size */
    for (;;) {
        void *p = align > __STDCPP_DEFAULT_NEW_ALIGNMENT__
                    ? aligned_alloc(align, (n + align - 1) / align * align)
                    : malloc(n);
        if (p)
            return p;
        std::new_handler h = std::get_new_handler();
        if (!h) {
#if __cpp_exceptions
            (void)nothrow;
            throw std::bad_alloc();
#else
            /* Without exceptions there is nothing to throw, and returning
             * null from a throwing operator new lets the caller construct
             * into address 0. Stop where the memory ran out -- what
             * libsupc++ built with -fno-exceptions does (abort). */
            if (nothrow)
                return nullptr;
            __builtin_trap();
#endif
        }
        h();
    }
}

static void *allocate_nothrow(std::size_t n, std::size_t align) noexcept
{
    /* [new.delete.single]/7: the nothrow forms are specified to call the
     * throwing form and catch, NOT to skip the new-handler. A program that
     * installs a handler expects it to run here too. */
#if __cpp_exceptions
    try {
        return allocate(n, align);
    } catch (...) {
        return nullptr;
    }
#else
    return allocate(n, align, true);
#endif
}

REPLACEABLE void *operator new(std::size_t n)
{ return allocate(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }

REPLACEABLE void *operator new[](std::size_t n)
{ return ::operator new(n); }

REPLACEABLE void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{ return allocate_nothrow(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }

REPLACEABLE void *operator new[](std::size_t n, const std::nothrow_t &nt) noexcept
{ return ::operator new(n, nt); }

REPLACEABLE void *operator new(std::size_t n, std::align_val_t a)
{ return allocate(n, static_cast<std::size_t>(a)); }

REPLACEABLE void *operator new[](std::size_t n, std::align_val_t a)
{ return ::operator new(n, a); }

REPLACEABLE void *operator new(std::size_t n, std::align_val_t a,
                   const std::nothrow_t &) noexcept
{ return allocate_nothrow(n, static_cast<std::size_t>(a)); }

REPLACEABLE void *operator new[](std::size_t n, std::align_val_t a,
                     const std::nothrow_t &nt) noexcept
{ return ::operator new(n, a, nt); }

/* free() handles an aligned block and a null pointer, so every delete is
 * the same call. The sized and aligned forms exist so an allocator that
 * WANTS the size or the alignment can use them; this one does not need
 * either, and saying so once is better than four copies of free(). */
REPLACEABLE void operator delete(void *p) noexcept { free(p); }
REPLACEABLE void operator delete[](void *p) noexcept { ::operator delete(p); }
REPLACEABLE void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
REPLACEABLE void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
REPLACEABLE void operator delete(void *p, const std::nothrow_t &) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete[](void *p, const std::nothrow_t &) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete(void *p, std::align_val_t) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete[](void *p, std::align_val_t) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete(void *p, std::size_t, std::align_val_t) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete[](void *p, std::size_t, std::align_val_t) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept
{ ::operator delete(p); }
REPLACEABLE void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept
{ ::operator delete(p); }
