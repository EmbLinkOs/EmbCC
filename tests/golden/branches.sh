#!/bin/sh
# Branches that reach in a byte are emitted in two.
#
# `74 cb` reaches 127 bytes and costs two; `0f 84 cd` reaches anywhere
# and costs six. Emitting every branch long cost about a tenth of all
# the code EmbCC produced.
#
# Which ones fit is not knowable while emitting -- it depends on where
# everything after them lands, which depends on which of THOSE are short
# -- so the function is emitted more than once, assuming short and
# growing back whatever does not reach. Two things can go wrong with
# that, and both are checked here: it can fail to converge (a branch
# that grows pushes another out of range, forever), and it can emit a
# displacement that does not fit, which is not a slower program but a
# jump to the middle of an instruction.
set -eu
echo "TEST-MARKER branches"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/branches
rm -rf "$out"; mkdir -p "$out"

[ "$ARCH" = x86_64 ] || {
    echo "skipped: branch relaxation is x86-64 so far"; exit 0; }

# ---- 1. a near branch is two bytes ------------------------------------
cat > "$out/near.c" <<'EOF'
int f(int a, int b) { if (a > b) return a - b; return b - a; }
long g(const char *p) { long n = 0; while (*p) { n++; p++; } return n; }
EOF
"$EMBCC" --target=x86_64-linux-gnu -O2 -S "$out/near.c" -o "$out/near.s" \
    2> "$out/cc.log" || { echo "FAIL: could not compile:"
                          cat "$out/cc.log"; exit 1; }
long=$(grep -cE '^\s*\.byte\s+0x0f,0x8[0-9a-f]' "$out/near.s" || true)
short=$(grep -cE '^\s*\.byte\s+0x7[0-9a-f],0x[0-9a-f]+\s*#\s*j' "$out/near.s" || true)
[ "$short" -ge 2 ] || {
    echo "FAIL: only $short short branch(es) in two functions whose every"
    echo "      branch is a few bytes away:"; cat "$out/near.s"; exit 1; }
[ "$long" = 0 ] || {
    echo "FAIL: $long branch(es) still take six bytes to reach somewhere"
    echo "      a byte would have:"; cat "$out/near.s"; exit 1; }
echo "every branch in a small function is two bytes ($short of them)"

# ---- 2. and a far one is still six --------------------------------------
#
# The body between the test and its target is well past 127 bytes, so
# the first attempt's short form cannot reach and must grow back.
{
    echo 'int sink[64];'
    echo 'int f(int c) {'
    echo '    if (c) {'
    for i in $(seq 0 60); do echo "        sink[$i] = c * $((i + 3)) + $i;"; done
    echo '    }'
    echo '    return sink[0];'
    echo '}'
} > "$out/far.c"
"$EMBCC" --target=x86_64-linux-gnu -O2 -S "$out/far.c" -o "$out/far.s" \
    2> "$out/cc.log" || { echo "FAIL: could not compile the far case:"
                          cat "$out/cc.log"; exit 1; }
grep -qE '^\s*\.byte\s+0x0f,0x8[0-9a-f]' "$out/far.s" || {
    echo "FAIL: a branch over 200 bytes of code was emitted short, which"
    echo "      does not reach its target at all:"; exit 1; }
echo "a branch that cannot reach in a byte grows back to six"

# ---- 3. and all of it runs ---------------------------------------------
#
# Displacements are what this changes, so the case is built to have many
# of them at many distances: a switch, nested conditions, short loops and
# long ones, with the answer checked against gcc.
cat > "$out/run.c" <<'EOF'
#include <stdio.h>
int table[256];

static int classify(int c)
{
    switch (c & 15) {
    case 0:  return 1;
    case 1:  return 2;
    case 2:  return 4;
    case 3:  return 8;
    case 4:  return 16;
    case 5:  return 32;
    case 6:  return 64;
    case 7:  return 128;
    case 8:  return 256;
    case 9:  return 512;
    case 10: return 1024;
    case 11: return 2048;
    case 12: return 4096;
    case 13: return 8192;
    case 14: return 16384;
    default: return 32768;
    }
}

/* long enough that a branch over it cannot be short */
static int bulk(int c)
{
    int s = 0;
    if (c & 1) {
        for (int i = 0; i < 60; i++) table[i] = c * (i + 3) + i;
        for (int i = 0; i < 60; i++) s += table[i] ^ (i << 2);
    } else {
        for (int i = 0; i < 60; i++) table[i] = c - i * 7;
        for (int i = 0; i < 60; i++) s -= table[i] & (i | 1);
    }
    return s;
}

static long walk(const char *p) { long n = 0; while (*p) { n++; p++; } return n; }

static int nest(int a, int b, int c)
{
    if (a > b) { if (b > c) return a + b + c; else if (a > c) return a - c;
                 else return c - a; }
    if (b > c) { if (a > c) return b * 2; return c * 3; }
    return a ^ b ^ c;
}

int main(void)
{
    unsigned long h = 0;
    const char *strs[] = { "", "a", "hello", "a longer string to walk over" };
    for (int c = -20; c <= 20; c++) {
        h = h*1000003u + (unsigned)classify(c);
        h = h*1000003u + (unsigned)bulk(c);
        h = h*1000003u + (unsigned)nest(c, c ^ 3, c + 5);
        h = h*1000003u + (unsigned long)walk(strs[(c + 20) & 3]);
    }
    printf("%lu\n", h);
    return 42;
}
EOF

LIBDIR=$EMBCC_ROOT/build/libc/linux-x86_64
if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    prev=
    for O in 0 1 2; do
        "$EMBCC" --target=x86_64-linux-gnu "-O$O" "$out/run.c" -o "$out/r" \
            2> "$out/cc.log" || { echo "FAIL: build at -O$O:"
                                  cat "$out/cc.log"; exit 1; }
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/r" \
            > "$out/r.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: exited $rc at -O$O:"
                            cat "$out/r.txt"; exit 1; }
        ans=$(cat "$out/r.txt")
        [ -z "$prev" ] || [ "$prev" = "$ans" ] || {
            echo "FAIL: answers $ans at -O$O where a lower level said $prev."
            exit 1; }
        prev=$ans
    done
    echo "the same answer at -O0, -O1 and -O2 ($prev)"

    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    if command -v "$GCC" > /dev/null 2>&1; then
        "$GCC" -O2 -c "$out/run.c" -I"$EMBCC_ROOT/lib/libc/include" \
            -o "$out/g.o" 2> /dev/null || {
            echo "FAIL: gcc will not build the case"; exit 1; }
        "$EMBCC_ROOT/embld" -o "$out/g" "$LIBDIR/crt1.o" "$out/g.o" \
            "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
            "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/g" \
            > "$out/g.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the gcc reference exited $rc"; exit 1; }
        [ "$(cat "$out/g.txt")" = "$prev" ] || {
            echo "FAIL: gcc answers $(cat "$out/g.txt"), embcc answers $prev"
            exit 1; }
        echo "and the same answer as gcc"
    fi
else
    echo "(not run: needs a kernel and a libc for tests/harness/linux)"
fi

# ---- 4. and at the boundary itself --------------------------------------
#
# The interesting distances are the ones either side of 127, where the
# first attempt's short form does not reach and growing it back pushes
# the next one out too. Bodies stepping through that range are generated
# and RUN, because a displacement that is four bytes wrong is not a
# slower program, it is a jump into the middle of an instruction.
{
    echo '#include <stdio.h>'
    echo 'int sink[64];'
    k=0
    while [ "$k" -le 24 ]; do
        echo "static int b$k(int c) {"
        echo "    int s = 0;"
        echo "    if (c & 1) {"
        i=0
        while [ "$i" -le "$k" ]; do
            echo "        sink[$i] = c * $((i + 3)) + $i;"
            i=$((i + 1))
        done
        echo "    } else { s = c * 7; }"
        echo "    return s + sink[0];"
        echo "}"
        k=$((k + 1))
    done
    echo 'int main(void) {'
    echo '    unsigned long h = 0;'
    echo '    for (int c = -8; c <= 8; c++) {'
    k=0
    while [ "$k" -le 24 ]; do
        echo "        h = h*1000003u + (unsigned)b$k(c);"
        k=$((k + 1))
    done
    echo '    }'
    echo '    printf("%lu", h); putchar(10);'
    echo '    return 42;'
    echo '}'
} > "$out/edge.c"

if [ -f "$LIBDIR/libc.a" ] &&
   "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1
then
    "$EMBCC" --target=x86_64-linux-gnu -O2 "$out/edge.c" -o "$out/e" \
        2> "$out/cc.log" || { echo "FAIL: could not build the boundary sweep:"
                              cat "$out/cc.log"; exit 1; }
    set +e
    "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/e" > "$out/e.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "FAIL: the boundary sweep exited $rc:"
                        cat "$out/e.txt"; exit 1; }
    GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
    if command -v "$GCC" > /dev/null 2>&1; then
        "$GCC" -O2 -c "$out/edge.c" -I"$EMBCC_ROOT/lib/libc/include" \
            -o "$out/eg.o" 2> /dev/null || {
            echo "FAIL: gcc will not build the boundary sweep"; exit 1; }
        "$EMBCC_ROOT/embld" -o "$out/eg" "$LIBDIR/crt1.o" "$out/eg.o" \
            "$LIBDIR/libc.a" "$LIBDIR/librt.a" \
            "$("$GCC" -print-libgcc-file-name)" 2>/dev/null
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/eg" \
            > "$out/eg.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the gcc boundary reference exited $rc"
                            exit 1; }
        [ "$(cat "$out/eg.txt")" = "$(cat "$out/e.txt")" ] || {
            echo "FAIL: at some branch distance the answer differs from gcc's"
            exit 1; }
    fi
    echo "twenty-five branch distances across the 127-byte boundary agree"
fi
