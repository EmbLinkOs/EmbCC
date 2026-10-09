#!/usr/bin/env python3
"""Random C programs, version 3: version 2 plus __int128 operands (for the
64-bit targets only -- x86-64, aarch64, RV64). Every 128-bit value is
mixed into the checksum by BOTH halves (mix_w). Version 2: integers AND floating point, structs by
value, bitfields, unions, pointers into arrays and structs, and calls
through function-pointer tables.

No undefined behaviour by construction (see gen.py for the integer rules):
  - float->int conversions go through range-checked helpers;
  - floating division never divides by zero (the divisor is forced away
    from it), and NaN results are canonicalised before they reach the
    checksum, since x86 and ARM produce different NaN bit patterns;
  - struct padding is never read: only fields are mixed;
  - pointers stay inside their arrays (indices masked to len - 2 when a
    neighbour is read through the pointer);
  - bitfields are assigned through masks, so no out-of-range store;
  - the host reference is built with -ffp-contract=off, so it never fuses
    a multiply and an add where a target would not.
Plain char and long are still avoided; only fixed-size types are used.

usage: gen2.py SEED [EXPECT]
"""
import random
import sys

INTS = [("i8", 8, True), ("u8", 8, False), ("i16", 16, True), ("u16", 16, False),
        ("i32", 32, True), ("u32", 32, False), ("i64", 64, True), ("u64", 64, False),
        ("i128", 128, True), ("u128", 128, False)]
FLTS = ["f32", "f64"]
INFO = {t[0]: t for t in INTS}


def uns(t):
    return {64: "u64", 128: "u128"}.get(INFO[t][1], "u32")


def mixer(t, e):
    """The statement-level mix of an integer expression: both halves of a
    128-bit one."""
    if t in INFO and INFO[t][1] == 128:
        return "mix_w((u128)%s)" % e
    return "mix_i((u64)%s)" % e


class G:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.funcs = []      # (name, ret, [(type, name)]) -- scalar functions
        self.sfuncs = []     # functions taking/returning structs
        self.globals = []    # (type, name, n)
        self.ftab = None     # (name, n, sig)

    # ---- types --------------------------------------------------------
    def ity(self):
        return self.r.choice(INTS)[0]

    def any_scalar(self):
        return self.r.choice(FLTS) if self.r.random() < 0.25 else self.ity()

    # ---- constants ----------------------------------------------------
    def iconst(self, t):
        bits, sg = INFO[t][1], INFO[t][2]
        r = self.r.random()
        if r < 0.35:
            v = self.r.choice([0, 1, 2, 3, 7, 15, 16, 31, 63, 100, 127, 255])
        elif r < 0.5:
            v = (1 << (bits - 1)) - self.r.choice([1, 2, 3])
        else:
            v = self.r.getrandbits(bits)
            if sg and v >= (1 << (bits - 1)):
                v -= 1 << bits
        if bits == 128:
            u = v & ((1 << 128) - 1)
            return "((%s)(((u128)%dULL << 64) | %dULL))" % (t, u >> 64, u & ((1 << 64) - 1))
        if bits == 64:
            return "((i64)%dLL)" % v if sg else "((u64)%dULL)" % (v & ((1 << 64) - 1))
        if sg and v < 0:
            return "((%s)(%d))" % (t, v) if bits < 32 else (
                "((i32)(-2147483647 - 1))" if v == -(1 << 31) else "((i32)(%d))" % v)
        return "((%s)%dU)" % (t, v & ((1 << bits) - 1)) if not sg else "((%s)%d)" % (t, v)

    def fconst(self, t):
        v = self.r.choice(["0.0", "1.0", "-1.0", "0.5", "-0.25", "3.0", "10.0",
                           "1e10", "-1e10", "1e-10", "123.456", "-7.125",
                           "65536.0", "2.5e-5", "1e30", "-0.0", "0.1",
                           "4294967296.0", "1e-300" if t == "f64" else "1e-30"])
        return "((%s)%s%s)" % (t, v, "f" if t == "f32" else "")

    def const(self, t):
        return self.fconst(t) if t in FLTS else self.iconst(t)

    # ---- expressions --------------------------------------------------
    def expr(self, t, sc, d=0):
        return self.fexpr(t, sc, d) if t in FLTS else self.iexpr(t, sc, d)

    def leaf(self, t, sc):
        r = self.r.random()
        scal = [v for v in sc if v[2] == 0 and v[0] in INFO or v[0] in FLTS and v[2] == 0]
        arrs = [v for v in sc if v[2] > 0]
        structs = [v for v in sc if v[2] < 0]
        if r < 0.4 and scal:
            vt, vn, _ = self.r.choice(scal)
            return self.conv(t, vt, vn)
        if r < 0.6 and arrs:
            at, an, n = self.r.choice(arrs)
            return self.conv(t, at, "%s[%s & %d]" % (an, self.idx(sc), n - 1))
        if r < 0.8 and structs:
            st, sn, _ = self.r.choice(structs)
            ft, fe = self.r.choice(self.fields(st))
            return self.conv(t, ft, "%s.%s" % (sn, fe))
        return self.const(t)

    def conv(self, to, frm, e):
        """A conversion with no undefined behaviour."""
        if to in FLTS or frm not in FLTS:
            return "((%s)%s)" % (to, e)
        # float -> integer: through a helper that checks the range
        if INFO[to][1] == 128:
            return "((%s)f2%s_128(%s))" % (to, "i" if INFO[to][2] else "u", e)
        return "((%s)f2i_%s(%s))" % (to, "64" if INFO[to][1] == 64 else "32", e)

    def idx(self, sc):
        iv = [v for v in sc if v[2] == 0 and v[0] in INFO]
        if iv and self.r.random() < 0.8:
            return "(u32)%s" % self.r.choice(iv)[1]
        return "%dU" % self.r.randrange(64)

    def iexpr(self, t, sc, d):
        if d > 3 or self.r.random() < 0.25:
            return self.leaf(t, sc)
        k = self.r.randrange(12)
        a, b = self.any_scalar(), self.any_scalar()
        u = uns(t)
        if k <= 2:
            op = "+-*"[k]
            return "((%s)((%s)%s %s (%s)%s))" % (t, u, self.iexpr(t, sc, d + 1), op, u,
                                                 self.iexpr(t, sc, d + 1))
        if k == 3:
            return "((%s)(%s %s %s))" % (t, self.iexpr(t, sc, d + 1), self.r.choice("&|^"),
                                         self.iexpr(t, sc, d + 1))
        if k == 4:
            w = {"u64": 63, "u128": 127}.get(u, 31)
            return "((%s)((%s)%s %s (%s & %d)))" % (t, u, self.iexpr(t, sc, d + 1),
                                                    self.r.choice(["<<", ">>"]),
                                                    self.iexpr("u32", sc, d + 1), w)
        if k == 5:
            return "((%s)((%s)%s %s ((%s)%s | 1)))" % (t, u, self.iexpr(t, sc, d + 1),
                                                        self.r.choice("/%"), u,
                                                        self.iexpr(t, sc, d + 1))
        if k == 6:   # a comparison, integer or floating
            return "((%s)(%s %s %s))" % (t, self.expr(a, sc, d + 1),
                                         self.r.choice(["<", "<=", ">", ">=", "==", "!="]),
                                         self.expr(b, sc, d + 1))
        if k == 7:
            return "(%s ? %s : %s)" % (self.cond(sc, d), self.iexpr(t, sc, d + 1),
                                       self.iexpr(t, sc, d + 1))
        if k == 8:   # from floating point
            f = self.r.choice(FLTS)
            return self.conv(t, f, self.fexpr(f, sc, d + 1))
        if k == 9:
            return "((%s)(%s(%s)%s))" % (t, self.r.choice(["-", "~", "!"]), u,
                                         self.iexpr(t, sc, d + 1))
        if k == 10 and self.ptrs(sc):
            pt, pn, n = self.r.choice(self.ptrs(sc))
            # through conv: the element may be a float, out of range
            return self.conv(t, pt, "%s[%d]" % (pn, self.r.randrange(2)))
        return self.leaf(t, sc)

    def fexpr(self, t, sc, d):
        if d > 3 or self.r.random() < 0.3:
            return self.leaf(t, sc)
        k = self.r.randrange(8)
        if k <= 2:
            op = "+-*"[k]
            return "((%s)(%s %s %s))" % (t, self.fexpr(t, sc, d + 1), op,
                                         self.fexpr(t, sc, d + 1))
        if k == 3:   # division away from zero
            return "((%s)(%s / nz_%s(%s)))" % (t, self.fexpr(t, sc, d + 1), t,
                                               self.fexpr(t, sc, d + 1))
        if k == 4:
            return "((%s)(-%s))" % (t, self.fexpr(t, sc, d + 1))
        if k == 5:   # from an integer
            it = self.ity()
            return "((%s)%s)" % (t, self.iexpr(it, sc, d + 1))
        if k == 6:
            return "(%s ? %s : %s)" % (self.cond(sc, d), self.fexpr(t, sc, d + 1),
                                       self.fexpr(t, sc, d + 1))
        o = "f64" if t == "f32" else "f32"
        return "((%s)%s)" % (t, self.fexpr(o, sc, d + 1))

    def cond(self, sc, d):
        a, b = self.any_scalar(), self.any_scalar()
        return "(%s %s %s)" % (self.expr(a, sc, d + 1),
                               self.r.choice(["<", "<=", ">", ">=", "==", "!="]),
                               self.expr(b, sc, d + 1))

    def ptrs(self, sc):
        return [v for v in sc if v[1].startswith("pp")]

    # ---- structs ------------------------------------------------------
    STRUCTS = {
        "SA": [("i8", "a"), ("u32", "b"), ("f64", "c"), ("i16", "d")],
        "SB": [("u64", "x"), ("f32", "y"), ("u8", "z"), ("i32", "w")],
        "SC": [("u16", "p"), ("u16", "q")],
    }
    BITS = [("u32", "b3", 3, False), ("i32", "s5", 5, True), ("u32", "b12", 12, False),
            ("i32", "s7", 7, True)]

    def fields(self, st):
        if st == "SBF":
            return [(t, n) for t, n, _, _ in self.BITS]
        return self.STRUCTS[st]

    def mix_struct(self, st, e):
        return "".join("    mix_%s(%s.%s);\n" % ("f" if t in FLTS else "i", e, n)
                       if t in FLTS else "    mix((u64)%s.%s);\n" % (e, n)
                       for t, n in self.fields(st))

    # ---- statements ---------------------------------------------------
    def assign(self, sc, ind, writable):
        pad = "    " * ind
        cands = [v for v in writable if not v[1].startswith("pp")]
        vt, vn, n = self.r.choice(cands)
        if n > 0:
            return "%s%s[%s & %d] = %s;\n" % (pad, vn, self.idx(sc), n - 1, self.expr(vt, sc))
        if n < 0:
            if vt == "SBF":
                ft, fe, bits, sg = self.r.choice(self.BITS)
                mask = (1 << bits) - 1
                if sg:   # store a value inside the field's range
                    return "%s%s.%s = (i32)((u32)%s & %dU) - %d;\n" % (
                        pad, vn, fe, self.expr("u32", sc), mask, 1 << (bits - 1))
                return "%s%s.%s = (u32)%s & %dU;\n" % (pad, vn, fe, self.expr("u32", sc), mask)
            ft, fe = self.r.choice(self.fields(vt))
            return "%s%s.%s = %s;\n" % (pad, vn, fe, self.expr(ft, sc))
        return "%s%s = %s;\n" % (pad, vn, self.expr(vt, sc))

    def stmt(self, sc, ind, writable, depth):
        pad = "    " * ind
        r = self.r.random()
        if depth > 2:
            r *= 0.5
        if r < 0.38:
            return self.assign(sc, ind, writable)
        if r < 0.46:
            t = self.any_scalar()
            if t in FLTS:
                return "%smix_f(%s);\n" % (pad, self.expr(t, sc))
            return "%s%s;\n" % (pad, mixer(t, self.expr(t, sc)))
        if r < 0.52 and self.funcs:
            name, rt, ps = self.r.choice(self.funcs)
            args = ", ".join(self.expr(pt, sc, 2) for pt, _ in ps)
            if rt in FLTS:
                return "%smix_f(%s(%s));\n" % (pad, name, args)
            return "%s%s;\n" % (pad, mixer(rt, "%s(%s)" % (name, args)))
        if r < 0.56 and self.sfuncs:
            name, rt, pt = self.r.choice(self.sfuncs)
            locs = [v for v in writable if v[2] < 0 and v[0] == pt]
            if locs:
                vt, vn, _ = self.r.choice(locs)
                rl = [v for v in writable if v[2] < 0 and v[0] == rt]
                if rl:
                    dst = self.r.choice(rl)[1]
                    return "%s%s = %s(%s, %s);\n" % (pad, dst, name, vn, self.expr("i32", sc, 2))
        if r < 0.6 and self.ftab:
            tn, n, (rt, pts) = self.ftab
            args = ", ".join(self.expr(pt, sc, 2) for pt in pts)
            return "%smix((u64)%s[(u32)%s & %d](%s));\n" % (pad, tn, self.expr("u32", sc, 2),
                                                            n - 1, args)
        if r < 0.64:
            ss = [v for v in writable if v[2] < 0 and v[0] != "SBF"]
            if len(ss) >= 2:
                a, b = self.r.sample(ss, 2)
                if a[0] == b[0]:
                    return "%s%s = %s;\n" % (pad, a[1], b[1])
        if r < 0.72:
            s = "%sif %s {\n" % (pad, self.cond(sc, 1))
            s += self.block(sc, ind + 1, writable, depth + 1)
            if self.r.random() < 0.5:
                s += "%s} else {\n" % pad + self.block(sc, ind + 1, writable, depth + 1)
            return s + "%s}\n" % pad
        if r < 0.84:
            lv = "i%d" % depth
            lt = self.r.choice(["i32", "u32", "i64", "u8"])
            hi = self.r.choice(["%d" % self.r.randrange(0, 24),
                                "(%s & 15)" % self.expr("u32", sc, 2)])
            s = "%sfor (%s %s = 0; %s < (%s)%s; %s++) {\n" % (pad, lt, lv, lv, lt, hi, lv)
            s += self.block(sc + [(lt, lv, 0)], ind + 1, writable, depth + 1)
            return s + "%s}\n" % pad
        if r < 0.88:
            # A sentinel walk by index (see gen2.py): the test reads memory
            # at the index it steps.
            arrs = [v for v in sc if v[2] > 1 and v[0] not in FLTS and v[0] != "f128"]
            if arrs:
                at, an, n = self.r.choice(arrs)
                step = self.r.choice([1, 1, 2])
                it = self.r.choice(["i32", "u32", "i64", "u8"])
                bn, wi = "wb%d" % depth, "wi%d" % depth
                sent = self.iconst(at)
                s = "%s{ %s %s[%d];\n" % (pad, at, bn, n + 2)
                s += "%s  for (u32 z = 0; z < %d; z++) %s[z] = %s[z];\n" % (pad, n, bn, an)
                s += "%s  %s[%d] = %s; %s[%d] = %s;\n" % (pad, bn, n, sent, bn, n + 1, sent)
                s += "%s  %s %s = 0;\n" % (pad, it, wi)
                k = self.r.random()
                if k < 0.3:
                    s += "%s  while (%s[%s] != %s) %s += %d;\n" % (pad, bn, wi, sent, wi, step)
                elif k < 0.55:
                    # a copy whose test is the stored value: the header has a store
                    cn = "wc%d" % depth
                    s += "%s  %s %s[%d] = { 0 };\n" % (pad, at, cn, n + 2)
                    s += "%s  while ((%s[%s] = %s[%s]) != %s) %s += %d;\n" % (
                        pad, cn, wi, bn, wi, sent, wi, step)
                    s += "%s  for (u32 z = 0; z < (u32)%s; z++) mix((u64)%s[z]);\n" % (pad, wi, cn)
                else:
                    s += "%s  while (%s[%s] != %s) { mix((u64)%s[%s]); %s += %d; }\n" % (
                        pad, bn, wi, sent, bn, wi, wi, step)
                s += "%s  mix((u64)%s); }\n" % (pad, wi)
                return s
        if r < 0.92:
            arrs = [v for v in sc if v[2] > 1]
            if arrs:
                at, an, n = self.r.choice(arrs)
                pv = "pp%d" % depth
                s = "%s{ %s *%s = &%s[%s & %d];\n" % (pad, at, pv, an, self.idx(sc), n - 2)
                inner = sc + [(at, pv, 2)]   # two valid elements: pp[0], pp[1]
                s += "%s  %s[1] = %s;\n" % (pad, pv, self.expr(at, inner, 2))
                s += "%s  mix_%s(%s[0] %s %s[1]); }\n" % (
                    pad, "f" if at in FLTS else "i",
                    pv, "+" if at in FLTS else "^", pv) if at in FLTS else \
                    "%s  mix((u64)%s[0] ^ (u64)%s[1]); }\n" % (pad, pv, pv)
                return s
        n = self.r.choice([4, 8])
        s = "%sswitch ((u32)%s & %d) {\n" % (pad, self.expr("u32", sc, 1), n - 1)
        for c in sorted(self.r.sample(range(n), self.r.randrange(1, n))):
            s += "%scase %d:\n" % (pad, c) + self.block(sc, ind + 1, writable, depth + 1)
            if self.r.random() < 0.8:
                s += "%s    break;\n" % pad
        s += "%sdefault:\n" % pad + self.block(sc, ind + 1, writable, depth + 1)
        return s + "%s}\n" % pad

    def block(self, sc, ind, writable, depth):
        return "".join(self.stmt(sc, ind, writable, depth)
                       for _ in range(self.r.randrange(1, 4)))

    # ---- functions ----------------------------------------------------
    def body(self, params, rt, k):
        locs = [(self.any_scalar(), "l%d" % i, 0) for i in range(self.r.randrange(1, 5))]
        arr = []
        if self.r.random() < 0.6:
            arr = [(self.any_scalar(), "la", self.r.choice([4, 8, 16]))]
        sl = []
        for i in range(self.r.randrange(0, 3)):
            sl.append((self.r.choice(["SA", "SB", "SC", "SBF"]), "ls%d" % i, -1))
        sc = params + locs + arr + sl + self.globals
        s = ""
        # /*K*/ marks a line the reducer must keep: deleting a declaration or
        # its initialisation would leave a read of uninitialised memory
        for t, n, _ in locs:
            s += "    %s %s = %s; /*K*/\n" % (t, n, self.const(t))
        for t, n, ln in arr:
            s += "    %s %s[%d]; /*K*/\n" % (t, n, ln)
            s += "    for (u32 z = 0; z < %d; z++) %s[z] = (%s)(z * %s + %s); /*K*/\n" % (
                ln, n, t, self.iconst("u32"), self.iconst("u32"))
        for t, n, _ in sl:
            s += "    struct %s %s; /*K*/\n" % (t, n)
            for ft, fe in self.fields(t):
                if t == "SBF":
                    s += "    %s.%s = 0; /*K*/\n" % (n, fe)
                else:
                    s += "    %s.%s = %s; /*K*/\n" % (n, fe, self.const(ft))
        writable = [v for v in sc]
        for _ in range(self.r.randrange(2, 6)):
            s += self.stmt(sc, 1, writable, 0)
        for t, n, _ in locs:
            s += "    %s;\n" % ("mix_f(%s)" % n if t in FLTS else mixer(t, n))
        for t, n, _ in sl:
            s += self.mix_struct(t, n)
        return s, sc

    def scalar_func(self, k):
        rt = self.any_scalar()
        params = [(self.any_scalar(), "a%d" % i, 0) for i in range(self.r.randrange(0, 5))]
        s = "__attribute__((noinline)) static %s f%d(%s)\n{\n" % (
            rt, k, ", ".join("%s %s" % (t, n) for t, n, _ in params) or "void")
        b, sc = self.body(params, rt, k)
        s += b + "    return %s;\n}\n" % self.expr(rt, sc)
        self.funcs.append(("f%d" % k, rt, [(t, n) for t, n, _ in params]))
        return s

    def struct_func(self, k):
        pt = self.r.choice(["SA", "SB", "SC"])
        rt = self.r.choice(["SA", "SB", "SC"])
        params = [(pt, "sp", -1), ("i32", "ka", 0)]
        s = "__attribute__((noinline)) static struct %s g%d(struct %s sp, i32 ka)\n{\n" % (rt, k, pt)
        b, sc = self.body(params, rt, k)
        s += b + "    struct %s ret; /*K*/\n" % rt
        for ft, fe in self.fields(rt):
            s += "    ret.%s = %s; /*K*/\n" % (fe, self.expr(ft, sc))
        s += "    return ret;\n}\n"
        self.sfuncs.append(("g%d" % k, rt, pt))
        return s

    def program(self, expect):
        out = ["/* generated by fuzz/gen2.py */",
               "typedef signed char i8; typedef unsigned char u8;",
               "typedef short i16; typedef unsigned short u16;",
               "typedef int i32; typedef unsigned int u32;",
               "typedef long long i64; typedef unsigned long long u64;",
               "typedef float f32; typedef double f64;",
               "u64 ck = 1469598103934665603ULL;",
               "static void mix(u64 v) { ck = (ck ^ v) * 1099511628211ULL; ck ^= ck >> 29; }",
               "static void mix_i(u64 v) { mix(v); }",
               "static void mix_f(f64 d) { union { f64 d; u64 u; } x; if (d != d) { mix(0x7ff8000000000000ULL); return; } x.d = d; mix(x.u); }  /* one NaN */",
               "static i32 f2i_32(f64 d) { return (d > -2147483648.0 && d < 2147483647.0) ? (i32)d : 0; }",
               "static i64 f2i_64(f64 d) { return (d > -9.2e18 && d < 9.2e18) ? (i64)d : 0; }",
               "typedef __int128 i128; typedef unsigned __int128 u128;",
               "static void mix_w(u128 v) { mix((u64)v); mix((u64)(v >> 64)); }",
               "static i128 f2i_128(f64 d) { return (d > -1.7e38 && d < 1.7e38) ? (i128)d : 0; }",
               "static u128 f2u_128(f64 d) { return (d >= 0.0 && d < 3.4e38) ? (u128)d : 0; }",
               "static f32 nz_f32(f32 v) { return (v > -1e-20f && v < 1e-20f) ? 1.5f : v; }",
               "static f64 nz_f64(f64 v) { return (v > -1e-200 && v < 1e-200) ? 1.5 : v; }",
               "static volatile u32 vzero = 0;"]
        for st, fl in self.STRUCTS.items():
            out.append("struct %s { %s };" % (st, " ".join("%s %s;" % f for f in fl)))
        out.append("struct SBF { %s };" % " ".join(
            "%s %s : %d;" % (("unsigned" if not sg else "signed"), n, b)
            for t, n, b, sg in self.BITS))
        for i in range(self.r.randrange(1, 4)):
            t = self.any_scalar()
            n = self.r.choice([0, 0, 4, 8, 16])
            self.globals.append((t, "gv%d" % i, n))
            out.append("static %s gv%d%s;" % (t, i, "[%d]" % n if n else ""))
        self.globals.append(("SA", "gs", -1))
        out.append("static struct SA gs;")
        k = 0
        for _ in range(self.r.randrange(2, 5)):
            out.append(self.scalar_func(k)); k += 1
        for _ in range(self.r.randrange(1, 3)):
            out.append(self.struct_func(k)); k += 1
        # a function-pointer table: functions of one signature
        sig = ("u32", ["u32", "i32"])
        tn = []
        for j in range(4):
            name = "h%d" % j
            body = "__attribute__((noinline)) static u32 %s(u32 x, i32 y) { return %s; }" % (
                name, self.iexpr("u32", [("u32", "x", 0), ("i32", "y", 0)], 1))
            out.append(body)
            tn.append(name)
        out.append("static u32 (*const htab[4])(u32, i32) = { %s };" % ", ".join(tn))
        self.ftab = ("htab", 4, sig)
        out.append(self.scalar_func(k)); k += 1
        out.append("int main(void)\n{")
        for t, n, ln in self.globals:
            if ln > 0:
                out.append("    for (u32 z = 0; z < %d; z++) %s[z] = (%s)(z * 2654435761U + vzero);" % (ln, n, t))
            elif ln == 0:
                out.append("    %s = (%s)(%s + vzero);" % (n, t, self.const(t)))
        out.append("    gs.a = 3; gs.b = 77u + vzero; gs.c = 2.5; gs.d = -9;")
        for name, rt, ps in self.funcs:
            args = ", ".join("(%s)(%s + vzero)" % (pt, self.const(pt)) for pt, _ in ps)
            out.append("    %s;" % ("mix_f(%s(%s))" % (name, args) if rt in FLTS
                                    else mixer(rt, "%s(%s)" % (name, args))))
        for name, rt, pt in self.sfuncs:
            out.append("    { struct %s in = { 0 }; struct %s o;" % (pt, rt))
            for ft, fe in self.fields(pt):
                out.append("      in.%s = %s;" % (fe, self.const(ft)))
            out.append("      o = %s(in, (i32)(5 + vzero));" % name)
            out.append(self.mix_struct(rt, "o").replace("\n", " ") + " }")
        for t, n, ln in self.globals:
            if ln > 0:
                out.append("    for (u32 z = 0; z < %d; z++) %s;" % (
                    ln, "mix_f(%s[z])" % n if t in FLTS else mixer(t, "%s[z]" % n)))
            elif ln == 0:
                out.append("    %s;" % ("mix_f(%s)" % n if t in FLTS else mixer(t, n)))
        out.append(self.mix_struct("SA", "gs").rstrip())
        out.append("    return %s;" % ("0" if expect is None else
                                       "ck == %sULL ? 42 : 1" % expect))
        out.append("}")
        return "\n".join(out) + "\n"


if __name__ == "__main__":
    seed = int(sys.argv[1])
    sys.stdout.write(G(seed).program(sys.argv[2] if len(sys.argv) > 2 else None))
