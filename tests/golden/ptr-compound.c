/* Pointer compound assignment -- `p += n` and `p -= n` -- in loops shaped so the pointers live in
 * registers at -O2. The IR once made each of these a 64-bit add on every
 * target, a four-byte pointer read as eight bytes; see ptr-compound.sh. */
void writec(int c);
void puts_(const char *s);
typedef unsigned u32;
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static unsigned char buf[256];
static u32 words[64];

__attribute__((noinline)) u32 walk_up(const unsigned char *p, int n, int step)
{
    u32 h = 7;
    while (n-- > 0) { h = h * 31 + *p; p += step; }
    return h;
}
__attribute__((noinline)) u32 walk_down(const u32 *p, int n)
{
    u32 h = 3;
    p += n - 1;
    while (n-- > 0) { h ^= *p + (h << 5); p -= 1; }
    return h;
}
__attribute__((noinline)) u32 copy_words(unsigned char *d, const unsigned char *s, u32 n)
{
    while (n >= 4) { *(u32 *)d = *(const u32 *)s; d += 4; s += 4; n -= 4; }
    while (n--) *d++ = *s++;
    return (u32)d[-1];
}
int main(void)
{
    for (int i = 0; i < 256; i++) buf[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < 64; i++) words[i] = (u32)i * 2654435761u;
    hx(walk_up(buf, 60, 3));
    hx(walk_up(buf + 250, 40, -6));
    hx(walk_down(words, 64));
    hx(copy_words(buf + 128, buf, 127));
    puts_("\n==END==\n");
    return 0;
}
