# Libraries

EmbCC ships three libraries of its own, all compiled by EmbCC: a C library
(`lib/libc`), a C++ runtime and standard library (`lib/libcxx`), and the
compiler runtime (`lib/rt`), which holds the routines generated code calls
for operations the machine has no instruction for. This chapter says what
each contains and leaves out, which targets each is built for, how the
driver links them, and how to build them. It is for anyone who links a
program with EmbCC or wants to know which part of the C or C++ library a
program can rely on. Firmware-specific use of the runtime is in
[Bare-metal programming](embedded.md).

## Overview

| Library | Source | Archive | Built for | Make target |
|---|---|---|---|---|
| C library | `lib/libc` | `libc.a` (+ `crt1.o` on Linux) | `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu`, `aarch64-linux-gnu`, EmbLinkOS | `libc-x86_64`, `libc-aarch64`, `libc-linux-x86_64`, `libc-linux-aarch64`, `libc-emblinkos` |
| C++ runtime and standard library | `lib/libcxx` | `libcxx.a` | `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu`, `aarch64-linux-gnu`, EmbLinkOS; the runtime alone (no exceptions) for 32-bit ARM and `riscv32-unknown-elf` | `libcxx-x86_64`, `libcxx-aarch64`, `libcxx-linux-x86_64`, `libcxx-linux-aarch64`, `libcxx-emblinkos`, `libcxx-embedded` |
| Compiler runtime | `lib/rt` | `librt.a` | `x86_64-linux-gnu`, `aarch64-linux-gnu`, EmbLinkOS, and the embedded triples | `libc-linux-*` (Linux), `libc-emblinkos`, `rt-embedded` (embedded) |

Every library is built per target by the `embcc` in the same tree, so it
always matches the compiler's ABI. No library exists for the macOS or
Windows triples: a Darwin object is linked by the system linker against
the system's libraries, and the Windows triple has no C library.

## How the driver links the libraries

### When the driver links at all

`embcc [FILE] [OBJECTS...] -o OUT` (no `-c`, `-S`, `-E` or
`--syntax-only`) compiles the source, if there is one, and links in the
same process with [EmbLD](tools/embld.md). The driver links the x86-64 ELF
targets (`x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu`) and, given a
memory map (`--link-script=board.ld`, or `-Wl,-Ttext`/`-Tdata`), the ARM, RISC-V and
AVR firmware targets; for AArch64 ELF, Mach-O and COFF targets it stops
with `embcc: error: cannot link for TRIPLE`. The details are in
[Invoking](invoking.md#linking).

### The link line

The driver builds a fixed link line, in this order:

1. `crt1.o`, if the target has one (only the Linux triples do);
2. the object it just compiled;
3. `libcxx.a`, when the source is C++;
4. `libc.a`, if the target has one;
5. `librt.a`, if the target has one.

`libcxx.a` comes before `libc.a` because the C++ runtime calls into the C
library (`operator new` is `malloc`), and `librt.a` comes last because a
C library routine can call the runtime (printing a 128-bit value divides
it). Archives are searched left to right with a final pass that
revisits them, so a member pulled in late can still resolve against an
earlier archive. Only the archive members a program references are
linked, and the embedded `libc.a` and `librt.a` have a section per
function and per object, so an image linked with `--gc-sections` keeps
only the functions it reaches inside those members
([Embedded](embedded.md#leaving-out-what-is-never-called---gc-sections)).

A missing library is reported by name rather than as a list of undefined
symbols:

```text
embcc: error: no crt1.o for x86_64-linux-gnu: its C library is not built; `make libc-linux-x86_64` in /path/to/EmbCC builds it
embcc: --print-search-dirs says where it looked
```

An installed compiler names `make install` instead of a make target. A
C++ program on a target with no `libcxx.a` fails the same way, naming
the C++ runtime's make target. On a hosted
(Linux) target a missing `crt1.o` or `libc.a` is an error; on a
freestanding target the driver links what it finds, and the program must
define `_start`.

### Where the libraries are found

The driver looks for a target's files relative to its own executable:

| Layout | Path of `libc.a` for `x86_64-linux-gnu` |
|---|---|
| Build tree (`embcc` beside `lib/libc/include/stdio.h`) | `build/libc/linux-x86_64/libc.a` |
| Installation (`PREFIX/bin/embcc`) | `PREFIX/lib/embcc/VERSION/x86_64-linux-gnu/libc.a` |

In a build tree the directory is `build/libc/x86_64` for `x86_64-elf`
and `x86_64-emblink`, `build/libc/aarch64` for `aarch64-elf`,
`build/libc/linux-x86_64` and `build/libc/linux-aarch64` for the Linux
triples, `build/libc/darwin-aarch64` and `build/libc/darwin-x86_64` for
macOS (`make rt-darwin-aarch64 rt-darwin-x86_64`), and
`build/libc/TRIPLE` for every other triple. `libcxx.a` is
under `build/libcxx/` in the same scheme. An installation has one
directory per canonical triple and none for `x86_64-emblink`. The search
for the installation itself, and `EMBCC_PREFIX`, are described in
[Invoking](invoking.md#how-the-compiler-finds-its-own-files).

### Opting out

The driver has no `--no-stdlib`, `--no-default-libs` or `--no-start-files`, and no
`-l` or `-L`; each is rejected as an unknown argument:

```text
embcc: error: unknown argument '-nostdlib'
```

The driver's link takes EmbLD's own options through `-Wl,` and
`--linker-arg` (`--entry`, `-Ttext`, `-Tdata`, `-Tstack`, `--rom-limit`,
`--lma-offset`; see [Invoking](invoking.md#--linker-argarg)), but not
other inputs. To link without EmbCC's libraries, or with others, compile
with `-c` and run `embld` with exactly the inputs wanted:

```sh
embcc -c --opt=speed kernel.c -o kernel.o
embld -e _start -Ttext 0xFFFFFFFF80100000 -o kernel.elf entry.o kernel.o
```

`--freestanding` and `--no-builtins` are accepted and change neither the
link line nor the header search. `--no-std-include` removes EmbCC's header
directories from the search (see [Invoking](invoking.md#--no-std-include)).

### Header directories

EmbCC's own headers are searched after every `-I` and `--system-include`
directory, as system directories:

| Headers | Build tree | Installation |
|---|---|---|
| C++ library | `lib/libcxx/include` | `PREFIX/lib/embcc/VERSION/include/c++` |
| C library | `lib/libc/include` | `PREFIX/lib/embcc/VERSION/include` |
| Freestanding (`<stddef.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stdint.h>`, `<limits.h>`, `<float.h>`, a declarations-only `<string.h>`, `<unwind.h>`, `<cpuid.h>`, for Cortex-M `<arm_cmse.h>`, and for Cortex-M and ARMv7-A `<arm_acle.h>`) | `include` | `PREFIX/lib/embcc/VERSION/freestanding` |

`--print-search-dirs` prints the directories found. The full search order,
including the `include` directory beside the invoked executable, is in
[Invoking](invoking.md#search-order).

<!-- In a build tree, the `include` directory beside the executable IS the
     freestanding directory, and it is searched before lib/libc/include
     (src/driver/main.c appends it before compile_unit appends the three
     default directories). So `#include <string.h>` from a build tree
     finds the declarations-only header (no strcoll, so <cstring> and
     <iostream> fail to compile) unless -I lib/libc/include is given. The
     tests pass that -I explicitly. Reported to the lead. -->

## The C library: `lib/libc`

### Scope

`lib/libc` is a C11 library written for EmbCC, with C23's additions:
the portable part is the same source on every target, and each operating
system is reached through one small backend file (see
[Backends](#backends)). Its headers are the C11 hosted set:

`<assert.h>` `<complex.h>` `<ctype.h>` `<errno.h>` `<fenv.h>`
`<inttypes.h>` `<iso646.h>` `<limits.h>` `<locale.h>` `<math.h>`
`<setjmp.h>` `<signal.h>` `<stdalign.h>` `<stdatomic.h>` `<stdio.h>`
`<stdlib.h>` `<stdnoreturn.h>` `<string.h>` `<tgmath.h>` `<threads.h>`
`<time.h>` `<uchar.h>` `<wchar.h>` `<wctype.h>`

and, of POSIX's and the common Unix ones, `<aio.h>` `<arpa/inet.h>`
`<dirent.h>` `<dlfcn.h>` `<elf.h>` `<endian.h>` `<err.h>` `<error.h>` `<fcntl.h>`
`<fmtmsg.h>` `<fnmatch.h>` `<fts.h>` `<ftw.h>` `<getopt.h>` `<glob.h>` `<grp.h>`
`<iconv.h>` `<ifaddrs.h>` `<langinfo.h>` `<libgen.h>` `<libintl.h>` `<link.h>`
`<malloc.h>` `<mntent.h>` `<monetary.h>` `<mqueue.h>` `<net/if.h>` `<netdb.h>`
`<netinet/ether.h>` `<netinet/in.h>` `<netinet/tcp.h>` `<netpacket/packet.h>` `<nl_types.h>` `<paths.h>` `<poll.h>`
`<pthread.h>` `<pty.h>` `<pwd.h>` `<regex.h>` `<sched.h>` `<search.h>`
`<semaphore.h>` `<shadow.h>` `<spawn.h>` `<strings.h>` `<syslog.h>` `<termios.h>`
`<ucontext.h>` `<ulimit.h>` `<unistd.h>` `<utime.h>` `<utmp.h>` `<utmpx.h>`
`<wordexp.h>` and
`<sys/auxv.h>` `<sys/epoll.h>` `<sys/eventfd.h>` `<sys/file.h>` `<sys/inotify.h>`
`<sys/ioctl.h>` `<sys/ipc.h>` `<sys/klog.h>` `<sys/mman.h>` `<sys/mount.h>`
`<sys/msg.h>` `<sys/param.h>` `<sys/personality.h>` `<sys/prctl.h>`
`<sys/random.h>` `<sys/reboot.h>` `<sys/resource.h>` `<sys/select.h>`
`<sys/sem.h>` `<sys/sendfile.h>` `<sys/shm.h>` `<sys/signalfd.h>`
`<sys/socket.h>` `<sys/stat.h>` `<sys/statfs.h>` `<sys/statvfs.h>` `<sys/swap.h>`
`<sys/syscall.h>` `<sys/sysinfo.h>` `<sys/sysmacros.h>` `<sys/time.h>`
`<sys/timeb.h>` `<sys/timerfd.h>` `<sys/times.h>` `<sys/types.h>` `<sys/ucontext.h>`
`<sys/uio.h>` `<sys/un.h>` `<sys/utsname.h>` `<sys/vfs.h>` `<sys/wait.h>`
`<sys/xattr.h>`. Where a C header also
declares POSIX names (`getline` in `<stdio.h>`, `strtok_r` in
`<string.h>`), `<features.h>` decides: they are declared unless the
program asked for ISO C alone (`-std=c11` with no `_POSIX_C_SOURCE`,
`_XOPEN_SOURCE`, `_DEFAULT_SOURCE`, `_BSD_SOURCE` or `_GNU_SOURCE`), and
the GNU and BSD ones (`strchrnul`, `memrchr`, `clearenv`, `fopencookie`)
are hidden too when the program names a POSIX version and nothing more,
as with glibc. In the default mode `<string.h>` also brings
`<strings.h>`, and `<stdlib.h>` `<alloca.h>`, as glibc's do. The
large-file names (`open64`, `stat64`, `fopen64` ...) are the same calls,
off_t being 64 bits already: macros under `_GNU_SOURCE` or
`_LARGEFILE64_SOURCE`, and symbols for a program that declares its own.

Linux's own interfaces -- `<sys/epoll.h>` and the other descriptor
kinds, `<sys/syscall.h>` and `syscall`, `statfs`, `sysinfo`, `prctl`,
mounting, extended attributes, processor affinity, POSIX message queues
and System V IPC -- are its system calls where the kernel is Linux.
Elsewhere each fails with `ENOSYS`, unless it has a meaning the library
can give without that kernel: `sendfile` and `copy_file_range` copy
through a buffer, `statfs` comes from `statvfs`, `close_range` closes,
locking memory where nothing pages already holds.

Every POSIX function is defined weak, so a program -- or an operating
system's own support library linked beside this one -- may define any of
them itself and its definition is the one used. C's names are strong.

`<stddef.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stdint.h>` and `<float.h>`
come from the freestanding directory, and so do C23's `<stdbit.h>` and
`<stdckdint.h>`, which a program with no C library has too. `<limits.h>` defines the library's
own limits (`PATH_MAX` 4096, `NAME_MAX` 255, ...) and includes the
freestanding `<limits.h>` for the type widths.

| Area | What is provided |
|---|---|
| `<string.h>` | all of C11 §7.24, plus `strdup`, `strndup`, `strnlen`, `stpcpy`, `stpncpy` and `mempcpy`; POSIX's `strtok_r`, `strerror_r` (XSI's, returning an error number), `strsep`, `strlcpy`, `strlcat`, `memmem`; and `strcasestr`, `memrchr`, `strchrnul`, `explicit_bzero`. Each beyond C is weak, as in `<strings.h>` |
| `<strings.h>` | `bzero`, `bcopy`, `bcmp`, `index`, `rindex`, `strcasecmp`, `strncasecmp`, `ffs`. Each is defined weak, so a program's own `index` or `bcopy` takes its place |
| `<ctype.h>`, `<wctype.h>` | complete, for the `"C"` locale; the classification functions return 0 or 1 |
| `<stdlib.h>` | `malloc` family with `aligned_alloc`; `strtol` family, `strtod`/`strtof`/`strtold`, `ato*`; `qsort` (heapsort) and `bsearch`; `rand`/`srand` (`RAND_MAX` is 2147483647); `abs`/`div` families; `atexit`, `exit`, `_Exit`, `abort`, `at_quick_exit`, `quick_exit`; the multibyte conversions without a state (`mblen`, `mbtowc`, `wctomb`, `mbstowcs`, `wcstombs`), in UTF-8; `getenv`; `system` (always reports no command processor). POSIX's: `setenv`, `unsetenv`, `putenv`, `clearenv`; `mkstemp`, `mkostemp`, `mkstemps`, `mkostemps`, `mkdtemp`, `mktemp`; `realpath`; `posix_memalign`, `memalign`, `reallocarray`; `qsort_r` (POSIX.1-2024's argument order, glibc's); `getsubopt`; `random`, `srandom`, `initstate`, `setstate`, the `*rand48` family and `rand_r`, each giving glibc's sequence for a seed |
| `<stdio.h>` | `FILE` streams with buffering, files by name, positioning (`fseek`, `ftell`, `fgetpos`, `fsetpos`, `rewind`), `remove`, `rename`, `tmpfile`, `tmpnam`, C11's exclusive `"x"` open mode, `setvbuf`, `ungetc`, `perror`, and the complete `printf` and `scanf` families (`v`, `f`, `s`, `sn` forms). POSIX's: `fdopen`, `fileno`, `fmemopen`, `open_memstream`, `getline`, `getdelim`, `dprintf`, `asprintf` and their `v` forms, `fseeko`, `ftello`, `flockfile`, `ftrylockfile`, `funlockfile`, the `_unlocked` functions, `ctermid`; and GNU's `fopencookie`, `setbuffer`, `setlinebuf`. More streams may be open at once than `FOPEN_MAX`, which is C's minimum and not a limit here |
| POSIX modules | `basename` and `dirname` (`<libgen.h>`, the POSIX ones, which may write into their argument); `fnmatch`, with glibc's flags including `FNM_CASEFOLD` and `FNM_LEADING_DIR`; `getopt`, `getopt_long` and `getopt_long_only`, which permute as glibc's and newlib's do (an option string starting with `+`, or `POSIXLY_CORRECT`, stops at the first operand); `<search.h>` complete, `tsearch` an AVL tree, `hsearch` growing past its `hcreate` size, and GNU's `hsearch_r` family and `tdestroy`; `<err.h>`'s `err`, `warn` and their `x` and `v` forms, which begin with `program_invocation_short_name` where startup has set it (Linux's does) |
| `<math.h>`, `<tgmath.h>` | every C11 function at all three widths, `erf`, `erfc`, `lgamma` and `tgamma` FreeBSD's (accurate to an ulp or two; `lgamma` of a negative argument beside a zero of Γ loses digits, ~100 ulp at -2.75, as FreeBSD's does); C23's `sinpi`, `cospi`, `tanpi`, `asinpi`, `acospi`, `atanpi` and `atan2pi`; `signgam`, `lgamma_r`; `float_t`, `double_t`, `FP_ILOGB0` and `FP_ILOGBNAN`, and POSIX's `M_PI` family outside strict ISO mode; `<tgmath.h>` chooses the width from every generic argument, as C11 7.25 says (`pow(2.0f, 3.0L)` is `powl`) |
| `<complex.h>` | complete: every function at all three widths |
| `<fenv.h>` | exception flags and rounding modes, on x86-64 (`MXCSR`) and AArch64 (`FPSR`/`FPCR`) |
| `<time.h>` | `time`, `clock`, `difftime`, `mktime`, `timegm`, `gmtime`, `localtime`, the `_r` forms, `asctime`, `ctime`, `strftime`, `timespec_get`. POSIX's: `tzset` with `tzname`, `timezone` and `daylight`; `strptime`; `tm_gmtoff` and `tm_zone` in `struct tm`; `clock_gettime`, `clock_getres`, `clock_settime`, `clock_nanosleep`, `nanosleep`; `<sys/time.h>`'s `gettimeofday`, `settimeofday`, `setitimer`, `getitimer`, `utimes`; `times` |
| `<setjmp.h>` | `setjmp`/`longjmp` on every target: x86-64, AArch64, ARM state, Thumb (every Cortex-M, with s16-s31 where there is an FPU), RISC-V (with fs0-fs11 under F or D), MIPS o32 and n64, LoongArch64, PowerPC, SPARC, Xtensa, TriCore, ColdFire, RX, and on AVR in `lib/avr`. `jmp_buf` is 256 bytes everywhere but AVR, where it is 24 |
| `<signal.h>` | `signal` and `raise`; POSIX's `sigaction` (with `SA_SIGINFO`, `SA_RESETHAND`, `SA_NODEFER`, `SA_RESTART`), `sigprocmask`, `pthread_sigmask`, `sigpending`, `sigsuspend`, `sigwait`, `sigwaitinfo`, `sigtimedwait`, `sigaltstack`, `kill`, `killpg`, `pthread_kill`, the `sigset_t` functions and GNU's `sigisemptyset`, `sigorset`, `sigandset`; `strsignal` (glibc's words), `psignal`, `psiginfo`, `siginterrupt`. See [Signals](#behavior-worth-knowing) for who delivers them |
| `<unistd.h>`, `<fcntl.h>`, `<sys/uio.h>` | descriptors: `open`, `openat`, `creat`, `close`, `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `preadv`, `pwritev`, `lseek`, `dup`, `dup2`, `dup3`, `pipe`, `pipe2`, `fcntl` (descriptor and status flags, record locks), `flock`, `lockf`, `fsync`, `fdatasync`, `ftruncate`, `sync`, `isatty`, `ioctl`, `posix_fadvise`, `getentropy`, `getrandom` |
| `<sys/stat.h>`, `<unistd.h>`, `<sys/statvfs.h>` | files: `stat`, `lstat`, `fstat`, `access`, `unlink`, `rmdir`, `link`, `symlink`, `readlink`, `truncate`, `mkdir`, `chdir`, `fchdir`, `getcwd`, `chmod`, `fchmod`, `umask`, `chown`, `lchown`, `fchown`, `mknod`, `mkfifo`, `futimens`, `utimensat`, `utime`, `utimes`, `lutimes`, `futimes`, `statvfs`, `fstatvfs`, `pathconf`, `fpathconf`, `chroot`, and every `*at` form (`fstatat`, `faccessat`, `unlinkat`, `linkat`, `symlinkat`, `readlinkat`, `renameat`, `mkdirat`, `fchmodat`, `fchownat`, `mknodat`, `mkfifoat`) |
| `<dirent.h>` | `opendir`, `fdopendir`, `readdir`, `readdir_r`, `rewinddir`, `telldir`, `seekdir`, `closedir`, `dirfd`, `scandir`, `alphasort`, `versionsort`; `d_type` is filled |
| Processes | `getpid`, `getppid`, the user and group ids and their `set` forms, `getgroups`, process groups and sessions (`getpgid`, `setpgid`, `getsid`, `setsid`, `getpgrp`, `setpgrp`), `getlogin`, `fork`, `vfork`, `_exit`, `execve` and every `exec` form (`execvp`'s search, and `/bin/sh` for a file that is not a program), `waitpid`, `wait`, `wait3`, `wait4`, `system`, `popen`, `pclose`, `sleep`, `usleep`, `pause`, `alarm`, `ualarm`, `daemon`; `<spawn.h>` complete, its attributes refused with `ENOTSUP` where the operating system cannot honour them |
| System | `sysconf`, `confstr`, `uname`, `gethostname`, `sethostname`, `getpagesize`, `getdtablesize`, `getrlimit`, `setrlimit`, `getrusage`, `sched_yield` and the priority ranges; `<sys/mman.h>`'s `mmap`, `munmap`, `mprotect`, `msync`, `madvise`, `posix_madvise`, `mlock`, `munlock`, `shm_open`, `shm_unlink`; `<poll.h>`'s `poll`, `ppoll`, and `select`, `pselect` |
| Sockets | `<sys/socket.h>` complete (`socket` to `recvmsg` and `sockatmark`, `accept4`, `socketpair`); `<arpa/inet.h>`: `inet_pton`, `inet_ntop`, `inet_aton`, `inet_addr`, `inet_network`, `inet_ntoa` and the byte-order functions; `<netdb.h>`: `getaddrinfo`, `getnameinfo`, `freeaddrinfo`, `gai_strerror`, `gethostbyname`, `gethostbyname2`, `gethostbyaddr`, `getservbyname`, `getservbyport`, `getprotobyname`, `getprotobynumber`, `herror`, `hstrerror`. A name is tried as a number, in `/etc/hosts`, by the operating system's own resolver where it has one (EmbLinkOS's), then by DNS to the servers `/etc/resolv.conf` names; services and protocols come from `/etc/services` and `/etc/protocols`, or without them from a table of IANA's well-known ones |
| Terminals | `<termios.h>` complete (`tcgetattr` to `tcsetwinsize`, `cfmakeraw`, `cfsetspeed`), `ttyname`, `ttyname_r`, `getpass`; pseudo-terminals: `posix_openpt`, `grantpt`, `unlockpt`, `ptsname`, `ptsname_r`, and `<pty.h>`'s `openpty`, `login_tty`, `forkpty` |
| Users and groups | `<pwd.h>` and `<grp.h>` complete, from `/etc/passwd` and `/etc/group`: `getpwnam`, `getpwuid`, their `_r` forms, `getpwent` and its siblings, the same for groups, `getgrouplist`, `setgroups`, `initgroups` |
| `<malloc.h>` | glibc's: `malloc_usable_size`, `memalign`, `valloc`, `pvalloc`, `mallinfo`, `mallinfo2`, `malloc_stats`; `mallopt` and `malloc_trim` are accepted and change nothing |
| `<locale.h>` | `setlocale` and `localeconv`, one locale |
| `<threads.h>` | complete: threads, mutexes, condition variables, `call_once`, thread-specific storage |
| `<pthread.h>` | threads (`pthread_create`, `join`, `detach`, `exit`, `self`, `equal`, `once`, `atfork`, GNU's `tryjoin_np`, `timedjoin_np`, `setname_np`, `getname_np`), their attributes, the three kinds of mutex with `timedlock` and POSIX.1-2024's `clocklock`, condition variables with a clock attribute and `clockwait`, read-write locks (readers first, as glibc's default), barriers, spin locks, thread-specific data (`pthread_key_create` and the rest, with destructor rounds). Process-shared objects, robust and priority-protocol mutexes are refused with `ENOTSUP`; `pthread_cancel` is `ENOTSUP`, as no call here is a cancellation point; a scheduling policy other than `SCHED_OTHER` is `EPERM` |
| `<semaphore.h>` | unnamed semaphores: `sem_init`, `sem_wait`, `sem_trywait`, `sem_timedwait`, `sem_clockwait`, `sem_post`, `sem_getvalue`, `sem_destroy`. `sem_open` is `ENOSYS` and a process-shared `sem_init` `ENOTSUP`: both need memory and a futex another process can reach |
| `<regex.h>` | POSIX's basic and extended syntax with glibc's numbers and messages, and GNU's `\+ \? \|` in basic syntax, `\< \> \b \B \w \W \s \S`, back-references in both, `REG_STARTEND`. The match is POSIX's -- leftmost, then longest -- found by backtracking; a pattern that would take more than 20 million steps at one position is `REG_ESPACE`, not a hang |
| `<iconv.h>` | UTF-8, UTF-16, UTF-32, UCS-2 and UCS-4 (each also as `LE` and `BE`), `WCHAR_T`, ASCII, ISO-8859-1, ISO-8859-15 and CP1252, any to any; names matched ignoring case and punctuation. As glibc: unmarked UTF-16 and UTF-32 write a byte-order mark and use the machine's order, `//IGNORE` drops what cannot be converted and reports `EILSEQ` at the end, and `//TRANSLIT` makes the same 1784 substitutions glibc's C locale makes (`EUR` for the euro sign, `1/2`, `(C)`), `?` for the rest. UTF-8 past U+10FFFF is refused, where glibc passes it |
| `<wordexp.h>` | Every expansion the shell does to a word, in its order: tildes, parameters (with all of `${...}`'s forms), command substitution (`/bin/sh -c`, its standard error discarded unless `WRDE_SHOWERR`), arithmetic, field splitting on `IFS`, pathname expansion, quote removal. glibc's numbers. Where glibc departs from the shell, this follows the shell: a quoted empty expansion is one empty word, an expansion's result is matched against files, arithmetic reads names and has every C operator, and `${x:?}` is `WRDE_BADVAL` |
| `<glob.h>`, `<ftw.h>` | `glob` and `globfree` with every flag glibc has (`GLOB_BRACE`, `GLOB_TILDE`, `GLOB_ONLYDIR`, `GLOB_PERIOD`, `GLOB_ALTDIRFUNC` among them), `glob_pattern_p`; `nftw` (with GNU's `FTW_ACTIONRETVAL`) and `ftw`, holding one directory open at a time whatever `fds` allows |
| `<syslog.h>` | `openlog`, `syslog`, `vsyslog`, `closelog`, `setlogmask`: RFC 3164 lines to `/dev/log`, as glibc writes them. With nothing listening there a message is not dropped, as glibc drops it: it goes to `/dev/console` under `LOG_CONS` and to standard error otherwise |
| `<langinfo.h>`, `<nl_types.h>` | `nl_langinfo` for the `"C"` locale -- its formats, names, `RADIXCHAR` and the yes/no expressions -- with `CODESET` "UTF-8", the encoding its multibyte functions use (glibc's plain "C" says "ANSI_X3.4-1968"); no message catalogs: `catopen` fails with `ENOENT` and `catgets` returns the program's own message |
| `<wchar.h>`, `<uchar.h>` | wide strings, `wcsto*`, restartable UTF-8 conversions (`mbrtowc`, `wcrtomb`, `mbsrtowcs`, `wcsrtombs`), `btowc`, `wctob`, wide streams (`fgetwc`, `fputwc`, `fgetws`, `fputws`, `ungetwc`, `fwide` and the rest), the wide `printf` and `scanf` families, `wcsftime`, `mbrtoc16`/`c16rtomb`/`mbrtoc32`/`c32rtomb` |
| `<stdatomic.h>` | the C11 interface over the compiler's atomic builtins; no library code |
| `<stdbit.h>` (C23) | all fourteen operations at the five unsigned types and type-generic; each named function is also a macro over the compiler's bit builtins, so a call is the instruction, and the functions themselves are in `lib/libc` for a pointer to one |
| `<stdckdint.h>` (C23) | `ckd_add`, `ckd_sub`, `ckd_mul` over the compiler's overflow builtins; no library code |
| C23 elsewhere | `memccpy`, `memset_explicit`; `free_sized`, `free_aligned_sized`, `memalignment`, `strfromd`/`strfromf`/`strfroml`, `call_once` in `<stdlib.h>`; `char8_t`, `mbrtoc8`, `c8rtomb`; `timespec_getres`; `roundeven`, `nextup`, `nextdown`, `fmaximum`, `fminimum` and their `_num`, `_mag` and `_mag_num` forms, `exp10`, at all three widths and in `<tgmath.h>`, and `iszero`, `issubnormal`; printf's `%B`, `#` on `%b`, and the `wN` and `wfN` length modifiers, scanf's `%b` and `wN`; the `0b` prefix in `strtol` and `%i`; the `_WIDTH` macros of `<stdint.h>`; a variadic `assert` |
| `<assert.h>`, `<inttypes.h>`, `<errno.h>` | complete; each `PRI`/`SCN` macro has the length modifier of its type on the target (`PRId64` is `"lld"` where `int64_t` is `long long`); `errno` values are the traditional POSIX numbers |

### Behavior worth knowing

**Formatted output.** `printf` converts floating-point values exactly,
by integer arithmetic on the binary value, so every precision is
correctly rounded with ties to even (`%.0f` of 2.5 is `2`). `long double`
is converted at its own width. `%a` is exact and rounds a precision to
nearest, ties to even; a subnormal is printed normalized with a leading
`1`. `%b` prints binary. A null `%s` prints `(null)` and a null `%p`
prints `(nil)`. `%n` stores into the type its length modifier names
(`%hhn` a `signed char`, `%jn` an `intmax_t`). An unknown conversion
prints itself. `%lc` and `%ls` take wide characters and write their
UTF-8, a precision on `%ls` counting bytes and never cutting a character;
one with no UTF-8, a lone surrogate, makes the call return -1 with
`errno` `EILSEQ`.

**Formatted input.** `scanf` supports `%[`, `%n`, `%a` and hexadecimal
floating-point input. It distinguishes an input failure (returns `EOF`)
from a matching failure (returns the number of items assigned, possibly
0): `sscanf("x", "%d", &n)` returns 0. The character it reads ahead and
does not use goes back to the stream: `fscanf(f, "%d", &n)` on `12x`
leaves the `x` for the next read. `%lc`, `%ls` and `%l[` store the wide
characters the input's UTF-8 decodes to.

**Wide streams.** `fwprintf`, `fgetwc` and the rest read and write a
stream's bytes as UTF-8, a character at a time, so a `FILE` costs nothing
more for being able to be wide. The first byte or wide operation on a
stream fixes its orientation, as C11 7.21.2 says, and `fwide` reports it.
`ungetwc` holds one wide character. A byte sequence that is not UTF-8, or
a wide character with no UTF-8, is an encoding error: `WEOF` or -1, `errno`
`EILSEQ`, and the stream's error indicator. The wide printfs count width,
precision and `%n` in wide characters; `swprintf` returns -1 when the
result does not fit, where `snprintf` returns the length it would have
had. A byte printf with no `%ls` or `%lc` links none of this.

**C23's `0b`.** `strtol` and its family with base 0 or 2, and scanf's
`%i` and `%b`, read a `0b` or `0B` prefix in every language mode, as C23
does; a C17 program that parses `"0b1"` with base 0 gets 1, not 0. A
prefix with no digit after it is not one: `strtol("0xz", &end, 16)` is 0
and leaves `end` at the `x`.

**`strtod`.** `strtod`, `strtof` and `strtold` are correctly rounded:
the nearest value of the type, ties to even, for every input, each at its
own precision -- a `long double` keeps the x87's 64 bits or binary128's
113. Hexadecimal input is rounded the same way, and its binary exponent
is optional (`strtod("0x1")` is 1). scanf's floating conversions and
`wcstod` give the same answers; scanf keeps a numeral's first 900 digits
and reads the rest as one, which can matter only to a `long double`.

**Buffering.** `BUFSIZ` is 4096 and `FOPEN_MAX` is 32. `stdout` is fully
buffered, or line-buffered when the backend reports it is a terminal;
`stderr` is unbuffered. `exit` flushes every stream after running the
`atexit` handlers.

**`atexit`.** `atexit` and `__cxa_atexit` share one list, so C handlers
and C++ static destructors run in reverse order of registration,
interleaved. The list holds 256 entries; registering more is reported on
`stderr`.

**`malloc`.** First fit over an explicit free list, with coalescing, on a
heap grown with the backend's `sbrk`. Every block is 16-byte aligned.
`aligned_alloc` returns a block that `free` releases like any other. When
the backend cannot grow the heap, `malloc` returns a null pointer with
`errno` set to `ENOMEM`.

**Math.** The double-precision functions are Sun's fdlibm (kept
verbatim in `src/math/fdlibm/`, about 1 ulp), plus `log2`, `exp2`,
`log1p`, the inverse hyperbolics, `lgamma`, `tgamma` and `erf`. `sqrt` is
the hardware instruction. The `float` forms compute in `float`, by
newlib's conversions of the same functions (`fdlibm/ef_*.c`, `kf_*.c`,
`sf_*.c`): within 1 ulp, and 2 for `tanhf`, `log10f`, `cbrtf`, `log2f`
and `exp2f`, where computing in `double` was correctly rounded but cost
a software `double` on a single-precision FPU -- `sinf` takes about 225
cycles on a Cortex-M4F where it took 25,000. `sqrtf` is the FPU's
instruction where the target has a single-precision one. Where `double`
is binary32 (RX), the `double` functions are these `float` ones. The `long
double` forms of the functions that compute a new value (`sinl`, `expl`,
`powl`, ...) compute in `double`, so on x86-64 they deliver 53 of the 64
significand bits; the functions that only move or inspect a value
(`fabsl`, `truncl`, `floorl`, `frexpl`, `ldexpl`, `fmodl`, `copysignl`,
...) are exact at full width. The classification macros (`fpclassify`,
`isnan`, `isinf`, `isfinite`, `isnormal`, `signbit`) read a `long double`
in its own format, so `isinf(LDBL_MAX)` is 0 and a `long double`
subnormal is `FP_SUBNORMAL`. `nearbyint` and `rint` follow the current
rounding mode; `remainder` rounds the quotient to nearest, ties to even.

**Complex.** The exponential, logarithm, square root, power, and the
trigonometric and hyperbolic functions with their inverses are
FreeBSD's (`src/math/msun/`, by their authors' notices): every special
value Annex G lists, and accuracy across the plane -- the inverse
functions are Hull, Fairgrieve and Tang's algorithm, which keeps its
digits beside the branch points. `clog`'s real part is exact near
|z| = 1, where `log(cabs(z))` keeps none. The `float` forms compute in
`float`; the `long double` forms compute in `double` (as above), except
`creall`, `cimagl`, `conjl` and `cprojl`, which only take a value apart
or put it back. `CMPLX`, `CMPLXF` and `CMPLXL` are the header's own and
call nothing in the library.

**Locale.** There is one locale, `"C"`. `setlocale` accepts `"C"`,
`"POSIX"` and `""` and returns a null pointer for any other name. The
multibyte encoding is UTF-8: overlong forms, surrogates and values above
U+10FFFF are rejected. `wchar_t` is 32 bits on every target but Xtensa,
where it is 16, as GCC has it; there a character past U+FFFF has no wide
form, and its UTF-8 is rejected too rather than cut down to fit.

**Temporary files.** `tmpfile` creates its file under `/tmp`, named with
twelve letters from the backend's entropy (or the clock, where there is
none), opens it exclusively and for the owner only, and removes the name
at once: the file lasts as long as the stream. `tmpnam`'s names are under
`/tmp` too, each one no file had when it was made. A target with no
filesystem has no temporary files: `tmpfile` returns a null pointer.

**Time.** `localtime` follows `TZ` as POSIX writes it
(`CET-1CEST,M3.5.0,M10.5.0/3`, with quoted names like `<+0530>-5:30`,
`Jn` and `n` rules, and rule times past 24 hours or negative), or names a
compiled zone file (`Europe/Paris`, `:Europe/Paris`) under `TZDIR` or
`/usr/share/zoneinfo`, read whole: TZif versions 1 to 3, with the footer's
rule for times after the last transition. With no `TZ`, `/etc/localtime`
where there is one, UTC where there is not; a `TZ` that is neither is UTC
named `"UTC"`, as no error is possible. `mktime` reads `tm_isdst` as glibc
does at the hours the clocks change: 0 or 1 chooses between the two
meanings of an ambiguous time and moves a missing one by the hour that is
missing. `strptime` is glibc's in the `"C"` locale, down to its readings
of fields it leaves unsaid (`tm_wday` and `tm_yday` once a date is read,
and the date from `%U` or `%W` with a weekday). Two choices differ from
glibc 2.36: a rule applies before 1970 too, and where glibc reads past its
month table (day 366 of a common year, week 0) the calendar is followed.
`time` and `clock` return what the backend provides, which on the
freestanding targets is -1.

**Signals.** Two models, one interface. Where the kernel delivers signals
(Linux), `sigaction` installs the handler with the kernel, and signals
arrive from outside: `alarm`, `kill` from another process, a child's
`SIGCHLD`. Elsewhere the library keeps the table itself -- the handlers, a
mask, a pending set -- and is the only sender: `raise`, and `kill` of the
program's own id, call the handler; a blocked signal waits in the pending
set and is delivered when it is unblocked; a signal at its default action
ends the program as the default says. `abort` raises `SIGABRT`, and if a
handler returns, resets it and raises again. The same test program passes
under both (`tests/exec/posix-signals.c`).

**What the operating system cannot do.** A POSIX function whose primitive
the backend has not got fails with `ENOSYS`, never with a made-up answer:
on a board, `fork` is `ENOSYS`, and so is `socket`. `sysconf` answers -1
for what it cannot know rather than guess. The `*at` functions use the
backend's own where it has them, and otherwise turn the directory
descriptor back into a path. `posix_spawn` runs a program through the
backend's spawn -- `fork` and `exec` on Linux, the kernel's spawn on
EmbLinkOS -- and `posix_spawnattr_setflags` refuses with `ENOTSUP` a flag
that spawn cannot honour, rather than a spawn that would ignore it.

**Threads.** `pthread_create` and `thrd_create` start a thread where the
backend can (Linux and EmbLinkOS); elsewhere they fail (`EAGAIN`,
`thrd_error`) and everything else still works with the one thread there
is -- locks, `once`, keys. One implementation serves both spellings, so a
C11 thread's `tss_t` is a POSIX key. Each thread's keys live in a record
the library keeps for it, so no thread-local storage is needed beneath
them. `pthread_exit` returns to the start of its thread and runs the
keys' destructors there; frames it leaves are not unwound, so a C++
destructor between it and the thread's function does not run (as under
musl). A thread's stack is the backend's: 8 MiB on Linux, mapped as it
is used; `pthread_attr_setstacksize` is kept and reported, and
`pthread_attr_setstack` is refused at `pthread_create`. Timed waits take
an absolute time, POSIX's and C11's alike. `thread_local` is defined as
`_Thread_local`.

**Environment.** `getenv` reads the vector the startup code publishes in
`environ`. The first change copies that vector into the heap; the strings
`setenv` makes are freed when they are replaced or removed, the startup
vector's and `putenv`'s never are (a `putenv` string IS the entry, and a
later change to it changes the environment).

**POSIX's choices.** Where POSIX lets an implementation choose, this one
chose glibc's answer, except: `fmemopen` refuses a size of 0 (POSIX lets
it); `open_memstream`'s length moves only when a write passes it, as
POSIX says, so a seek to `SEEK_END` after rewriting the middle goes to
the end and not to where the write stopped (glibc 2.36 differs); and
`dirname("//")` is `"//"`, as glibc's is.

### Thread safety

The library's shared state is locked. The heap has one lock; each
`FILE` has its own, held for the whole of a call, so the output of one
`printf` or `puts` is never interleaved with another thread's; the table
of open streams has a third. A stream's lock nests: a thread that holds
it with `flockfile` may still `printf` to the stream. The locks are futex-based: uncontended,
locking and unlocking are one atomic instruction each. On a target whose
backend has no futex the wait degrades to a yield, which is only reached
when a lock is contended, and such a target has one thread.

`errno` is per thread on the Linux triples, where the C library sets up
thread-local storage, and on EmbLinkOS, whose `crt0` sets up the first
thread's and hands the template over (`lib/libc/os/tls-template.h`) for
the library to copy into every thread it starts. Elsewhere it is one
variable. Functions that keep
state between calls by definition (`strtok`, `rand`, `gmtime`,
`localtime`, `asctime`, `ctime`) share it between threads; use the `_r`
forms of the time functions.

### Not provided

| Missing | Notes |
|---|---|
| C11 Annex K (`*_s` functions) | |
| named and process-shared semaphores, process-shared locks, thread cancellation | see `<pthread.h>` and `<semaphore.h>` above |
| locales other than `"C"` (`"C.UTF-8"` is `"C"`, whose encoding is UTF-8; the `_l` functions and `newlocale` work with that one) | |
| System V IPC and POSIX message queues where the kernel is not Linux | `ENOSYS` |
| GNU's `argp`, `obstack`, `argz`; the resolver's `res_*` and `dn_*`; Sun RPC | |
| `<execinfo.h>` (`backtrace`) | C code here has no unwind tables unless built with `--unwind-tables` |
| `getcontext` and the rest of `<ucontext.h>` on targets other than x86-64 and AArch64 | |

### glibc instead: `--libc=system`

For a Linux target, `--libc=system` replaces this library with the
system's own, glibc -- the way to POSIX threads, and to whatever else of
glibc's this library has not got. EmbCC does what GCC does:

| | With `--libc=system` |
|---|---|
| Headers | the root's `usr/local/include`, `usr/include/TRIPLE` and `usr/include`, then EmbCC's freestanding headers (`<stddef.h>`, `<stdarg.h>`...); EmbCC presents itself to them as GCC 4.2.1 (`__GNUC__` 4, as clang does), unless `-fgnuc-version=` says otherwise |
| `-pthread` | defines `_REENTRANT` and links `-lpthread` |
| Start files | glibc's `crt1.o`, `crti.o` and `crtn.o`, from `usr/lib/TRIPLE`, `usr/lib64` or `usr/lib` |
| Libraries | `-lc` (glibc's `libc.so`, which names `libc.so.6` and the dynamic linker), EmbCC's `librt.a` when built, and any `-l` and `.so` given |
| Linker | `--linker=PATH` (`--ld-path=`), or `-fuse-ld=bfd\|gold\|lld`; else `TRIPLE-ld`, `ld.lld`, and on Linux `ld` |
| Dynamic linker | `/lib64/ld-linux-x86-64.so.2`, `/lib/ld-linux-aarch64.so.1` |
| The root | `/`, or `--sysroot=DIR` |

glibc's types are laid out as GCC lays them out: `tests/golden/libc-system.sh`
compares 40 of them, `pthread_mutex_t` and `struct stat` among them, byte
for byte with GCC's, and runs a threaded program on a Linux kernel. One
difference does not cross a call: EmbCC's `va_list` is a pointer to the
ABI's argument tag, 8 bytes where GCC's is the tag, and passing one to
`vprintf` passes that pointer either way.

`-static` links glibc's `libc.a`, which needs GCC's `libgcc.a` and
`libgcc_eh.a`; EmbCC takes them from the root's `usr/lib/gcc/TRIPLE/N/`
and says so when there are none. A C++ program is refused: EmbCC's C++
runtime is built on its own C library.

### Backends

Everything above is portable C. An operating system is reached through
the 140 functions declared in `lib/libc/os/backend.h`, and one file per
OS implements them. The C part is small:

| Group | Functions | Without them |
|---|---|---|
| Core | `__os_write`, `__os_read`, `__os_open`, `__os_close`, `__os_lseek`, `__os_remove`, `__os_rename`, `__os_sbrk`, `__os_time`, `__os_clock_ns`, `__os_exit`, `__os_isatty`, `__os_getentropy` | required |
| Threads | `__os_thread_create`, `__os_thread_join`, `__os_thread_detach`, `__os_thread_self`, `__os_thread_yield`, `__os_sleep_ns`, `__os_futex_wait`, `__os_futex_wake` | single-threaded: each returns -1 with `ENOSYS` |
| Filesystem | `__os_stat`, `__os_lstat`, `__os_mkdir`, `__os_rmdir`, `__os_unlink`, `__os_chmod`, `__os_truncate`, `__os_utime`, `__os_symlink`, `__os_readlink`, `__os_link`, `__os_getcwd`, `__os_chdir`, `__os_statfs`, `__os_opendir`, `__os_readdir`, `__os_closedir` | no filesystem beyond file descriptors; each returns -1 with `ENOSYS` |

C++'s `<thread>`, `<mutex>` and the other synchronization headers are
built on the thread group, and `<filesystem>` on the filesystem group.

The rest, the second half of `backend.h`, is POSIX's: descriptors, the
rest of the filesystem and its `*at` forms, directories, processes and
spawn, signals, clocks and timers, mapped memory, the system's
identity and limits, polling, sockets and the resolver, terminals. Each
is shaped like the POSIX call it serves, and where a meaning needs a
number (a flag, an `fcntl` command, a signal, an `ioctl` request, a
socket address's layout) it is Linux's, which a backend whose kernel
numbers it differently translates. All of it is optional:
`os/absent.inc` defines each one as an `ENOSYS` failure, and a backend
includes it after defining `OS_HAS_name` for each it implements itself.
`tests/golden/libc-backends.sh` compiles every backend and fails if any
of the 140 is missing from one, which would otherwise be a link error
in the first program that called it.

Four backends exist:

| Backend | Used by | What it does |
|---|---|---|
| `os/posixlike/backend.c` | `libc-x86_64` and `libc-aarch64` (`x86_64-elf`, `aarch64-elf`) | forwards to `write`, `read`, `open`, `close`, `lseek`, `sbrk`, `_exit` and `isatty`, which the program or its environment must define. The thread functions (`thread_create`, `futex_wait`, ...) and filesystem functions (`fs_stat`, ...) are weak references: defined, they are used; absent, the group reports `ENOSYS`. `remove`, `rename`, `time`, `clock` and entropy report `ENOSYS`, and so does POSIX's half (`os/absent.inc`); the program's own `kill`, `getpid` or `fork` replace the library's, being weak. |
| `os/linux/` | `libc-linux-x86_64`, `libc-linux-aarch64` | issues Linux system calls directly, so a program needs no other C library and no dynamic loader. Provides `crt1.o` (the `_start` that calls `main` and `exit`), threads through `clone`, thread-local storage (per-thread `errno`), and all of POSIX's half: `fork`, `execve`, kernel-delivered signals, sockets, terminals. Static images only. |
| `os/emblinkos/backend.c` | `libc-emblinkos` | EmbLinkOS's system calls, translated: its absolute-only paths (the working directory is kept here), its open flags and `d_type` numbers, its spawn in place of `fork` and `exec` (with descriptor, directory and environment actions), its IPv4 sockets and resolver, its terminal control. Threads get their own thread-local block, copied from the template `crt0` hands over, so `errno` and `__thread` variables are per thread. What it has not got -- `fork`, signals from outside, record locks, `mmap` of a file -- is `ENOSYS`. EmbLinkOS builds its whole userland on it with `make LIBC=emblibc`. `clock` reports failure. |
| `os/baremetal/backend.c` | `libc-embedded` (the embedded targets) | for a part with no operating system, and needs nothing from the program: `write`, `read`, `open`, `close`, `lseek`, `sbrk`, `_exit` and `isatty` are defined WEAKLY with defaults (output discarded, input at end of file, a heap from the end of the image to the stack, `_exit` stops), and a program's own definition of any of them replaces the default. One thread, no clock, no filesystem: those report `ENOSYS`, as POSIX's half does (`os/absent.inc`) -- except signals, which the library delivers itself. |

A new operating system needs one new backend file and nothing else.

## The C++ library: `lib/libcxx`

### The runtime

`lib/libcxx/src` implements the Itanium C++ ABI runtime, what GCC calls
`libsupc++`:

| File | Provides |
|---|---|
| `new.cc` | every replaceable `operator new` and `operator delete`, including the sized, aligned and `nothrow` forms |
| `guard.cc` | `__cxa_guard_acquire`/`release`/`abort` for function-local statics, `__cxa_pure_virtual`, `__cxa_deleted_virtual` |
| `typeinfo.cc` | `std::type_info` and the `__cxxabiv1` type-information classes |
| `dyncast.cc` | `__dynamic_cast` and the base-class search exception matching uses |
| `eh.cc` | `__cxa_allocate_exception`, `__cxa_throw`, `__cxa_begin_catch`, `__cxa_end_catch`, `__cxa_rethrow`, the reference counting behind `std::exception_ptr`, and `__gxx_personality_v0` |
| `terminate.cc` | `std::terminate`, `std::set_terminate`, `std::get_terminate` |
| `stdexcept.cc`, `string.cc`, `iostream.cc`, `random.cc` | the out-of-line parts of the standard library: exception classes, string support, the standard streams, `random_device` |

`__cxa_atexit` and `__cxa_finalize` are in the C library, beside `exit`.
`__cxa_vec_*` are not provided; EmbCC lowers array `new` and `delete`
itself. Function-local static initialization is thread-safe: a second
thread waits for the first to finish, and recursive initialization of
the same object traps.

### Exceptions and the unwinder

A `throw` reaches the unwinder through `_Unwind_RaiseException`, and
every landing pad ends with `_Unwind_Resume`. Which unwinder that is
depends on the target:

| Target | Unwinder |
|---|---|
| `x86_64-linux-gnu`, `aarch64-linux-gnu` | EmbCC's own, `lib/rt/unwind.c`, in `librt.a` |
| EmbLinkOS | the same, in the `librt.a` that `make libc-emblinkos` builds beside its `libc.a`; EmbLinkOS's linker script names `__eh_frame_start` and `__eh_frame_end` for it, as EmbLD does |
| `x86_64-elf`, `aarch64-elf` | libgcc's; the program links `libgcc.a` (or `libgcc_eh.a`) |
| macOS | the platform's |
| the embedded targets | none. C++ exceptions are refused on every embedded target, C++ code generation on AVR, and at RV64 a function that needs a landing pad is refused (see [Bare-metal programming](embedded.md#c-on-the-embedded-targets)) |

`<unwind.h>` in the freestanding directory declares the level-I unwinder
interface (`_Unwind_RaiseException`, `_Unwind_GetIP`, ...) with the GCC
extensions `libsupc++` uses.

### The standard library

`lib/libcxx/include` is EmbCC's own standard library: 95 standard
headers.

| Group | Headers |
|---|---|
| Language support | `<new>` `<typeinfo>` `<exception>` `<initializer_list>` `<limits>` `<compare>` `<coroutine>` `<source_location>` `<version>` `<typeindex>` |
| Concepts and utilities | `<concepts>` `<type_traits>` `<utility>` `<tuple>` `<optional>` `<variant>` `<any>` `<bitset>` `<functional>` `<memory>` `<memory_resource>` `<scoped_allocator>` `<ratio>` `<chrono>` `<bit>` `<numbers>` `<charconv>` `<format>` `<system_error>` `<stdexcept>` |
| Containers and views | `<array>` `<vector>` `<deque>` `<list>` `<forward_list>` `<map>` `<set>` `<unordered_map>` `<unordered_set>` `<queue>` `<stack>` `<span>` `<string>` `<string_view>` `<valarray>` |
| Iterators, ranges, algorithms | `<iterator>` `<ranges>` `<algorithm>` `<numeric>` `<execution>` `<random>` `<complex>` `<regex>` |
| Input and output | `<iosfwd>` `<ios>` `<istream>` `<ostream>` `<iostream>` `<streambuf>` `<sstream>` `<fstream>` `<iomanip>` `<syncstream>` `<filesystem>` |
| Concurrency | `<atomic>` `<thread>` `<mutex>` `<shared_mutex>` `<condition_variable>` `<semaphore>` `<latch>` `<barrier>` `<stop_token>` `<future>` |
| C library wrappers | `<cassert>` `<cctype>` `<cerrno>` `<cfenv>` `<cfloat>` `<cinttypes>` `<climits>` `<clocale>` `<cmath>` `<csetjmp>` `<csignal>` `<cstdarg>` `<cstddef>` `<cstdint>` `<cstdio>` `<cstdlib>` `<cstring>` `<ctime>` `<cuchar>` `<cwchar>` `<cwctype>` |

Not provided: `<locale>`, `<codecvt>`, `<strstream>`, `<ciso646>`,
`<cstdbool>`, `<cstdalign>`, `<ctgmath>`, `<ccomplex>`, and the C++23
headers (`<expected>`, `<print>`, `<stacktrace>`, `<spanstream>`,
`<flat_map>`, `<flat_set>`, `<mdspan>`, `<generator>`, `<stdfloat>`).
`<thread>` and the other concurrency headers create threads only where
the C library's backend can; elsewhere `std::thread`'s constructor throws
`std::system_error`. The language level EmbCC accepts is described in
[C++](cxx.md).

`lib/libcxx/include/__tree` and `__hash` are internal: the red-black tree
shared by `<map>` and `<set>`, and the hash table shared by
`<unordered_map>` and `<unordered_set>`.

## The compiler runtime: `lib/rt`

### Contents

`lib/rt` holds the routines generated code calls instead of an
instruction. No program names them; the backend does. They use libgcc's
names and calling conventions, so EmbCC objects link against libgcc and
GCC- or Clang-compiled objects link against `librt.a`. Every file is
compiled for every target and compiles to an empty object where it does
not apply.

| File | Routines | Built where |
|---|---|---|
| `int64.c` | `__divdi3` `__udivdi3` `__moddi3` `__umoddi3` | the 32-bit targets but AVR |
| `int128.c` | `__multi3` `__divti3` `__udivti3` `__modti3` `__umodti3` `__ashlti3` `__ashrti3` `__lshrti3` `__negti2` | the 64-bit targets, which have `__int128` |
| `fp128.c` | conversions between 128-bit integers and `float`/`double`: `__floattidf` `__floatuntidf` `__floattisf` `__floatuntisf` `__fixdfti` `__fixunsdfti` `__fixsfti` `__fixunssfti` | as `int128.c` |
| `complex.c` | `__mulsc3` `__muldc3` `__divsc3` `__divdc3` | every target |
| `half.c` | `_Float16` and `__bf16` to and from the wider formats, on the bits: libgcc's `__extendhfsf2` `__truncsfhf2` `__truncdfhf2` and `__truncxfhf2` (x87) or `__trunctfhf2` (binary128) on x86-64 and AArch64; libgcc's `__gnu_h2f_ieee` `__gnu_f2h_ieee` `__gnu_d2h_ieee` and the run-time ABI's `__aeabi_h2f` `__aeabi_f2h` `__aeabi_d2h` on ARM; EmbCC's own `__embcc_h2f` `__embcc_f2h` `__embcc_d2h` elsewhere; `__truncsfbf2` `__extendbfsf2`, `__truncdfbf2` where `double` is eight bytes and `__embcc_ll2bf` `__embcc_ull2bf` where it is four | every target |
| `clear_cache.c` | `__clear_cache`, `__builtin___clear_cache`'s: AArch64's `dc cvau`/`ic ivau` by `CTR_EL0`'s line sizes, ARMv7-A's CP15 operations, Cortex-M's `dsb`/`isb`, RISC-V's `fence.i`, MIPS's `synci` loop returning by `jr.hb`, PowerPC's `dcbst`/`icbi`, SPARC's `flush`, LoongArch's `ibar` | every target |
| `powi.c` | `__powisf2` `__powidf2`, and `__powixf2` (x87) or `__powitf2` (binary128) where `long double` is wider than `double`: `__builtin_powi` and kin | every target |
| `builtin-setjmp.c` | `__embcc_setjmp` `__embcc_longjmp`, which `__builtin_setjmp` and `__builtin_longjmp` call: the C library's `setjmp` and `longjmp` under names no program has | every target but Darwin |
| `atomic8.c` | libatomic's eight-byte entry points, which the compiler calls for every eight-byte atomic there: `__atomic_load_8` `__atomic_store_8` `__atomic_exchange_8` `__atomic_compare_exchange_8`, `__atomic_fetch_OP_8` and `__atomic_OP_fetch_8`; and the generic `__atomic_load` `__atomic_store` `__atomic_exchange` `__atomic_compare_exchange`, which take the size first | the 32-bit targets but AVR |
| `atomic-riscv.c` | libatomic's sized entry points for every atomic on a core without the A extension (an ESP32-C3's `rv32imc`): `__atomic_exchange_N`, `__atomic_compare_exchange_N`, `__atomic_fetch_OP_N` and `__atomic_OP_fetch_N` | RISC-V |
| `embtrace.c` | EmbTrace's recorder: `__cyg_profile_func_enter` and `__cyg_profile_func_exit`, which `-finstrument-functions` calls, and `embtrace_enable` `embtrace_reset` `embtrace_dump` `embtrace_clock` `embtrace_putc` | every target |
| `ldouble.c` | x86-64: the x87 `long double` conversions `__floattixf` `__floatuntixf` `__fixxfti` `__fixunsxfti` and `__mulxc3` `__divxc3`; where `long double` is binary128: `__multc3` `__divtc3` | x86-64, AArch64, RISC-V, LoongArch, MIPS64, SPARC |
| `softtf.c` | IEEE binary128 in software, `long double` where no instruction is behind it: `__addtf3` `__subtf3` `__multf3` `__divtf3` `__negtf2`, the comparisons `__eqtf2` ... `__unordtf2`, and the conversions `__extenddftf2` `__trunctfdf2` `__floatsitf` `__fixtfdi` ... | AArch64, RISC-V, LoongArch, MIPS64, SPARC |
| `softfp.c` | binary32 and binary64 in software: `__addsf3` `__subsf3` `__mulsf3` `__divsf3` `__negsf2` and the `df` forms, comparisons (`__eqsf2` `__nesf2` `__ltsf2` `__lesf2` `__gtsf2` `__gesf2` `__unordsf2` and `df`), conversions to and from 32- and 64-bit integers, `__extendsfdf2`, `__truncdfsf2` | the embedded targets but AVR and RX; built for a double-precision unit (the Cortex-M7's, RISC-V's `ilp32d`), only the eight conversions to and from 64-bit integers, `__fixdfdi` ... `__floatundisf`, and for RV64's `lp64d` none |
| `avr.c` | `__mulsi3` `__divsi3` `__udivsi3` `__modsi3` `__umodsi3` | AVR |
| `avrdivmod.c` | avr-gcc's division routines, under their own register conventions, which an object from avr-gcc or clang calls for a `/` and a `%` at once: `__udivmodqi4` `__divmodqi4` (r24 by r22, quotient in r24, remainder in r25), `__udivmodhi4` `__divmodhi4` (r25:r24 by r23:r22, quotient in r23:r22, remainder in r25:r24), `__udivmodsi4` `__divmodsi4` (r25:r22 by r21:r18, quotient in r21:r18, remainder in r25:r22); each changes only its results, r0 and the flags; weak | AVR |
| `avr64.c` | `__muldi3` `__divdi3` `__udivdi3` `__moddi3` `__umoddi3` | AVR |
| `avrfp*.c` | binary32 in software, one object per group: `__addsf3`/`__subsf3`/`__negsf2`, `__mulsf3`, `__divsf3`, the comparisons including `__cmpsf2`, and the integer conversions | AVR, and RX, whose `double` is four bytes too |
| `aeabi.c` | the ARM run-time ABI's names, which clang, GCC and the vendors' toolchains call and EmbCC's own code does not: `__aeabi_memcpy` `__aeabi_memmove` `__aeabi_memset` `__aeabi_memclr` and their 4- and 8-byte forms, `__aeabi_idiv` `__aeabi_uidiv` `__aeabi_idivmod` `__aeabi_uidivmod` where there is a divide instruction, `__aeabi_ldivmod` `__aeabi_uldivmod`, `__aeabi_lmul`, the 64-bit shifts and compares, and the soft-float arithmetic, comparisons and conversions (`__aeabi_fadd` ... `__aeabi_d2f`); all weak | every ARM triple |
| `aeabidiv.c` | `__aeabi_idiv` `__aeabi_uidiv` `__aeabi_idivmod` `__aeabi_uidivmod` for an ARM core with no divide instruction; weak | ARMv6-M, ARMv7-A |
| `armv6m.c` | what Thumb-1 has no instruction for: `__aeabi_lmul`, `__aeabi_llsl` `__aeabi_llsr` `__aeabi_lasr`, `__aeabi_memcpy` `__aeabi_memclr`, and the `__atomic_*` calls with interrupts masked; weak | ARMv6-M, and ARMv8-M Baseline without the atomics |
| `unwind.c` | the DWARF CFI unwinder and the `_Unwind_*` API, plus `__register_frame_info` and `__deregister_frame_info` | Linux triples |

On the ARM hard-float triples the helpers keep the base (core-register)
calling convention, as the ARM run-time ABI requires. Conversions from a
floating-point value too large for the integer type saturate. The
routines are written so that none uses the operation it implements: a
128-bit value is only ever taken apart into 64-bit halves, and the
software floating point works on the bit patterns.

`unwind.c` finds the unwind tables through the symbols `__eh_frame_start`
and `__eh_frame_end`, which EmbLD defines for the `.eh_frame` section and
`lib/libc/os/linux/link.ld` defines for a GNU link. It scans the FDEs
linearly; there is no `.eh_frame_hdr` search table. CFA rules given as
DWARF expressions are refused rather than guessed.

When a routine is missing at link time, EmbLD says what it is:

```text
embld: undefined symbol '__divdi3' (referenced by main.o)
  note: this is a compiler-runtime helper (libgcc's __muldi3 family) — the routine a backend calls for an operation the machine has no instruction for, such as 128-bit multiply or divide. EmbCC ships these in librt.a (lib/rt); the driver puts it on the link line by itself, and a hand-written link has to name it after libc.a
```

The same kind of note is given for the conversion routines (`__fix*`,
`__float*`, `__trunc*`, `__extend*`) and for the unwinder's symbols.

### Per-target archives

Each `librt.a` holds `complex.c`, `half.c`, `powi.c`, `clear_cache.c`
and `embtrace.c`, and all but Darwin's `builtin-setjmp.c`; beyond those:

| Triple | Also |
|---|---|
| `x86_64-elf`, `x86_64-linux-gnu` | `int128.c`, `fp128.c`, `ldouble.c`; on Linux `unwind.c` |
| `aarch64-elf`, `aarch64-linux-gnu` | `int128.c`, `fp128.c`, `ldouble.c`, `softtf.c`; on Linux `unwind.c` |
| `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf` | `aeabi.c`, `int64.c`, `softfp.c`, `atomic8.c` |
| `thumbv8m.base-none-eabi` | the same, and `armv6m.c` |
| `thumbv6m-none-eabi`, `armv7a-none-eabi`, `armv7a-none-eabihf` | `aeabi.c`, `aeabidiv.c`, `int64.c`, `softfp.c`, `atomic8.c`; on ARMv6-M `armv6m.c` |
| `riscv32-unknown-elf`, each ABI | `int64.c`, `softfp.c`, `softtf.c`, `ldouble.c`, `atomic8.c`, `atomic-riscv.c` |
| `riscv64-unknown-elf`, each ABI | `int128.c`, `fp128.c`, `softfp.c` (empty for `lp64d`), `softtf.c`, `ldouble.c`, `atomic-riscv.c` |
| `loongarch64-unknown-elf`, `mips64-none-elf`, `mips64el-none-elf` | `int128.c`, `fp128.c`, `softfp.c`, `softtf.c`, `ldouble.c` |
| `sparc-none-elf` | `int64.c`, `softfp.c`, `softtf.c`, `ldouble.c`, `atomic8.c` |
| `mips-none-elf`, `mipsel-none-elf`, `powerpc-none-eabi`, `m68k-none-elf`, `tricore-none-elf`, `xtensa-none-elf` | `int64.c`, `softfp.c`, `atomic8.c` |
| `rx-none-elf` | `int64.c`, `avrfp*.c`, `atomic8.c` |
| `avr` | `avr.c`, `avr64.c`, `avrdivmod.c`, `avrfp*.c` |
| `aarch64-apple-darwin`, `x86_64-apple-darwin` | `int128.c`, `fp128.c`, `ldouble.c`, and on AArch64 `softtf.c`; linked after libSystem: the routines it does not export -- `__divdc3` and `__divsc3`, the 128-bit shifts and `__multi3` -- which clang takes from its own `libclang_rt.osx.a`. The archive is in the BSD format Apple's linker reads, as `embar` writes for Mach-O members |

## Building the libraries

All libraries are compiled by `./embcc` from the same tree, so build the
compiler first (`make embcc`). Each rule rebuilds its archive from
scratch.

| Target | Builds | Output |
|---|---|---|
| `make libc-x86_64` | C library, posixlike backend, `x86_64-elf`, `--opt=basic` | `build/libc/x86_64/libc.a` |
| `make libc-aarch64` | the same for `aarch64-elf` | `build/libc/aarch64/libc.a` |
| `make libc` | both of the above | |
| `make libc-linux-x86_64` | C library with the Linux backend, `crt1.o`, and `librt.a`, for `x86_64-linux-gnu` | `build/libc/linux-x86_64/{libc.a,crt1.o,librt.a}` |
| `make libc-linux-aarch64` | the same for `aarch64-linux-gnu` | `build/libc/linux-aarch64/{libc.a,crt1.o,librt.a}` |
| `make libc-linux` | both Linux C libraries | |
| `make libc-embedded` | C library with the bare-metal backend, `--opt=size --sections=functions,data`, for each embedded target it builds on (all but `avr`) | `build/libc/TRIPLE/libc.a` |
| `make libc-emblinkos EMBLINKOS=DIR` | C library with the EmbLinkOS backend, `--opt=speed`, an empty `libm.a`, and `librt.a` with the unwinder; needs `DIR/user/lib/embk.h` (default `DIR` is `$HOME/EmbLinkOs`) | `build/libc/emblinkos/{libc.a,libm.a,librt.a}` |
| `make libcxx-emblinkos EMBLINKOS=DIR` | C++ library over the EmbLinkOS C library | `build/libcxx/emblinkos/libcxx.a` |
| `make libcxx-x86_64`, `make libcxx-aarch64` | C++ library for `x86_64-elf`, `aarch64-elf`, `--opt=speed` | `build/libcxx/x86_64/libcxx.a`, `build/libcxx/aarch64/libcxx.a` |
| `make libcxx` | both of the above | |
| `make libcxx-linux-x86_64`, `make libcxx-linux-aarch64` | C++ library for the Linux triples | `build/libcxx/linux-x86_64/libcxx.a`, `build/libcxx/linux-aarch64/libcxx.a` |
| `make libcxx-embedded` | the C++ runtime for the 32-bit ARM triples and `riscv32-unknown-elf`, `--opt=size --no-exceptions --no-rtti`, through `tools/build-libcxx.sh`: `operator new`/`delete` (weak), the guard functions, `__cxa_pure_virtual`, `__aeabi_atexit`, `__dso_handle`, the RTTI classes and `__dynamic_cast` | `build/libcxx/TRIPLE/libcxx.a` |
| `make libc-linux-all` | `libc-linux` and both Linux C++ libraries | |
| `make rt-embedded` | `librt.a` for `avr`, the five Cortex-M triples and `riscv32-unknown-elf`, `--opt=size`, through `tools/build-rt.sh` | `build/libc/TRIPLE/librt.a` |

The archiver is a cross `ar`: `x86_64-elf-ar` and `aarch64-elf-ar` by
default, overridden with `EMBCC_X86_AR` and `EMBCC_AARCH64_AR`.
`tools/build-rt.sh` uses `llvm-ar`, or `EMBCC_AR`, or `ar` when neither is
found, and `EMBCC` names the compiler it runs.

`make install PREFIX=DIR` builds all of the above except
`libc-emblinkos` and installs them; `make install-files` only copies.
Each triple's files go to `PREFIX/lib/embcc/VERSION/TRIPLE/`, the Linux
triples also get `link.ld` there, and the headers go to
`PREFIX/lib/embcc/VERSION/include`, `include/c++` and `freestanding`.
The EmbLinkOS C library is not installed. See
[Getting started](getting-started.md).

### Linking a Linux program by hand

The driver links `x86_64-linux-gnu` programs itself. For
`aarch64-linux-gnu`, or to add objects, link with a GNU-compatible
linker and the installed script, which keeps the program headers inside
the first loadable segment (the C library finds thread-local storage
through them):

```sh
embcc --target=aarch64-linux-gnu --opt=speed -c prog.c -o prog.o
aarch64-elf-ld -static -T link.ld -o prog crt1.o prog.o libc.a librt.a
```

All four files are in the triple's library directory.
