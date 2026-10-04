/* The program the SiFive-shaped script links (RV32 and RV64). */
void puts_(const char *s);
void putn(long v);
long long big = 0x123456789abcdefLL;    /* .data, 8-aligned */
char msg[] = "riscv data";
static long zeros[64];
static const short table[] = { 10, 20, 30, 40 };
int main(void)
{
    int nz = 0, sum = 0;
    for (int i = 0; i < 64; i++) nz += zeros[i] != 0;
    for (int i = 0; i < 4; i++) sum += table[i];
    puts_(msg); puts_("\n");
    putn((long)(big >> 32)); putn((long)(big & 0xffff)); putn(nz); putn(sum);
    puts_("\n");
    return 0;
}
