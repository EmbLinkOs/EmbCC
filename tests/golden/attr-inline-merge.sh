#!/bin/sh
# noinline and always_inline count on ANY declaration, as GCC takes them:
# a header declares a function plainly and its definition carries the
# attribute. EmbCC read only the first declaration, so a definition
#
#     int flashfn(int x);                                  (the header)
#     __attribute__((noinline)) int flashfn(int x) { ... }
#
# was inlined anyway -- into a function placed in RAM, which then ran the
# copy instead of calling the code the program kept in flash -- and an
# always_inline definition after a plain prototype, too big for the
# inliner's own judgement at -Os, was called.
set -u
echo "TEST-MARKER attr-inline-merge"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/attr-inline-merge
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
# body OBJ FN: FN's disassembly with its relocations -- a call is named
# there whether the assembler resolved it or left it to the linker
body() {
    "${EMBCC_LLVM_OBJDUMP:-llvm-objdump}" -dr "$1" | sed -n "/<$2>:/,/^\$/p" | sed 1d
}
cat > "$out/ni.c" <<'X'
int flashfn(int x);
int ramfn(int x) { return flashfn(x) + 1; }
__attribute__((noinline)) int flashfn(int x) { return x * 2; }
X
cat > "$out/ai.c" <<'X'
int big(int *p, int n);
int caller1(int *p) { return big(p, 7) + 1; }
int caller2(int *p) { return big(p, 9) * 3; }
__attribute__((always_inline)) inline int big(int *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += p[i] * (i + 1);
        if (s > 1000) s -= p[i + 1] ^ i;
        p[i] = s;
    }
    for (int i = n; i > 0; i--) {
        s ^= p[i] << (i & 7);
        if (s & 1) s += p[i - 1];
    }
    return s;
}
X
for T in x86_64-elf thumbv7m-none-eabi riscv32-unknown-elf; do
    "$EMBCC" --target=$T -O2 -c "$out/ni.c" -o "$out/ni-$T.o" || fail "ni.c for $T"
    body "$out/ni-$T.o" ramfn | grep -q "flashfn" ||
        fail "$T -O2: the noinline flashfn was inlined into ramfn: $(body "$out/ni-$T.o" ramfn)"
    "$EMBCC" --target=$T -Os -c "$out/ai.c" -o "$out/ai-$T.o" || fail "ai.c for $T"
    body "$out/ai-$T.o" caller1 | grep -q "big" &&
        fail "$T -Os: the always_inline big was called, not inlined"
done
echo "noinline and always_inline on a definition after a plain prototype hold"
echo "ok attr-inline-merge"
