# Sourced by the golden tests, right after each prints its TEST-MARKER.
#
# Where this host keeps the tools and trees the golden tests need, and how to
# LINK and RUN an x86-64 program on it. Before this file each test hard-coded
# /home/motsou paths and called the host `cc` as its x86 gcc; on any other
# machine those tests quietly skipped — or linked arm64 clang output — and
# passed while proving nothing. Every lookup here tries this host's layout
# first and the original Linux one second, and each can be set outright.

# The repository root, whatever the caller's working directory.
EMBCC_ROOT=${EMBCC_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}


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
        cc -no-pie -o "$exe" "$@"
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
pinned_elsewhere() {
    only=$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$1" | head -1)
    [ -n "$only" ] && [ "$only" != "$TARGET" ]
}
