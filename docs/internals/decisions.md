# Design decisions

This page records the standing design decisions of EmbCC: what was decided,
why, and where a decision has since been revised or superseded. Each has a
permanent identifier, `D-001` to `D-019`, which source comments and tests
cite; identifiers are never reused or renumbered. The page is for
contributors who need to know why the compiler is shaped as it is before
they change it. A new decision is added at the end with the next free
identifier, its reason, and what would reopen it
([Contributing](contributing.md#recording-decisions-and-facts)).

Each entry gives the decision as it stands today. Where the original
decision was revised, the entry says so in its **Status** line.

| Id | Decision | Status |
|---|---|---|
| [D-001](#d-001) | EmbCC is a separate, parallel project, not OS work | current |
| [D-002](#d-002) | Bootstrap as a C compiler, not a new language | current |
| [D-003](#d-003) | Emit EMBX (native, capability-carrying) and keep ELF for porting | revised; implemented |
| [D-004](#d-004) | Grow backward from a runnable artifact, not forward from a lexer | applied |
| [D-005](#d-005) | Prove on the host first; the target is the final judge | current |
| [D-006](#d-006) | Adoption is earned by capability, not authorship | current |
| [D-007](#d-007) | The kernel stays out of scope | superseded: the kernel is in scope |
| [D-008](#d-008) | Target languages: C, then C++; no language of our own | current |
| [D-009](#d-009) | Own libc | superseded in form: one library, per-OS backends |
| [D-010](#d-010) | Debug info: DWARF as the bridge, native `.embdbg` derived last | current |
| [D-011](#d-011) | A second architecture: aarch64, as a peer backend | current; parts superseded by D-012 |
| [D-012](#d-012) | Each architecture in its own directory | current |
| [D-013](#d-013) | C++: C++20 and libstdc++, lowered through C | current |
| [D-014](#d-014) | Host operating systems as targets: Linux, macOS, Windows | current; partly implemented |
| [D-015](#d-015) | A third architecture: ARMv7-M, the first 32-bit target | current |
| [D-016](#d-016) | RISC-V as two targets and one backend | current |
| [D-017](#d-017) | A configured default target, compiled in | current |
| [D-018](#d-018) | AVR, the first 8-bit target | current |
| [D-019](#d-019) | Running a program is an optional host capability | current |

<a id="d-001"></a>
## D-001: EmbCC is a separate, parallel project, not OS work

**Decision.** EmbCC lives in its own repository and moves on its own
schedule. EmbLinkOS does not depend on it; whether the operating system
uses EmbCC for any build is decided separately, under [D-006](#d-006).

**Why.** A half-built compiler inside the operating system's tree would put
a shippable system at the mercy of an unfinished one. Testing against the
OS does not require living in it, and adopting EmbCC, once it has earned
it, is a one-line change in an EmbBuild manifest.

**Status.** Current. EmbCC can build the EmbLinkOS kernel
([D-007](#d-007)); the separation is now a choice rather than a necessity.

<a id="d-002"></a>
## D-002: Bootstrap as a C compiler, not a new language

**Decision.** EmbCC's first language is C.

**Why.** C can be tested against existing compilers and existing programs
from the first object EmbCC emits, where a new language would have to
invent its own notion of "correct" while being implemented. C can express
a compiler, which the self-hosting loop needs. Starting with C does not
rule out a language of EmbLink's typed values later.

**Status.** Current, and extended by [D-008](#d-008) and [D-013](#d-013).
**Reopens if** ten lines written in the language one wishes existed create
a real need for it, or a concrete program proves painful in C and obvious
in the other language.

<a id="d-003"></a>
## D-003: Emit EMBX (native, capability-carrying) and keep ELF for porting

**Decision.** The compiler emits ELF relocatable objects on every ELF
target. The native executable format of EmbLinkOS, EMBX, is a link output:
`embld --embx --cap NAME` writes an EMBX image with a declared capability
table from the same linked layout it would write as an ELF executable. ELF
executables remain the format for programs ported from elsewhere; the
operating system has a loader for each.

**Why.** An EMBX application is fully linked, with fixed addresses and no
relocations, so only the linker can produce one. The format exists to
carry a declared capability set, and it was designed after the capability
model it carries existed in the operating system, not before. Keeping ELF
for ported software mirrors the operating system's two filesystems (its
own for its data, FAT32 for foreign disks): two loaders, no converter.

**Status.** Revised once. The original decision was to emit ELF only and to
add any native format as an ELF superset; it named a capability model as
the one thing that could justify a native format, and that condition was
met. EMBX output is implemented in EmbLD. Still open: a program declaring
its capabilities in its source, recorded by EmbCC and collected by EmbLD,
instead of `--cap` on the link line.

<a id="d-004"></a>
## D-004: Grow backward from a runnable artifact, not forward from a lexer

**Decision.** The first milestone was an object that links and runs on the
operating system (a program that exits with 42), with a front end only as
large as that needed. The language grew afterward, in increments each
covered by a test.

**Why.** A compiler's invariant is that the machine runs what it emitted.
Starting from the output end validates the target contract (object format,
relocations, calling convention, loader) while it is still cheap to be
wrong about it.

**Status.** Applied. The milestone it ordered,
[M1](status.md#m1), closed on 2026-07-20.

<a id="d-005"></a>
## D-005: Prove on the host first; the target is the final judge

**Decision.** Development is done on the host: EmbCC's output is compared
against reference compilers and tools (`readelf`, `objdump`, another
linker) and run under emulation. Running on the target confirms; it is
not the first check.

**Why.** A host cycle takes seconds and an operating-system boot takes
minutes, under an emulator that runs at a fraction of native speed. The
same bugs are found far sooner on the host.

**Status.** Current. It is the shape of the test suite: golden tests
against references, and QEMU harnesses for every target
([Testing](testing.md)).

<a id="d-006"></a>
## D-006: Adoption is earned by capability, not authorship

**Decision.** EmbLinkOS moves a build from its existing toolchain to EmbCC
only when EmbCC is better for a stated reason: it clears a wall the other
toolchain cannot (C++, thread-local storage, code quality), it serves a
language that fits the operating system, or the operating system
reproducing its own toolchain becomes a goal in itself.

**Why.** The refusal rule applied to the project itself: "we wrote it" is
not a capability.

**Status.** Current. Being able to build something (the kernel, the C
library) is a capability; choosing EmbCC for the operating system's
official build is a separate decision this one governs.

<a id="d-007"></a>
## D-007: The kernel stays out of scope

**Decision as it stands.** The EmbLinkOS kernel is in scope. EmbCC compiles
it, EmbAS assembles its hand-written assembly, EmbLD links it with no
linker script, and the result boots. Kernel design questions (the
debugging contract, syscall numbering, capabilities) remain the operating
system's, not EmbCC's.

**Why.** The original decision kept the kernel out because it needs
freestanding code generation, inline assembly and linker control that a
young compiler should not attempt. Each obstacle was then built: the
freestanding options (`-mno-sse`, `-mno-red-zone`, `-mcmodel=kernel`, with
floating point refused where SSE is off), an inline-assembly assembler
checked against `objdump`, EmbAS (byte-identical to `nasm` on the kernel's
`.asm` files), linker-defined end symbols and a load-address offset in
EmbLD, and an optimizer whose `-O0`, `-O1` and `-O2` kernels all boot. No
kernel source was changed to achieve it.

**Status.** Superseded by its own reopen condition (EmbCC became an
optimizing compiler with proven freestanding support). Whether the
operating system adopts EmbCC for its kernel build is governed by
[D-006](#d-006). `tests/golden/x86_64/embbuild-kernel.sh` builds and boots
the kernel.

<a id="d-008"></a>
## D-008: Target languages: C, then C++; no language of our own

**Decision.** EmbCC's languages are C and C++. A new language built around
EmbLink's typed values is not planned.

**Why.** What the operating system needs is to build the software that
exists, which is C and C++; C++ is also the wall the operating system's
other compiler cannot clear. A new language would multiply every cost:
no existing compiler to test against, a second bootstrap problem, and
programs to rewrite.

**Status.** Current. C++ became concrete in [D-013](#d-013). The gate back
to a new language is still [D-002](#d-002)'s ten-lines test.

<a id="d-009"></a>
## D-009: Own libc

**Decision as it stands.** EmbCC's freestanding and Linux targets link
against EmbCC's own C library, `lib/libc`: portable C above a small
operating-system contract (`lib/libc/os/backend.h`), with one backend per
operating system (`os/emblinkos`, `os/linux`, `os/posixlike`). EmbLinkOS's
former separate library, emlibc, is now the `os/emblinkos` backend, so the
operating system gets the same `printf`, `strtod`, `malloc` and math as
every other target. Hosted macOS and Windows use the platform's own C
library ([D-014](#d-014)). musl and glibc are not used.

**Why.** The ownership loop the project aims at (language, compiler,
library, format, loader, operating system) closes only if the library is
owned too. Most of a C library is independent of the operating system; only
a thin rim of I/O, process, time and entropy calls is not, and EmbLinkOS
already owned that rim. One implementation for every target means one set
of bugs, fixed once.

**Status.** Superseded in form: the decision originally made emlibc, a
separate non-POSIX library in the operating system's tree, the link target.
The library is now one implementation shared by all targets, and emlibc
survives as its EmbLinkOS backend. See [Libraries](../manual/libraries.md).

<a id="d-010"></a>
## D-010: Debug info: DWARF as the bridge, native `.embdbg` derived last

**Decision.** `-g` emits DWARF 4 (`.debug_info`, `.debug_abbrev`,
`.debug_line`). EmbLD carries the DWARF into the linked image, so gdb can
read it, and also writes the native `.embdbg` sidecar, whose layout was
derived from what EmbDBG needs. EmbDBG reads both.

**Why.** DWARF has a real consumer (gdb) on the host from the day it is
emitted, so debug information could be proved on the host
([D-005](#d-005)) before EmbDBG existed. A native format designed before
its producer and consumer existed would have repeated the mistake
[D-003](#d-003) avoided. The linker writes `.embdbg` because only the link
assigns the final addresses.

**Status.** Current. `-g` is refused for Darwin and Windows targets, whose
debug formats (a `__DWARF` segment, CodeView) are not written.

<a id="d-011"></a>
## D-011: A second architecture: aarch64, as a peer backend, not a fork

**Decision.** One `embcc` binary emits for every architecture, chosen at
run time with `--target=`. Only the phases that genuinely differ consult
the target. Code generation records machine-neutral relocation kinds and
the driver maps each (kind, target) to a concrete relocation type, because
the same act (taking a symbol's address) costs one relocation on x86-64 and
two on aarch64. Each backend places arguments itself from their types,
because calling conventions disagree in ways the IR's System V
classification cannot carry.

**Why.** A compiler meant to run on the operating system it compiles for
must choose its target when it is used, not when it is built. Reusing the
System V numbers for AAPCS64 would have miscompiled every call with more
than six arguments, every large composite and every homogeneous
floating-point aggregate.

**Status.** Current. Everything this decision listed as refused at the
time (inline assembly, atomics, `va_start`, HFA arguments, `-g`) has since
been implemented. Its instruction to keep the two backends' code
generation separate until a real algorithm was duplicated was acted on:
the register allocator was lifted into shared code ([D-012](#d-012)).

<a id="d-012"></a>
## D-012: Each architecture in its own directory

**Decision.** Everything that depends on the target machine lives under
`src/arch/`: the shared pieces at the top (`target.c` for selection, the
data model and relocation kinds; `backend.h` for the backend contract;
`code.c` for the machine-code buffer; `predef.c` for choosing the macro
table; `regalloc.c`, the shared register allocator), and one directory per
architecture (`x86_64/`, `aarch64/`, `thumb/`, `riscv/`, `avr/`), each with
its code generator, encoder, share of IR generation, assemblers and
predefined macros. `thumbv8m/`, `riscv32/` and `riscv64/` hold only
generated macro tables. The rest of `src/` never names a machine. Tests
follow the same rule: `tests/golden/` and `tests/exec/` apply to every
target, `tests/golden/<arch>/` and `tests/exec/<arch>/` to one.

**Why.** A target becomes a directory with a known set of files, and what
it lacks becomes a table, instead of something found by searching.

**Status.** Current. The reorganization was a pure move, verified by
byte-identical output on both targets. The shared register allocator
(Chaitin-Briggs colouring, used by all six backends, each through a
`struct ra_target`) was lifted out of the x86-64 backend afterwards, as
[D-011](#d-011) foresaw.

<a id="d-013"></a>
## D-013: C++: C++20 and libstdc++, on both architectures, lowered through C

**Decision.** EmbCC compiles C++20 toward the Itanium C++ ABI, so its
objects link with g++'s. The front end (`src/cxx/`) parses C++ with
semantic analysis interleaved, and lowers it to C that goes through the
existing pipeline: classes become structs laid out by the Itanium rules,
member functions become mangled functions with an explicit `this`,
templates are instantiated by replaying their tokens. What plain C cannot
express (landing pads and unwind tables, the aarch64 result pointer) is a
small internal extension of EmbCC's C. The standard library is GCC's
libstdc++, compiled by EmbCC. The runtime below it (`operator new`, RTTI,
the `__cxa_*` layer, the personality routine) is EmbCC's own `lib/libcxx`;
the reference g++ referees every C++ test.

**Why.** One front end serves every backend from the first line. The C
parser could not be extended in place: its parse-then-check shape cannot
resolve C++'s ambiguities, and C output had to stay byte-identical while
C++ grew. Writing a standard library would be a multi-year project whose
conformance only EmbCC could vouch for; libc++ is not compatible with the
g++-built C++ already on the operating system.

**Status.** Current. The milestones CX1 to CX9 are tracked in
[C++ support](../manual/cxx.md). C++ is tested on x86-64 and aarch64;
C++ exceptions do not work on the 32-bit and 8-bit targets
([Status](status.md#c)).

<a id="d-014"></a>
## D-014: Host operating systems as targets: Linux, macOS, Windows

**Decision.** A target is a triple: architecture, operating system and
object format. EmbLinkOS is an operating system in that set
(`x86_64-emblink`, `aarch64-emblink`, predefining `__emblink__`), not
"no operating system". Two predicates keep apart questions that only
EmbLinkOS answers differently: `target_has_os()` and
`target_is_hosted()` (whether the platform supplies its own C library and
start-up). EMBX is a link output, not a fourth object format
([D-003](#d-003)). A calling convention is a property of the target, so
Microsoft x64 is a second convention on x86-64.

Per operating system:

| | Object format | Convention | C library and link |
|---|---|---|---|
| Linux | ELF | System V, AAPCS64 | EmbCC's `lib/libc` over raw syscalls, static, with EmbCC's compiler runtime and unwinder (`lib/rt`); linked by EmbLD on x86-64, by an external linker on aarch64 |
| macOS | Mach-O | Apple's arm64 convention and data model; System V on x86-64 | the system's; link with the system toolchain |
| Windows (MinGW) | COFF | Microsoft x64 | none yet; link with the system toolchain |

Windows means MinGW first, because it uses the Itanium C++ ABI that
EmbCC's runtime implements; MSVC compatibility is a separate decision not
taken. A capability a triple lacks is an error naming the triple, never a
silent fallback to another platform's behaviour.

**Why.** EmbCC had a second architecture, an optimizer, exceptions and a
C++ front end; the remaining distance to a hosted platform was mostly
format and convention. Linux came first because three of its five pieces
already existed, so a failure there was a failure of the triple refactor
and nothing else. On Linux EmbCC uses its own library rather than glibc
because the library's seam is shaped for a kernel, because it needs no
sysroot, and because it can be built where there is no glibc; a static
image built against the syscall ABI runs on any kernel of that
architecture.

**Status.** Current; partly implemented. Linux runs C and C++ on a real
kernel (`tests/golden/linux.sh`), with threads and TLS; no dynamic linking
or PIE, and a kernel floor of 4.11 (`statx`). macOS runs natively
(`tests/golden/darwin-abi.sh`). On Windows no program has run: Win64 code
is executed only inside an x86-64 test image (`win-abi-run.sh`). The
"system linker" half of the original decision is not implemented: the
driver refuses to link for macOS, Windows and aarch64 Linux and asks for
`-c` and a platform linker. What each hosted target still refuses is listed
in [Status](status.md#targets).

<a id="d-015"></a>
## D-015: A third architecture: ARMv7-M (Cortex-M), and the first 32-bit target

**Decision.** `thumbv7m-none-eabi` (Cortex-M3) and its variants generate
Thumb-2 code with AAPCS32. Because it was the first ILP32 target, the data
model became a table of named questions (`target_ptr_size`,
`target_long_size`, `target_ldouble_size`, `target_char_unsigned`,
`target_wchar_unsigned`, `target_has_int128`, ...) with one row per
architecture, and `long long` became a type distinct from `long`. Objects
are ELF32, built in the 64-bit structures and converted where they are
written. The predefined macros come from clang. EmbLD reads and writes
ELF32 ARM (`SHT_REL` relocations, a `.vectors` section placed first, a
firmware layout with `-Tdata`), so a firmware image needs no other
toolchain. Floating point without an FPU is a call into `lib/rt/softfp.c`
under libgcc's names; 64-bit integers are register pairs in the backend,
not an IR legalization pass. Variants are levels of the same target, not
new ones: `thumbv7em` (Cortex-M4/M7), the hard-float `-eabihf` triples and
`-mfloat-abi=hard` with the FPv4-SP/FPv5-SP single-precision FPU, and
`thumbv8m.main` (Cortex-M33). Objects carry `.ARM.attributes`, and EmbLD
refuses to link objects whose float ABI or enum size disagree.

**Why.** EmbLinkOS is meant to carry embedded tooling. Testing "is the
target aarch64?" had stood in for half a dozen different questions; each now
has a name, and a new target fills in a row rather than inheriting
x86-64's answers. A soft-float runtime that computes binary32 by widening
to binary64 and rounding back gives the same results with one core.

**Status.** Current. Statements in the original record that no longer
hold: there is a code generator (with the shared register allocator), and
atomics, variable-length arrays, `-g`, long double (as a double) and
hard-float are implemented. Still refused: computed `goto`, `__int128`.
ARMv8-M's security extension is not supported.

<a id="d-016"></a>
## D-016: RISC-V, as two targets and one backend

**Decision.** `riscv32-unknown-elf` and `riscv64-unknown-elf` are two
values of `enum target_arch`, because their data models differ (ILP32
against LP64, `__int128` only at RV64), but one backend, `src/arch/riscv/`,
parameterized by `target_xlen()`, because they are one instruction set at
two widths. `riscv32/` and `riscv64/` hold only the generated macro
tables. Code is RV32IMAC or RV64IMAC with the soft-float ABI; the C
extension is applied where instructions become bytes, not by a selector.
Addresses use the `medany` model (`auipc`+`addi`) at both widths, because
`lui` sign-extends bit 31 and cannot reach RAM at `0x80000000` on RV64.
`embld -Tstack ADDR` writes the instructions that set `sp` before the
entry point, so a firmware image needs no assembly. `long double` is IEEE
binary128 and `wchar_t` is signed while `char` is unsigned.

**Why.** The enum keys the data model, and two data models cannot share a
value; two copies of one instruction set's backend would drift.

**Status.** Current. The backend, register allocator, inline assembly,
atomics (A extension) and compressed instructions are implemented. Still
refused: 128-bit values in the backend (so `long double` arithmetic at both
widths and `__int128` at RV64), computed `goto`, hardware floating point
and `-march=`. **Reopens if** RV32 and RV64 come to need different
lowering beyond 64-bit register pairs.

<a id="d-017"></a>
## D-017: A configured default target, compiled in, with every backend still there

**Decision.** `make DEFAULT_TARGET=TRIPLE` builds a compiler whose default
target is `TRIPLE`. `EMBCC_DEFAULT_TARGET` overrides it for one shell, and
`--target=` overrides both. Only the default is compiled in: every backend
is still present and reachable. An unknown default is refused at start-up,
naming where it came from.

**Why.** A cross compiler should not have to be told it is one on every
command line, and an installed compiler should behave the same for every
caller, including a build that scrubs the environment; so the durable
setting is compiled in and the variable is the temporary override, the
order GCC's `./configure --target=` users expect. The variable is not
`EMBCC_TARGET` because the test suite uses that name for the target it is
exercising; `tests/golden/default-target.sh` keeps the two apart. Falling
back to `x86_64-elf` on a bad default would produce objects that link and
a board that does not run.

**Status.** Current.

<a id="d-018"></a>
## D-018: AVR, the first 8-bit target, and what a byte-wide register file changes

**Decision.** `avr` targets the ATmega328P (`__AVR_ATmega328P__`,
avr5) with avr-gcc's documented ABI. `int` and pointers are two bytes, and
`double` and `long double` are four-byte binary32. Because the IR computes
at four bytes, the backend works out how many low bytes each use reads and
computes only those. A local is reachable only through Y with a six-bit
displacement, so Y is the frame pointer; temporaries share frame slots by
live range, and slots are placed densest first. Program memory is a
separate, word-addressed space: function pointers use the program-memory
relocations, and `.rodata` is placed in RAM and copied from flash at
start-up, since no instruction EmbCC emits reads flash. Values live in
runs of registers: the shared allocator assigns call-saved register pairs,
and quads and call-clobbered homes are added by the backend, which
generates each allocation mode and keeps the shortest. Multiply, divide and
floating point are calls into `lib/rt` (`avr.c`, `avr64.c`, and binary32
arithmetic in eight `avrfp*.c` objects so a program links only what it
uses). EmbCC assembles AVR source itself, and supports interrupt handlers
(`signal`, `interrupt`) and inline assembly with AVR's constraint letters.

**Why.** Every scalar here is wider than a register, a frame larger than
63 bytes is out of `ldd` reach, and the part has 2 KB of RAM and 32 KB of
flash, so frame size and code size are budgets rather than preferences.
The calling convention is checked against avr-libc's documented rules
rather than against clang, whose AVR struct convention is not avr-gcc's.

**Status.** Current. The original record's "no register allocator yet" no
longer holds. Still refused: variable-length arrays, label addresses and
computed `goto`, byte swaps, `sqrt`, and atomic accesses wider than one
byte; dense switches are not lowered to jump tables on this target. The
`__flash` qualifier is not supported, and the `progmem` attribute is
ignored with a `-Wattributes` warning. Plain `char` is signed, following
clang; whether avr-gcc's default agrees has not been checked against
avr-gcc.

<a id="d-019"></a>
## D-019: Running a program is an optional host capability

**Decision.** The platform layer gains one optional capability: running
a program and waiting for it (`plat_can_run`, `plat_run_start`,
`plat_run_wait`, `plat_ncpus`). The build chooses the implementation --
`process_spawn.c` (POSIX `posix_spawn`, the default on macOS and Linux)
or `process_none.c` (`PROCESS=none`: EmbLinkOS, and the default with
`PLATFORM=iso`). The driver uses it for one thing: several sources in one
command, each compiled by a run of the driver of its own, `-j N` at a
time. Nothing else may use it, and where it is absent the driver does
what it always did -- one source per command, the second refused with
the way round it.

**Why.** Every Makefile and CMake build hands the compiler several
sources, and refusing them made EmbCC the one compiler a build had to be
rewritten for. Compiling them in one process would mean resetting every
global the front end, the optimizer and the backends keep -- hundreds,
in every stage -- and a reset that missed one would carry one unit's
state into the next silently. A process per source has no such state to
leak, is what GCC's driver does (it runs cc1 per file), and gives
parallel compiles for free. Threads were considered and rejected for the
same reason: the stages are not reentrant, and making them so is a much
larger change than this one. The rule that EmbLinkOS cannot spawn stays
true and stays respected: the assembler and the linker remain libraries
in the driver's process, and a host without the capability loses only
the convenience.

**Status.** Current. `posix_spawn` is the only implementation that runs
anything; Windows (`CreateProcess`) and EmbLinkOS (should it gain a spawn
call -- EmbBuild already runs one program per recipe) would each be a
`process_NAME.c`.

## D-020: ARMv8-M Baseline is ARMv6-M's code generator with a flag

**Decision.** `thumbv8m.base-none-eabi` (the Cortex-M23) is Thumb level 6
-- `target_thumb_arch()` answers 6 and `src/arch/thumb/v6m.c` generates
its code -- with a flag, `target_thumb_v8m_base()`, which the few places
where Baseline differs from ARMv6-M ask: the 32-bit divide and the one-,
two- and four-byte atomics become instructions instead of `librt.a`
calls, the encoding scan (`t_thumb1_ok32`) admits Baseline's 32-bit
encodings, the predefined macros and build attributes are Baseline's,
and the security extension (`-mcmse`) is available. Constants stay in
literal pools and branches keep ARMv6-M's forms, as clang keeps them for
this core.

**Why.** Every other question asked of level 6 -- no IT block, no
unaligned access, r0-r7 only, no FPU, word-aligned arrays -- has the same
answer on Baseline, and a separate level would have had to be added to
each of those checks to say so. A third code generator was not worth
writing for what Baseline adds; ARMv7-M's is Thumb-2 throughout and
emits nothing a Cortex-M23 has a use for that ARMv6-M's lacks.

**Status.** Current. MOVW/MOVT for constants and CBZ for a compare with
zero are instructions the scan admits and the backend does not choose
yet; if code size on Baseline matters, they are the next step.

## D-021: TrustZone-M's Secure side, and veneers in the linker

**Decision.** `-mcmse` compiles for the Secure state of an ARMv8-M part,
as ACLE's CMSE and clang define it: `cmse_nonsecure_entry` functions
return through BXNS with every register and flag that could hold a
secret overwritten, calls through a `cmse_nonsecure_call` pointer save
r4-r11 and clear the rest before BLXNS, and `<arm_cmse.h>` provides the
TT intrinsics and the pointer checks. `embld` makes the secure gateway
veneers from the `__acle_se_` symbols, writes the import library, and
adds a long-branch veneer for a Thumb call to an absolute symbol out of
reach -- the first veneers it makes. The floating-point state is not
handled: `-mcmse` is refused with an FPU.

**Why.** The security extension is useless without the linker half, and
both halves have exactly one correct shape, which clang and GNU ld
define; following them lets a Secure image built by EmbCC serve a
Non-secure one built by either. The long-branch veneer is limited to
absolute targets because that is the case an import library creates and
the only one a pre-layout pass can decide; a general range-extension
pass would need layout to iterate. Refusing the FPU rather than clearing
s0-s15 keeps every Secure image this compiler produces free of floating
point state to leak.

**Status.** Current. `--in-implib` (stable veneer addresses across
releases), the FPU, `cmse_nonsecure_caller()`, and entry functions or
Non-secure calls with arguments on the stack are refused by name; the
last is what clang refuses too.

