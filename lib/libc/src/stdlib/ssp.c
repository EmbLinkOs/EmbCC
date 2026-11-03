/* -fstack-protector's failure: a function's canary was overwritten, so
 * its frame -- the return address included -- cannot be trusted, and the
 * program stops here rather than return through it. Written with the
 * backend directly, as assert's report is (stdlib/assert.c): the stack
 * that just broke may be what stdio would run on. The guard itself is
 * lib/rt's (rt/ssp.c), set once before main. */
#include <stdlib.h>
#include <string.h>

#include "../../os/backend.h"

_Noreturn void __stack_chk_fail(void)
{
    static const char m[] = "*** stack smashing detected ***: terminated\n";
    (void)__os_write(2, m, sizeof m - 1);
    abort();
}

/* glibc's name for the same, which a -fstack-protector object built
 * against it with -fPIC may call */
_Noreturn void __stack_chk_fail_local(void)
{
    __stack_chk_fail();
}
