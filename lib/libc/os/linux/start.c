/* crt1 for Linux: the entry point, and what runs between it and main.
 *
 * On every other target this library has been built for, somebody else
 * supplied this. The test harnesses have their own start.S assembled by
 * a cross gcc; a macOS program gets Apple's crt1.o from the SDK; an
 * EmbLinkOS program gets the OS's. A static Linux program built by
 * EmbCC gets this file and nothing else -- there is no glibc in the
 * image to have done it.
 *
 * ---- what the kernel hands us --------------------------------------------
 *
 * execve does not call _start; it JUMPS to it, with no return address
 * and no frame. The stack pointer points at argc, and everything else
 * follows it:
 *
 *     sp ->  argc
 *            argv[0] ... argv[argc-1], NULL
 *            envp[0] ... NULL
 *            the auxiliary vector, terminated by AT_NULL
 *
 * So _start has exactly two jobs: hand that pointer to C, and make sure
 * the C it calls is entered the way the ABI says a function is entered.
 * Both architectures zero the frame-pointer register first, which is
 * what tells an unwinder walking back from a crash that there is
 * nothing above this frame.
 *
 * The auxiliary vector is read by nothing here yet. It is where the
 * vDSO's address and the kernel's random seed live, and reading it is
 * what a later clock_gettime that does not enter the kernel would
 * need; see the note in backend.c.
 *
 * ---- why it is written as bytes -------------------------------------------
 *
 * EmbCC has no general text assembler (src/arch/x86_64/topasm.c says
 * so): a file-scope asm block may use labels, the data directives, and
 * a handful of x86-64 mnemonics. So the instructions that cannot be
 * spelled are written as .byte/.long with the disassembly beside them,
 * exactly as lib/libc/src/setjmp does -- and, exactly as there,
 * tests/golden/linux.sh feeds those comments to a REAL assembler and
 * checks that it produces these bytes. The bytes are what runs, so
 * nothing gets to assert they are right; an assembler has to agree.
 *
 * On aarch64 the branch to C goes through a `.quad __libc_start` rather
 * than a `bl`, because a block with no encodable instructions has no
 * way to carry a call relocation. The address is loaded from the word
 * after the code and branched through.
 */
#include "../backend.h"

#include <stdlib.h>

void __libc_start(long *sp);
int main(int argc, char **argv);
/* ./tls.c: finds PT_TLS in the auxiliary vector and gives this thread
 * its copy. It runs before anything else because errno is itself
 * thread-local, so the first syscall that could fail already needs a
 * thread pointer to exist. */
void __libc_tls_init(long *sp);

#if defined(__x86_64__)

__asm__(
    ".globl _start\n"
    "_start:\n"
    ".byte 0x48, 0x31, 0xed\n"   /* xor  %rbp, %rbp   -- end of the chain */
    ".byte 0x48, 0x89, 0xe7\n"   /* mov  %rsp, %rdi   -- the block, as arg 1 */
    "and $-16, %rsp\n"           /* the ABI's alignment; call then makes it 8 */
    "call __libc_start\n"
    ".byte 0xf4\n"               /* hlt -- unreachable; traps if it is not */
);

#elif defined(__aarch64__)

__asm__(
    ".globl _start\n"
    "_start:\n"
    ".long 0xd280001d\n"         /* mov  x29, #0      -- end of the chain */
    ".long 0xd280001e\n"         /* mov  x30, #0      -- and no return addr */
    ".long 0x910003e0\n"         /* mov  x0, sp       -- the block, as arg 1 */
    ".long 0x58000041\n"         /* ldr  x1, #8       -- the .quad below */
    ".long 0xd61f0020\n"         /* br   x1 */
    ".quad __libc_start\n"
);

#else
#error "crt1 for Linux has no entry stub for this architecture"
#endif

/* ---- static constructors and destructors ---------------------------------
 *
 * A C++ global's constructor, and a C function marked
 * __attribute__((constructor)), both become a pointer in .init_array;
 * the linker brackets the section with these two symbols. They are
 * WEAK because a link that contains no such function has no section
 * and therefore no symbols, and an undefined weak is zero -- so the
 * loop below runs zero times instead of failing to link.
 */
typedef void (*initfn)(void);
extern initfn __init_array_start[] __attribute__((weak));
extern initfn __init_array_end[] __attribute__((weak));
extern initfn __fini_array_start[] __attribute__((weak));
extern initfn __fini_array_end[] __attribute__((weak));

/* The token __cxa_atexit is handed to say WHICH module registered a
 * destructor, so that unloading a shared object can run just its own.
 * A static image is one module, and its address is the only thing that
 * ever has to be unique -- which is why it points at itself. Every C++
 * program with a global that has a destructor references this, so a
 * crt without it fails to link rather than misbehaving. */
void *__dso_handle = &__dso_handle;

/* .fini_array runs BACKWARD, and through atexit rather than after main
 * returns: a program that calls exit() must still run it, and exit() is
 * the only path both endings share. */
static void run_fini(void)
{
    for (initfn *f = __fini_array_end; f > __fini_array_start; )
        (*--f)();
}

void __libc_start(long *sp)
{
    /* First of all, before any code that could touch a thread-local. */
    __libc_tls_init(sp);

    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    /* envp begins after argv's NULL terminator. `environ` is the
     * library's own (lib/libc/src/stdlib/env.c) and getenv reads it;
     * nothing copies the strings, which is correct -- they are in the
     * stack block the kernel built and they outlive the program. */
    environ = argv + argc + 1;

    if (__fini_array_start != __fini_array_end)
        atexit(run_fini);
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();

    /* Through exit(), not a return: exit() flushes stdio and runs the
     * atexit handlers, and a program whose last printf never reached
     * the kernel has not printed it. */
    exit(main(argc, argv));
}
