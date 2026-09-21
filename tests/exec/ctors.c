/* __attribute__((constructor)) in plain C.
 *
 * This was accepted and silently DROPPED until 2026-09-21: the
 * attribute parsed, nothing recorded it, no .init_array section was
 * emitted, and a function written to run before main simply never ran.
 * Nothing failed — which is the whole problem, and why the exit status
 * below can only be right if the constructors executed.
 *
 * EmbLinkOS uses this attribute in its own crt0 and in shell/tools, so
 * the silent version was wrong on a tree EmbCC compiles.
 *
 * The section TYPE is what carries the meaning: a linker gathers
 * SHT_INIT_ARRAY, and the startup code walks the bracket symbols around
 * it (tests/harness/crt.c here, lib/libc/os/linux/start.c on Linux).
 *
 * What is asserted is that BOTH ran, exactly once each -- not the order
 * they ran in. Constructors without a priority have no specified
 * relative order, and the toolchains disagree in practice: EmbCC emits
 * one .init_array in source order, while x86_64-elf-gcc emits .ctors,
 * which the startup walks backward. Asserting an order here made gcc's
 * build of this same file fail, which is a test being wrong rather than
 * a compiler. A priority is the only way to ASK for an order, and
 * EmbCC refuses one rather than ignoring it
 * (tests/compile/reject-unimplemented.sh).
 *
 * Destructors are NOT checked here, and deliberately: a .fini_array
 * entry runs after main has returned its value, so no exit status can
 * witness one. tests/golden/linux.sh checks those, where the program's
 * OUTPUT is compared and our crt walks .fini_array.
 */
// expect-exit: 42

static int seq;
static int first_ran, second_ran;

__attribute__((constructor)) static void first(void)
{
    first_ran = ++seq;
}

/* A second one, so the array has to hold more than a single entry. */
__attribute__((constructor)) static void second(void)
{
    second_ran = ++seq;
}

int main(void)
{
    /* seq counts the runs, and the two stamps must be the two distinct
     * values it handed out -- so neither ran twice and neither was
     * skipped, whichever order they came in. */
    if (seq != 2 || !first_ran || !second_ran || first_ran == second_ran)
        return 1;
    return 42;
}
