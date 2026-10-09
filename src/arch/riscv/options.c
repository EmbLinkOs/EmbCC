/* RISC-V's command-line options: -march= and -mabi=, recorded while the
 * arguments are read and settled together once all are
 * (riscv_options_done), since either may come first. The driver asks
 * through the backend registry (src/arch/backends.c, `option` and
 * `options_done`); this moved here from the driver. */
#include "../backend.h"
#include "../target.h"
#include "../../driver/util.h"

#include <stdlib.h>
#include <string.h>

/* RISC-V's -march= and -mabi=, as recorded while parsing. */
static const char *g_rv_march, *g_rv_mabi;

/* What -march= and -mabi= mean together, decided once every argument has
 * been seen, as GCC and clang spell them:
 *
 *   -march=rv32i<exts>[_zicsr][_zifencei] or rv64..., the base `i` or `g`
 *       (imafd_zicsr_zifencei), then single-letter extensions in any
 *       order. EmbCC's code needs M and A (the multiply and the atomics it
 *       emits); F and D are the FPU the backend may use; C is the
 *       compressed encodings. Anything else is refused by name.
 *   -mabi=ilp32|ilp32f|ilp32d for RV32, lp64|lp64f|lp64d for RV64: where
 *       floating point travels across a call (the psABI's integer,
 *       single and double hardware-float conventions). Without -mabi= it
 *       follows -march= as clang's does: D gives the double ABI, F alone
 *       the single one, neither the integer one.
 *
 * The default is today's: rv32imac/ilp32 and rv64imac/lp64. */
void riscv_options_done(void)
{
    /* GCC's bare-metal riscv64-unknown-elf toolchain is multilib: it
     * builds RV32 under -march=rv32..., which is how a CMake toolchain
     * file written for it (and the SiFive and xPack toolchains' docs)
     * spells an RV32 build. So for that freestanding triple -march=rv32
     * picks RV32. The other way is not GCC's -- a riscv32 toolchain is
     * RV32 only -- and a hosted triple (Linux) is one width; both keep
     * the refusal below. */
    if (g_rv_march && target_os_get() == TGT_OS_NONE &&
        target_get() == TARGET_RISCV64 && strncmp(g_rv_march, "rv32", 4) == 0)
        target_set(TARGET_RISCV32);
    int rv64 = target_get() == TARGET_RISCV64;
    int f = 0, d = 0, c = 1, m = 1, a = 1, zifencei = 0, abi;
    if (!rv64 && target_get() != TARGET_RISCV32)
        return;
    if (g_rv_march) {
        const char *p = g_rv_march, *want = rv64 ? "rv64" : "rv32";
        if (strncmp(p, "rv32", 4) != 0 && strncmp(p, "rv64", 4) != 0)
            diag_fatal(NULL, 0, "-march=%s is not a RISC-V ISA string: it "
                       "starts with rv32 or rv64, then i or g", g_rv_march);
        if (strncmp(p, want, 4) != 0)
            diag_fatal(NULL, 0, "-march=%s is a %.4s ISA, and %s is %s: use "
                       "--target=riscv%.2s-unknown-elf", g_rv_march, p,
                       target_triple_now(), want, p + 2);
        p += 4;
        c = m = a = 0;
        if (*p == 'g') {
            f = d = m = a = zifencei = 1;
        } else if (*p == 'e') {
            diag_fatal(NULL, 0, "-march=%s: the E base (16 registers, "
                       "ilp32e) is not supported; EmbCC emits the I base",
                       g_rv_march);
        } else if (*p != 'i') {
            diag_fatal(NULL, 0, "-march=%s: the base ISA after %s is i or g",
                       g_rv_march, want);
        }
        for (p++; *p && *p != '_'; p++) {
            switch (*p) {
            case 'm': m = 1; break;
            case 'a': a = 1; break;
            case 'f': f = 1; break;
            case 'd': d = 1; break;
            case 'c': c = 1; break;
            default:
                diag_fatal(NULL, 0, "-march=%s: the '%c' extension is not "
                           "supported: EmbCC emits I, M, A, F, D and C "
                           "(and no version numbers)", g_rv_march, *p);
            }
        }
        while (*p == '_') {
            const char *e = ++p;
            size_t n;
            while (*p && *p != '_')
                p++;
            n = (size_t)(p - e);
            if (n == 5 && strncmp(e, "zicsr", 5) == 0)
                continue;          /* the CSR instructions: implied by F */
            if (n == 8 && strncmp(e, "zifencei", 8) == 0) {
                zifencei = 1;
                continue;
            }
            diag_fatal(NULL, 0, "-march=%s: the '%.*s' extension is not "
                       "supported: EmbCC accepts zicsr and zifencei after "
                       "the single-letter ones", g_rv_march, (int)n, e);
        }
        if (d && !f)
            diag_fatal(NULL, 0, "-march=%s: the D extension needs F (double "
                       "precision is built on the single-precision "
                       "registers): add f", g_rv_march);
        if (!m)
            diag_fatal(NULL, 0, "-march=%s: EmbCC needs the M extension -- "
                       "its code multiplies and divides with mul and div, "
                       "and has no __mulsi3 path: add m", g_rv_march);
        if (!a)
            diag_fatal(NULL, 0, "-march=%s: EmbCC needs the A extension -- "
                       "its atomics are lr/sc and the amo instructions, and "
                       "it has no __atomic_* library path: add a",
                       g_rv_march);
    }
    abi = d ? 64 : f ? 32 : 0;
    if (g_rv_mabi) {
        const char *v = g_rv_mabi, *base = rv64 ? "lp64" : "ilp32";
        size_t bn = strlen(base);
        if ((rv64 && !strncmp(v, "ilp32", 5)) ||
            (!rv64 && !strncmp(v, "lp64", 4)))
            diag_fatal(NULL, 0, "-mabi=%s is a %s ABI, and %s is %s", v,
                       rv64 ? "32-bit" : "64-bit", target_triple_now(),
                       rv64 ? "RV64 (lp64, lp64f, lp64d)"
                            : "RV32 (ilp32, ilp32f, ilp32d)");
        if (strncmp(v, base, bn) != 0 ||
            (v[bn] && (v[bn + 1] || (v[bn] != 'f' && v[bn] != 'd'))))
            diag_fatal(NULL, 0, "-mabi=%s is not supported for %s: EmbCC "
                       "emits %s, %sf and %sd%s", v, target_triple_now(),
                       base, base, base,
                       v[bn] == 'e' ? " (not the E base's)" : "");
        abi = v[bn] == 'd' ? 64 : v[bn] == 'f' ? 32 : 0;
        if (abi > (d ? 64 : f ? 32 : 0))
            diag_fatal(NULL, 0, "-mabi=%s passes %s in floating-point "
                       "registers, and -march=%s has no %s extension: use "
                       "-march=%s%s", v, abi == 64 ? "doubles" : "floats",
                       g_rv_march ? g_rv_march : rv64 ? "rv64imac"
                                                      : "rv32imac",
                       abi == 64 ? "D" : "F", rv64 ? "rv64" : "rv32",
                       abi == 64 ? "imafdc" : "imafc");
    }
    target_set_riscv_isa(f, d, c, zifencei);
    target_set_riscv_abi_flen(abi);
}

int riscv_target_option(const char *arg)
{
    if (strncmp(arg, "-mabi=", 6) != 0 && strncmp(arg, "-march=", 7) != 0)
        return 0;
    /* RECORDED, and resolved once every argument has been read
     * (riscv_options_done): -mabi=ilp32f -march=rv32imafc and the other
     * order mean the same thing. */
    if (arg[3] == 'b')
        g_rv_mabi = arg + 6;
    else
        g_rv_march = arg + 7;
    return 1;
}
