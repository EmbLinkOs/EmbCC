// expect-exit: 42
/* A comparison's 0 or 1 as a VALUE, for every predicate, signed and
 * unsigned, between registers and against constants -- 0, the ones an
 * add or subtract immediate holds (+-1..7), a byte, past a byte, -1,
 * INT_MIN and INT_MAX -- and a float's and a double's, over values at
 * the edges: checked against the same comparison made as a BRANCH, which
 * lowers to a compare and a conditional jump instead. ARMv6-M makes the
 * value without a branch (v6m.c's cmp_set and zero_cc: the carry of a
 * subtraction, the sign), which is where a wrong carry would show. */
#include <limits.h>

#define NI __attribute__((noinline))

#define PREDS(X) X(eq, ==) X(ne, !=) X(lt, <) X(le, <=) X(gt, >) X(ge, >=)

#define VAL(T, name, op)                                                    \
    NI static int v_##T##_##name(T a, T b) { return a op b; }               \
    NI static int b_##T##_##name(T a, T b) { if (a op b) return 1; return 0; }
typedef unsigned uns;
#define VAL_I(name, op) VAL(int, name, op) VAL(uns, name, op)
PREDS(VAL_I)
#define VAL_F(name, op) VAL(float, name, op) VAL(double, name, op)
PREDS(VAL_F)

/* against a constant: the constant is the comparison's immediate */
#define KV(T, name, op, tag, K)                                             \
    NI static int k_##T##_##name##_##tag(T a) { return a op (T)(K); }       \
    NI static int kb_##T##_##name##_##tag(T a) { if (a op (T)(K)) return 1; return 0; }
#define KS(T, tag, K)                                                       \
    KV(T, eq, ==, tag, K) KV(T, ne, !=, tag, K) KV(T, lt, <, tag, K)        \
    KV(T, le, <=, tag, K) KV(T, gt, >, tag, K) KV(T, ge, >=, tag, K)
#define KT(tag, K) KS(int, tag, K) KS(uns, tag, K)
KT(z, 0) KT(p1, 1) KT(p7, 7) KT(m1, -1) KT(m7, -7) KT(p8, 8) KT(p200, 200)
KT(p300, 300) KT(m200, -200) KT(imin, INT_MIN) KT(imax, INT_MAX)

static const int vals[] = {
    0, 1, -1, 2, -2, 6, 7, 8, -7, -8, 199, 200, 201, 255, 256, 299, 300, 301,
    -199, -200, -201, INT_MAX, INT_MAX - 1, INT_MIN, INT_MIN + 1, 0x7f, 0x80,
};
#define NV ((int)(sizeof vals / sizeof vals[0]))
static const float fvals[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.25f, -3.25f,
                               1e30f, -1e30f };
#define NF ((int)(sizeof fvals / sizeof fvals[0]))

static int bad;
static void chk(int v, int b) { if (v != b || (v != 0 && v != 1)) bad++; }

int main(void)
{
    for (int i = 0; i < NV; i++) {
        int a = vals[i];
        uns u = (uns)a;
        for (int j = 0; j < NV; j++) {
            int b = vals[j];
            uns w = (uns)b;
#define RR(name, op)                                                        \
            chk(v_int_##name(a, b), b_int_##name(a, b));                    \
            chk(v_uns_##name(u, w), b_uns_##name(u, w));
            PREDS(RR)
        }
#define K1(T, x, tag)                                                       \
        chk(k_##T##_eq_##tag(x), kb_##T##_eq_##tag(x));                     \
        chk(k_##T##_ne_##tag(x), kb_##T##_ne_##tag(x));                     \
        chk(k_##T##_lt_##tag(x), kb_##T##_lt_##tag(x));                     \
        chk(k_##T##_le_##tag(x), kb_##T##_le_##tag(x));                     \
        chk(k_##T##_gt_##tag(x), kb_##T##_gt_##tag(x));                     \
        chk(k_##T##_ge_##tag(x), kb_##T##_ge_##tag(x));
#define KK(tag) K1(int, a, tag) K1(uns, u, tag)
        KK(z) KK(p1) KK(p7) KK(m1) KK(m7) KK(p8) KK(p200) KK(p300) KK(m200)
        KK(imin) KK(imax)
    }
    for (int i = 0; i < NF; i++)
        for (int j = 0; j < NF; j++) {
            float x = fvals[i], y = fvals[j];
            double dx = x, dy = y;
#define FF(name, op)                                                        \
            chk(v_float_##name(x, y), b_float_##name(x, y));                \
            chk(v_double_##name(dx, dy), b_double_##name(dx, dy));
            PREDS(FF)
        }
    /* and a few the branch form cannot get wrong the same way */
    if (v_uns_lt(0u, 0xffffffffu) != 1 || v_uns_gt(0xffffffffu, 0u) != 1 ||
        k_uns_ge_m1(0xffffffffu) != 1 || k_uns_gt_m1(0xffffffffu) != 0 ||
        k_int_lt_z(INT_MIN) != 1 || k_int_ge_z(INT_MAX) != 1 ||
        k_uns_le_imax(0x80000000u) != 0)
        bad++;
    return bad ? 1 : 42;
}
