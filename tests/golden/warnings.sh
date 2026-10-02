#!/bin/sh
# Warnings that mean something (docs/tools/diagnostics.md T4): each has a name, a
# group, and an off switch, and each is a real analysis rather than a
# spelling of "maybe". The check that matters is the last one: on the same
# code, EmbCC warns where gcc warns — a warning nobody else raises is a
# false positive, and one only gcc raises is a gap.
set -eu
echo "TEST-MARKER warnings"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/warnings-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/w.c" << 'EOF'
static int never_called(int n) { return n; }

int compute(int n, int scale)
{
    int unused_one = 5;
    unsigned u = 3;
    int total = 0;
    for (int i = 0; i < n; i++) {
        int total = i * 2;
        total += 1;
    }
    if (n < u)
        total = 1;
    return total;
}

int main(void) { return compute(2, 3); }
EOF

warns() { # flags... -> the warning names raised
    "$EMBCC" --target="$TARGET" "$@" -c "$out/w.c" -o "$out/w.o" 2>&1 |
        sed -n 's/.*\[-W\([a-z-]*\)\].*/\1/p' | sort -u
}

# 1. Silent by default: a warning nobody asked for is noise.
[ -z "$(warns)" ] || { echo "warnings without -W:"; warns; exit 1; }
echo "silent unless asked"

# 2. The groups, as gcc draws them.
w=$(warns -Wall)
echo "$w" | grep -qx "unused-variable" || { echo "-Wall lacks unused-variable"; exit 1; }
echo "$w" | grep -qx "unused-function" || { echo "-Wall lacks unused-function"; exit 1; }
echo "$w" | grep -qx "unused-parameter" && { echo "-Wall should not include unused-parameter"; exit 1; }
w=$(warns -Wextra)
echo "$w" | grep -qx "unused-parameter" || { echo "-Wextra lacks unused-parameter"; exit 1; }
echo "$w" | grep -qx "sign-compare" || { echo "-Wextra lacks sign-compare"; exit 1; }
echo "-Wall and -Wextra hold what gcc puts in them"

# 3. One at a time, and off again.
[ "$(warns -Wshadow)" = "shadow" ] || { echo "-Wshadow alone:"; warns -Wshadow; exit 1; }
w=$(warns -Wall -Wextra -Wshadow -Wno-unused-variable)
echo "$w" | grep -qx "unused-variable" && { echo "-Wno-unused-variable ignored"; exit 1; }
echo "$w" | grep -qx "shadow" || { echo "-Wno- turned off the wrong one"; exit 1; }
echo "-Wname turns one on; -Wno-name turns one off"

# 4. The name is in the message, and -Werror makes it fatal.
"$EMBCC" --target="$TARGET" -Wall -c "$out/w.c" -o "$out/w.o" 2>&1 |
    grep -q "unused variable 'unused_one' \[-Wunused-variable\]" || {
    echo "the option is not named in the message"; exit 1; }
if "$EMBCC" --target="$TARGET" -Wall -Werror -c "$out/w.c" -o "$out/w.o" \
        > "$out/werror.log" 2>&1; then
    echo "-Werror did not fail the build"; exit 1
fi
echo "each names its option; -Werror fails the build"

# 5. -Wshadow says where the hidden one is.
"$EMBCC" --target="$TARGET" -Wshadow -c "$out/w.c" -o "$out/w.o" 2>&1 |
    grep -q "note: the one it hides is here" || {
    echo "-Wshadow does not point at the declaration it hides"; exit 1; }
echo "-Wshadow points at what is hidden"

# 6. The judge: gcc, on the same file, with the same flags.
GCC=$([ "$ARCH" = aarch64 ] && echo aarch64-elf-gcc || echo x86_64-elf-gcc)
if command -v "$GCC" >/dev/null 2>&1; then
    NL=$([ "$ARCH" = aarch64 ] && echo "$AARCH64_NEWLIB" || echo "$X86_NEWLIB")
    "$GCC" -isystem "$NL/include" -Wall -Wextra -Wshadow -c "$out/w.c" \
        -o "$out/w.gcc.o" 2>&1 |
        sed -n 's/.*\[-W\([a-z-]*\)\].*/\1/p' | sort -u > "$out/gcc.warns"
    warns -Wall -Wextra -Wshadow > "$out/mine.warns"
    # Only the warnings EmbCC has are comparable: gcc knows more kinds.
    comm -12 "$out/gcc.warns" "$out/mine.warns" > "$out/both"
    for k in $(cat "$out/mine.warns"); do
        grep -qx "$k" "$out/gcc.warns" || {
            echo "EmbCC raises $k where gcc does not:"; exit 1; }
    done
    for k in unused-variable unused-parameter unused-function shadow sign-compare; do
        if grep -qx "$k" "$out/gcc.warns"; then
            grep -qx "$k" "$out/mine.warns" || {
                echo "gcc raises $k here and EmbCC does not"; exit 1; }
        fi
    done
    echo "gcc agrees: $(tr '\n' ' ' < "$out/both")"
fi

# 7. A system header's warnings are not the project's to fix: dropped, as
#    gcc drops them, unless -Wsystem-headers asks.
mkdir -p "$out/sys"
cat > "$out/sys/noisy.h" << 'EOF2'
static int unused_in_header(int a) { (void)0; return 1; }
EOF2
cat > "$out/user.c" << 'EOF2'
#include <noisy.h>
int main(void) { return 0; }
EOF2
"$EMBCC" --target="$TARGET" -Wall -Wextra -isystem "$out/sys" -c "$out/user.c"     -o "$out/user.o" 2> "$out/sys.log" || true
grep -q "warning:" "$out/sys.log" && {
    echo "a system header's warnings leaked:"; cat "$out/sys.log"; exit 1; }
"$EMBCC" --target="$TARGET" -Wall -Wextra -Wsystem-headers -isystem "$out/sys"     -c "$out/user.c" -o "$out/user.o" 2> "$out/sys2.log" || true
grep -q "unused-parameter\|unused-function" "$out/sys2.log" || {
    echo "-Wsystem-headers did not bring them back:"; cat "$out/sys2.log"; exit 1; }
echo "a system header is quiet; -Wsystem-headers unmutes it"

# 8. C++ lowers to C, and these analyses would describe the LOWERED code —
#    so they are C-only rather than pointing at lines that mean nothing.
printf 'struct P { int x; P(int v) : x(v) {} };
int main() { P p(3); return p.x - 3; }
' > "$out/q.cc"
NLX=$([ "$ARCH" = aarch64 ] && echo "$AARCH64_NEWLIB" || echo "$X86_NEWLIB")
"$EMBCC" --target="$TARGET" -Wall -Wextra -isystem "$NLX/include" -c "$out/q.cc"     -o "$out/q.o" 2> "$out/cxx.log" || true
grep -q "warning:" "$out/cxx.log" && {
    echo "a C++ compile warned about its own lowering:"; cat "$out/cxx.log"; exit 1; }
echo "C++ does not warn about the C it lowers to"

# 9. --help-warnings lists them with their groups.
"$EMBCC" --help-warnings | grep -q -- "-Wsign-compare" || {
    echo "--help-warnings omits one"; exit 1; }
echo "--help-warnings lists them"

# ---- the statically-decidable warnings --------------------------------
#
# Six questions about the program's TEXT, answered with what sema
# already knows. Each is paired with the idiom that looks like it and is
# CORRECT, because a warning that fires on correct code is worse than no
# warning at all: the first version of these fired nineteen times on
# EmbCC's own sources, every one of them a false positive.
#
#   -Wparentheses        `REG & MASK == 0` is `REG & (MASK == 0)`, since
#                        == binds tighter than &. The classic MMIO bug.
#                        Silent when the author wrote the parentheses.
#   -Waddress            `if (f)` on a function name. Silent for a WEAK
#                        function, whose address really can be null --
#                        that is the whole idiom -- and for a function
#                        POINTER, which is an ordinary question.
#   -Wtype-limits        `u < 0` for unsigned u.
#   -Wshift-count-overflow, -Wdiv-by-zero, -Wlogical-op.
cat > "$out/sd.c" <<'EOF'
/* --- each of these is a bug --- */
int p1(int r, int m)      { return r & m == 0; }
int p2(int a, int b,int c) { return a == b & c; }
int s1(int x)             { return x << 40; }
int s2(int x)             { return x << -1; }
int d1(int a)             { return a / 0; }
int l1(int a)             { return a || a; }
int t1(unsigned u)        { return u < 0; }
int t2(unsigned u)        { return 0 <= u; }
extern void plain(void);
int a1(void)              { if (plain) return 1; return 0; }
int a2(void)              { return plain != 0; }
EOF
"$EMBCC" -Wall -Wextra -fsyntax-only "$out/sd.c" > "$out/sd.log" 2>&1
for w in parentheses shift-count-overflow div-by-zero logical-op type-limits \
         address; do
    grep -q -- "-W$w" "$out/sd.log" || {
        echo "FAIL: -W$w did not fire on code that needs it:"
        cat "$out/sd.log"; exit 1; }
done
# Two of each of the two-sided ones, so a check that only looks at the
# left operand cannot pass.
n=$(grep -c -- '-Wparentheses' "$out/sd.log" || true)
[ "$n" = 2 ] || { echo "FAIL: -Wparentheses fired $n times, wanted 2 (one
per operand side)"; cat "$out/sd.log"; exit 1; }
n=$(grep -c -- '-Wtype-limits' "$out/sd.log" || true)
[ "$n" = 2 ] || { echo "FAIL: -Wtype-limits fired $n times, wanted 2"
                  cat "$out/sd.log"; exit 1; }
echo "six statically-decidable warnings each fire on the code that needs
them, on both operand sides"

# --- and the idioms that look like those bugs and are correct ---
cat > "$out/sdok.c" <<'EOF'
/* The author wrote the parentheses: that IS how you say you meant it,
 * and EmbCC's own x86 encoder is full of this shape. */
int q1(int r, int m)  { return r & (m == 0); }
int q2(int g)         { return 0x40 | (g >= 8); }
int q3(int a, int b, int c) { return (a == b) & c; }
/* Shifts and divisions that are in range. */
long long q4(long long x) { return x << 40; }
int q5(int x)         { return x << 31; }
int q6(int a)         { return a / 7; }
/* Comparisons the types do NOT decide. */
int q7(unsigned u)    { return u < 1; }
int q8(int i)         { return i < 0; }
/* A weak function's address really can be null -- this is how libc asks
 * whether the host provides something. */
extern void weakfn(void) __attribute__((weak));
int q9(void)          { if (weakfn) return 1; return 0; }
int q10(void)         { return weakfn != 0; }
/* A function POINTER is an ordinary question, not a mistake. */
int q11(void (*fp)(void)) { if (fp) return 1; return 0; }
struct S { void (*h)(void); };
int q12(struct S *s)  { return s->h == 0; }
/* Repeated operands WITH side effects are not the same expression. */
int side(void);
int q13(void)         { return side() || side(); }
EOF
"$EMBCC" -Wall -Wextra -fsyntax-only "$out/sdok.c" > "$out/sdok.log" 2>&1 || {
    echo "FAIL: the correct-idiom file does not compile:"
    cat "$out/sdok.log"; exit 1; }
[ -s "$out/sdok.log" ] && {
    echo "FAIL: a warning fired on correct code:"
    cat "$out/sdok.log"; exit 1; }
echo "and none of them fires on the correct idiom that looks like it"

# None is on without being asked for, and each is switched off by the
# name it printed.
"$EMBCC" -fsyntax-only "$out/sd.c" > "$out/sdoff.log" 2>&1
grep -qE -- '-W(parentheses|shift-count-overflow|type-limits|logical-op|address)' \
    "$out/sdoff.log" && {
    echo "FAIL: a -Wall/-Wextra warning fired without being asked:"
    cat "$out/sdoff.log"; exit 1; }
"$EMBCC" -Wall -Wextra -Wno-parentheses -Wno-shift-count-overflow \
    -Wno-div-by-zero -Wno-logical-op -Wno-type-limits -Wno-address \
    -fsyntax-only "$out/sd.c" > "$out/sdno.log" 2>&1
[ -s "$out/sdno.log" ] && {
    echo "FAIL: -Wno- did not silence them:"; cat "$out/sdno.log"; exit 1; }
echo "each is off unless asked for, and each is switched off by the name
it prints"

# The whole tree compiles clean under them -- which is the check that
# actually caught the two false-positive classes above.
bad=0
for f in $(find lib src -name '*.c' | sort); do
    "$EMBCC" -Wall -Wextra -fsyntax-only -Ilib/libc/include -Iinclude "$f" \
        2>> "$out/tree.log" > /dev/null || true
done
n=$(grep -cE -- '-W(parentheses|shift-count-overflow|type-limits|div-by-zero|logical-op|address)' \
    "$out/tree.log" 2>/dev/null || true)
[ "$n" = 0 ] || {
    echo "FAIL: $n of these warnings fire on EmbCC's own sources:"
    grep -E -- '-W(parentheses|shift-count-overflow|type-limits|div-by-zero|logical-op|address)' \
        "$out/tree.log" | head -6; exit 1; }
echo "and EmbCC's own sources are clean under all six"
