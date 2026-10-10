/* Multi-character constants: 'RIFF' and 'ab' are ints whose bytes are the
 * characters', the last one lowest -- GCC's and clang's value, which file
 * formats and protocol code compare against (FourCC tags, magic numbers).
 * EmbCC refused them: "a character constant holds one character". More
 * characters than an int holds keep the last ones; a 'é' is its two
 * UTF-8 bytes. On AVR an int holds two.
 */
// expect-exit: 42
#pragma GCC diagnostic ignored "-Wmultichar"

static volatile int vz;

static int tag(const unsigned char *p)
{
    return (int)((unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
                 (unsigned)p[2] << 8 | p[3]);
}

int main(void)
{
    int r = 0;
    if ('ab' == 0x6162) r += 10;
    if ('\xff\xfe' == (sizeof(int) == 2 ? -2 : 0xfffe)) r += 5;
#if __SIZEOF_INT__ >= 4
    static const unsigned char riff[4] = { 'R', 'I', 'F', 'F' };
    if ('RIFF' + vz == tag(riff)) r += 10;
    if ('\xff\xff\xff\xff' == -1) r += 5;
#else
    (void)tag;
    r += 15;
#endif
    switch ('ok' + vz) {                    /* a case label too */
    case 'ok': r += 7; break;
    default: break;
    }
    if ('\xc3\xa9' == 0xc3a9 || sizeof(int) == 2) r += 5;
    return r;
}
