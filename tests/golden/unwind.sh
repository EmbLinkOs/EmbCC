#!/bin/sh
# The unwinder, against libgcc's.
#
# An unwinder is not testable by asking it questions. It either puts the
# machine back the way a frame left it or it does not, and "does not"
# shows up as a wrong answer somewhere far away -- a destructor that did
# not run, a handler that was skipped, a jump into a frame whose
# registers are somebody else's. So the test is a PROGRAM whose output
# says what happened: every destructor announces itself, every handler
# announces which one it is, and the order of those lines is the
# property under test.
#
# The oracle is libgcc's unwinder, and getting it required a detour
# worth recording. libgcc's cannot simply be linked into the static
# Linux image: it finds its tables through a `__register_frame_info`
# that a crtbegin normally calls, and our crt1 does not, so it links and
# then finds nothing. But the BARE-METAL harness already runs C++
# exceptions on libgcc's unwinder -- tests/harness/crt.c registers the
# tables by hand, which is exactly what a bare-metal image has to do.
#
# So the same source is built twice: once for the freestanding target
# and run on the bare-metal harness, where libgcc unwinds it, and once
# for Linux, where lib/rt/unwind.c does. Same compiler, same C++
# runtime, same libc -- the unwinder is the only thing that differs, and
# the two must print the same lines.
set -eu
echo "TEST-MARKER unwind"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/unwind
rm -rf "$out"; mkdir -p "$out"

SRC=$EMBCC_ROOT/tests/golden/rt/eh.cc
. "$EMBCC_ROOT/tools/hostpaths.sh"

case "$ARCH" in
    x86_64)
        GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}; LD=${EMBCC_X86_LD:-x86_64-elf-ld}
        NEWLIB=$X86_NEWLIB; LDFLAGS="-n -z max-page-size=0x1000"
        GCCFLAGS="-ffreestanding -mno-red-zone" ;;
    aarch64)
        GCC=${EMBCC_AARCH64_GCC:-aarch64-elf-gcc}
        LD=${EMBCC_AARCH64_LD:-aarch64-elf-ld}
        NEWLIB=$AARCH64_NEWLIB; LDFLAGS=""
        GCCFLAGS="-ffreestanding" ;;
    *) echo "skipped: no toolchain for $ARCH"; exit 0 ;;
esac

LIBDIR=$EMBCC_ROOT/build/libc/linux-$ARCH
CXXDIR=$EMBCC_ROOT/build/libcxx/linux-$ARCH
[ -f "$LIBDIR/librt.a" ] && [ -f "$CXXDIR/libcxx.a" ] || {
    echo "skipped: no librt.a/libcxx.a for linux-$ARCH"
    echo "         (make libc-linux-$ARCH libcxx-linux-$ARCH)"; exit 0; }
[ -f "$EMBCC_ROOT/build/libcxx/$ARCH/libcxx.a" ] || {
    echo "skipped: no freestanding libcxx to build the reference"; exit 0; }
command -v "$GCC" > /dev/null 2>&1 || { echo "skipped: no $GCC"; exit 0; }
[ -d "$NEWLIB/include" ] || { echo "skipped: no newlib for the harness"
                              exit 0; }
"$EMBCC_ROOT/tests/harness/linux/run.sh" "$ARCH" --check > /dev/null 2>&1 || {
    echo "skipped: running it needs a kernel for tests/harness/linux"
    exit 0; }

# ---- the reference: the bare-metal harness, unwound by libgcc -----------
H=$EMBCC_ROOT/tests/harness/$ARCH
LIBGCC=$(dirname "$("$GCC" -print-libgcc-file-name)")
mkdir -p "$out/bm"
for src in "$H"/start.S "$H"/sys.c "$H"/semihost.c "$EMBCC_ROOT/tests/harness/crt.c"; do
    [ -f "$src" ] || continue
    o=$out/bm/$(basename "$src" | sed 's/\.[cS]$/.o/')
    "$GCC" $GCCFLAGS -isystem "$NEWLIB/include" -c "$src" -o "$o" \
        2> "$out/bm.log" || { echo "FAIL: building the harness:"
                              cat "$out/bm.log"; exit 1; }
done
"$EMBCC" --target="$TARGET" -x c++ -O1 -I"$EMBCC_ROOT/lib/libcxx/include" \
    -I"$EMBCC_ROOT/lib/libc/include" -c "$SRC" -o "$out/ref.o" \
    2> "$out/ref-cc.log" || {
    echo "FAIL: embcc could not compile the corpus:"
    cat "$out/ref-cc.log"; exit 1; }
"$LD" $LDFLAGS -T "$H/link.ld" -o "$out/ref.64" "$out"/bm/*.o "$out/ref.o" \
    "$EMBCC_ROOT/build/libcxx/$ARCH/libcxx.a" \
    "$EMBCC_ROOT/build/libc/$ARCH/libc.a" -L"$LIBGCC" -lgcc \
    2>&1 | grep -v 'RWX permissions' >&2 || true
[ -f "$out/ref.64" ] || { echo "FAIL: linking the reference"; exit 1; }
if [ "$ARCH" = x86_64 ]; then
    x86_64-elf-objcopy -I elf64-x86-64 -O elf32-i386 "$out/ref.64" "$out/ref.elf"
else
    cp "$out/ref.64" "$out/ref.elf"
fi
set +e
"$H/run.sh" "$out/ref.elf" > "$out/ref.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || {
    echo "FAIL: the libgcc reference itself exited $rc, so there is"
    echo "      nothing to compare against:"; tail -8 "$out/ref.txt"; exit 1; }

# ---- ours: the same source on Linux, unwound by lib/rt ------------------
"$EMBCC" --target="$ARCH-linux-gnu" -x c++ -O1 \
    -I"$EMBCC_ROOT/lib/libcxx/include" -I"$EMBCC_ROOT/lib/libc/include" \
    -c "$SRC" -o "$out/ours.o" 2> "$out/ours-cc.log" || {
    echo "FAIL: embcc could not compile for Linux:"
    cat "$out/ours-cc.log"; exit 1; }
"$LD" -static -T "$EMBCC_ROOT/lib/libc/os/linux/link.ld" -o "$out/ours" \
    "$LIBDIR/crt1.o" "$out/ours.o" "$CXXDIR/libcxx.a" "$LIBDIR/libc.a" \
    "$LIBDIR/librt.a" 2> "$out/ours-ld.log" || {
    echo "FAIL: linking ours:"; cat "$out/ours-ld.log"
    echo "      an undefined _Unwind_Resume here means lib/rt/unwind.c"
    echo "      did not make it into librt.a; an undefined"
    echo "      __eh_frame_start means the link script lost its brackets"
    exit 1; }
set +e
"$EMBCC_ROOT/tests/harness/linux/run.sh" "$ARCH" "$out/ours" \
    > "$out/ours.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || {
    echo "FAIL: our build exited $rc:"; tail -12 "$out/ours.txt"
    echo "      output stopping mid-way is the signature of a throw that"
    echo "      never found its handler"
    exit 1; }

if ! diff -u "$out/ref.txt" "$out/ours.txt" > "$out/diff.txt"; then
    echo "FAIL: lib/rt's unwinder does not behave as libgcc's:"
    head -30 "$out/diff.txt"
    exit 1
fi
echo "$(grep -c . "$out/ref.txt") lines identical to libgcc's unwinder over
ten cases: a catch several frames up with destructors between, a throw
through a C frame, a handler chosen by inheritance, a rethrow that finds
a different handler, an exception object with a destructor, catch-all, a
twelve-deep recursion, a nested try whose inner handler does not match,
an escape from a loop, and destructor order within one frame"

# ---- and the C frames are why it works ----------------------------------
#
# gcc emits unwind tables for C by default on Linux and EmbCC now does
# too. Checking it here rather than trusting it: without them the walk
# stops at the first C frame it meets, and since the unwinder's own
# frames are C it stops immediately -- every throw a terminate, which is
# exactly the symptom this started from.
cat > "$out/c.c" << 'EOF'
int f(int x) { return x + 1; }
EOF
"$EMBCC" --target="$ARCH-linux-gnu" -c "$out/c.c" -o "$out/c.o"
case "$ARCH" in
    x86_64) RE=x86_64-elf-readelf ;;
    *)      RE=aarch64-elf-readelf ;;
esac
"$RE" -SW "$out/c.o" | grep -q '\.eh_frame' || {
    echo "FAIL: a C translation unit for a hosted target has no"
    echo "      .eh_frame. An exception cannot unwind through it, and"
    echo "      the unwinder's own frames are C"; exit 1; }
"$EMBCC" --target="$TARGET" -c "$out/c.c" -o "$out/cf.o"
"$RE" -SW "$out/cf.o" | grep -q '\.eh_frame' && {
    echo "FAIL: the FREESTANDING target grew unwind tables, which are"
    echo "      pure size on a kernel's stack"; exit 1; }
echo "C compiles with unwind tables for the hosted target and without
them for the freestanding one, as gcc does"
