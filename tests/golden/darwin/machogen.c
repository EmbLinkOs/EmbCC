/* Builds a minimal Mach-O object with our writer, for the system's own
 * tools to judge.
 *
 * The program it contains is `int main(void) { return 42; }`, hand
 * assembled, because the point of this test is the CONTAINER: whether
 * ld, otool and nm accept what we wrote and whether the result runs.
 * Compiling real C into it would bring codegen into a test that is
 * about the object format, and a failure would have two suspects.
 */
#include "../../../src/macho/write.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: machogen arm64|x86_64 out.o\n");
        return 2;
    }
    int arm = strcmp(argv[1], "arm64") == 0;

    /* mov w0, #42 ; ret          — and — mov eax, 42 ; ret */
    static const unsigned char code_arm[] = {
        0x40, 0x05, 0x80, 0x52,       /* mov w0, #42 */
        0xc0, 0x03, 0x5f, 0xd6        /* ret         */
    };
    static const unsigned char code_x86[] = {
        0xb8, 0x2a, 0x00, 0x00, 0x00, /* mov eax, 42 */
        0xc3                          /* ret         */
    };
    const unsigned char *code = arm ? code_arm : code_x86;
    unsigned long long n = arm ? sizeof code_arm : sizeof code_x86;

    struct machow *w = machow_new(arm ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64,
                                  arm ? CPU_SUBTYPE_ARM64_ALL
                                      : CPU_SUBTYPE_X86_64_ALL);
    int text = machow_add_section(w, "__TEXT", "__text",
                                  S_REGULAR | S_ATTR_PURE_INSTRUCTIONS |
                                  S_ATTR_SOME_INSTRUCTIONS,
                                  code, n, 2);
    machow_add_symbol(w, "main", 0, text, 1);
    int rc = machow_write(w, argv[2]);
    machow_free(w);
    return rc != 0;
}
