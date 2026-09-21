/* A minimal COFF object, written by src/coff/write.c and read by
 * whatever the host has. The point is that nothing downstream is ours:
 * objdump, llvm-readobj and ld each parse this independently.
 *
 * The program it contains returns 42 from main, and calls an undefined
 * external so there is a REL32 relocation to check -- a relocation is
 * where a format writer is most likely to be wrong in a way that still
 * links.
 */
#include <stdio.h>
#include <string.h>
#include "write.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: coffgen OUT.obj\n");
        return 2;
    }
    struct coffw *w = coffw_new(IMAGE_FILE_MACHINE_AMD64);

    /*  0: 48 83 ec 28          sub  $0x28,%rsp       (shadow space)
     *  4: e8 00 00 00 00       call helper           <- REL32 at 5
     *  9: b8 2a 00 00 00       mov  $42,%eax
     *  e: 48 83 c4 28          add  $0x28,%rsp
     * 12: c3                   ret
     */
    static const unsigned char text[] = {
        0x48, 0x83, 0xec, 0x28,
        0xe8, 0x00, 0x00, 0x00, 0x00,
        0xb8, 0x2a, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xc4, 0x28,
        0xc3,
    };
    static const unsigned char data[] = { 7, 0, 0, 0 };

    int t = coffw_add_section(w, ".text",
                              IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                              IMAGE_SCN_MEM_READ,
                              text, sizeof text, 16);
    int d = coffw_add_section(w, ".data",
                              IMAGE_SCN_CNT_INITIALIZED_DATA |
                              IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE,
                              data, sizeof data, 4);
    int b = coffw_add_section(w, ".bss",
                              IMAGE_SCN_CNT_UNINITIALIZED_DATA |
                              IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE,
                              NULL, 16, 8);

    /* A defined external, a static, an undefined external, and a name
     * too long for the eight-byte field -- which is the case that
     * exercises the string table and its two different spellings. */
    coffw_add_symbol(w, "main", 0, t, IMAGE_SYM_DTYPE_FUNCTION,
                     IMAGE_SYM_CLASS_EXTERNAL);
    coffw_add_symbol(w, "counter", 0, d, IMAGE_SYM_TYPE_NULL,
                     IMAGE_SYM_CLASS_STATIC);
    coffw_add_symbol(w, "scratch", 0, b, IMAGE_SYM_TYPE_NULL,
                     IMAGE_SYM_CLASS_STATIC);
    int hs = coffw_add_symbol(w, "helper", 0, IMAGE_SYM_UNDEFINED,
                              IMAGE_SYM_DTYPE_FUNCTION,
                              IMAGE_SYM_CLASS_EXTERNAL);
    coffw_add_symbol(w, "a_name_far_longer_than_eight_bytes", 0, t,
                     IMAGE_SYM_TYPE_NULL, IMAGE_SYM_CLASS_EXTERNAL);

    coffw_add_reloc(w, t, 5, hs, IMAGE_REL_AMD64_REL32);

    if (coffw_write(w, argv[1]) != 0)
        return 1;
    coffw_free(w);
    return 0;
}
