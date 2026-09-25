/* Which predefined-macro table the selected target uses. Hand-written:
 * the tables themselves are generated, so the choice between them cannot
 * live in either file. */
#include "predef.h"

#include "target.h"
#include "../driver/util.h"

#include <string.h>

static int g_cxx;

void predef_set_cxx(int cxx) { g_cxx = cxx; }
int predef_is_cxx(void) { return g_cxx; }

static const struct predef_macro *arch_table(int *count)
{
    if (g_cxx) {
        switch (target_get()) {
        case TARGET_AARCH64:
            *count = predef_macro_count_cxx_aarch64;
            return predef_macros_cxx_aarch64;
        case TARGET_THUMB:
            *count = predef_macro_count_cxx_thumb;
            return predef_macros_cxx_thumb;
        default:
            *count = predef_macro_count_cxx_x86_64;
            return predef_macros_cxx_x86_64;
        }
    }
    switch (target_get()) {
    case TARGET_AARCH64:
        *count = predef_macro_count_aarch64;
        return predef_macros_aarch64;
    case TARGET_THUMB:
        *count = predef_macro_count_thumb;
        return predef_macros_thumb;
    default:
        *count = predef_macro_count_x86_64;
        return predef_macros_x86_64;
    }
}

/* What an operating system adds on top of its architecture's table
 * (D-014).
 *
 * Hand-written, where the architecture tables are generated, and the
 * reason is that these are not the reference compiler's opinion about a
 * machine -- they are the NAME of the platform the code will run on, and
 * a system header branches on them to decide which declarations exist.
 * There are few of them and they have been stable for thirty years.
 *
 * Only what the platform's own headers actually read. The unprefixed
 * `linux` and `unix` are left out: gcc defines them only in GNU mode,
 * nothing in a header requires them, and they collide with ordinary
 * identifiers. Nor is __GNUC__ added here, for the reason predef.h
 * already gives -- EmbCC is not gcc, and claiming to be routes headers
 * onto extension paths it cannot honour (THE RULE).
 */
/* EmbLinkOS. Nothing in the OS tree reads a macro today -- the platform
 * is selected by WHICH backend.c gets compiled (Makefile
 * libc-emblinkos) -- so this names it for the first time rather than
 * matching an existing convention. It is still the right thing to
 * define: `x86_64-emblink` is a target in its own right, and code that
 * wants to ask "am I being built for the OS?" should not have to infer
 * it from the absence of __linux__. */
static const struct predef_macro os_emblink[] = {
    { "__emblink__", "1" },
    { "__emblink", "1" },
    { "__EmbLinkOS__", "1" },
};
static const struct predef_macro os_linux[] = {
    { "__linux__", "1" },
    { "__linux", "1" },
    { "__gnu_linux__", "1" },
    { "__unix__", "1" },
    { "__unix", "1" },
};
static const struct predef_macro os_darwin[] = {
    { "__APPLE__", "1" },
    { "__MACH__", "1" },
    { "__unix__", "1" },
    { "__unix", "1" },
};
static const struct predef_macro os_windows[] = {
    { "_WIN32", "1" },
    { "_WIN64", "1" },
    { "__MINGW32__", "1" },
    { "__MINGW64__", "1" },
};

/* __ELF__ lives in the generated architecture tables, because the
 * compilers they were generated from were the *-elf ones. It is a
 * statement about the OBJECT FORMAT, so on a target that is not ELF it
 * is simply false, and a header that reads it would be told the wrong
 * thing. Dropped rather than overridden: there is no "__ELF__ 0". */
static int contradicted(const char *name)
{
    return target_fmt_get() != TGT_FMT_ELF && strcmp(name, "__ELF__") == 0;
}

const struct predef_macro *predef_table(int *count)
{
    int narch = 0;
    const struct predef_macro *arch = arch_table(&narch);

    const struct predef_macro *os = NULL;
    int nos = 0;
    switch (target_os_get()) {
    case TGT_OS_EMBLINK:
        os = os_emblink; nos = (int)(sizeof os_emblink / sizeof *os_emblink);
        break;
    case TGT_OS_LINUX:
        os = os_linux;   nos = (int)(sizeof os_linux / sizeof *os_linux);
        break;
    case TGT_OS_DARWIN:
        os = os_darwin;  nos = (int)(sizeof os_darwin / sizeof *os_darwin);
        break;
    case TGT_OS_WINDOWS:
        os = os_windows; nos = (int)(sizeof os_windows / sizeof *os_windows);
        break;
    default:
        break;
    }

    /* The freestanding case is every target that existed before D-014,
     * and it hands back the generated table itself -- no copy, no
     * filtering, nothing to go wrong in the path that everything else
     * depends on. */
    if (!os && target_fmt_get() == TGT_FMT_ELF) {
        *count = narch;
        return arch;
    }

    static struct predef_macro *merged;
    static int nmerged;
    if (!merged) {
        merged = xmalloc((size_t)(narch + nos) * sizeof *merged);
        for (int i = 0; i < narch; i++)
            if (!contradicted(arch[i].name))
                merged[nmerged++] = arch[i];
        for (int i = 0; i < nos; i++)
            merged[nmerged++] = os[i];
    }
    *count = nmerged;
    return merged;
}
