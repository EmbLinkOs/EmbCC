#!/bin/sh
# The Linux target (D-014): a static executable with nothing under it.
#
# Linux is the first operating system this library reaches WITHOUT
# another C library in between. The EmbLinkOS backend calls that
# kernel; the posixlike backend forwards to whatever host library is
# already loaded. lib/libc/os/linux/backend.c issues syscalls, so the
# program this test builds contains our printf, our malloc, our
# strtod -- and no glibc, no musl, no dynamic loader and no crt from
# anyone else. `nm -u` on the result is the check that says so, and it
# is the one that matters most: a single undefined symbol would mean
# something was expected to arrive from outside.
#
# ---- what is checked where ------------------------------------------------
#
# Three of the four parts below run anywhere a cross ld exists, because
# they are about the FILE: its entry code, its shape, its symbols.
#
# The fourth cannot. Every number the backend depends on -- syscall
# numbers, errno values, open flags, the offsets inside statx -- belongs
# to the kernel, and there is no kernel header on a macOS build machine
# to check them against. So that part compiles our syscall.h TOGETHER
# WITH the kernel's own headers and makes the compiler assert that every
# pair agrees, and it runs only on Linux. Everywhere else it says it
# skipped rather than passing quietly: unverified numbers should look
# unverified.
set -eu
echo "TEST-MARKER linux"
. "$(dirname "$0")/../lib.sh"

case "$ARCH" in
    x86_64)  triple=x86_64-linux-gnu;  LD=${EMBCC_X86_LD:-x86_64-elf-ld}
             OD=x86_64-elf-objdump;    RE=x86_64-elf-readelf
             NM=x86_64-elf-nm ;;
    aarch64) triple=aarch64-linux-gnu; LD=${EMBCC_AARCH64_LD:-aarch64-elf-ld}
             OD=aarch64-elf-objdump;   RE=aarch64-elf-readelf
             NM=aarch64-elf-nm ;;
    *) echo "skipped: no Linux triple for $ARCH"; exit 0 ;;
esac

LIBDIR=$EMBCC_ROOT/build/libc/linux-$ARCH
[ -f "$LIBDIR/libc.a" ] || {
    echo "skipped: $LIBDIR/libc.a absent (make libc-linux-$ARCH)"; exit 0; }
command -v "$LD" > /dev/null 2>&1 || {
    echo "skipped: no $LD to link a Linux image with"; exit 0; }
command -v "$OD" > /dev/null 2>&1 || {
    echo "skipped: no $OD"; exit 0; }

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/linux-$ARCH
rm -rf "$out"; mkdir -p "$out"

# ---- 1. the entry stub, decoded by something that is not us ---------------
#
# _start is written as .byte/.long in lib/libc/os/linux/start.c because
# EmbCC has no general text assembler. Bytes with a comment claiming
# what they mean are worth nothing on their own, so the comment is not
# what is checked here: objdump decodes the bytes and its answer is
# compared against what the kernel's entry protocol requires. If the
# bytes were wrong, objdump would say something else.
"$OD" -dr "$LIBDIR/crt1.o" > "$out/crt1.txt"
sed -n '/<_start>:/,/^$/p' "$out/crt1.txt" > "$out/entry.txt"
[ -s "$out/entry.txt" ] || {
    echo "FAIL: crt1.o defines no _start"; cat "$out/crt1.txt"; exit 1; }

want_entry() {
    grep -q "$1" "$out/entry.txt" || {
        echo "FAIL: _start does not $2"
        echo "      (looked for /$1/ in the disassembly)"
        cat "$out/entry.txt"; exit 1; }
}
if [ "$ARCH" = x86_64 ]; then
    #  The frame pointer is zeroed so an unwinder stops here; rsp is
    #  captured BEFORE it is aligned, because rsp IS the argument.
    want_entry 'xor *%rbp,%rbp'      'zero the frame pointer'
    want_entry 'mov *%rsp,%rdi'      'pass the stack block as argument 1'
    want_entry 'and *\$0xfff*0,%rsp' 'align the stack the ABI requires'
    want_entry 'call'                'call into C'
else
    want_entry 'mov.*x29, #0x0'      'zero the frame pointer'
    want_entry 'mov.*x30, #0x0'      'zero the link register'
    want_entry 'mov.*x0, sp'         'pass the stack block as argument 1'
    want_entry 'ldr.*x1'             'load the C entry point'
    want_entry 'br.*x1'              'branch to it'
    #  On aarch64 the branch goes through a .quad, because an asm block
    #  with no encodable instructions cannot carry a call relocation.
    #  That the .quad became a real relocation -- and not eight zeroes,
    #  which would link and jump to address 0 -- is the check.
    grep -q 'R_AARCH64_ABS64.*__libc_start' "$out/crt1.txt" || {
        echo "FAIL: the .quad naming __libc_start carries no relocation"
        cat "$out/crt1.txt"; exit 1; }
fi
grep -q '__libc_start' "$out/crt1.txt" || {
    echo "FAIL: _start does not reach __libc_start"; exit 1; }
echo "_start: objdump decodes the entry stub as the kernel's protocol
requires, and its reference to C is a real relocation"

# ---- 2. a hosted program, built the way a user would ----------------------
#
# It reaches past stdio and calls the seam directly, because stdio only
# uses five of the backend's thirty-eight entry points and the other
# thirty-three include the two whose STRUCTURE LAYOUTS could not be
# checked on the build machine: statx and getdents64. Those are exactly
# the places where a wrong offset reads a plausible number out of the
# wrong field, so they are what part 5 runs.
cat > "$out/hello.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backend.h"

static int fails;
static void ok(const char *what, int cond)
{
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}

int main(int argc, char **argv)
{
    /* The library through its public face: stdio's formatting and
     * buffering, the arguments the kernel put on the stack, and the
     * heap through brk. */
    printf("hello from EmbCC on Linux\n");
    printf("argc=%d arg0=%s\n", argc, argc > 0 ? argv[0] : "(none)");
    char *p = malloc(64);
    ok("malloc", p != 0);
    if (p) {
        strcpy(p, "heap");
        printf("%s %.3f %ld\n", p, 1.5, strtol("41", 0, 10) + 1);
        free(p);
    }

    /* A directory and a file in it, written and read back. The path is
     * relative so this works both as PID 1 in an initramfs (where the
     * working directory is /) and as an ordinary program. */
    ok("mkdir", __os_mkdir("embcc-d", 0755) == 0);
    FILE *f = fopen("embcc-d/f", "w");
    ok("fopen w", f != 0);
    if (f) {
        fprintf(f, "%s %d\n", "written", 7);
        ok("fclose", fclose(f) == 0);
    }
    char line[64];
    line[0] = 0;
    f = fopen("embcc-d/f", "r");
    ok("fopen r", f != 0);
    if (f) {
        ok("fgets", fgets(line, sizeof line, f) != 0);
        fclose(f);
    }
    ok("round trip", strcmp(line, "written 7\n") == 0);

    /* statx. Ten bytes went in, so ten bytes is what the size field has
     * to say -- and it says it from offset 40 of a structure this tree
     * wrote down from the kernel's documentation and could not verify
     * until here. */
    struct __os_fileinfo st;
    ok("stat", __os_stat("embcc-d/f", &st) == 0);
    ok("stat type", st.type == __OS_FT_REGULAR);
    ok("stat size", st.size == 10);
    ok("stat nlink", st.nlink == 1);
    ok("stat mode", (st.mode & 0600) == 0600);
    ok("stat dir", __os_stat("embcc-d", &st) == 0 &&
                   st.type == __OS_FT_DIRECTORY);

    /* getdents64, the other variable-length structure. One entry, since
     * the seam filters `.` and `..` and nothing else is in there. */
    void *d = __os_opendir("embcc-d");
    ok("opendir", d != 0);
    char name[64];
    int type = -1, n = 0;
    name[0] = 0;
    while (d && __os_readdir(d, name, sizeof name, &type) == 1)
        n++;
    if (d) __os_closedir(d);
    ok("readdir count", n == 1);
    ok("readdir name", strcmp(name, "f") == 0);
    ok("readdir type", type == __OS_FT_REGULAR);

    char cwd[128];
    cwd[0] = 0;
    ok("getcwd", __os_getcwd(cwd, sizeof cwd) == 0 && cwd[0] == '/');

    /* A plausible clock rather than a fixed one: the check is that the
     * seconds field was read from the right place, not what time it is. */
    ok("time", __os_time() > 1600000000L);

    ok("unlink", __os_unlink("embcc-d/f") == 0);
    ok("rmdir", __os_rmdir("embcc-d") == 0);
    ok("gone", __os_stat("embcc-d", &st) != 0);

    printf("seam: %d checks, %d failed\n", 21, fails);
    return fails ? 1 : 42;
}
EOF
"$EMBCC" --target="$triple" -c "$out/hello.c" -I"$EMBCC_ROOT/lib/libc/include" \
    -I"$EMBCC_ROOT/lib/libc/os" -o "$out/hello.o" 2> "$out/cc.log" || {
    echo "FAIL: embcc could not compile for $triple:"; cat "$out/cc.log"
    exit 1; }
"$LD" -static -e _start -o "$out/hello" \
    "$LIBDIR/crt1.o" "$out/hello.o" "$LIBDIR/libc.a" 2> "$out/ld.log" || {
    echo "FAIL: linking a static Linux image:"; cat "$out/ld.log"; exit 1; }

# ---- 3. the shape Linux will accept, and what is NOT in it ----------------
"$RE" -hlS "$out/hello" > "$out/elf.txt" 2>&1
grep -q 'Type: *EXEC' "$out/elf.txt" || {
    echo "FAIL: not an executable:"; head -20 "$out/elf.txt"; exit 1; }
#  An INTERP segment names a dynamic loader; a DYNAMIC section names
#  shared libraries. This image is supposed to have neither -- it is the
#  difference between "runs on a kernel" and "runs on a distribution".
! grep -q 'INTERP' "$out/elf.txt" || {
    echo "FAIL: the image asks for a dynamic loader:"
    grep INTERP "$out/elf.txt"; exit 1; }
! grep -q ' DYNAMIC ' "$out/elf.txt" || {
    echo "FAIL: the image has a dynamic section:"; exit 1; }
grep -q 'LOAD' "$out/elf.txt" || {
    echo "FAIL: nothing to load:"; head -20 "$out/elf.txt"; exit 1; }

#  The entry address the kernel will jump to has to BE _start. A file
#  whose entry is anything else runs whatever happens to sit there.
entry=$(sed -n 's/ *Entry point address: *0x*//p' "$out/elf.txt" | tr -d ' ')
startaddr=$("$NM" "$out/hello" | sed -n 's/^0*\([0-9a-f]*\) T _start$/\1/p')
[ "$(printf '%d' "0x$entry")" = "$(printf '%d' "0x$startaddr")" ] || {
    echo "FAIL: the entry point is 0x$entry but _start is at 0x$startaddr"
    exit 1; }

#  The claim of the whole file: nothing is expected from outside.
"$NM" -u "$out/hello" > "$out/undef.txt" 2>&1 || true
[ ! -s "$out/undef.txt" ] || {
    echo "FAIL: the image expects symbols from somewhere else:"
    cat "$out/undef.txt"; exit 1; }

#  And it really does talk to the kernel itself, rather than having
#  been quietly linked against something that would. There are five of
#  these and not forty: syscall.h routes every call through one helper
#  per ARITY, so the instruction appears once per helper and the
#  numbers travel in a register.
if [ "$ARCH" = x86_64 ]; then insn=syscall; else insn='svc'; fi
n=$("$OD" -d "$out/hello" | grep -c "	$insn" || true)
[ "$n" -ge 1 ] || {
    echo "FAIL: no $insn instruction in the image -- the backend is not"
    echo "      reaching the kernel, so something else must be"; exit 1; }
for f in __os_write __os_read __os_sbrk __os_exit; do
    "$NM" "$out/hello" | grep -q " T $f\$" || {
        echo "FAIL: $f is not defined in the image"; exit 1; }
done
echo "static image: EXEC, entry is _start, no interpreter, no dynamic
section, no undefined symbol, and $n $insn sites of its own behind the
seam's own __os_* functions"

# ---- 4. the kernel's numbers, checked against the kernel -------------------
if [ "$(uname -s)" != Linux ]; then
    echo "skipped on $(uname -s): the syscall numbers, errno values, open
flags and statx offsets in lib/libc/os/linux/syscall.h are Linux's, and
checking them needs Linux's headers. Run this on Linux to verify them."
else
    cat > "$out/abi.c" << 'EOF'
/* Our header and the kernel's, compiled together, with the compiler
 * asked to prove that every number we wrote down is the number the
 * kernel actually uses. Nothing is printed on success: a _Static_assert
 * that holds produces no code, and one that does not stops the build
 * naming the pair that disagreed. */
#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <linux/futex.h>
#include <asm/ioctls.h>
#include <stddef.h>
#include "syscall.h"

#define SAME(ours, theirs) \
    _Static_assert((ours) == (theirs), #ours " is not " #theirs)

SAME(LSYS_read, __NR_read);            SAME(LSYS_write, __NR_write);
SAME(LSYS_close, __NR_close);          SAME(LSYS_lseek, __NR_lseek);
SAME(LSYS_brk, __NR_brk);              SAME(LSYS_ioctl, __NR_ioctl);
SAME(LSYS_sched_yield, __NR_sched_yield);
SAME(LSYS_nanosleep, __NR_nanosleep);  SAME(LSYS_exit, __NR_exit);
SAME(LSYS_exit_group, __NR_exit_group);
SAME(LSYS_ftruncate, __NR_ftruncate);  SAME(LSYS_truncate, __NR_truncate);
SAME(LSYS_getcwd, __NR_getcwd);        SAME(LSYS_chdir, __NR_chdir);
SAME(LSYS_statfs, __NR_statfs);        SAME(LSYS_futex, __NR_futex);
SAME(LSYS_getdents64, __NR_getdents64);
SAME(LSYS_clock_gettime, __NR_clock_gettime);
SAME(LSYS_openat, __NR_openat);        SAME(LSYS_mkdirat, __NR_mkdirat);
SAME(LSYS_newfstatat, __NR_newfstatat);
SAME(LSYS_unlinkat, __NR_unlinkat);    SAME(LSYS_renameat, __NR_renameat);
SAME(LSYS_linkat, __NR_linkat);        SAME(LSYS_symlinkat, __NR_symlinkat);
SAME(LSYS_readlinkat, __NR_readlinkat);
SAME(LSYS_fchmodat, __NR_fchmodat);    SAME(LSYS_utimensat, __NR_utimensat);
SAME(LSYS_getrandom, __NR_getrandom);  SAME(LSYS_statx, __NR_statx);

/* The *at calls' constants, the futex operations, and the terminal
 * question isatty asks. */
SAME(LAT_FDCWD, AT_FDCWD);
SAME(LAT_SYMLINK_NOFOLLOW, AT_SYMLINK_NOFOLLOW);
SAME(LAT_REMOVEDIR, AT_REMOVEDIR);
SAME(LAT_EMPTY_PATH, AT_EMPTY_PATH);
SAME(LAT_STATX_SYNC_AS_STAT, AT_STATX_SYNC_AS_STAT);
SAME(LSTATX_BASIC_STATS, STATX_BASIC_STATS);
SAME(LFUTEX_WAIT, FUTEX_WAIT);
SAME(LFUTEX_WAKE, FUTEX_WAKE);
SAME(LFUTEX_PRIVATE, FUTEX_PRIVATE_FLAG);
SAME(LTCGETS, TCGETS);

/* The seam's own open flags (lib/libc/os/backend.h) claim to be the
 * ones Linux chose, which is why this backend passes them straight
 * through instead of translating. */
SAME(__OS_O_RDONLY, O_RDONLY);  SAME(__OS_O_WRONLY, O_WRONLY);
SAME(__OS_O_RDWR, O_RDWR);      SAME(__OS_O_CREAT, O_CREAT);
SAME(__OS_O_EXCL, O_EXCL);      SAME(__OS_O_TRUNC, O_TRUNC);
SAME(__OS_O_APPEND, O_APPEND);
/* The one flag whose value DIFFERS between the two architectures --
 * x86-64 and aarch64 swap it with O_DIRECT -- so it is the one most
 * worth asking the kernel about. */
SAME(LO_DIRECTORY, O_DIRECTORY);
SAME(LS_IFMT, S_IFMT);  SAME(LS_IFREG, S_IFREG);  SAME(LS_IFDIR, S_IFDIR);
SAME(LS_IFLNK, S_IFLNK);

/* errno. The backend sets errno to -result with no translation table,
 * which is only correct if the two numberings really are one. */
SAME(EPERM, 1); SAME(ENOENT, 2); SAME(EBADF, 9); SAME(EAGAIN, 11);
SAME(ENOMEM, 12); SAME(EEXIST, 17); SAME(EISDIR, 21); SAME(EINVAL, 22);
SAME(ENOSYS, 38); SAME(ELOOP, 40); SAME(ENAMETOOLONG, 36);

/* And the structures, field by field. This is the reason statx was
 * chosen over fstatat: one layout to get right instead of two. */
#define AT_SAME(f) _Static_assert( \
    offsetof(struct lstatx, f) == offsetof(struct statx, f), \
    "lstatx." #f " is at the wrong offset")
_Static_assert(sizeof(struct lstatx) == sizeof(struct statx),
               "lstatx is the wrong size");
AT_SAME(stx_mask); AT_SAME(stx_blksize); AT_SAME(stx_nlink);
AT_SAME(stx_mode); AT_SAME(stx_ino); AT_SAME(stx_size);
AT_SAME(stx_mtime); AT_SAME(stx_dev_major); AT_SAME(stx_dev_minor);
_Static_assert(offsetof(struct lstatx_timestamp, tv_sec) == 0 &&
               offsetof(struct lstatx_timestamp, tv_nsec) == 8,
               "a statx timestamp is laid out differently");
int main(void) { return 0; }
EOF
    cc -I"$EMBCC_ROOT/lib/libc/os/linux" -I"$EMBCC_ROOT/lib/libc/include" \
        -o "$out/abi" "$out/abi.c" 2> "$out/abi.log" || {
        echo "FAIL: our numbers disagree with this kernel's headers:"
        grep -E 'error|static.assert' "$out/abi.log" | head -20; exit 1; }
    echo "every syscall number, flag, errno value and statx offset in
syscall.h agrees with this machine's own kernel headers"
fi

# ---- 5. and it runs -------------------------------------------------------
#
# Everything above is about a FILE. This is the only part that proves a
# syscall was ever issued, and it needs a Linux kernel of the right
# architecture. There are two ways to have one: be Linux, or boot one --
# tests/harness/linux/run.sh starts a real kernel under QEMU and runs
# the image as PID 1, which is what makes this checkable from a machine
# that is not Linux at all.
#
# A hand-written stub could not stand in for either. It would implement
# the same syscall numbers this backend calls, agree with them by
# construction, and go on agreeing if every one of them were wrong.
host=$(uname -s)-$(uname -m)
runner=none
case "$ARCH-$host" in
    x86_64-Linux-x86_64|aarch64-Linux-aarch64) runner=native ;;
    *) if "$EMBCC_ROOT/tests/harness/linux/run.sh" "$ARCH" --check \
            > /dev/null 2>&1; then
           runner=qemu
       fi ;;
esac

if [ "$runner" = none ]; then
    echo "skipped: running it needs Linux/$ARCH (this is $host) or a
kernel for tests/harness/linux/run.sh, which says where to get one. The
image above is checked as a file only -- no syscall in it has run."
    exit 0
fi

set +e
if [ "$runner" = native ]; then
    ( cd "$out" && ./hello one two ) > "$out/run.txt" 2>&1
else
    "$EMBCC_ROOT/tests/harness/linux/run.sh" "$ARCH" "$out/hello" \
        > "$out/run.txt" 2>&1
fi
rc=$?
set -e

[ "$rc" != 124 ] || { echo "FAIL: the guest timed out"; exit 1; }
[ "$rc" = 42 ] || {
    echo "FAIL: the program exited $rc, wanted 42:"; cat "$out/run.txt"
    exit 1; }
grep -qx 'hello from EmbCC on Linux' "$out/run.txt" || {
    echo "FAIL: stdio did not reach the kernel:"; cat "$out/run.txt"; exit 1; }
#  argv is read off the initial stack by our own _start, so what it
#  contains depends on who started the program: a shell passes the two
#  arguments, and the kernel starting PID 1 passes only its path.
if [ "$runner" = native ]; then
    want_argv='argc=3 arg0=./hello'
else
    want_argv='argc=1 arg0=/init'
fi
grep -qx "$want_argv" "$out/run.txt" || {
    echo "FAIL: argv did not come off the initial stack correctly"
    echo "      (wanted \"$want_argv\")"; cat "$out/run.txt"; exit 1; }
grep -qx 'heap 1.500 42' "$out/run.txt" || {
    echo "FAIL: the heap, float formatting or strtol is wrong:"
    cat "$out/run.txt"; exit 1; }
grep -qx 'seam: 21 checks, 0 failed' "$out/run.txt" || {
    echo "FAIL: the seam checks did not all pass:"; cat "$out/run.txt"
    exit 1; }

if [ "$runner" = native ]; then
    echo "and it RUNS on this kernel: stdio, argv off the initial stack,
the heap through brk, and 21 checks over files, statx, getdents64, getcwd
and the clock -- exit status 42"
else
    echo "and it RUNS as PID 1 on a real Linux kernel under QEMU: stdio,
argv off the initial stack, the heap through brk, and 21 checks over
files, statx, getdents64, getcwd and the clock. The exit status 42 is
the kernel's own report of what main returned."
fi
