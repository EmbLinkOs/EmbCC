#!/bin/sh
# Warnings that mean something (docs/manual/diagnostics.md T4): each has a name, a
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

# 5b. A local or parameter hiding a file-scope variable, as GCC and clang
#     report it -- but not one declared after the function, and not a
#     function's name.
cat > "$out/g.c" << 'EOF'
int level;
int set(int level) { return level; }
int get(void) { int level = 2; return level; }
int early(void) { int later = 1; return later; }
int later;
int fn(void);
int name_of_fn(void) { int fn = 3; return fn; }
EOF
"$EMBCC" --target="$TARGET" -Wshadow -c "$out/g.c" -o "$out/g.o" \
    > "$out/g.txt" 2>&1
lines=$(sed -n "s|.*g\.c:\([0-9]*\):[0-9]*: warning: declaration of '\([a-z]*\)' shadows a global declaration.*|\1 \2|p" \
        "$out/g.txt" | tr '\n' ' ')
[ "$lines" = "2 level 3 level " ] || {
    echo "-Wshadow on globals: expected lines 2 and 3, got:"; cat "$out/g.txt"
    exit 1; }
echo "-Wshadow: a local or parameter hiding a global, not a later one"

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

# -Werror makes the compile FAIL, and a failed compile leaves no output:
# the object used to be written anyway, with exit status 1, and make
# then took it for up to date and never rebuilt it.
printf 'int f(void) { int unused; return 1; }\n' > "$out/we.c"
rm -f "$out/we.o"
if "$EMBCC" -Wall -Werror -c "$out/we.c" -o "$out/we.o" 2> /dev/null; then
    echo "FAIL: -Werror with a warning exited 0"; exit 1
fi
[ ! -e "$out/we.o" ] || {
    echo "FAIL: -Werror failed the compile and still wrote $out/we.o"; exit 1; }
"$EMBCC" -Wall -c "$out/we.c" -o "$out/we.o" 2> /dev/null || {
    echo "FAIL: a warning without -Werror failed the compile"; exit 1; }
[ -e "$out/we.o" ] || { echo "FAIL: a warning stopped the object"; exit 1; }
echo "and -Werror fails the compile without leaving its object behind"

# -Werror=NAME makes that one warning an error (and turns it on), and
# -Wno-error=NAME exempts it from -Werror; a NAME that is no warning is
# said, not ignored.
printf 'int f(void) { int unused; return 1; }\n' > "$out/wn.c"
rm -f "$out/wn.o"
if "$EMBCC" -Werror=unused-variable -c "$out/wn.c" -o "$out/wn.o" \
       2> "$out/wn.err"; then
    echo "FAIL: -Werror=unused-variable left the warning a warning"; exit 1
fi
grep -q "error: unused variable" "$out/wn.err" || {
    echo "FAIL: -Werror=unused-variable did not report an error:"
    cat "$out/wn.err"; exit 1; }
"$EMBCC" -Werror=unused-variable -Wno-unused-variable -c "$out/wn.c" \
    -o "$out/wn.o" 2> /dev/null || {
    echo "FAIL: -Wno-unused-variable after -Werror= did not turn it off"; exit 1; }
"$EMBCC" -Wall -Werror -Wno-error=unused-variable -c "$out/wn.c" \
    -o "$out/wn.o" 2> "$out/wn.err" || {
    echo "FAIL: -Wno-error=unused-variable still failed the compile"; exit 1; }
grep -q "warning: unused variable" "$out/wn.err" || {
    echo "FAIL: -Wno-error=unused-variable lost the warning itself"; exit 1; }
"$EMBCC" -Werror=no-such-warning -c "$out/wn.c" -o "$out/wn.o" \
    2> "$out/wn.err" || true
grep -q "names no warning" "$out/wn.err" || {
    echo "FAIL: -Werror=no-such-warning was ignored silently"; exit 1; }
echo "and -Werror=NAME / -Wno-error=NAME pick out one warning"

# 7. __attribute__((unused)) on a parameter, wherever GCC takes it. After
# the name it was a syntax error, and in the other two places it was read
# and dropped, so -Wunused-parameter fired on a parameter marked unused.
cat > "$out/up.c" <<'SRC'
int a(__attribute__((unused)) int x) { return 1; }
int b(int __attribute__((unused)) x) { return 1; }
int c(int x __attribute__((unused))) { return 1; }
int d(char *p __attribute__((unused)), int y) { return 1; }
int e([[maybe_unused]] int x) { return 1; }
int proto(int x __attribute__((unused)));
SRC
"$EMBCC" -Wall -Wextra -c "$out/up.c" -o "$out/up.o" 2> "$out/up.err" || {
    echo "FAIL: an attribute after a parameter's name was refused:"
    cat "$out/up.err"; exit 1; }
[ "$(grep -c 'unused parameter' "$out/up.err")" = 1 ] &&
grep -q "unused parameter 'y'" "$out/up.err" || {
    echo "FAIL: -Wunused-parameter should name y alone:"; cat "$out/up.err"; exit 1; }
echo "and a parameter marked unused, before, inside or after its declarator, is not reported"

# 8. -Wsign-compare speaks only when the signed side can be negative. A
#    narrow unsigned value promoted to int is not -- `(flags & MASK) != 0u`
#    with a uint8_t flags, a kernel's every flag test -- and neither is a &
#    with such a value, or a shift, a comparison or a ! of one. EmbLinkRTOS
#    stopped at 50 of these under -Werror; gcc and clang say nothing.
cat > "$out/sc.c" << 'EOF2'
typedef unsigned char u8;
struct o { u8 flags; signed char s; unsigned short h; };
int f(struct o *q, int i, unsigned u) {
    int n = 0;
    if ((q->flags & 1u) != 0u) n++;
    if ((q->flags & (u8)1) != 0u) n++;
    if ((q->flags >> 2) == u) n++;
    if (q->s == u) n++;
    if ((q->s & 3) == u) n++;
    if (i == u) n++;
    if ((i | q->flags) == u) n++;
    if ((q->h ^ q->flags) < u) n++;
    if ((i != 0) == u) n++;
    if ((i ? q->h : q->flags) > u) n++;
    if ((i ? q->h : i) > u) n++;
    if (-q->flags < u) n++;
    return n;
}
EOF2
"$EMBCC" --target="$TARGET" -Wsign-compare -fsyntax-only "$out/sc.c" 2> "$out/sc.txt" || true
lines=$(grep -o 'sc.c:[0-9]*' "$out/sc.txt" | cut -d: -f2 | sort -n | tr '\n' ' ')
[ "$lines" = "8 10 11 15 16 " ] || {
    sed "s|$out/||" "$out/sc.txt"
    echo "FAIL: -Wsign-compare at lines '$lines', expected '8 10 11 15 16 '"; exit 1; }
if command -v clang > /dev/null 2>&1; then
    clang -fsyntax-only -Wsign-compare "$out/sc.c" 2> "$out/sc-clang.txt" || true
    cl=$(grep -o 'sc.c:[0-9]*' "$out/sc-clang.txt" | cut -d: -f2 | sort -n | tr '\n' ' ')
    [ "$cl" = "$lines" ] || {
        echo "FAIL: clang warns at lines '$cl', EmbCC at '$lines'"; exit 1; }
    echo "-Wsign-compare: the five lines clang warns on, and none of the seven it does not"
else
    echo "-Wsign-compare: lines 8 10 11 15 16 only (no clang to referee)"
fi

# 9. What a coding standard turns on. EmbLinkRTOS builds -Werror with
#    gcc's set, and 11 of these were names EmbCC did not have: they
#    turned nothing on and said so. Each is now a check, reported at the
#    lines clang reports it (when clang has it, and is here), or at the
#    lines gcc's documentation says (missing-declarations, redundant-decls,
#    nested-externs, format-nonliteral, which clang lacks in C).
cat > "$out/cs.c" << 'EOF2'
#include <stdarg.h>
#if FOO_UNDEFINED || 0 && BAR_SKIPPED
#endif
#if 1 ? 1 : (1 / 0)
#endif
int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *, va_list) __attribute__((format(printf, 1, 0)));
double sq(double);
int f();
int g(void);
int h(int x) { return x; }
const int ci = 1;
int *pc = (int *)&ci;
float fl = 1.5f;
double dp(void) { return fl * 2.0; }
void vl(int n) { int a[n]; (void)a; }
int sw(int x) {
    int r = 0;
    switch (x) {
    case 1: r++;
    case 2: r++; __attribute__((fallthrough));
    case 3: r++; break;
    case 4: r--;
    }
    return r;
}
void fmt(const char *m) { printf(m); }
void fmt2(const char *m, int v) { printf(m, v); }
EOF2
code_lines() {      # code_lines FILE OPTION: the lines of FILE's -WOPTION
    grep -o "cs.c:[0-9]*:.*\[-W$2\]" "$1" | cut -d: -f2 | sort -n | tr '\n' ' '
}
for spec in "undef:2" "strict-prototypes:9" "missing-prototypes:11 15 16 17 27 28" \
            "cast-qual:13" "double-promotion:15" "vla:16" "switch-default:19" \
            "implicit-fallthrough:20" "format-security:27" "format-nonliteral:28"; do
    w=${spec%%:*}; want="${spec#*:} "
    "$EMBCC" --target="$TARGET" -W$w -fsyntax-only "$out/cs.c" 2> "$out/cs-$w.txt" || true
    got=$(code_lines "$out/cs-$w.txt" "$w")
    [ "$got" = "$want" ] || { sed "s|$out/||" "$out/cs-$w.txt"
        echo "FAIL: -W$w at lines '$got', expected '$want'"; exit 1; }
done
"$EMBCC" --target="$TARGET" -Wformat=2 -fsyntax-only "$out/cs.c" 2> "$out/cs-f2.txt" || true
[ "$(code_lines "$out/cs-f2.txt" 'format-[a-z]*')" = "27 28 " ] || {
    cat "$out/cs-f2.txt"; echo "FAIL: -Wformat=2 is not -Wformat-security and -nonliteral"; exit 1; }
grep -q 'cs.c:21:.*here' "$out/cs-implicit-fallthrough.txt" || {
    cat "$out/cs-implicit-fallthrough.txt"; echo "FAIL: the fall-through note is not at the next label"; exit 1; }
# the referee, for those clang has in C: the same lines
if command -v clang > /dev/null 2>&1; then
    for w in undef strict-prototypes missing-prototypes cast-qual double-promotion \
             vla switch-default implicit-fallthrough format-security; do
        clang --target=x86_64-elf -W$w -fsyntax-only -isystem "$X86_NEWLIB/include" \
            "$out/cs.c" 2> "$out/cl-$w.txt" || true
        cl=$(grep "warning:.*\[-W$w" "$out/cl-$w.txt" | grep -o 'cs.c:[0-9]*' | cut -d: -f2 | sort -nu | tr '\n' ' ')
        mine=$(code_lines "$out/cs-$w.txt" "$w")
        # clang reports a fall-through at the label, gcc and EmbCC at the
        # statement with a note at the label: compare the labels
        [ $w = implicit-fallthrough ] &&
            mine=$(grep -o 'cs.c:[0-9]*:[0-9]*: note: here' "$out/cs-$w.txt" | cut -d: -f2 | tr '\n' ' ')
        [ "$cl" = "$mine" ] || { echo "FAIL: -W$w: clang at '$cl', EmbCC at '$mine'"; exit 1; }
    done
    echo "the coding-standard warnings: clang's lines for the nine it has in C"
fi
# the gcc-only ones, at gcc's lines
cat > "$out/gc.c" << 'EOF2'
extern int x;
extern int x;
int p(int);
int p(int);
int q(void) { return 1; }
int main(void) { extern int y; int k(int); return p(x) + y + k(1) + q(); }
EOF2
for spec in "redundant-decls:2 4" "missing-declarations:5" "nested-externs:6 6"; do
    w=${spec%%:*}; want="${spec#*:} "
    "$EMBCC" --target="$TARGET" -W$w -fsyntax-only "$out/gc.c" 2> "$out/gc-$w.txt" || true
    got=$(grep -o "gc.c:[0-9]*:.*\[-W$w\]" "$out/gc-$w.txt" | cut -d: -f2 | sort -n | tr '\n' ' ')
    [ "$got" = "$want" ] || { cat "$out/gc-$w.txt"; echo "FAIL: -W$w at '$got', expected '$want'"; exit 1; }
done
# a note follows only its own warning: -Wshadow silenced by a pragma left
# its "the one it hides" note on whatever was reported before it
printf 'int g1;\n#pragma GCC diagnostic ignored "-Wshadow"\nint t(void) { int unused_v; int g1 = 2; return g1; }\n' > "$out/note.c"
"$EMBCC" --target="$TARGET" -Wall -Wshadow -fsyntax-only "$out/note.c" 2> "$out/note.txt" || true
if grep -q 'the one it hides' "$out/note.txt"; then
    cat "$out/note.txt"; echo "FAIL: a silenced -Wshadow left its note behind"; exit 1
fi
# a storage class after the type: accepted, as C allows, and reported
# in -Wextra as GCC reports it (-Wold-style-declaration)
printf 'const static int a = 1;\nint static b;\nunsigned extern int c;\nlong typedef L;\nstatic inline int f(void) { int register r = a; return r + b; }\nL lg;\nint const static k2 = 4;\ntypedef int T;\nT const static k3 = 5;\n' > "$out/osd.c"
"$EMBCC" --target="$TARGET" -Wextra -fsyntax-only "$out/osd.c" 2> "$out/osd.txt" ||
    { cat "$out/osd.txt"; echo "FAIL: a storage class after the type was refused"; exit 1; }
got=$(grep -o "osd.c:[0-9]*:.*\[-Wold-style-declaration\]" "$out/osd.txt" | cut -d: -f2 | tr '\n' ' ')
[ "$got" = "1 2 3 4 5 7 9 " ] || { cat "$out/osd.txt"; echo "FAIL: -Wold-style-declaration at '$got'"; exit 1; }
"$EMBCC" --target="$TARGET" -fsyntax-only "$out/osd.c" 2>&1 | grep -q old-style &&
    { echo "FAIL: -Wold-style-declaration without -Wextra"; exit 1; }
# and the storage class is the one written: `const static` is internal
if command -v llvm-nm > /dev/null 2>&1; then
    "$EMBCC" --target="$TARGET" -c "$out/osd.c" -o "$out/osd.o" 2> /dev/null
    for v in a k2 k3; do
        llvm-nm "$out/osd.o" | grep -q " r $v\$" ||
            { llvm-nm "$out/osd.o"; echo "FAIL: '$v' is not a local read-only symbol"; exit 1; }
    done
fi
# a multi-character constant: GCC's value, and its -Wmultichar
printf "int a = 'ab';\nint b = 'abcde';\nint c = 'a';\n#if 'ab' != 0x6162\n#error\n#endif\nint d = '\303\251';\nint e = '\\\\xff\\\\xfe';\n" > "$out/mc.c"
"$EMBCC" --target=x86_64-elf -c "$out/mc.c" -o "$out/mc.o" 2> "$out/mc.txt" ||
    { cat "$out/mc.txt"; echo "FAIL: a multi-character constant was refused"; exit 1; }
[ "$(grep -c 'mc.c:1:.*multi-character character constant \[-Wmultichar\]' "$out/mc.txt")" = 1 ] &&
    grep -q 'mc.c:2:.*character constant too long for its type' "$out/mc.txt" &&
    [ "$(grep -c warning "$out/mc.txt")" = 5 ] ||   # the #if's 'ab' too
    { cat "$out/mc.txt"; echo "FAIL: -Wmultichar"; exit 1; }
"$EMBCC" --target=x86_64-elf -Wno-multichar -fsyntax-only "$out/mc.c" 2>&1 | grep -q Wmultichar &&
    { echo "FAIL: -Wno-multichar"; exit 1; }
if command -v x86_64-elf-gcc > /dev/null 2>&1 && command -v llvm-objdump > /dev/null 2>&1; then
    x86_64-elf-gcc -c "$out/mc.c" -o "$out/mcg.o" 2> /dev/null
    mcdata() { llvm-objdump -s -j .data "$1" | grep '^ [0-9a-f][0-9a-f]* '; }
    [ -n "$(mcdata "$out/mc.o")" ] && [ "$(mcdata "$out/mc.o")" = "$(mcdata "$out/mcg.o")" ] ||
        { echo "FAIL: multi-character values differ from GCC's"; exit 1; }
fi
echo "the coding-standard warnings at their lines, -Wformat=2, the gcc-only ones, no stray notes, -Wold-style-declaration and -Wmultichar"
