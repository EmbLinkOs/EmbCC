/* `extern void sym;`, a linker symbol only taken the address of (GNU C),
 * here the address of an object defined under that assembler name. */
// expect-exit: 42
extern void sect_start;
extern const void sect_const;
char storage[4] __asm__("sect_start") = "abc";
const char cstorage[2] __asm__("sect_const") = "q";
void *p = &sect_start;
int main(void)
{
    const void *q = &sect_const;
    return p == (void *)storage && ((const char *)q)[0] == 'q' &&
           ((char *)p)[2] == 'c' ? 42 : 1;
}
