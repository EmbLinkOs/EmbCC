/* A switch whose cases lie more than 128 KB past its table. ARMv7-M's
 * one-instruction dispatch, `tbh`, holds each case's distance as a count
 * of halfwords, so it reaches 128 KB and no further -- and large
 * functions pass that, at -O0 especially. Each case body here is a few
 * thousand volatile updates -- at -O0 the later cases are well out of
 * tbh's reach, and the switch must still dispatch to every one of them.
 * Found by compiling random programs: "a tbh entry cannot reach its
 * label". (Kept under 1 MB at -O0 on every target: aarch64's and RISC-V's
 * conditional branches stop there, and refuse rather than relax.) */
// expect-exit: 42
static volatile unsigned v;
#define S1   v = v * 3u + 1u;
#define S10  S1 S1 S1 S1 S1 S1 S1 S1 S1 S1
#define S100 S10 S10 S10 S10 S10 S10 S10 S10 S10 S10
#define S1K  S100 S100 S100 S100 S100 S100 S100 S100 S100 S100
#define BODY S1K

__attribute__((noinline)) static unsigned far(int k)
{
    v = (unsigned)k;
    switch (k) {
    case 0: BODY return v + 0;
    case 1: BODY return v + 1;
    case 2: BODY return v + 2;
    case 3: BODY return v + 3;
    case 4: BODY return v + 4;
    case 5: BODY return v + 5;
    default: return 7;
    }
}

int main(void)
{
    for (int k = 0; k < 7; k++) {
        unsigned want = (unsigned)k;
        if (k < 6) {
            for (int i = 0; i < 1000; i++) want = want * 3u + 1u;
            want += (unsigned)k;
        } else {
            want = 7;
        }
        if (far(k) != want) return 1 + k;
    }
    return 42;
}
