/* Hands EmbCC's whole VFP vocabulary to llvm-mc and compares the bytes.
 *
 * A hand-written encoder with no referee is a guess, and VFP is the
 * worst place for one: the register-number split is opposite for singles
 * and doubles, so encoding a double the single way names a DIFFERENT
 * register and produces a perfectly valid instruction. Nothing but a
 * byte comparison catches that.
 *
 * `--list` writes the assembly, `bytes` writes what emit.c produced.
 * The test feeds the first to llvm-mc and cmp's the two, which is the
 * same arrangement tools/rvasmcheck uses for RISC-V.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/thumb/emit.h"

int main(int argc, char **argv)
{
    int list = argc > 1 && !strcmp(argv[1], "--list");
    struct code c = { 0 };
    char buf[64];

    /* The vocabulary, written once. Each entry prints its own assembly
     * and emits its own bytes, so the two cannot drift apart. */
#define P(FMT, ...)  do { if (list) printf(FMT "\n", __VA_ARGS__); } while (0)
#define E(CALL)      do { if (!list) { CALL; } } while (0)

    for (int dbl = 0; dbl < 2; dbl++) {
        const char *w = dbl ? "64" : "32";
        const char *r = dbl ? "d" : "s";
        /* three-register: vary the operands so every field and flag
         * takes both values */
        int trip[4][3] = { {0,1,2}, {3,4,5}, {6,7,8}, {9,10,11} };
        for (int k = 0; k < 4; k++) {
            int d = trip[k][0], n = trip[k][1], m = trip[k][2];
            P("vadd.f%s %s%d, %s%d, %s%d", w, r, d, r, n, r, m);
            E(t_vadd(&c, d, n, m, dbl));
            P("vsub.f%s %s%d, %s%d, %s%d", w, r, d, r, n, r, m);
            E(t_vsub(&c, d, n, m, dbl));
            P("vmul.f%s %s%d, %s%d, %s%d", w, r, d, r, n, r, m);
            E(t_vmul(&c, d, n, m, dbl));
            P("vdiv.f%s %s%d, %s%d, %s%d", w, r, d, r, n, r, m);
            E(t_vdiv(&c, d, n, m, dbl));
            P("vfma.f%s %s%d, %s%d, %s%d", w, r, d, r, n, r, m);
            E(t_vfma(&c, d, n, m, dbl));
        }
        int pair[5][2] = { {0,1}, {2,3}, {5,4}, {10,11}, {12,13} };
        for (int k = 0; k < 5; k++) {
            int d = pair[k][0], m = pair[k][1];
            P("vmov.f%s %s%d, %s%d", w, r, d, r, m);
            E(t_vmov_reg(&c, d, m, dbl));
            P("vabs.f%s %s%d, %s%d", w, r, d, r, m);
            E(t_vabs(&c, d, m, dbl));
            P("vneg.f%s %s%d, %s%d", w, r, d, r, m);
            E(t_vneg(&c, d, m, dbl));
            P("vsqrt.f%s %s%d, %s%d", w, r, d, r, m);
            E(t_vsqrt(&c, d, m, dbl));
            P("vcmp.f%s %s%d, %s%d", w, r, d, r, m);
            E(t_vcmp(&c, d, m, dbl));
            P("vcmpe.f%s %s%d, %s%d", w, r, d, r, m);
            E(t_vcmpe(&c, d, m, dbl));
        }
        /* the conversions, both directions and both signednesses. The
         * INTEGER side of a conversion is always a single register even
         * when the float side is a double, which is why the register
         * letter differs across the comma here and nowhere else. */
        for (int k = 0; k < 4; k++) {
            int d = k * 3, m = k * 3 + 1;
            snprintf(buf, sizeof buf, "vcvt.f%s.s32 %s%d, s%d", w, r, d, m);
            P("%s", buf); E(t_vcvt_f_from_i(&c, d, m, 1, dbl));
            snprintf(buf, sizeof buf, "vcvt.f%s.u32 %s%d, s%d", w, r, d, m);
            P("%s", buf); E(t_vcvt_f_from_i(&c, d, m, 0, dbl));
            snprintf(buf, sizeof buf, "vcvt.s32.f%s s%d, %s%d", w, d, r, m);
            P("%s", buf); E(t_vcvt_i_from_f(&c, d, m, 1, dbl));
            snprintf(buf, sizeof buf, "vcvt.u32.f%s s%d, %s%d", w, d, r, m);
            P("%s", buf); E(t_vcvt_i_from_f(&c, d, m, 0, dbl));
        }
        /* loads and stores: the offset is in words, so a byte count
         * that is not a multiple of four cannot be encoded and is not
         * offered here. Negative offsets take the U bit's other value. */
        int offs[5] = { 0, 4, 8, 252, -8 };
        for (int k = 0; k < 5; k++) {
            int o = offs[k], rn = k + 1, sd = k * 2;
            if (o < 0) P("vldr %s%d, [r%d, #%d]", r, sd, rn, o);
            else       P("vldr %s%d, [r%d, #%d]", r, sd, rn, o);
            E(t_vldst(&c, sd, rn, o, dbl, 0));
            P("vstr %s%d, [r%d, #%d]", r, sd, rn, o);
            E(t_vldst(&c, sd, rn, o, dbl, 1));
        }
    }
    /* core <-> FP, which exists at one width each: a single pairs with
     * one core register, a double with two. */
    for (int k = 0; k < 4; k++) {
        int sn = k * 5, rt = k + 1;
        P("vmov s%d, r%d", sn, rt);  E(t_vmov_core(&c, sn, rt, 1));
        P("vmov r%d, s%d", rt, sn);  E(t_vmov_core(&c, sn, rt, 0));
    }
    for (int k = 0; k < 3; k++) {
        int dm = k * 4, rt = k + 1, rt2 = k + 5;
        P("vmov d%d, r%d, r%d", dm, rt, rt2);
        E(t_vmov_core_pair(&c, dm, rt, rt2, 1));
        P("vmov r%d, r%d, d%d", rt, rt2, dm);
        E(t_vmov_core_pair(&c, dm, rt, rt2, 0));
    }
    /* Into a d register, one core register may supply both words -- how a
     * double whose halves are equal (0.0) is built. The other way round
     * the architecture makes that UNPREDICTABLE, so it is not offered. */
    P("vmov d%d, r12, r12", 9);  E(t_vmov_core_pair(&c, 9, 12, 12, 1));
    P("vmrs apsr_nzcv, fpscr%s", "");  E(t_vmrs_apsr(&c));
    /* the callee-saved block, at the counts a prologue uses */
    for (int n = 2; n <= 16; n += 2) {
        P("vpush {s16-s%d}", 15 + n);  E(t_vpush_s(&c, 16, n, 0));
        P("vpop {s16-s%d}", 15 + n);   E(t_vpush_s(&c, 16, n, 1));
    }
    /* ...and the same block by its D names, one register to all eight,
     * which is what a function keeping doubles in d8-d15 saves */
    for (int n = 1; n <= 8; n++) {
        if (n == 1) { P("vpush {d8}%s", ""); P("vpop {d8}%s", ""); }
        else { P("vpush {d8-d%d}", 7 + n); P("vpop {d8-d%d}", 7 + n); }
        E(t_vpush_d(&c, 8, n, 0)); E(t_vpush_d(&c, 8, n, 1));
    }
    /* Between the widths, both ways, every parity on each side: the
     * double and the single are split by different rules in the SAME
     * instruction, as in the integer conversions above. */
    for (int k = 0; k < 6; k++) {
        int dd = (k * 5) % 16, ss = (k * 7 + 1) % 32;
        P("vcvt.f64.f32 d%d, s%d", dd, ss);  E(t_vcvt_f_f(&c, dd, ss, 1));
        P("vcvt.f32.f64 s%d, d%d", ss, dd);  E(t_vcvt_f_f(&c, ss, dd, 0));
    }
    /* The immediate moves: all 256 constants at both widths, printed as
     * the DECIMAL value llvm-mc must turn back into the same byte -- so
     * t_vfp_imm8's expansion is checked as well as the packing. */
    for (int dbl = 0; dbl < 2; dbl++)
        for (int k = 0; k < 256; k++) {
            union { double d; unsigned long long u; } x;
            union { float f; unsigned u; } y;
            int reg = (k * 3) % (dbl ? 16 : 32), back;
            /* VFPExpandImm written out again here, so that llvm-mc, which
             * reads the decimal and finds its own byte, is the referee of
             * both this and t_vfp_imm8 */
            unsigned long long bits = 0;
            if (dbl) {
                unsigned b = (k >> 6) & 1u;
                bits = (unsigned long long)(k >> 7) << 63 |
                       (unsigned long long)!b << 62 |
                       (b ? 0xffULL : 0ULL) << 54 |
                       (unsigned long long)((k >> 4) & 3) << 52 |
                       (unsigned long long)(k & 15) << 48;
                x.u = bits;
                /* with a point: `#2` would be read as the encoded byte */
                snprintf(buf, sizeof buf, "%.17g", x.d);
                P("vmov.f64 d%d, #%s%s", reg, buf,
                  strpbrk(buf, ".e") ? "" : ".0");
            } else {
                unsigned b = (k >> 6) & 1u;
                bits = (unsigned long long)(k >> 7) << 31 |
                       (unsigned long long)!b << 30 |
                       (b ? 0x1fULL : 0ULL) << 25 |
                       (unsigned long long)((k >> 4) & 3) << 23 |
                       (unsigned long long)(k & 15) << 19;
                y.u = (unsigned)bits;
                snprintf(buf, sizeof buf, "%.9g", (double)y.f);
                P("vmov.f32 s%d, #%s%s", reg, buf,
                  strpbrk(buf, ".e") ? "" : ".0");
            }
            back = t_vfp_imm8(bits, dbl);
            if (back != k) {
                fprintf(stderr, "t_vfp_imm8 finds %d for constant %d\n",
                        back, k);
                return 1;
            }
            E(t_vmov_imm(&c, reg, back, dbl));
        }
    if (t_vfp_imm8(0, 1) != -1 || t_vfp_imm8(0x3fb999999999999aULL, 1) != -1) {
        fprintf(stderr, "t_vfp_imm8 claims 0.0 or 0.1 is an immediate\n");
        return 1;
    }
#undef P
#undef E

    if (!list)
        fwrite(c.p, 1, (size_t)c.len, stdout);
    return 0;
}
