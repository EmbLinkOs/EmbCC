#!/bin/sh
# Exec programs that need an operating system under them, run on Linux
# under QEMU against build/libc/linux-* (lib/libc/os/linux): no other C
# library, a real kernel's filesystem and the two long double formats the
# boards do not cover -- the x87's on x86-64, binary128 on AArch64.
#
#   tmpfile.c        tmpfile, tmpnam and fopen's "wx" against a real /tmp
#                    (the root is the program and an empty /tmp; built with
#                    EMBCC_TEST_HAS_FS, the test requires the file to open
#                    and its name to be gone while the stream is open)
#   wide-streams.c   wide streams on a temporary file, required to open
#   next-toward.c    nextafterl at 64 and 113 significand bits
#   quick-exit.c, multibyte-stdlib.c, inttypes-formats.c,
#   tgmath-selection.c, wide-printf.c, wide-scanf.c, c23-library.c
#   strtod-rounding.c  strtold at the x87's 64 bits and binary128's 113
#   math-special.c   ilogbl and logbl at long double's range
#   posix-files.c    mkstemp's family, realpath through links, fdopen,
#                    and freopen keeping the stream it was given
#   posix-strings.c, posix-stdio.c
#                    POSIX's strings, getopt, generators, memory streams:
#                    built with no -I, so <string.h> must be the C
#                    library's and not the compiler's declarations-only one
#   posix-fd.c, posix-process.c, posix-signals.c, posix-net.c
#                    descriptors and the *at calls, fork/spawn/system/popen,
#                    signals the kernel delivers, sockets and lookups. Each
#                    program is also the guest's /bin/sh, which
#                    posix-process.c is written to be (see it)
#   posix-time.c     TZ rules, mktime's tm_isdst, %z/%Z, strptime
#   malloc-gnu.c     <malloc.h>: usable sizes, mallinfo2, valloc/pvalloc
#   posix-threads.c  pthreads, semaphores and C11 threads, with real threads
#   posix-glob.c     glob, nftw, ftw and syslog over a real tree
#   posix-regex.c    regcomp/regexec, 88 cases with glibc's answers
#   posix-wordexp.c  wordexp, 105 strings; its command substitutions run
#                    this program as /bin/sh, as posix-process.c's do
#   posix-iconv.c    iconv, 59 conversions with glibc's answers
#   posix-lib2.c     locale objects, wide extras, generators, strfmon,
#                    Bessel and C23 math, dlfcn, databases, utmpx, fts
#   posix-linux2.c   waitid, timers, epoll and the other descriptors, IPC,
#                    aio, interfaces, error(), on_exit -- Linux's kernel
#   posix-sigjmp.c   sigsetjmp and siglongjmp, out of a handler too
#   posix-lib3.c     XSI signals, open_wmemstream, traps, mntent, ether,
#                    the pthread _np calls, errno's names, statx
#   posix-ucontext.c getcontext/makecontext/swapcontext coroutines
#   posix-lib4.c     argp's parsing and help, obstack, argz, backtrace, the
#                    res_* name codecs and parser, getaddrinfo_a, getopt's
#                    optind as GNU's permutes
#
# At -O0 and -O2, each exiting 42. Skipped without a kernel for
# tests/harness/linux (EMBCC_LINUX_KERNEL_X86_64 / _AARCH64).
set -u
echo "TEST-MARKER linux-libc-exec"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/linux-libc-exec
rm -rf "${out:?}"; mkdir -p "$out/root/tmp"
progs="tmpfile next-toward quick-exit multibyte-stdlib inttypes-formats
       tgmath-selection wide-streams wide-printf wide-scanf c23-library
       strtod-rounding math-special posix-files posix-strings posix-stdio
       posix-fd posix-process posix-signals posix-net posix-time
       malloc-gnu posix-threads posix-glob posix-regex posix-wordexp
       posix-iconv posix-lib2 posix-linux2 posix-sigjmp
       posix-lib3 posix-ucontext posix-lib4"

A64LD=${EMBCC_AARCH64_LD:-aarch64-elf-ld}
A64GCC=$(dirname "$(aarch64-elf-gcc -print-libgcc-file-name 2>/dev/null)")

ran=0; fail=0
for arch in x86_64 aarch64; do
    LIBDIR=$EMBCC_ROOT/build/libc/linux-$arch
    [ -f "$LIBDIR/libc.a" ] || { echo "skip $arch: no $LIBDIR/libc.a"; continue; }
    if [ $arch = aarch64 ] && { ! command -v "$A64LD" > /dev/null 2>&1 ||
                                [ ! -f "$A64GCC/libgcc.a" ]; }; then
        echo "skip $arch: no $A64LD and libgcc to link with"; continue
    fi
    "$EMBCC_ROOT/tests/harness/linux/run.sh" $arch --check > /dev/null 2>&1 || {
        echo "skip $arch: no kernel for tests/harness/linux"; continue; }
    for p in $progs; do
        for O in -O0 -O2; do
            tag=$arch-$p$O
            "$EMBCC" --target=$arch-linux-gnu $O -DEMBCC_TEST_HAS_FS \
                -c "$EMBCC_ROOT/tests/exec/$p.c" -o "$out/$tag.o" \
                > "$out/$tag.cc" 2>&1 || {
                echo "FAIL $tag: did not compile"; head -3 "$out/$tag.cc"
                fail=1; continue; }
            if [ $arch = x86_64 ]; then
                "$EMBCC_ROOT/embld" -o "$out/$tag.bin" "$LIBDIR/crt1.o" \
                    "$out/$tag.o" "$LIBDIR/libc.a" > "$out/$tag.ld" 2>&1
            else
                # embld writes no AArch64 executable; binary128 arithmetic
                # comes from libgcc, after libc.a (as tests/golden/linux.sh).
                "$A64LD" -static -T "$EMBCC_ROOT/lib/libc/os/linux/link.ld" \
                    -o "$out/$tag.bin" "$LIBDIR/crt1.o" "$out/$tag.o" \
                    "$LIBDIR/libc.a" -L"$A64GCC" -lgcc > "$out/$tag.ld" 2>&1
            fi || { echo "FAIL $tag: did not link"; head -3 "$out/$tag.ld"
                    fail=1; continue; }
            mkdir -p "$out/root/bin"
            cp "$out/$tag.bin" "$out/root/bin/sh"
            chmod 755 "$out/root/bin/sh"
            EMBCC_LINUX_ROOT=$out/root \
                "$EMBCC_ROOT/tests/harness/linux/run.sh" $arch "$out/$tag.bin" \
                > "$out/$tag.run" 2>&1
            rc=$?
            if [ "$rc" = 42 ]; then
                ran=$((ran + 1))
            else
                echo "FAIL $tag: exit $rc (42 expected)"; fail=1
            fi
        done
    done
done
[ $fail = 0 ] || exit 1
echo "linux-libc-exec: $ran runs on Linux exit 42"
