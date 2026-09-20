/* std::terminate and its handler — C++ [exception.terminate].
 *
 * The last thing the runtime does. It writes with the OS backend rather
 * than through iostreams or even stdio: terminate() is called when the
 * program's invariants are already broken -- an exception with no handler,
 * a second exception during unwinding, a destructor that threw -- and a
 * final message that needs a working allocator is a message that vanishes
 * in exactly the cases worth diagnosing.
 */
#include <exception>
#include <typeinfo>

#include <stdlib.h>
#include <string.h>

#include "../../libc/os/backend.h"

namespace __cxxabiv1 {
/* Declared here rather than by including abi.h: this file needs exactly
 * one thing from the exception machinery and nothing from the type_info
 * hierarchy. */
extern "C" std::type_info *__cxa_current_exception_type() noexcept;
}

namespace {

void say(const char *s)
{
    size_t n = strlen(s), off = 0;
    while (off < n) {
        long w = __os_write(2, s + off, n - off);
        if (w <= 0)
            return;                  /* nowhere to report; still abort */
        off += static_cast<size_t>(w);
    }
}

/* The default handler. It names the exception's type when there is one,
 * because "terminate called" on its own has sent a generation of
 * programmers to a debugger to learn something the runtime already knew. */
[[noreturn]] void default_terminate()
{
    say("terminate called");
    const std::type_info *t = __cxxabiv1::__cxa_current_exception_type();
    if (t) {
        say(" after throwing an instance of '");
        say(t->name());
        say("'");
    }
    say("\n");
    abort();
}

std::terminate_handler g_terminate = default_terminate;

}  // namespace

namespace std {

terminate_handler set_terminate(terminate_handler h) noexcept
{
    terminate_handler old = g_terminate;
    g_terminate = h ? h : default_terminate;
    return old;
}

terminate_handler get_terminate() noexcept { return g_terminate; }

void terminate() noexcept
{
    /* [except.terminate]/2: the handler must not return. If it does --
     * or throws -- there is nothing left to try, so abort directly rather
     * than recursing into whatever it did. */
    g_terminate();
    abort();
}

}  // namespace std
