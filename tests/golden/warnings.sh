#!/bin/sh
# Warnings that mean something (docs/TOOLING.md T4): each has a name, a
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
