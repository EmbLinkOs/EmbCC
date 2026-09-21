/* Initialized block-scope statics, of every scalar width and in nested
 * blocks. sema built the one-element initializer list for a scalar static
 * local on the stack and set only three of its five fields; a stale
 * bit_width made lower_static_bytes write the value as a BITFIELD at a stale
 * offset. What came out depended on whatever the stack held: in the
 * EmbLinkOS kernel, udp.c's ephemeral-port counter (49152) was 0, tcp.c's
 * initial sequence number (0x50000000) was 0, and dns.c's query sequence
 * (0x1000) was 0x457f. Build embcc with -ftrivial-auto-var-init=pattern to
 * make any such read deterministic — this test fails under that build
 * without the fix.
 */
// expect-exit: 42
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;

static u16 next_port(int want)
{
    if (want) {
        if (want > 0) {                       /* nested, as udp.c's is */
            static u16 eph = 49152;
            return eph++;
        }
    }
    return 0;
}

static u32 isn(void)  { static u32 v = 0x50000000u; return v += 0x10; }
static u64 big(void)  { static u64 v = 0x1122334455667788UL; return v; }
static int neg(void)  { static signed char c = -5; static short s = -300;
                        return c + s; }
static u8  byte(void) { static u8 b = 0xA5; return b; }

int main(void)
{
    if (next_port(1) != 49152 || next_port(1) != 49153) return 1;
    if (isn() != 0x50000010u) return 2;
    if (big() != 0x1122334455667788UL) return 3;
    if (neg() != -305) return 4;
    if (byte() != 0xA5) return 5;
    return 42;
}
