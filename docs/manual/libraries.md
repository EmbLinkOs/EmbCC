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
| C++ runtime and standard library | `lib/libcxx` | `libcxx.a` | `x86_64-elf`, `aarch64-elf`, `x86_64-linux-gnu`, `aarch64-linux-gnu` | `libcxx-x86_64`, `libcxx-aarch64`, `libcxx-linux-x86_64`, `libcxx-linux-aarch64` |
| Compiler runtime | `lib/rt` | `librt.a` | `x86_64-linux-gnu`, `aarch64-linux-gnu`, and the embedded triples | `libc-linux-*` (Linux), `rt-embedded` (embedded) |

Every library is built per target by the `embcc` in the same tree, so it
always matches the compiler's ABI. No library exists for the macOS or
Windows triples: a Darwin object is linked by the system linker against
the system's libraries, and the Windows triple has no C library.

## How the driver links the libraries

### When the driver links at all

`embcc [FILE] [OBJECTS...] -o OUT` (no `-c`, `-S`, `-E` or
`-fsyntax-only`) compiles the source, if there is one, and links in the
same process with [EmbLD](tools/embld.md). The driver links the x86-64 ELF
targets (`x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu`) and, given a
memory map (`-T board.ld`, or `-Wl,-Ttext`/`-Tdata`), the ARM, RISC-V and
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
linked.

A missing library is reported by name rather than as a list of undefined
symbols:

```text
embcc: error: no crt1.o for x86_64-linux-gnu -- the target's library is not built or not installed
embcc: --print-search-dirs says where it looked
```

A C++ program on a target with no `libcxx.a` fails the same way
(`embcc: error: no libcxx.a for TRIPLE -- a C++ program needs the C++
runtime, and this target's is not built or not installed`). On a hosted
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
triples, and `build/libc/TRIPLE` for every other triple. `libcxx.a` is
under `build/libcxx/` in the same scheme. An installation has one
directory per canonical triple and none for `x86_64-emblink`. The search
for the installation itself, and `EMBCC_PREFIX`, are described in
[Invoking](invoking.md#how-the-compiler-finds-its-own-files).

### Opting out

The driver has no `-nostdlib`, `-nodefaultlibs` or `-nostartfiles`, and no
`-l` or `-L`; each is rejected as an unknown argument:

```text
embcc: error: unknown argument '-nostdlib'
```

The driver's link takes EmbLD's own options through `-Wl,` and
`-Xlinker` (`-e`, `-Ttext`, `-Tdata`, `-Tstack`, `--rom-limit`,
`--lma-offset`; see [Invoking](invoking.md#-wlargs--xlinker-arg)), but not
other inputs. To link without EmbCC's libraries, or with others, compile
with `-c` and run `embld` with exactly the inputs wanted:

```sh
embcc -c -O2 kernel.c -o kernel.o
embld -e _start -Ttext 0xFFFFFFFF80100000 -o kernel.elf entry.o kernel.o
```

`-ffreestanding` and `-fno-builtin` are accepted and change neither the
link line nor the header search. `-nostdinc` removes EmbCC's header
directories from the search (see [Invoking](invoking.md#-nostdinc)).

### Header directories

EmbCC's own headers are searched after every `-I` and `-isystem`
directory, as system directories:

| Headers | Build tree | Installation |
|---|---|---|
| C++ library | `lib/libcxx/include` | `PREFIX/lib/embcc/VERSION/include/c++` |
| C library | `lib/libc/include` | `PREFIX/lib/embcc/VERSION/include` |
| Freestanding (`<stddef.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stdint.h>`, `<limits.h>`, `<float.h>`, a declarations-only `<string.h>`, `<unwind.h>`, `<cpuid.h>`) | `include` | `PREFIX/lib/embcc/VERSION/freestanding` |

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

`lib/libc` is a C11 library written for EmbCC: the portable part is the
same source on every target, and each operating system is reached through
one small backend file (see [Backends](#backends)). Its headers are the
C11 hosted set:

`<assert.h>` `<complex.h>` `<ctype.h>` `<errno.h>` `<fenv.h>`
`<inttypes.h>` `<iso646.h>` `<limits.h>` `<locale.h>` `<math.h>`
`<setjmp.h>` `<signal.h>` `<stdalign.h>` `<stdatomic.h>` `<stdio.h>`
`<stdlib.h>` `<stdnoreturn.h>` `<string.h>` `<tgmath.h>` `<threads.h>`
`<time.h>` `<uchar.h>` `<wchar.h>` `<wctype.h>`

`<stddef.h>`, `<stdarg.h>`, `<stdbool.h>`, `<stdint.h>` and `<float.h>`
come from the freestanding directory. `<limits.h>` defines the library's
own limits (`PATH_MAX` 4096, `NAME_MAX` 255, ...) and includes the
freestanding `<limits.h>` for the type widths.

| Area | What is provided |
|---|---|
| `<string.h>` | all of C11 §7.24, plus `strdup`, `strndup`, `strnlen` |
| `<ctype.h>`, `<wctype.h>` | complete, for the `"C"` locale; the classification functions return 0 or 1 |
| `<stdlib.h>` | `malloc` family with `aligned_alloc`; `strtol` family, `strtod`/`strtof`/`strtold`, `ato*`; `qsort` (heapsort) and `bsearch`; `rand`/`srand` (`RAND_MAX` is 2147483647); `abs`/`div` families; `atexit`, `exit`, `_Exit`, `abort`; `getenv`; `system` (always reports no command processor) |
| `<stdio.h>` | `FILE` streams with buffering, files by name, positioning (`fseek`, `ftell`, `fgetpos`, `fsetpos`, `rewind`), `remove`, `rename`, `setvbuf`, `ungetc`, `perror`, and the complete `printf` and `scanf` families (`v`, `f`, `s`, `sn` forms) |
| `<math.h>`, `<tgmath.h>` | every C11 function at all three widths |
| `<complex.h>` | complete |
| `<fenv.h>` | exception flags and rounding modes, on x86-64 (`MXCSR`) and AArch64 (`FPSR`/`FPCR`) |
| `<time.h>` | `time`, `clock`, `difftime`, `mktime`, `timegm`, `gmtime`, `localtime`, the `_r` forms, `asctime`, `ctime`, `strftime`, `timespec_get` |
| `<setjmp.h>` | `setjmp`/`longjmp` for x86-64 and AArch64 |
| `<signal.h>` | `signal` and `raise` |
| `<locale.h>` | `setlocale` and `localeconv`, one locale |
| `<threads.h>` | threads, mutexes, condition variables, `call_once` |
| `<wchar.h>`, `<uchar.h>` | wide strings, `wcsto*`, restartable UTF-8 conversions (`mbrtowc`, `wcrtomb`, `mbsrtowcs`, `wcsrtombs`), `mbrtoc16`/`c16rtomb`/`mbrtoc32`/`c32rtomb` |
| `<stdatomic.h>` | the C11 interface over the compiler's atomic builtins; no library code |
| `<assert.h>`, `<inttypes.h>`, `<errno.h>` | complete; `errno` values are the traditional POSIX numbers |

### Behavior worth knowing

**Formatted output.** `printf` converts floating-point values exactly,
by integer arithmetic on the binary value, so every precision is
correctly rounded with ties to even (`%.0f` of 2.5 is `2`). `long double`
is converted at its own width. `%a` is exact and rounds a precision to
nearest, ties to even; a subnormal is printed normalized with a leading
`1`. `%b` prints binary. A null `%s` prints `(null)` and a null `%p`
prints `(nil)`. `%n` always stores an `int`, whatever its length
modifier. An unknown conversion prints itself. `%lc` and `%ls` are not
wide: the `l` is ignored, and the argument is read as an `int` or a
`char *`.

**Formatted input.** `scanf` supports `%[`, `%n`, `%a` and hexadecimal
floating-point input. It distinguishes an input failure (returns `EOF`)
from a matching failure (returns the number of items assigned, possibly
0): `sscanf("x", "%d", &n)` returns 0.

**`strtod`.** A decimal with at most 15 significant digits and a power
of ten up to 10^22 is converted exactly. Other inputs may be one unit in
the last place from the nearest value, which C11 permits. Hexadecimal
input is exact.

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
the hardware instruction. `float` forms compute in `double`. The `long
double` forms of the functions that compute a new value (`sinl`, `expl`,
`powl`, ...) compute in `double`, so on x86-64 they deliver 53 of the 64
significand bits; the functions that only move or inspect a value
(`fabsl`, `truncl`, `floorl`, `frexpl`, `ldexpl`, `fmodl`, `copysignl`,
...) are exact at full width. The classification macros (`fpclassify`,
`isnan`, `isinf`, `isfinite`, `isnormal`, `signbit`) read a `long double`
in its own format, so `isinf(LDBL_MAX)` is 0 and a `long double`
subnormal is `FP_SUBNORMAL`. `nearbyint` and `rint` follow the current
rounding mode; `remainder` rounds the quotient to nearest, ties to even.

**Locale.** There is one locale, `"C"`. `setlocale` accepts `"C"`,
`"POSIX"` and `""` and returns a null pointer for any other name. The
multibyte encoding is UTF-8: overlong forms, surrogates and values above
U+10FFFF are rejected. `wchar_t` is 32 bits on the targets this library is
built for.

**Time.** There is no time-zone database: `localtime` is `gmtime`,
`tm_isdst` is 0, and `strftime`'s `%z` is `+0000`. `time` and `clock`
return what the backend provides, which on the freestanding targets is
-1.

**Signals.** The C model only: `raise` calls the handler directly, and
nothing delivers a signal from outside.

**Threads.** `thrd_create` creates a thread where the backend can (Linux
and EmbLinkOS) and returns `thrd_error` elsewhere. Mutexes, condition
variables and `call_once` work on every target, threaded or not.
`tss_create` always fails and `tss_get` returns a null pointer.
`thread_local` is defined as `_Thread_local`.

**Environment.** `getenv` reads the vector the startup code publishes in
`environ`. `setenv` and `unsetenv` are declared in `<stdlib.h>` but not
defined: a program that calls them fails to link.

### Thread safety

The library's shared state is locked. The heap has one lock; each
`FILE` has its own, held for the whole of a call, so the output of one
`printf` or `puts` is never interleaved with another thread's; the table
of open streams has a third. The locks are futex-based: uncontended,
locking and unlocking are one atomic instruction each. On a target whose
backend has no futex the wait degrades to a yield, which is only reached
when a lock is contended, and such a target has one thread.

`errno` is per thread on the Linux triples, where the C library sets up
thread-local storage, and one variable elsewhere. Functions that keep
state between calls by definition (`strtok`, `rand`, `gmtime`,
`localtime`, `asctime`, `ctime`) share it between threads; use the `_r`
forms of the time functions.

### Not provided

| Missing | Notes |
|---|---|
| `tmpfile`, `tmpnam` | |
| `quick_exit`, `at_quick_exit` | |
| `mblen`, `mbtowc`, `wctomb`, `mbstowcs`, `wcstombs`, `btowc`, `wctob` | use the restartable `mbr*`/`wcr*` forms |
| wide-character I/O: `fwide`, `fwprintf`, `wprintf`, `swprintf`, `fgetws`, ... | |
| `setenv`, `unsetenv` | declared, not defined |
| C11 Annex K (`*_s` functions) | |
| POSIX extensions such as `fdopen`, `fileno`, `popen`, `strtok_r`, `strerror_r`, `fseeko`, `fmemopen` | |
| a time-zone database, locales other than `"C"`, signal delivery | |

### Backends

Everything above is portable C. An operating system is reached through
the functions declared in `lib/libc/os/backend.h`, and one file per OS
implements them:

| Group | Functions | Without them |
|---|---|---|
| Core | `__os_write`, `__os_read`, `__os_open`, `__os_close`, `__os_lseek`, `__os_remove`, `__os_rename`, `__os_sbrk`, `__os_time`, `__os_clock_ns`, `__os_exit`, `__os_isatty`, `__os_getentropy` | required |
| Threads | `__os_thread_create`, `__os_thread_join`, `__os_thread_detach`, `__os_thread_self`, `__os_thread_yield`, `__os_sleep_ns`, `__os_futex_wait`, `__os_futex_wake` | single-threaded: each returns -1 with `ENOSYS` |
| Filesystem | `__os_stat`, `__os_lstat`, `__os_mkdir`, `__os_rmdir`, `__os_unlink`, `__os_chmod`, `__os_truncate`, `__os_utime`, `__os_symlink`, `__os_readlink`, `__os_link`, `__os_getcwd`, `__os_chdir`, `__os_statfs`, `__os_opendir`, `__os_readdir`, `__os_closedir` | no filesystem beyond file descriptors; each returns -1 with `ENOSYS` |

C++'s `<thread>`, `<mutex>` and the other synchronization headers are
built on the thread group, and `<filesystem>` on the filesystem group.

Four backends exist:

| Backend | Used by | What it does |
|---|---|---|
| `os/posixlike/backend.c` | `libc-x86_64` and `libc-aarch64` (`x86_64-elf`, `aarch64-elf`) | forwards to `write`, `read`, `open`, `close`, `lseek`, `sbrk`, `_exit` and `isatty`, which the program or its environment must define. The thread functions (`thread_create`, `futex_wait`, ...) and filesystem functions (`fs_stat`, ...) are weak references: defined, they are used; absent, the group reports `ENOSYS`. `remove`, `rename`, `time`, `clock` and entropy report `ENOSYS`. |
| `os/linux/` | `libc-linux-x86_64`, `libc-linux-aarch64` | issues Linux system calls directly, so a program needs no other C library and no dynamic loader. Provides `crt1.o` (the `_start` that calls `main` and `exit`), threads through `clone`, thread-local storage (per-thread `errno`), and the filesystem group. Static images only. |
| `os/emblinkos/backend.c` | `libc-emblinkos` | EmbLinkOS's system calls, including threads and the filesystem group. `clock` reports failure. |
| `os/baremetal/backend.c` | `libc-embedded` (the embedded targets) | for a part with no operating system, and needs nothing from the program: `write`, `read`, `open`, `close`, `lseek`, `sbrk`, `_exit` and `isatty` are defined WEAKLY with defaults (output discarded, input at end of file, a heap from the end of the image to the stack, `_exit` stops), and a program's own definition of any of them replaces the default. One thread, no clock, no filesystem: those report `ENOSYS`. |

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
| `x86_64-elf`, `aarch64-elf` | libgcc's; the program links `libgcc.a` (or `libgcc_eh.a`) |
| EmbLinkOS, macOS | the platform's |
| the embedded targets | none. C++ code generation is refused on Cortex-M, RV32 and AVR, and at RV64 a function that needs a landing pad is refused (see [Bare-metal programming](embedded.md#c-on-the-embedded-targets)) |

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
| `int64.c` | `__divdi3` `__udivdi3` `__moddi3` `__umoddi3` | 32-bit targets except AVR |
| `int128.c` | `__multi3` `__divti3` `__udivti3` `__modti3` `__umodti3` `__ashlti3` `__ashrti3` `__lshrti3` `__negti2` | targets with `__int128` (not RV64) |
| `fp128.c` | conversions between 128-bit integers and `float`/`double`: `__floattidf` `__floatuntidf` `__floattisf` `__floatuntisf` `__fixdfti` `__fixunsdfti` `__fixsfti` `__fixunssfti` | as `int128.c` |
| `complex.c` | `__mulsc3` `__muldc3` `__divsc3` `__divdc3` | every target |
| `ldouble.c` | x86-64: the x87 `long double` conversions `__floattixf` `__floatuntixf` `__fixxfti` `__fixunsxfti` and `__mulxc3` `__divxc3`; AArch64: `__multc3` `__divtc3` | x86-64, AArch64 |
| `softtf.c` | IEEE binary128 in software, AArch64's `long double`: `__addtf3` `__subtf3` `__multf3` `__divtf3` `__negtf2`, the comparisons `__eqtf2` ... `__unordtf2`, and the conversions `__extenddftf2` `__trunctfdf2` `__floatsitf` `__fixtfdi` ... | AArch64 |
| `softfp.c` | binary32 and binary64 in software: `__addsf3` `__subsf3` `__mulsf3` `__divsf3` `__negsf2` and the `df` forms, comparisons (`__eqsf2` `__nesf2` `__ltsf2` `__lesf2` `__gtsf2` `__gesf2` `__unordsf2` and `df`), conversions to and from 32- and 64-bit integers, `__extendsfdf2`, `__truncdfsf2` | Cortex-M (all variants; built for the Cortex-M7's double-precision unit, only the eight conversions to and from 64-bit integers, `__fixdfdi` ... `__floatundisf`), RISC-V |
| `avr.c` | `__mulsi3` `__divsi3` `__udivsi3` `__modsi3` `__umodsi3` | AVR |
| `avr64.c` | `__muldi3` `__divdi3` `__udivdi3` `__moddi3` `__umoddi3` | AVR |
| `avrfp*.c` | binary32 in software, one object per group: `__addsf3`/`__subsf3`/`__negsf2`, `__mulsf3`, `__divsf3`, the comparisons including `__cmpsf2`, and the integer conversions | AVR |
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

| Triple | `librt.a` | Contents |
|---|---|---|
| `x86_64-linux-gnu` | yes | `int128.c`, `fp128.c`, `complex.c`, `ldouble.c`, `unwind.c` |
| `aarch64-linux-gnu` | yes | `int128.c`, `fp128.c`, `complex.c`, `ldouble.c`, `softtf.c`, `unwind.c` |
| `thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi`, `thumbv8m.main-none-eabihf`, `riscv32-unknown-elf` | yes | `int64.c`, `softfp.c`, `complex.c` |
| `thumbv6m-none-eabi` | yes | `armv6m.c` (the `__aeabi_*` divide, multiply, shift and block routines, and the `__atomic_*` calls), `int64.c`, `softfp.c`, `complex.c` |
| `avr` | yes | `avr.c`, `avr64.c`, `avrfp*.c`, `complex.c` |
| `x86_64-elf`, `aarch64-elf` | no | these link libgcc |
| `riscv64-unknown-elf` | no | `int128.c` and `fp128.c` do not compile there; compile `softfp.c` and `complex.c` by hand |

## Building the libraries

All libraries are compiled by `./embcc` from the same tree, so build the
compiler first (`make embcc`). Each rule rebuilds its archive from
scratch.

| Target | Builds | Output |
|---|---|---|
| `make libc-x86_64` | C library, posixlike backend, `x86_64-elf`, `-O1` | `build/libc/x86_64/libc.a` |
| `make libc-aarch64` | the same for `aarch64-elf` | `build/libc/aarch64/libc.a` |
| `make libc` | both of the above | |
| `make libc-linux-x86_64` | C library with the Linux backend, `crt1.o`, and `librt.a`, for `x86_64-linux-gnu` | `build/libc/linux-x86_64/{libc.a,crt1.o,librt.a}` |
| `make libc-linux-aarch64` | the same for `aarch64-linux-gnu` | `build/libc/linux-aarch64/{libc.a,crt1.o,librt.a}` |
| `make libc-linux` | both Linux C libraries | |
| `make libc-embedded` | C library with the bare-metal backend, `-Os`, for each embedded target it builds on (all but `avr`) | `build/libc/TRIPLE/libc.a` |
| `make libc-emblinkos EMBLINKOS=DIR` | C library with the EmbLinkOS backend, `-O2`; needs `DIR/user/lib/embk.h` (default `DIR` is `$HOME/EmbLinkOs`) | `build/libc/emblinkos/libc.a` |
| `make libcxx-x86_64`, `make libcxx-aarch64` | C++ library for `x86_64-elf`, `aarch64-elf`, `-O2` | `build/libcxx/x86_64/libcxx.a`, `build/libcxx/aarch64/libcxx.a` |
| `make libcxx` | both of the above | |
| `make libcxx-linux-x86_64`, `make libcxx-linux-aarch64` | C++ library for the Linux triples | `build/libcxx/linux-x86_64/libcxx.a`, `build/libcxx/linux-aarch64/libcxx.a` |
| `make libc-linux-all` | `libc-linux` and both Linux C++ libraries | |
| `make rt-embedded` | `librt.a` for `avr`, the five Cortex-M triples and `riscv32-unknown-elf`, `-Os`, through `tools/build-rt.sh` | `build/libc/TRIPLE/librt.a` |

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
embcc --target=aarch64-linux-gnu -O2 -c prog.c -o prog.o
aarch64-elf-ld -static -T link.ld -o prog crt1.o prog.o libc.a librt.a
```

All four files are in the triple's library directory.
