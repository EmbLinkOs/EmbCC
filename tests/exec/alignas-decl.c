/* _Alignas is a declaration specifier: it applies to every declarator
 * of the declaration, in any position among the specifiers. At file
 * scope EmbCC dropped it before anything read it, so a 64-byte-aligned
 * buffer -- for DMA, or a cache line of its own -- was laid out at its
 * type's alignment, silently; in a block only the first declarator got
 * it. Each object here follows an odd-sized one, so no base address can
 * align them all by luck. */
// expect-exit: 42
#include <stdint.h>
char p1 = 1;
_Alignas(64) int a1 = 1;
char p2 = 2;
int _Alignas(32) a2 = 2;
char p3 = 3;
static _Alignas(16) char a3[3] = { 3 };
char p4 = 4;
_Alignas(64) static int a4 = 4, a5 = 5;
int main(void)
{
    char q = 1;
    _Alignas(16) char l1 = 1, l2 = 2;
    int bad = 0;
    if ((uintptr_t)&a1 % 64) bad |= 1;
    if ((uintptr_t)&a2 % 32) bad |= 2;
    if ((uintptr_t)a3 % 16) bad |= 4;
    if ((uintptr_t)&a4 % 64 || (uintptr_t)&a5 % 64) bad |= 8;
    if ((uintptr_t)&l1 % 16 || (uintptr_t)&l2 % 16) bad |= 16;
    return bad ? bad : 42 + (p1 + p2 + p3 + p4 + q + a1 + a2 + a3[0] + a4 + a5 + l1 + l2) * 0;
}
