/* A workload for comparing compilers on the boards: the kinds of code an
 * embedded system spends its time in, each a kernel that returns a
 * checksum of everything it computed. Built per kernel (-DKERNEL=n) and
 * per amount of work (-DN=n): tools/bench/run.sh counts a kernel at two
 * N and takes the difference, so startup, dead data and one-time setup
 * cancel. A compiler that gets faster by computing something else
 * changes the checksum. No library calls: the same object links against
 * any runtime. */

typedef unsigned int u32;
typedef unsigned char u8;

#ifndef KERNEL
#define KERNEL 0
#endif
#ifndef N
#define N 4
#endif

volatile u32 bench_result;   /* printed by tools/bench/run.sh's driver */

/* 1. CRC-32, table-driven: byte loads, shifts, table lookups. */
static u32 crc_table[256];
static u8 crc_buf[1024];
static u32 k_crc(int n)
{
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    for (int i = 0; i < 1024; i++)
        crc_buf[i] = (u8)(i * 7 + (i >> 3));
    u32 sum = 0;
    for (int r = 0; r < n * 8; r++) {
        u32 c = 0xffffffffu;
        for (int i = 0; i < 1024; i++)
            c = crc_table[(c ^ crc_buf[i]) & 0xff] ^ (c >> 8);
        sum += c ^ (u32)r;
        crc_buf[r & 1023] ^= (u8)c;
    }
    return sum;
}

/* 2. Sorting: an insertion sort for small runs inside a quicksort. */
static int sort_a[512];
static void isort(int *a, int lo, int hi)
{
    for (int i = lo + 1; i <= hi; i++) {
        int v = a[i], j = i - 1;
        while (j >= lo && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}
static void qsort_i(int *a, int lo, int hi)
{
    while (hi - lo > 12) {
        int p = a[(lo + hi) / 2], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { qsort_i(a, lo, j); lo = i; }
        else { qsort_i(a, i, hi); hi = j; }
    }
    isort(a, lo, hi);
}
static u32 k_sort(int n)
{
    u32 seed = 12345, sum = 0;
    for (int r = 0; r < n * 4; r++) {
        for (int i = 0; i < 512; i++) {
            seed = seed * 1103515245u + 12345u;
            sort_a[i] = (int)(seed >> 8) % 100000 - 50000;
        }
        qsort_i(sort_a, 0, 511);
        for (int i = 0; i < 512; i += 37)
            sum = sum * 31 + (u32)sort_a[i];
    }
    return sum;
}

/* 3. Matrix product, integers: nested loops, multiply-accumulate. */
#define MD 20
static int ma[MD][MD], mb[MD][MD], mc[MD][MD];
static u32 k_matrix(int n)
{
    u32 sum = 0;
    for (int i = 0; i < MD; i++)
        for (int j = 0; j < MD; j++) {
            ma[i][j] = (i * 3 + j) % 17 - 8;
            mb[i][j] = (i + j * 5) % 13 - 6;
        }
    for (int r = 0; r < n * 4; r++) {
        for (int i = 0; i < MD; i++)
            for (int j = 0; j < MD; j++) {
                int s = 0;
                for (int k = 0; k < MD; k++)
                    s += ma[i][k] * mb[k][j];
                mc[i][j] = s;
            }
        for (int i = 0; i < MD; i++)
            sum += (u32)mc[i][(i + r) % MD];
        ma[r % MD][(r * 7) % MD] += 1;
    }
    return sum;
}

/* 4. A linked list from a pool: pointer chasing, insertion, deletion. */
struct node { struct node *next; int key; int val; };
static struct node pool[256];
static u32 k_list(int n)
{
    u32 sum = 0;
    for (int r = 0; r < n * 6; r++) {
        struct node *head = 0;
        for (int i = 0; i < 256; i++) {
            struct node *x = &pool[i];
            x->key = (i * 97 + r) & 255;
            x->val = i;
            struct node **pp = &head;
            while (*pp && (*pp)->key < x->key)
                pp = &(*pp)->next;
            x->next = *pp;
            *pp = x;
        }
        for (struct node **pp = &head; *pp; ) {
            if ((*pp)->key % 3 == 0) *pp = (*pp)->next;
            else { sum += (u32)(*pp)->val; pp = &(*pp)->next; }
        }
    }
    return sum;
}

/* 5. A bytecode interpreter: a switch on the hot path, over a small
 * register machine (op, a, b) running a counted loop. */
enum { OP_LDI, OP_MULI, OP_ADD, OP_XORI, OP_DEC, OP_JNZ, OP_SHRI, OP_HALT };
static const u8 prog[] = {
    OP_LDI, 0, 1,       /* r0 = 1          */
    OP_LDI, 1, 200,     /* r1 = 200        */
    OP_MULI, 0, 3,      /* 6: r0 *= 3      */
    OP_ADD, 0, 1,       /*    r0 += r1     */
    OP_XORI, 0, 0x55,   /*    r0 ^= 0x55   */
    OP_SHRI, 2, 1,      /*    r2 = r0 >> 1 */
    OP_ADD, 3, 2,       /*    r3 += r2     */
    OP_DEC, 1, 0,       /*    r1--         */
    OP_JNZ, 1, 6,       /*    if (r1) goto 6 */
    OP_HALT, 0, 0,
};
static u32 k_interp(int n)
{
    u32 sum = 0;
    for (int r = 0; r < n * 16; r++) {
        u32 reg[4] = { 0, 0, 0, (u32)r };
        int pc = 0;
        for (;;) {
            u8 op = prog[pc], a = prog[pc + 1], b = prog[pc + 2];
            pc += 3;
            switch (op) {
            case OP_LDI: reg[a & 3] = b + (a == 1 ? (u32)(r & 15) : 0); break;
            case OP_MULI: reg[a & 3] *= b; break;
            case OP_ADD: reg[a & 3] += reg[b & 3]; break;
            case OP_XORI: reg[a & 3] ^= b; break;
            case OP_SHRI: reg[a & 3] = reg[0] >> b; break;
            case OP_DEC: reg[a & 3]--; break;
            case OP_JNZ: if (reg[a & 3]) pc = b; break;
            default: goto done;
            }
        }
    done:
        sum += reg[0] ^ reg[3];
    }
    return sum;
}

/* 6. A hash table with open addressing: hashing, probing, compares. */
static u32 hkeys[512], hvals[512];
static u32 k_hash(int n)
{
    u32 sum = 0;
    for (int r = 0; r < n * 4; r++) {
        for (int i = 0; i < 512; i++) hkeys[i] = 0;
        for (u32 i = 1; i < 380; i++) {
            u32 k = i * 2654435761u ^ (u32)r, h = 2166136261u;
            for (int b = 0; b < 4; b++) h = (h ^ ((k >> (b * 8)) & 255)) * 16777619u;
            u32 s = h & 511;
            while (hkeys[s] && hkeys[s] != k) s = (s + 1) & 511;
            hkeys[s] = k; hvals[s] = i;
        }
        for (u32 i = 1; i < 500; i += 3) {
            u32 k = i * 2654435761u ^ (u32)r, h = 2166136261u;
            for (int b = 0; b < 4; b++) h = (h ^ ((k >> (b * 8)) & 255)) * 16777619u;
            u32 s = h & 511;
            while (hkeys[s] && hkeys[s] != k) s = (s + 1) & 511;
            if (hkeys[s]) sum += hvals[s];
        }
    }
    return sum;
}

/* 7. Text: scanning, classifying characters, counting words and numbers. */
static const char text[] =
    "The quick brown fox, 42 times, jumped over 7 lazy dogs; then 1999 more "
    "foxes ran past 3 rivers and 256 trees before the 12th hour of day 365. ";
static u32 k_text(int n)
{
    u32 sum = 0;
    for (int r = 0; r < n * 40; r++) {
        u32 words = 0, nums = 0, val = 0, upper = 0;
        int in_word = 0;
        for (const char *p = text; *p; p++) {
            char c = *p;
            if (c >= '0' && c <= '9') { val = val * 10 + (u32)(c - '0'); }
            else if (val) { nums += val; val = 0; }
            int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (c >= 'A' && c <= 'Z') upper++;
            if (alpha && !in_word) words++;
            in_word = alpha;
        }
        sum += words * 1000 + nums + upper + (u32)r;
    }
    return sum;
}

/* 8. A state machine: a protocol parser fed byte by byte. */
static u32 k_state(int n)
{
    u32 sum = 0, seed = 99;
    for (int r = 0; r < n * 24; r++) {
        int st = 0; u32 len = 0, crc = 0, frames = 0, errors = 0;
        for (int i = 0; i < 400; i++) {
            seed = seed * 1664525u + 1013904223u;
            u8 b = (u8)(seed >> 24);
            if ((i & 31) == 0) b = 0x7e;
            switch (st) {
            case 0: if (b == 0x7e) st = 1; break;
            case 1: len = b & 15; crc = b; st = len ? 2 : 0; if (!len) errors++; break;
            case 2: crc = (crc << 1 | crc >> 31) ^ b; if (--len == 0) st = 3; break;
            case 3: if ((u8)crc == b || (b & 3) == 0) frames++; else errors++; st = 0; break;
            }
        }
        sum += frames * 7 + errors + (u32)st;
    }
    return sum;
}

/* 9. Fixed point: a Q15 FIR filter and a biquad, as a DSP loop does. */
static short fir_x[256], fir_h[16];
static u32 k_fixed(int n)
{
    u32 sum = 0;
    for (int i = 0; i < 16; i++) fir_h[i] = (short)((i * 1234 % 8000) - 4000);
    for (int r = 0; r < n * 4; r++) {
        for (int i = 0; i < 256; i++) fir_x[i] = (short)(((i * 37 + r) % 2048) * 13 - 13000);
        int y1 = 0, y2 = 0;
        for (int i = 15; i < 256; i++) {
            int acc = 0;
            for (int k = 0; k < 16; k++) acc += fir_x[i - k] * fir_h[k];
            int y = (acc >> 15) + ((y1 * 30000) >> 15) - ((y2 * 14000) >> 15);
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;
            y2 = y1; y1 = y;
            sum += (u32)y;
        }
    }
    return sum;
}

/* The floating-point kernels: what a part with an FPU (a Cortex-M4F, an
 * rv32imafc core) is bought for. Each checksum is of the results' BITS,
 * so the two compilers must round every operation identically -- no
 * contraction into fused multiply-adds (clang's -ffp-contract=off), and
 * no reassociation. On a soft-float build each operation is a call. */
static u32 fbits(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }

/* 10. A single-precision FIR filter and a biquad, as a DSP loop does. */
static float ffir_x[256], ffir_h[16];
static u32 k_ffir(int n)
{
    u32 sum = 0;
    for (int i = 0; i < 16; i++) ffir_h[i] = (float)(i * 1234 % 8000 - 4000) * (1.0f / 32768.0f);
    for (int r = 0; r < n * 2; r++) {
        for (int i = 0; i < 256; i++) ffir_x[i] = (float)((i * 37 + r) % 2048) * 0.01f - 10.0f;
        float y1 = 0.0f, y2 = 0.0f;
        for (int i = 15; i < 256; i++) {
            float acc = 0.0f;
            for (int k = 0; k < 16; k++) acc += ffir_x[i - k] * ffir_h[k];
            float y = acc + 0.915f * y1 - 0.427f * y2;
            if (y > 100.0f) y = 100.0f;
            if (y < -100.0f) y = -100.0f;
            y2 = y1; y1 = y;
            sum += fbits(y);
        }
    }
    return sum;
}

/* 11. A single-precision matrix product, 16 by 16, accumulated. */
static float fm_a[16][16], fm_b[16][16], fm_c[16][16];
static u32 k_fmatrix(int n)
{
    u32 sum = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) {
            fm_a[i][j] = (float)((i * 7 + j * 3) % 11) * 0.25f - 1.0f;
            fm_b[i][j] = (float)((i * 5 + j * 9) % 13) * 0.125f;
        }
    for (int r = 0; r < n; r++) {
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++) {
                float s = 0.0f;
                for (int k = 0; k < 16; k++) s += fm_a[i][k] * fm_b[k][j];
                fm_c[i][j] = s;
            }
        for (int i = 0; i < 16; i++) {
            sum += fbits(fm_c[i][(i + r) & 15]);
            fm_a[i][r & 15] = fm_c[i][i] * 0.0625f;
        }
    }
    return sum;
}

/* 12. Particles: positions, velocities and a 1/r falloff -- divides and
 * comparisons and conversions to integer, as a control loop has. */
static float pv_x[64], pv_y[64], pv_vx[64], pv_vy[64];
static u32 k_fvec(int n)
{
    u32 sum = 0;
    for (int i = 0; i < 64; i++) {
        pv_x[i] = (float)(i % 8) - 3.5f; pv_y[i] = (float)(i / 8) - 3.5f;
        pv_vx[i] = 0.0f; pv_vy[i] = 0.0f;
    }
    for (int r = 0; r < n * 6; r++) {
        for (int i = 0; i < 64; i++) {
            float d2 = pv_x[i] * pv_x[i] + pv_y[i] * pv_y[i] + 0.5f;
            float f = 1.0f / d2;
            pv_vx[i] = pv_vx[i] * 0.99f - pv_x[i] * f * 0.01f;
            pv_vy[i] = pv_vy[i] * 0.99f - pv_y[i] * f * 0.01f;
            pv_x[i] += pv_vx[i];
            pv_y[i] += pv_vy[i];
            if (pv_x[i] > 8.0f || pv_x[i] < -8.0f) pv_vx[i] = -pv_vx[i];
            sum += (u32)(int)(pv_x[i] * 1000.0f) + fbits(f);
        }
    }
    return sum;
}

/* 13. Double precision: a polynomial by Horner's rule and a running sum
 * -- an RV32 or M4F part with F alone does these in software. */
static u32 k_dpoly(int n)
{
    u32 sum = 0;
    double acc = 0.0;
    for (int r = 0; r < n * 40; r++) {
        double x = (double)(r % 97) * 0.03125 - 1.5;
        double p = ((((0.0083 * x - 0.041) * x + 0.166) * x - 0.5) * x + 1.0) * x;
        acc += p / (1.0 + x * x);
        union { double d; unsigned long long u; } b;
        b.d = acc;
        sum += (u32)b.u ^ (u32)(b.u >> 32);
    }
    return sum;
}

int main(void)
{
    u32 r = 0;
#if KERNEL == 1
    r = k_crc(N);
#elif KERNEL == 2
    r = k_sort(N);
#elif KERNEL == 3
    r = k_matrix(N);
#elif KERNEL == 4
    r = k_list(N);
#elif KERNEL == 5
    r = k_interp(N);
#elif KERNEL == 6
    r = k_hash(N);
#elif KERNEL == 7
    r = k_text(N);
#elif KERNEL == 8
    r = k_state(N);
#elif KERNEL == 9
    r = k_fixed(N);
#elif KERNEL == 10
    r = k_ffir(N);
#elif KERNEL == 11
    r = k_fmatrix(N);
#elif KERNEL == 12
    r = k_fvec(N);
#elif KERNEL == 13
    r = k_dpoly(N);
#endif
    bench_result = r;
    return (int)(r & 0x7f);
}
