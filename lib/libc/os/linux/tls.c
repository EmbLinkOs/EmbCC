/* Thread-local storage: finding the template, and giving each thread a
 * copy of it.
 *
 * `__thread int x;` puts x in .tdata or .tbss, which the linker gathers
 * into a PT_TLS program header. That header is not something the
 * program can use directly -- it is a TEMPLATE. Every thread, including
 * the first, needs its own copy, and something has to make one and
 * point the hardware at it. That is this file.
 *
 * ---- where the template is --------------------------------------------
 *
 * Nothing in the image records it: there is no symbol for "the PT_TLS
 * header". The kernel does record it, in the AUXILIARY VECTOR it leaves
 * on the initial stack past the environment -- AT_PHDR, AT_PHNUM and
 * AT_PHENT describe the program's own headers, and PT_TLS is one of
 * them. So `_start` passes the stack block down, and this walks it.
 *
 * That is also the first use this library makes of the auxv, and the
 * same door the vDSO goes through later.
 *
 * ---- the two layouts ---------------------------------------------------
 *
 * The architectures disagree structurally, and the LINKER has already
 * picked a side when it resolved the offsets, so this has to lay memory
 * out the way the linker assumed. Both were read off a linked program
 * rather than taken on trust:
 *
 *   x86-64 (variant II): the block sits BELOW the thread pointer, and
 *     offsets are negative -- a linked `add $imm,%rax` against the
 *     first word of a two-word block holds -8. The thread pointer also
 *     has to hold ITS OWN ADDRESS at offset 0, because the FS base is
 *     not readable from user space and `mov %fs:0,%rax` is how code
 *     gets it.
 *
 *   aarch64 (variant I): the block sits ABOVE the thread pointer, past
 *     a two-word reserved area, and offsets are positive -- the same
 *     program's first word resolved to tp + 16.
 *
 * Putting the copy in the wrong place would not fault. It would read
 * and write whatever else is there.
 */
#include "../backend.h"
#include "syscall.h"

#include <stdlib.h>
#include <string.h>

#define LAT_NULL   0
#define LAT_PHDR   3
#define LAT_PHENT  4
#define LAT_PHNUM  5
#define LPT_TLS    7

/* Just enough of a program header to find PT_TLS and read it. The
 * layout is ELF64's and is the same on both architectures. */
struct lphdr {
    unsigned p_type, p_flags;
    unsigned long long p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

/* The template, found once. filesz bytes to copy and (memsz - filesz)
 * to zero: .tdata is the initialised part and .tbss the rest, and the
 * program header describes them as one run. */
static const char *tpl_image;
static unsigned long tpl_filesz, tpl_memsz, tpl_align;

#if defined(__x86_64__)
/* The block is below the pointer, so the whole of it is "before". */
#define LTCB_SIZE 8            /* the self-pointer at offset 0 */
#elif defined(__aarch64__)
#define LTCB_SIZE 16           /* two reserved words, then the block */
#endif

static unsigned long align_up(unsigned long v, unsigned long a)
{
    return a > 1 ? (v + a - 1) & ~(a - 1) : v;
}

/* Build one thread's block and return the value its thread pointer
 * should have. NULL if there is nothing to allocate or no memory.
 *
 * The allocation is deliberately one malloc with room to align inside
 * it: a thread block has to satisfy the TLS segment's alignment, which
 * can be larger than malloc guarantees, and the pointer that has to be
 * freed later is the one malloc returned -- so it is kept in the word
 * before the thread pointer's own region. */
void *__libc_tls_new(void)
{
    if (!tpl_memsz && !tpl_align)
        return NULL;                       /* no thread-local data at all */

    /* The block's size is rounded to the TEMPLATE's alignment, and to
     * nothing else. This is not a detail: the linker computed every
     * offset in the program from exactly this number, so rounding it
     * to something larger "to be safe" moves the whole block and every
     * access reads past its own variable. It did, when this used a
     * minimum of eight: a 4-aligned four-byte block became eight, the
     * linker addressed tp-4 and the copy went to tp-8, and each thread
     * read a correctly-private zero. */
    unsigned long talign = tpl_align ? tpl_align : 1;
    unsigned long size = align_up(tpl_memsz, talign);
    /* The thread pointer's own alignment may be stricter than the
     * template's -- it holds a pointer at offset 0 on x86-64 -- and
     * that is fine as long as it is a multiple, which keeps tp - size
     * aligned for the template too. */
    unsigned long align = talign > sizeof(void *) ? talign : sizeof(void *);
    /* Room for the block, the TCB, the alignment slack, and one word to
     * remember the malloc pointer in. */
    unsigned long total = size + LTCB_SIZE + align + sizeof(void *);
    char *raw = (char *)malloc(total);
    if (!raw)
        return NULL;
    memset(raw, 0, total);

    char *tp;
#if defined(__x86_64__)
    /* tp must be aligned, so that tp - size (the block) is too. Leave a
     * word before the block for the malloc pointer. */
    tp = (char *)align_up((unsigned long)(raw + sizeof(void *)) + size, align);
    memcpy(tp - size, tpl_image, tpl_filesz);
    *(void **)tp = tp;                     /* the self-pointer %fs:0 reads */
    ((void **)(tp - size))[-1] = raw;
#elif defined(__aarch64__)
    /* Above the pointer, past the reserved words -- and past enough of
     * them to satisfy the template's alignment, which is what the
     * linker assumed when it resolved tp + 16 for a 4-aligned block. */
    unsigned long off = align_up(LTCB_SIZE, talign);
    tp = (char *)align_up((unsigned long)(raw + sizeof(void *)), align);
    memcpy(tp + off, tpl_image, tpl_filesz);
    ((void **)tp)[-1] = raw;
#endif
    return tp;
}

void __libc_tls_free(void *tp)
{
    if (!tp)
        return;
#if defined(__x86_64__)
    unsigned long size = align_up(tpl_memsz, tpl_align ? tpl_align : 1);
    free(((void **)((char *)tp - size))[-1]);
#elif defined(__aarch64__)
    free(((void **)tp)[-1]);
#endif
}

/* Point the hardware at a block. On aarch64 the thread-pointer register
 * is writable from EL0, which is what it is for; on x86-64 the FS base
 * is not, and setting it is a syscall. */
void __libc_tls_set(void *tp)
{
#if defined(__x86_64__)
#define LARCH_SET_FS 0x1002
    lsys2(LSYS_arch_prctl, LARCH_SET_FS, (long)tp);
#elif defined(__aarch64__)
    __asm__ volatile ("msr tpidr_el0, %0" :: "r"(tp) : "memory");
#endif
}

/* Called by crt1 with the initial stack block, before anything that
 * could touch a thread-local -- which includes errno, so it runs before
 * the first syscall that could fail. */
void __libc_tls_init(long *sp)
{
    int argc = (int)sp[0];
    char **envp = (char **)(sp + 1) + argc + 1;

    /* The auxiliary vector begins after the environment's terminator. */
    char **e = envp;
    while (*e)
        e++;
    long *auxv = (long *)(e + 1);

    const struct lphdr *phdr = NULL;
    unsigned long phnum = 0, phent = sizeof(struct lphdr);
    for (long *a = auxv; a[0] != LAT_NULL; a += 2) {
        if (a[0] == LAT_PHDR)  phdr = (const struct lphdr *)a[1];
        else if (a[0] == LAT_PHNUM) phnum = (unsigned long)a[1];
        else if (a[0] == LAT_PHENT) phent = (unsigned long)a[1];
    }
    if (!phdr || !phnum)
        return;                            /* nothing to find it with */

    for (unsigned long i = 0; i < phnum; i++) {
        const struct lphdr *p =
            (const struct lphdr *)((const char *)phdr + i * phent);
        if (p->p_type != LPT_TLS)
            continue;
        tpl_image  = (const char *)(unsigned long)p->p_vaddr;
        tpl_filesz = (unsigned long)p->p_filesz;
        tpl_memsz  = (unsigned long)p->p_memsz;
        tpl_align  = (unsigned long)p->p_align;
        break;
    }
    if (!tpl_memsz)
        return;                            /* the program has no TLS */

    void *tp = __libc_tls_new();
    if (tp)
        __libc_tls_set(tp);
}
