#!/usr/bin/env python3
"""Random C programs for differential testing of EmbCC.

Every program is free of undefined behaviour by construction:
  - arithmetic that can overflow is done in the unsigned type of the same
    width and converted back (conversion of an out-of-range value to a
    signed type wraps on every target we compare);
  - shift counts are masked below the promoted width, and the shifted value
    is unsigned of at least 32 bits;
  - division and remainder are unsigned with a divisor forced odd (never 0),
    or signed by a positive constant;
  - array indices are masked to the (power-of-two) array length;
  - loops are counted with small constant or masked bounds;
  - no plain char, no long: only types whose size is the same on LP64 and
    ILP32 (8/16/32/64-bit), so the host's answer is every target's answer.
Each value computed is folded into a 64-bit checksum; main returns 42 when it
equals EXPECT (filled in from a host run), else 1.

usage: gen.py SEED [EXPECT]  -> program on stdout
"""
import random
import sys

TYPES = [  # name, bits, signed
    ("i8", 8, True), ("u8", 8, False), ("i16", 16, True), ("u16", 16, False),
    ("i32", 32, True), ("u32", 32, False), ("i64", 64, True), ("u64", 64, False),
]
TINFO = {t[0]: t for t in TYPES}
UNS = {8: "u8", 16: "u16", 32: "u32", 64: "u64"}


def uns_of(t):
    """The unsigned type arithmetic on t is done in: at least 32 bits, so the
    integer promotions cannot turn it signed (u8 * u8 promotes to int)."""
    return "u64" if TINFO[t][1] == 64 else "u32"


AVR = False   # set by --avr: 16-bit int, so comparisons are made explicit


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.funcs = []          # (name, rettype, [(ptype, pname)])
        self.globals = []        # (type, name, length or 0)
        self.depth = 0

    def pick_type(self):
        if AVR:   # 64-bit values are many registers and helper calls there
            return self.r.choice(TYPES[:6])[0]
        return self.r.choice(TYPES)[0]

    def const(self, t):
        bits, signed = TINFO[t][1], TINFO[t][2]
        r = self.r.random()
        if r < 0.3:
            v = self.r.choice([0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255, 256])
        elif r < 0.5:
            v = (1 << (bits - 1)) - self.r.choice([1, 2, 3])          # near max, in range
        elif r < 0.6 and signed:
            v = -(1 << (bits - 1)) + self.r.choice([0, 1, 2])         # near min
        else:
            v = self.r.getrandbits(bits)
            if signed and v >= (1 << (bits - 1)):
                v -= 1 << bits
        # spell it so every compiler reads the same value and type
        if bits == 64:
            if signed:
                if v == -(1 << 63):
                    return "((i64)(-9223372036854775807LL - 1))"
                return "((i64)%dLL)" % v
            return "((u64)%dULL)" % (v & ((1 << 64) - 1))
        if signed and v < 0:
            return "((%s)(%d))" % (t, v) if bits < 32 else (
                "((i32)(-2147483647 - 1))" if v == -(1 << 31) else "((i32)(%d))" % v)
        return "((%s)%dU)" % (t, v & ((1 << bits) - 1)) if not signed else "((%s)%d)" % (t, v)

    # ---- expressions ----------------------------------------------------
    def expr(self, t, scope, d=0):
        """An expression of type t (cast to t at the top)."""
        r = self.r.random()
        if d > (1 if AVR else 3) or r < 0.25:
            return self.leaf(t, scope)
        k = self.r.randrange(13)
        a_t = self.pick_type()
        b_t = self.pick_type()
        if k <= 3:     # + - * via unsigned
            op = "+-*"[k % 3]
            u = uns_of(t)
            return "((%s)((%s)%s %s (%s)%s))" % (t, u, self.expr(a_t, scope, d + 1), op, u,
                                                  self.expr(b_t, scope, d + 1))
        if k == 4:     # bitwise
            op = self.r.choice("&|^")
            return "((%s)(%s %s %s))" % (t, self.expr(t, scope, d + 1), op, self.expr(t, scope, d + 1))
        if k == 5:     # shift
            u = uns_of(t)
            w = 63 if u == "u64" else 31
            op = self.r.choice(["<<", ">>"])
            if op == ">>" and TINFO[t][2] and self.r.random() < 0.5:
                # arithmetic right shift of a signed value: implementation-
                # defined, arithmetic on every target compared
                st = "i64" if u == "u64" else "i32"
                return "((%s)((%s)%s >> (%s & %d)))" % (t, st, self.expr(t, scope, d + 1),
                                                        self.expr("u32", scope, d + 1), w)
            return "((%s)((%s)%s %s (%s & %d)))" % (t, u, self.expr(a_t, scope, d + 1), op,
                                                    self.expr("u32", scope, d + 1), w)
        if k == 6:     # unsigned divide / remainder, odd divisor
            u = uns_of(t)
            op = self.r.choice("/%")
            return "((%s)((%s)%s %s ((%s)%s | 1)))" % (t, u, self.expr(a_t, scope, d + 1), op, u,
                                                        self.expr(b_t, scope, d + 1))
        if k == 7:     # signed divide by a positive constant
            st = "i64" if TINFO[t][1] == 64 else "i32"
            c = self.r.choice([1, 2, 3, 5, 7, 10, 16, 100, 255, 1000])
            op = self.r.choice("/%")
            return "((%s)((%s)%s %s %d))" % (t, st, self.expr(a_t, scope, d + 1), op, c)
        if k == 8:     # comparison
            op = self.r.choice(["<", "<=", ">", ">=", "==", "!="])
            if AVR:   # both sides to one 64-bit type: no int-width conversions
                ct = self.r.choice(["i32", "u32"])
                return "((%s)((%s)%s %s (%s)%s))" % (t, ct, self.expr(a_t, scope, d + 1), op,
                                                     ct, self.expr(b_t, scope, d + 1))
            return "((%s)(%s %s %s))" % (t, self.expr(a_t, scope, d + 1), op,
                                         self.expr(b_t, scope, d + 1))
        if k == 9:     # conditional
            return "(%s ? %s : %s)" % (self.cond(scope, d + 1), self.expr(t, scope, d + 1),
                                       self.expr(t, scope, d + 1))
        if k == 10:    # unary
            u = uns_of(t)
            op = self.r.choice(["-", "~", "!"])
            return "((%s)(%s(%s)%s))" % (t, op, u, self.expr(t, scope, d + 1))
        if k == 12:    # logical
            op = self.r.choice(["&&", "||"])
            return "((%s)(%s %s %s))" % (t, self.cond(scope, d + 1), op, self.cond(scope, d + 1))
        return self.leaf(t, scope)

    def cond(self, scope, d):
        a_t, b_t = self.pick_type(), self.pick_type()
        op = self.r.choice(["<", "<=", ">", ">=", "==", "!="])
        if AVR:
            ct = self.r.choice(["i32", "u32"])
            return "((%s)%s %s (%s)%s)" % (ct, self.expr(a_t, scope, d + 1), op,
                                           ct, self.expr(b_t, scope, d + 1))
        return "(%s %s %s)" % (self.expr(a_t, scope, d + 1), op, self.expr(b_t, scope, d + 1))

    def leaf(self, t, scope):
        r = self.r.random()
        vars_ = [v for v in scope if v[2] == 0]
        arrs = [v for v in scope if v[2] > 0]
        if r < 0.45 and vars_:
            vt, vn, _ = self.r.choice(vars_)
            return "((%s)%s)" % (t, vn)
        if r < 0.7 and arrs:
            at, an, n = self.r.choice(arrs)
            return "((%s)%s[%s & %d])" % (t, an, self.index(scope), n - 1)
        return self.const(t)

    def index(self, scope):
        vars_ = [v for v in scope if v[2] == 0]
        if vars_ and self.r.random() < 0.8:
            vt, vn, _ = self.r.choice(vars_)
            return "(u32)%s" % vn
        return "%dU" % self.r.randrange(64)

    # ---- statements -----------------------------------------------------
    def lvalue_pool(self, scope):
        return self.lvalue_scope(scope)

    def lvalue_scope(self, scope):
        return [v for v in scope if not v[1].startswith("p")]

    def lvalue(self, scope):
        cands = self.lvalue_scope(scope)
        vt, vn, n = self.r.choice(cands)
        if n:
            return vt, "%s[%s & %d]" % (vn, self.index(scope), n - 1)
        return vt, vn

    def stmt(self, scope, ind, loopvars):
        pad = "    " * ind
        r = self.r.random()
        if self.depth > (1 if AVR else 3):
            r = r * 0.45
        if r < 0.35:
            t, lv = self.lvalue(scope)
            return "%s%s = %s;\n" % (pad, lv, self.expr(t, scope))
        if r < 0.45:
            t, lv = self.lvalue(scope)
            u = uns_of(t)
            op = self.r.choice("+-*&|^")
            return "%s%s = (%s)((%s)%s %s (%s)%s);\n" % (pad, lv, t, u, lv, op, u,
                                                       self.expr(t, scope))
        if r < 0.5:
            t = self.pick_type()
            return "%smix((u64)%s);\n" % (pad, self.expr(t, scope))
        if r < 0.55 and self.funcs:
            # a call only as a whole statement, with pure arguments: the
            # order operands and arguments are evaluated in is unspecified,
            # and the callees write globals and the checksum
            name, rt, params = self.r.choice(self.funcs)
            args = ", ".join(self.expr(pt, scope, 2) for pt, _ in params)
            plain = [v for v in self.lvalue_pool(scope) if v[2] == 0]
            if plain and self.r.random() < 0.5:
                vt, vn, _ = self.r.choice(plain)
                return "%s%s = (%s)%s(%s);\n" % (pad, vn, vt, name, args)
            return "%smix((u64)%s(%s));\n" % (pad, name, args)
        self.depth += 1
        try:
            if r < 0.62:
                s = "%sif %s {\n" % (pad, self.cond(scope, 1))
                s += self.block(scope, ind + 1, loopvars)
                if self.r.random() < 0.6:
                    s += "%s} else {\n" % pad + self.block(scope, ind + 1, loopvars)
                return s + "%s}\n" % pad
            if r < 0.78:
                lv = "i%d" % len(loopvars)
                lt = self.r.choice(["i32", "u32", "i64", "u8", "i16"])
                lo = self.r.randrange(0, 4)
                hi = self.r.choice(["%d" % self.r.randrange(0, 40),
                                    "(%s & 31)" % self.expr("u32", scope, 2)])
                step = self.r.choice([1, 1, 1, 2, 3])
                cmpop = self.r.choice(["<", "<", "!="]) if step == 1 else "<"
                s = "%sfor (%s %s = %d; %s %s (%s)%s; %s += %d) {\n" % (
                    pad, lt, lv, lo, lv, cmpop, lt, hi, lv, step)
                if cmpop == "!=":   # a != loop must reach its bound: a constant
                    s = "%sfor (%s %s = %d; %s != (%s)%d; %s++) {\n" % (
                        pad, lt, lv, lo, lv, lt, lo + self.r.randrange(0, 30), lv)
                inner = scope + [(lt, lv, 0)]
                s += self.block(inner, ind + 1, loopvars + [lv], noassign=lv)
                return s + "%s}\n" % pad
            if r < 0.86:
                n = self.r.choice([4, 8])
                s = "%sswitch ((u32)%s & %d) {\n" % (pad, self.expr("u32", scope, 1), n - 1)
                for c in sorted(self.r.sample(range(n), self.r.randrange(1, n))):
                    s += "%scase %d:\n" % (pad, c) + self.block(scope, ind + 1, loopvars)
                    if self.r.random() < 0.8:
                        s += "%s    break;\n" % pad
                s += "%sdefault:\n" % pad + self.block(scope, ind + 1, loopvars)
                return s + "%s}\n" % pad
            if r < 0.93 and any(v[2] > 0 for v in scope):
                at, an, n = self.r.choice([v for v in scope if v[2] > 0])
                k = self.r.randrange(0, n + 1)
                pv = "p%d" % self.depth
                acc = "acc%d" % self.depth
                s = "%s{ %s *%s; u64 %s = 0;\n" % (pad, at, pv, acc)
                s += "%s  for (%s = %s; %s != %s + %d; %s++) %s = %s * 31 + (u64)*%s;\n" % (
                    pad, pv, an, pv, an, k, pv, acc, acc, pv)
                s += "%s  mix(%s); }\n" % (pad, acc)
                return s
            # while with a counter
            cv = "w%d" % self.depth
            s = "%s{ u32 %s = (%s & 15);\n" % (pad, cv, self.expr("u32", scope, 1))
            s += "%s  while (%s--) {\n" % (pad, cv)
            s += self.block(scope + [("u32", cv, 0)], ind + 2, loopvars, noassign=cv)
            return s + "%s  } }\n" % pad
        finally:
            self.depth -= 1

    def block(self, scope, ind, loopvars, noassign=None):
        sc = [v for v in scope if v[1] != noassign] if noassign else scope
        # loop counters may be read but never written inside their loop
        readable = scope
        out = ""
        for _ in range(self.r.randrange(1, 4)):
            st = self.stmt_with(sc, readable, ind, loopvars)
            out += st
        return out

    def stmt_with(self, writable, readable, ind, loopvars):
        # statements write only `writable`, read from `readable`
        saved, saved_s = self.lvalue, self.lvalue_scope
        def lv(scope, _w=writable):
            return saved(_w)
        def lvs(scope, _w=writable):
            return saved_s(_w)
        self.lvalue, self.lvalue_scope = lv, lvs
        try:
            return self.stmt(readable, ind, loopvars)
        finally:
            self.lvalue, self.lvalue_scope = saved, saved_s

    # ---- whole program --------------------------------------------------
    def func(self, k):
        rt = self.pick_type()
        params = [(self.pick_type(), "a%d" % i) for i in range(self.r.randrange(0, 5))]
        locs = [(self.pick_type(), "l%d" % i, 0) for i in range(self.r.randrange(1, 5))]
        arr = []
        if self.r.random() < 0.5:
            arr = [(self.pick_type(), "la", self.r.choice([4, 8, 16]))]
        scope = [(t, n, 0) for t, n in params] + locs + arr + \
                [(t, n, ln) for t, n, ln in self.globals]
        s = "__attribute__((noinline)) static %s f%d(%s)\n{\n" % (
            rt, k, ", ".join("%s %s" % p for p in params) or "void")
        for t, n, _ in locs:
            s += "    %s %s = %s;\n" % (t, n, self.const(t))
        for t, n, ln in arr:
            s += "    %s %s[%d];\n" % (t, n, ln)
            s += "    for (u32 z = 0; z < %d; z++) %s[z] = (%s)(z * %s + %s);\n" % (
                ln, n, t, self.const("u32"), self.const("u32"))
        body_scope = [v for v in scope]
        for _ in range(self.r.randrange(2, 4) if AVR else self.r.randrange(2, 7)):
            s += self.stmt(body_scope, 1, [])
        for t, n, _ in locs:
            s += "    mix((u64)%s);\n" % n
        s += "    return %s;\n}\n" % self.expr(rt, scope)
        self.funcs.append(("f%d" % k, rt, [(t, n) for t, n in params]))
        return s

    def program(self, expect):
        out = ["/* generated by fuzz/gen.py */",
               "typedef signed char i8; typedef unsigned char u8;",
               "typedef short i16; typedef unsigned short u16;",
               "#ifdef __AVR__",
               "typedef long i32; typedef unsigned long u32;",
               "#else",
               "typedef int i32; typedef unsigned int u32;",
               "#endif",
               "typedef long long i64; typedef unsigned long long u64;",
               "u64 ck = 1469598103934665603ULL;",
               "static void mix(u64 v) { ck = (ck ^ v) * 1099511628211ULL; ck ^= ck >> 29; }",
               "static volatile u32 vzero = 0;"]
        for i in range(self.r.randrange(1, 4)):
            t = self.pick_type()
            n = self.r.choice([0, 0, 4, 8, 16])
            self.globals.append((t, "g%d" % i, n))
            if n:
                out.append("static %s g%d[%d];" % (t, i, n))
            else:
                out.append("static %s g%d;" % (t, i))
        for k in range(self.r.randrange(1, 3) if AVR else self.r.randrange(2, 6)):
            out.append(self.func(k))
        out.append("int main(void)\n{")
        for t, n, ln in self.globals:
            if ln:
                out.append("    for (u32 z = 0; z < %d; z++) %s[z] = (%s)(z * 2654435761U + vzero);" % (ln, n, t))
            else:
                out.append("    %s = (%s)(%s + vzero);" % (n, t, self.const(t)))
        for name, rt, params in self.funcs:
            for _ in range(self.r.randrange(1, 3)):
                args = ", ".join("(%s)(%s + vzero)" % (pt, self.const(pt)) for pt, _ in params)
                out.append("    mix((u64)%s(%s));" % (name, args))
        for t, n, ln in self.globals:
            if ln:
                out.append("    for (u32 z = 0; z < %d; z++) mix((u64)%s[z]);" % (ln, n))
            else:
                out.append("    mix((u64)%s);" % n)
        if expect is None:
            out.append("    return 0;  /* the host driver prints ck */")
        else:
            out.append("    return ck == %sULL ? 42 : 1;" % expect)
        out.append("}")
        return "\n".join(out) + "\n"


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--avr"]
    AVR = "--avr" in sys.argv
    seed = int(args[0])
    expect = args[1] if len(args) > 1 else None
    sys.stdout.write(Gen(seed).program(expect))
