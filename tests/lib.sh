# Sourced by the golden tests, right after each prints its TEST-MARKER.
#
# Where this host keeps the tools and trees the golden tests need, and how to
# LINK and RUN an x86-64 program on it. Before this file each test hard-coded
# /home/motsou paths and called the host `cc` as its x86 gcc; on any other
# machine those tests quietly skipped — or linked arm64 clang output — and
# passed while proving nothing. Every lookup here tries this host's layout
# first and the original Linux one second, and each can be set outright.

# The repository root, whatever the caller's working directory — found by
# walking up from the test, since tests live at tests/golden/ and one level
# deeper in tests/golden/<arch>/.
if [ -z "${EMBCC_ROOT:-}" ]; then
    EMBCC_ROOT=$(cd "$(dirname "$0")" && pwd)
    while [ "$EMBCC_ROOT" != / ] && [ ! -f "$EMBCC_ROOT/tools/hostpaths.sh" ]; do
        EMBCC_ROOT=$(dirname "$EMBCC_ROOT")
    done
fi


. "$EMBCC_ROOT/tools/hostpaths.sh"
NEWLIB_INC=$X86_NEWLIB/include

# READELF / OBJDUMP / OBJCOPY come from tools/hostpaths.sh too.

# An x86-64 program is run natively only where the host IS x86-64 Linux; any
# other host boots it on the QEMU harness. EMBCC_X86_RUNNER forces one.
if [ -z "${EMBCC_X86_RUNNER:-}" ]; then
    if [ "$(uname -s)-$(uname -m)" = Linux-x86_64 ]; then
        EMBCC_X86_RUNNER=host
    else
        EMBCC_X86_RUNNER=qemu
    fi
fi

# The gcc that referees EmbCC's x86-64 output.
if [ "$EMBCC_X86_RUNNER" = host ]; then
    X86_GCC=${EMBCC_X86_GCC:-cc}
else
    X86_GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
fi

# x86_gcc_c SRC -o OBJ [flags...]  — compile C with the referee gcc.
x86_gcc_c() {
    if [ "$EMBCC_X86_RUNNER" = host ]; then
        "$X86_GCC" -c "$@"
    else
        # the cross gcc knows no libc of its own: point it at newlib's headers
        "$X86_GCC" -isystem "$NEWLIB_INC" -c "$@"
    fi
}

# x86_link EXE OBJ...  — link a hosted test program.
x86_link() {
    exe=$1; shift
    if [ "$EMBCC_X86_RUNNER" = host ]; then
        cc -no-pie -Wl,-z,noexecstack -o "$exe" "$@" -lm   # as tests/run.sh links
    else
        "$EMBCC_ROOT/tests/harness/x86_64/link.sh" -o "$exe" "$@"
    fi
}

# x86_run EXE  — run it; the guest's stdout, and its exit status.
x86_run() {
    if [ "$EMBCC_X86_RUNNER" = host ]; then
        "$1"
    else
        "$EMBCC_ROOT/tests/harness/x86_64/run.sh" "$1"
    fi
}

# x86_cannot_run  — prints why this host can neither run an x86-64 program
# itself nor boot one on the QEMU harness, and is true; false when it can.
# A Linux x86-64 host runs it; any other needs the harness's cross gcc and
# binutils, QEMU, and the cross newlib (docs/manual/getting-started.md).
x86_cannot_run() {
    [ "$EMBCC_X86_RUNNER" = host ] && return 1
    for _t in "$X86_GCC" "${EMBCC_X86_LD:-x86_64-elf-ld}" \
              "${EMBCC_X86_OBJCOPY:-x86_64-elf-objcopy}" qemu-system-x86_64; do
        command -v "$_t" >/dev/null 2>&1 || {
            echo "no $_t for the x86-64 QEMU harness"; return 0; }
    done
    [ -d "$X86_NEWLIB/lib" ] || {
        echo "no x86_64 newlib at $X86_NEWLIB for the x86-64 QEMU harness (set EMBCC_X86_NEWLIB)"
        return 0; }
    return 1
}

# qemu_plugin SRC OUT.so  — build a QEMU TCG plugin (the TriCore harness's
# output, tools/bench/icount.c's counts) against qemu-plugin.h, which must
# be the installed QEMU's own: QEMU_PLUGIN_INC names its directory, and
# Homebrew's is the default. Linux distributions package no qemu-plugin.h;
# it is include/qemu/qemu-plugin.h in the source of the installed version.
# Prints why, and is false, when the plugin cannot be built.
qemu_plugin() {
    _inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
    [ -f "$_inc/qemu-plugin.h" ] || {
        echo "no qemu-plugin.h in $_inc (set QEMU_PLUGIN_INC to the directory of the installed QEMU's include/qemu/qemu-plugin.h)"
        return 1; }
    # macOS links a plugin's calls into QEMU only when told they resolve
    # at load time; Linux does that unasked, and has no such flag
    # shellcheck disable=SC2046
    cc -shared -fPIC -O2 -I"$_inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
       -undefined dynamic_lookup -o "$2" "$1" > /dev/null 2>&1 ||
    cc -shared -fPIC -O2 -I"$_inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
       -o "$2" "$1" > "$2.log" 2>&1 || {
        echo "$1 does not build against $_inc/qemu-plugin.h (the plugins here are written for the plugin API of QEMU 9.1 or later): $(grep -m1 'error' "$2.log")"
        return 1; }
}

# objcopy_has_srec OBJCOPY  — that llvm-objcopy writes S-records, which
# LLVM 19 was the first to do (18 says "invalid output format: 'srec'").
# Asked of one byte of raw binary.
objcopy_has_srec() {
    _p=$(mktemp "${TMPDIR:-/tmp}/srec.XXXXXX")
    printf 'x' > "$_p"
    "$1" -I binary -O srec "$_p" "$_p.srec" > /dev/null 2>&1; _r=$?
    rm -f "$_p" "$_p.srec"
    return $_r
}

# x86_gcc_exe EXE SRC [flags...]  — compile and link with the referee gcc.
x86_gcc_exe() {
    exe=$1; src=$2; shift 2
    x86_gcc_c "$src" -o "$exe.gcc.o" "$@" && x86_link "$exe" "$exe.gcc.o"
}

# The tests call `readelf`, `objdump` and `objcopy` by name; route those names to the
# binaries found above. `command` skips functions, so READELF=readelf cannot
# recurse into itself.
readelf() { command "$READELF" "$@"; }
objdump() { command "$OBJDUMP" "$@"; }
objcopy() { command "$OBJCOPY" "$@"; }

# ---- target-generic: the machine tests/run.sh --target= selected ------------
TARGET=${EMBCC_TARGET:-x86_64-elf}
ARCH=${TARGET%-elf}

# t_gcc_c SRC -o OBJ [flags...]  — the target's referee gcc, compile only.
t_gcc_c() {
    if [ "$ARCH" = x86_64 ]; then
        x86_gcc_c "$@"
    else
        aarch64-elf-gcc -isystem "$AARCH64_NEWLIB/include" -c "$@"
    fi
}

# t_link EXE OBJ...  /  t_run EXE  — link and run a hosted program on the target.
t_link() {
    if [ "$ARCH" = x86_64 ]; then
        x86_link "$@"
    else
        exe=$1; shift
        "$EMBCC_ROOT/tests/harness/aarch64/link.sh" -o "$exe" "$@"
    fi
}
t_run() {
    if [ "$ARCH" = x86_64 ]; then
        x86_run "$1"
    else
        "$EMBCC_ROOT/tests/harness/aarch64/run.sh" "$1"
    fi
}

# pinned_elsewhere FILE.c  — true if the test says `// target: X` for another X.
# no_gcc_reference FILE: the program says gcc cannot build a reference for
# it (`// no-gcc-reference: why`) — e.g. gcc calls libatomic for what EmbCC
# does inline, and the cross toolchains have no libatomic. Prints why.
no_gcc_reference() {
    why=$(sed -n 's|.*// no-gcc-reference: *\(.*\)|\1|p' "$1" | head -1)
    [ -n "$why" ] || why=$(gcc_too_old_for "$1")
    [ -n "$why" ] && echo "$(basename "$1" .c): no gcc reference ($why), skipped"
}

# gcc_too_old_for FILE: prints what of FILE's C the target's referee gcc
# has not got, if anything -- _BitInt is GCC 14's, _Countof and the 0o
# octal constants GCC 16's, and Ubuntu 24.04's cc is GCC 13, 26.04's 15.
# Too old a referee is no reference, not a failure of the program;
# EMBCC_X86_GCC names a newer one. A feature is asked of the referee only
# for a program that uses it (gcc_has).
gcc_too_old_for() {
    if grep -q '_BitInt' "$1" && ! gcc_has bitint 'unsigned _BitInt(12) x;'; then
        echo "the referee gcc has no _BitInt; GCC 14 and later have"
    elif grep -q '_Countof' "$1" && ! gcc_has countof 'int a[3]; int n = _Countof(a);'; then
        echo "the referee gcc has no _Countof; GCC 16 and later have"
    elif grep -Eq '\b0[oO][0-7]' "$1" && ! gcc_has octal 'int n = 0o17;'; then
        echo "the referee gcc has no 0o constants; GCC 16 and later have"
    fi
}

# gcc_has NAME SOURCE: the target's referee gcc compiles SOURCE as C11, as
# the golden comparisons build. The answer is kept in _gcc_has_NAME (for
# the shell that asked).
gcc_has() {
    eval "_h=\${_gcc_has_$1:-}"
    if [ -z "$_h" ]; then
        _p=$(mktemp "${TMPDIR:-/tmp}/gcchas.XXXXXX")
        printf '%s\n' "$2" > "$_p.c"
        if t_gcc_c "$_p.c" -std=c11 -o "$_p.o" >/dev/null 2>&1; then
            _h=yes
        else
            _h=no
        fi
        rm -f "$_p" "$_p.c" "$_p.o"
        eval "_gcc_has_$1=\$_h"
    fi
    [ "$_h" = yes ]
}

pinned_elsewhere() {
    only=$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$1" | head -1)
    [ -n "$only" ] && [ "$only" != "$TARGET" ]
}
