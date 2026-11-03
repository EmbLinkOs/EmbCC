# Run-time checks and hardening

Three families of options make a program check itself while it runs:

- [Sanitizers](#sanitizers) find bugs in testing: out-of-bounds and
  use-after-free accesses, data races, reads of uninitialized memory,
  and undefined arithmetic.
- [Bounds safety](#bounds-safety) checks every array subscript, including
  a subscript of a flexible array member whose length another member
  holds.
- [Hardening](#hardening) is for the shipped program: stack canaries,
  stack-clash probes, and the control-flow protection of x86-64 (CET) and
  AArch64 (PAC and BTI).

All of them are added after the optimizer, to the code that will be
emitted. What is checked is what runs, and the checks do not change what
the optimizer does to the rest of the program.

## Sanitizers

| `--sanitize=` | GCC's spelling | Finds | Targets | Runtime |
|---|---|---|---|---|
| `address` | `-fsanitize=address` | Heap, stack and global buffer overflows, use after free, double free, a free of what was not allocated | Every target, the boards included | `libasan.a` |
| `thread` | `-fsanitize=thread` | Data races between threads | Linux, macOS | `libtsan.a` |
| `memory` | `-fsanitize=memory` | A branch, an address or a call that depends on uninitialized memory | Linux, macOS | `libmsan.a` |
| `undefined` | `-fsanitize=undefined` | Signed overflow, division by zero, a shift by a negative count or by the operand's width or more | Every target | A report in `librt` on a host; on a board a failed check traps |
| `signed-integer-overflow`, `integer-divide-by-zero`, `shift` | the same | One check each of `undefined`'s | Every target | The same |
| `bounds`, `array-bounds` | the same | Out-of-bounds subscripts, as [`--bounds-safety`](#bounds-safety) checks them | Every target | The same |

`address` cannot be combined with `thread` or `memory`, because each keeps
its own shadow of memory, and `thread` cannot be combined with `memory`.
`--no-sanitize=LIST` takes checks back out of the set. A check EmbCC does
not have, such as `leak`, `object-size` or `function`, is refused by name.
A build that asked for it and got nothing would be worse than an error.

The runtimes are linked by the same `--sanitize=` option at the link. They
are archives, so `-static-libasan` and its relatives are accepted and
change nothing.

### AddressSanitizer

```sh
embcc -g --sanitize=address heap.c -o heap && ./heap
```

```text
=================================================================
==78292==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x104871140 at pc 0x1042f4740
WRITE of size 4 at 0x104871140 thread T0
    #0 0x1042f4740 in main+0x130 (/home/me/heap+0x740)
    #1 0x193644d7c in start+0x19fc (/usr/lib/dyld+0x31d7c)
0x104871140 is located 0 bytes after 32-byte region [0x104871120,0x104871140)
allocated by thread T0 here:
    #0 0x1042f46d4 in main+0xc4 (/home/me/heap+0x6d4)
SUMMARY: AddressSanitizer: heap-buffer-overflow
```

Each frame is named as the dynamic symbol table names it, and always
given as its module and the offset into it, which `addr2line -e`,
`atos -o` and `llvm-symbolizer` turn into a file and line. A static
function, which the dynamic table does not name, still has its module
offset.

Every load and store is checked against a shadow that marks which bytes
are addressable, one shadow byte per eight bytes of memory, in LLVM's
encoding. Around what can be overrun there are poisoned redzones:

- Each heap block gets redzones from the runtime's `malloc`, which the
  instrumentation calls in place of the C library's. A freed block stays
  poisoned in a quarantine for a while, so a later use is caught.
- Each global gets redzone bytes after it, which the unit's constructor
  poisons when it registers the global.
- The locals that could be overrun (arrays, structures, anything whose
  address is taken) are moved into one block with redzones between them.
  The block is poisoned on entry and made good again before each return.
  A `longjmp`, a throw or an `exit` first unpoisons the stack it leaves.

`ASAN_OPTIONS` takes `halt_on_error`, `exitcode`, `quarantine_size_mb`
and `abort_on_error`. With `--sanitize-recover=address`, a report does
not stop the program, and later errors are reported too.

**On a board.** The shadow covers the RAM the program names, in two
variables the runtime declares weak:

```c
#include <stdint.h>

uintptr_t __embcc_asan_ram_lo = 0x20000000u;        /* the RAM checked */
uintptr_t __embcc_asan_ram_hi = 0x20010000u;
static unsigned char shadow[0x10000 / 8];
unsigned char *__embcc_asan_shadow_mem = shadow;   /* optional */
```

The shadow is an eighth of the RAM it covers. Without
`__embcc_asan_shadow_mem`, it is taken from the heap. An address outside
the covered RAM, such as flash or a peripheral register, is not checked,
and neither is anything before the shadow exists. A report is printed on
the console, and then the runtime calls `abort()`.

### ThreadSanitizer

```sh
embcc -g --sanitize=thread -pthread worker.c -o worker && ./worker
```

A happens-before race detector, built on FastTrack-style vector clocks.
Every access reports itself to the runtime, and the synchronization calls
(`pthread_*` and C11's `thrd_*`, `mtx_*`, `cnd_*`) record the order they
create. An atomic access is a release before and an acquire after. Two
accesses race when they come from different threads, overlap, include
a write, and neither is ordered before the other. A program with a race
exits with status 66, as with clang's and GCC's ThreadSanitizer.

On macOS, the C11 thread calls, the spin locks, the barriers and the
timed waits of pthreads do not exist in libSystem, so a program that uses
them does not link there either way.

### MemorySanitizer

```sh
embcc -g --sanitize=memory state.c -o state && ./state
```

Each byte of memory has a shadow byte that holds its uninitialized
bits. A function's locals and each new `malloc` block start poisoned, a
store writes its value's shadow, and arguments and return values carry
theirs beside the call. A report comes when a poisoned value decides a
branch, becomes an address, or is passed to a library call. Copying an
uninitialized value is not reported, since C allows it. The C library is
not instrumented, so the runtime wraps the calls that write memory
(`read`, `fread`, `fgets`, the `printf` and `scanf` families, `stat`,
`time` and others) to mark what they wrote as initialized, and the copies
(`memcpy`, `strcpy`, `strdup`) to copy the shadow. The exit status is 77
(`MSAN_OPTIONS=exitcode=N` changes it).

MemorySanitizer is exact at every `-O` level: the optimizer cannot fold an
uninitialized read away before the check sees it.

### Undefined behaviour

`--sanitize=undefined` and its parts check arithmetic in place, as
ordinary code that the optimizer folds away where it knows the operands:
`a / 7` keeps no check at `-O2`.

On a hosted target (Linux, macOS), a failed check says what happened and
where, as clang's and GCC's runtimes do, and then aborts:

```text
t.c:2:31: runtime error: signed integer overflow
```

With `--sanitize-recover=undefined` (or one check's name, or `all`), the
report is printed and the program goes on with the wrapped value, so one
run reports every failure.

On a board, and anywhere with `-fsanitize-trap` (with or without a list)
or `-fsanitize-undefined-trap-on-error`, a failed check runs the target's
trap instruction: `ud2`, `brk`, `udf`, `ebreak` or the equivalent. Under
a debugger, that stops the program at the faulting operation. Without
one, the program stops instead of going on with a wrong value. A trap
costs a few bytes per check and needs no runtime.

EmbCC [promises `-fwrapv`](c-language.md): signed overflow wraps, and no
optimization assumes it cannot happen. `signed-integer-overflow` is for
finding the overflow, not for making it defined.

### Turning a check off for one function

| Attribute | Effect |
|---|---|
| `no_sanitize("address")`, `no_sanitize_address`, `no_address_safety_analysis` | No AddressSanitizer checks in the function |
| `no_sanitize("thread")`, `no_sanitize_thread` | No ThreadSanitizer reports from the function |
| `no_sanitize("memory")`, `no_sanitize_memory` | No MemorySanitizer checks in the function |
| `no_sanitize("undefined")`, `no_sanitize_undefined` | No arithmetic checks in the function |
| `disable_sanitizer_instrumentation` | None of the above |

## Bounds safety

```c
#include <stdlib.h>

struct pkt {
    int len;
    char data[] __attribute__((counted_by(len)));
};

char nth(struct pkt *p, int i) { return p->data[i]; }   /* checked against p->len */
```

```sh
embcc -O2 --bounds-safety pkt.c -o pkt
```

Under `--bounds-safety` (GCC's `-fbounds-safety`, also
`--sanitize=bounds`), every subscript of an object whose size the
compiler knows is checked:

- an array, at any depth (`m[i][j]` checks both subscripts against their
  own bounds);
- a flexible array member marked `counted_by(member)`, against that
  member's value when the subscript runs;
- a pointer member marked `counted_by(member)` or `sized_by(member)`
  (`sized_by` counts bytes), the same way.

A subscript out of bounds traps. Taking the address one past the end,
`&a[n]`, is allowed, as C allows it. `counted_by` also gives
`__builtin_dynamic_object_size` the array's real size, so `_FORTIFY_SOURCE`
style checks see it.

`<ptrcheck.h>` spells the annotations as clang's `-fbounds-safety` does:
`__counted_by(N)`, `__sized_by(N)`, and their `_or_null` forms. The rest
of clang's vocabulary (`__single`, `__indexable`, `__bidi_indexable`,
`__ended_by`, `__terminated_by`, the ABI pragmas) is accepted and means
nothing, so a header written for clang compiles. EmbCC's pointers carry
no bounds of their own, and a pointer without an annotation is not
checked.

## Hardening

### Stack protector

| Option | GCC's spelling | Protects |
|---|---|---|
| `--stack-protector` | `-fstack-protector` | A function with a character array of 8 bytes or more (`--param=ssp-buffer-size=N`) |
| `--stack-protector=strong` | `-fstack-protector-strong` | A function with any array, or a local whose address is taken |
| `--stack-protector=all` | `-fstack-protector-all` | Every function |
| `--stack-protector=explicit` | `-fstack-protector-explicit` | A function marked `__attribute__((stack_protect))` |
| `--no-stack-protector` | `-fno-stack-protector` | None, the default |

A protected function's vulnerable locals are moved together below a
canary, a copy of `__stack_chk_guard` that sits between them and the
return address. Before each return the canary is compared with the guard,
and a mismatch calls `__stack_chk_fail`, which does not return. A
function marked `__attribute__((no_stack_protector))` is never protected.

The guard is the C library's when it has one (libSystem, glibc on
AArch64, a board's newlib). Otherwise it comes from EmbCC's runtime: on
x86-64 Linux, the thread block's (`%fs:0x28`, which glibc fills from the
kernel's random bytes); elsewhere, a value with a zero low byte, which a
string overrun cannot reproduce, mixed with the stack's address. On a
board with a random source, set `__stack_chk_guard` at reset, before any
protected function runs. EmbCC's C library's `__stack_chk_fail` says what
happened, then aborts. The runtime's fallback traps.

### Stack-clash protection

`--stack-clash-protection` (`-fstack-clash-protection`) makes sure no
single step of the stack pointer goes further than a page. A large
frame, a variable-length array or an `alloca` is allocated a page at a
time, with each page touched, so the stack cannot jump past a guard page
into other memory.

### x86-64: CET

`--cf-protection=KIND` (`-fcf-protection=`): `branch` puts an `endbr64`
at every function entry and after every call of a `returns_twice`
function such as `setjmp`, and marks a `switch` table's jump `notrack`.
`return` marks the object as shadow-stack compatible. `full` means both,
and `none` neither. The object's `.note.gnu.property` says which, so the
linker and the loader turn the protection on only when every object has
it.

### AArch64: PAC and BTI

`--branch-protection=KIND` (`-mbranch-protection=`):

| Kind | Effect |
|---|---|
| `pac-ret` | A function that saves the return address signs it on entry (`paciasp`) and checks it before returning (`autiasp`), so an overwritten return address faults |
| `pac-ret+leaf` | ...leaf functions too |
| `pac-ret+b-key` | ...with the B key (`pacibsp`) |
| `bti` | A `bti c` landing pad at each function entry and `bti j` at each computed-jump target, so an indirect branch elsewhere faults |
| `standard` | `bti` and `pac-ret` |
| `none` | Neither |

The signing instructions are in the hint space, so the code still runs,
unprotected, on a core without PAC or BTI. The unwind information records
the signed return address (`DW_CFA_AARCH64_negate_ra_state`), so a
debugger and C++ exceptions unwind through it. `.note.gnu.property`
records BTI and PAC for the linker.

### ARMv8.1-M: PACBTI

The same option on ARMv8.1-M Mainline (`thumbv8.1m.main-none-eabi` with
`-mcpu=cortex-m55` or `cortex-m85`, or `-march=armv8.1-m.main`), as
clang compiles it:

| Kind | Effect |
|---|---|
| `pac-ret` | A function that saves `lr` starts with `pac r12, lr, sp`, pushes `r12` with `lr`, and returns with `bxaut r12, lr, sp`, which faults on a changed return address |
| `pac-ret+leaf` | ...every function, which then pushes `r12` and `lr` |
| `bti` | `bti` is each function's first instruction |
| `standard` | both, `pacbti r12, lr, sp` doing the two at entry |

`bxaut` is the PACBTI extension's own instruction (`-mcpu=cortex-m85`, or
`-march=armv8.1-m.main+pacbti`, which also define `__ARM_FEATURE_PAUTH`
and `__ARM_FEATURE_BTI`). Without the extension the return is `aut r12,
lr, sp` and `bx lr`: `pac`, `aut` and `bti` are hints there, so the code
runs, unprotected, on a Cortex-M55. `__ARM_FEATURE_BTI_DEFAULT` and
`__ARM_FEATURE_PAC_DEFAULT` say what was asked for, on AArch64 too. An
interrupt handler and a CMSE entry function keep their own returns.
`+b-key` is refused: ARMv8.1-M has one key.
