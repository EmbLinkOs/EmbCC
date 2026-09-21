/* Bit-fields of a packed struct that cross their type's storage unit (a
 * 16-bit field at bit 29 of an `unsigned`): gcc packs them bit by bit and
 * reads and writes them whole; so must EmbCC — byte by byte, never past
 * the field's last byte. Signed ones sign-extend. */
// expect-exit: 42
#include <string.h>

struct __attribute__((packed)) P {
    char c[3];
    unsigned : 5;
    unsigned a : 16;
    unsigned b : 3;
    int s : 12;
    unsigned long long big : 40;
};

int main(void)
{
    struct P p;
    memset(&p, 0, sizeof p);
    p.a = 0xFFFF;
    p.b = 5;
    p.s = -300;
    p.big = 0x12345678ABULL;
    unsigned char *raw = (unsigned char *)&p;
    int ok = sizeof p == 13 && raw[3] == 0xe0 && raw[4] == 0xff &&
             raw[5] == 0xbf && p.a == 0xFFFF && p.b == 5 && p.s == -300 &&
             p.big == 0x12345678ABULL;
    p.a += 2;                               /* wraps to 1 */
    p.s++;
    ok = ok && p.a == 1 && p.s == -299 && p.b == 5 && p.big == 0x12345678ABULL;
    return ok ? 42 : 1;
}
