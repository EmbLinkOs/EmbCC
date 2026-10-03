/* The console and the program's own path in nothing but ISO C, for a host
 * that has a C library and no POSIX: a hobby operating system, a
 * non-POSIX one, or any host where <unistd.h> is not there. Built with
 * `make PLATFORM=iso`, beside platform_common.c, which every host shares.
 *
 * Nothing here can ask the host a question C does not have words for, so
 * the answers are the conservative ones: diagnostics are not assumed to
 * reach a terminal (no colour unless -fdiagnostics-color asks), and the
 * program is where argv[0] says it is, if argv[0] is a path at all. The
 * driver then finds its headers and libraries beside that path, under
 * EMBCC_PREFIX if the host has an environment, or under the prefix the
 * compiler was built with (EMBCC_DEFAULT_PREFIX). */
#include "platform.h"

int plat_stderr_is_terminal(void)
{
    return 0;
}

const char *plat_self_path(void)
{
    return plat_argv0_path();
}
