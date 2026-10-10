/* __attribute__((packed)) on an enum: the smallest integer type that holds
 * every value, unsigned when none is negative -- GCC's rule, and how
 * firmware keeps a state field in one byte: `typedef enum
 * __attribute__((packed)) { IDLE, RUN } state_t;`. EmbCC refused it ("a
 * packed or aligned enum is not supported"). Both spellings GCC reads are
 * here: the attribute before the body, and right after it.
 */
// expect-exit: 42
#include <stddef.h>

typedef enum __attribute__((packed)) { IDLE, RUN, STOP } state_t;
typedef enum { NEG = -3, POS = 100 } __attribute__((packed)) small_t;
enum __attribute__((__packed__)) wide { W0 = 300, W1 };
typedef enum { M0 = -1000, M1 = 1000 } __attribute__((packed)) mid_t;
typedef enum { PLAIN0, PLAIN1 } plain_t;

struct rec {
    char tag;
    state_t s;
    small_t k;
    enum wide w;
    mid_t m;
};

static volatile int vz;

static int classify(state_t s)
{
    switch (s) {
    case IDLE: return 1;
    case RUN: return 2;
    default: return 3;
    }
}

int main(void)
{
    int r = 0;
    if (sizeof(state_t) == 1 && sizeof(small_t) == 1) r += 10;
    if (sizeof(enum wide) == (sizeof(int) > 2 ? 2 : sizeof(int))) r += 5;
    if (sizeof(plain_t) == sizeof(int)) r += 5;
    if (offsetof(struct rec, s) == 1 && offsetof(struct rec, k) == 2) r += 5;
    small_t k = NEG;
    k = (small_t)(k + vz);
    if ((int)k == -3 && k < 0) r += 4;         /* signed char */
    state_t s = STOP;
    s = (state_t)(s + vz);
    if (classify(s) == 3 && classify((state_t)(RUN + vz)) == 2) r += 4;
    struct rec x = { 'a', RUN, POS, W1, M0 };
    x.m = (mid_t)(x.m + vz);
    if (x.w == 301 && x.m == -1000 && x.k == 100) r += 9;
    return r;
}
