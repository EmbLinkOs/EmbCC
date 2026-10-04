/* Running a program, for a host that cannot: EmbLinkOS, which has no
 * fork/exec (ARCHITECTURE §1), and any host with only a C library. The
 * driver then compiles one source per command, as it always has
 * (platform.h). */
#include "platform.h"

int plat_can_run(void)
{
    return 0;
}

int plat_run_start(const char *const argv[])
{
    (void)argv;
    return -1;
}

int plat_run_wait(int *which)
{
    (void)which;
    return -1;
}

int plat_ncpus(void)
{
    return 1;
}
