/* The reporting half of <assert.h>.
 *
 * It writes with the backend directly rather than through fprintf. An
 * assertion fires when the program's invariants are already broken, which
 * is exactly when stdio may be mid-update, out of heap, or the thing that
 * failed -- a diagnostic that needs a working allocator to print is a
 * diagnostic that disappears in the cases that matter most.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../../os/backend.h"

static void say(const char *s)
{
    size_t n = strlen(s), off = 0;
    while (off < n) {
        long w = __os_write(2, s + off, n - off);
        if (w <= 0)
            return;                    /* nowhere to report; still abort */
        off += (size_t)w;
    }
}

static void say_int(int v)
{
    char b[12];
    int i = (int)sizeof b;
    unsigned u = (unsigned)(v < 0 ? -v : v);
    b[--i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    say(b + i);
}

_Noreturn void __assert_fail(const char *expr, const char *file, int line,
                             const char *func)
{
    say(file); say(":"); say_int(line); say(": ");
    if (func) { say(func); say(": "); }
    say("Assertion `"); say(expr); say("' failed.\n");
    abort();
}
