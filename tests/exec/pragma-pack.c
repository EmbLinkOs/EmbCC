/* #pragma pack(n) caps every member's alignment at n for the structs
 * defined after it -- the wire formats and file headers embedded code
 * describes this way -- with push/pop and _Pragma. EmbCC refused it by
 * name, so such code had to be rewritten. The layouts are pinned by
 * static assertions gcc and clang agree with, and the packed members are
 * read and written at their odd offsets, directly, through a pointer and
 * in an array, and byte-compared with the image they must make. */
// expect-exit: 42
#include <stddef.h>
#include <stdint.h>
#pragma pack(push, 1)
struct hdr { uint8_t tag; uint32_t len; uint16_t crc; };
struct wide { uint8_t c; uint64_t v; };
#pragma pack(push, 2)
struct two { uint8_t c; uint32_t i; };
#pragma pack(pop)
struct inner { uint8_t a; uint16_t b; };
#pragma pack(pop)
struct outer { uint8_t x; struct hdr h; };
_Pragma("pack(push, 1)")
struct pr { uint8_t c; uint16_t s; };
_Pragma("pack(pop)")
struct normal { uint8_t c; uint32_t i; };
_Static_assert(sizeof(struct hdr) == 7 && offsetof(struct hdr, len) == 1 &&
               offsetof(struct hdr, crc) == 5 && _Alignof(struct hdr) == 1, "pack 1");
_Static_assert(sizeof(struct wide) == 9, "pack 1, 8-byte member");
/* (AVR aligns nothing past a byte, so pack(2) changes nothing there) */
#define A2 (_Alignof(uint32_t) < 2 ? 1 : 2)
_Static_assert(offsetof(struct two, i) == A2 && sizeof(struct two) == A2 + 4, "pack 2");
_Static_assert(sizeof(struct inner) == 3, "pop back to 1");
_Static_assert(offsetof(struct outer, h) == 1, "a packed member in a plain struct");
_Static_assert(sizeof(struct pr) == 3, "_Pragma");
_Static_assert(offsetof(struct normal, i) == _Alignof(uint32_t), "after the last pop");
static struct hdr ghdr = { 0xA5, 0x11223344, 0x5566 };
static const uint8_t gimg[7] = { 0xA5, 0x44, 0x33, 0x22, 0x11, 0x66, 0x55 };
static int same(const void *a, const void *b, int n)
{
    const uint8_t *x = a, *y = b;
    for (int i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}
static uint32_t sum(const struct hdr *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++)
        s += p[i].len + p[i].crc + p[i].tag;
    return s;
}
int main(void)
{
    int bad = 0;
    if (!same(&ghdr, gimg, 7)) bad |= 1;                  /* the static image */
    struct hdr h = { 1, 0, 0 };
    h.len = 0x01020304;
    h.crc = 0x0506;
    const uint8_t img[7] = { 1, 4, 3, 2, 1, 6, 5 };
    if (!same(&h, img, 7)) bad |= 2;                      /* stores at 1 and 5 */
    struct hdr arr[3];
    for (int i = 0; i < 3; i++) {
        arr[i].tag = (uint8_t)i;
        arr[i].len = 1000u * (uint32_t)(i + 1);
        arr[i].crc = (uint16_t)(7 + i);
    }
    if (sum(arr, 3) != 6000u + 24u + 3u) bad |= 4;        /* loads through a pointer */
    struct wide w = { 9, 0x0102030405060708ull };
    w.v += 1;
    if (w.v != 0x0102030405060709ull) bad |= 8;
    struct outer o;
    o.x = 7;
    o.h.len = 77;
    struct hdr *ph = &o.h;
    ph->crc = 3;
    if (o.h.len + o.h.crc != 80) bad |= 16;
    return bad ? bad : 42;
}
