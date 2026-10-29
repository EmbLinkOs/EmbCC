/* `extern void sym;`, a linker symbol only taken the address of (GNU C),
 * here the address of an object defined under that assembler name. An
 * asm label is the symbol as written, so it carries the target's user
 * label prefix to be the C name's symbol -- `_sect_start` on RX, whose
 * C names have one, as in GCC. */
// expect-exit: 42
#define STR_(x) #x
#define STR(x) STR_(x)
#define LABEL(n) STR(__USER_LABEL_PREFIX__) #n
extern void sect_start;
extern const void sect_const;
char storage[4] __asm__(LABEL(sect_start)) = "abc";
const char cstorage[2] __asm__(LABEL(sect_const)) = "q";
void *p = &sect_start;
int main(void)
{
    const void *q = &sect_const;
    return p == (void *)storage && ((const char *)q)[0] == 'q' &&
           ((char *)p)[2] == 'c' ? 42 : 1;
}
