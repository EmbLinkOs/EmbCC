# Decisions

*ADR-style: each decision states what was chosen, what was rejected, and **why**,
so the reasoning outlives the memory of the conversation. Decisions made before
any code exists are marked as such — they are commitments, not observations, and
may be revisited by evidence (each records what would reopen it).*

---

## D-001 — EmbCC is a separate, parallel project, not OS work

**Decided:** 2026-07-19 (design). **Status:** firm — the *separation* is
unchanged, though the "EmbLinkOS depends on none of it" half is now a choice
rather than a necessity: EmbCC can build the kernel (see D-007, revised), and
the OS still ships TCC because D-006 governs adoption.

EmbCC lives in its own repository, on its own clock. EmbLinkOS depends on none
of it and continues to use TCC.

**Why.** This is exactly how TCC and EmbBuild entered the system: prove the
thing standalone, adopt only when it is genuinely real. Smuggling a half-built
compiler into the OS would put a shippable, honest system at the mercy of an
unfinished one. Keeping them separate costs nothing — adoption, when earned, is
a one-line change in an EmbBuild manifest.

**Rejected:** developing EmbCC inside the `myos` tree "so it can be tested
easily." Testing against the OS does not require living in it.

**Reopens if:** never, really — but the *adoption* decision is separate and
governed by D-006.

---

## D-002 — Bootstrap as a **C compiler**, not a new language

**Decided:** 2026-07-19 (design). **Status:** firm for M1–M3; open beyond.

The first EmbCC compiles a subset of **C** targeting the EmbLink ABI.

**Why.** Three reasons, in order of weight:
1. **Testability from day one.** The OS already runs C. A C subset can be
   validated against real programs — and against TCC's output — the moment it
   emits an object. A new language would have to invent its own notion of
   "correct" simultaneously with its implementation.
2. **The self-hosting loop needs a language that can express a compiler**, and
   C demonstrably can.
3. **It does not foreclose the interesting future.** A language with EmbLink's
   typed values (records, tables, SQL-nulls — the shell's `sval` model) as
   first-class types is the genuinely differentiated idea, and it stays
   available. It is just not the opening move.

**Rejected:** starting with a novel language. The honest gate for that, recorded
now so it is not forgotten: *write ten lines of the program you wish you could
write, in the syntax you wish existed.* If those ten lines do not create real
desire, it is aesthetics, and aesthetics do not survive a multi-year compiler.

**Reopens if:** the ten-lines test passes convincingly, or a concrete program
turns out to be painful in C and obvious in the other language.

---

## D-003 — Emit **EMBX** (native, capability-carrying) + keep ELF for porting

**Decided:** 2026-07-19 (design). **REVISED 2026-07-24 — the reopen condition
at the bottom triggered.** The original decision (emit ELF, native format only
as an ELF superset) and its reasoning are kept below because the reasoning is
still correct; what changed is the conclusion, and honestly so — see the
revision block. **Realized 2026-07-26:** EmbLD emits EMBX natively (`embld
--embx --cap NAME`) — byte-identical to the reference producer, on the host and
on the OS — so the revised conclusion is not just decided but produced by the
toolchain (ELF stays the porting substrate; the dual loader is unchanged).

EmbCC emits ELF in the exact shape EmbLinkOS's in-kernel loader accepts
(see TARGET_ABI.md). It does **not** invent a container format.

**Why.** ELF is not the bottleneck: the kernel already parses only the subset it
needs, and every tool that produces binaries here (cross-gcc, newlib, TCC) emits
ELF. A from-scratch format imposes a permanent tax — a two-format world, or a
converter on every artifact, forever — in exchange for nothing the OS currently
lacks.

**The insight that resolves it** (and it is the better design): if a native
format is ever wanted, make it an **ELF superset** — plain ELF the existing
loader already reads, *plus* extra sections or `.note` entries a newer loader
understands. Old tools keep working; new capability is additive; no fork, no
converter, no tax.

**The one thing that could justify extensions:** a **declared capability
manifest**. EmbLinkOS is capability-based, but authority currently comes only
from the parent at spawn (file-actions); a binary that *declares* what it needs
would be a real architectural step, and it pairs with EMBKFS's existing signing.
Note the ordering, which is the whole point: **the format is derived from a
capability-declaration model that does not exist yet.** Design the model first;
prototype it as an ELF `.note` or a sidecar file in `/data/apps/<name>/`; only
then consider format work.

*(That ordering is exactly what happened, and it is why this decision was
revised rather than overturned: the capability model landed in EmbLinkOS first,
and only then did the format follow — as **EMBX**, which EmbLD now emits with a
declared capability table. The one piece still outstanding is the program
declaring its own authority **in source**, so EmbCC records it and EmbLD
collects it; today it comes from `--cap` on the link line.)*

**Rejected:** designing an "EmbLink executable format" up front. Designing the
container before deciding what it declares inverts the discipline the OS's own
docs insist on (derive the on-disk shape *last*, from the invariants).

### Revision — 2026-07-24

This decision **predicted its own supersession** and got the ordering right: it
said a declared capability manifest was the one thing that could justify a
native format, and to **build the model first**. That is exactly what happened.

1. **The capability model landed** in EmbLinkOS: a per-process capability set,
   seeded at init and attenuated at spawn, with a spawn-syscall path and a
   first handle-install gate (kernel `capabilities.h`, `sys_getcaps`,
   `SPAWN_ACTION_SET_CAPS`). The model came first, from invariants — the
   discipline this decision insisted on.
2. **Then the format**, EMBX (`myos/docs/EMBX_Specification_v2.md`), byte-exact,
   with a **working in-kernel loader** that enforces the capability check at
   load (§6 step 9). The declaration flows: binary table → step-9 check → the
   process's cap set → a gated handle.

**Honest note on the reopen basis.** This decision's stated reopen was "if the
model *cannot be expressed in ELF notes*." Strictly, it could have been — a
`.note.embx.caps` section would have worked. The reopen is therefore on a
*different* basis than anticipated: a deliberate **ownership** choice, made by
the OS's author, consistent with how EmbLinkOS already runs a dual-format world
for filesystems (EMBKFS native for its own data, FAT32 to read foreign disks).
Executables now mirror that: **EMBX for programs built for EmbLink, ELF kept as
the porting substrate** (foreign source recompiled — the git/CPython/C++ path).
It is a dual *loader*, not a converter. So EmbCC's eventual output is EMBX; ELF
stays for as long as porting does.

**Reopens if:** never for "ELF-only" again. The open question is now the
inverse — when EmbCC gains a relocation model, whether EMBX's `.embdll` linkage
contract (spec §4.2, still deferred) is the right shape.

**Original reopen (kept for the record):** a capability model lands and
genuinely cannot be expressed in ELF notes.

---

## D-004 — Grow **backward from a runnable artifact**, not forward from a lexer

**Decided:** 2026-07-19 (design). **Status:** firm.

M1 is "emit an object that links and *runs on the OS*, exit 42" — with the
frontend as thin as it must be. The C language surface grows afterward.

**Why.** The EmbLinkOS house rule is that a change is not done because it
compiles, but when a test exercises the invariant. For a compiler, the invariant
is *the machine runs what we emitted.* A project that starts with a beautiful
lexer and parser can go months before that is ever true, and every assumption
about the ELF/ABI end stays unvalidated the whole time. Starting at the output
end means the target contract (TARGET_ABI.md) is proven early, when it is cheap
to be wrong.

**Consequence:** early EmbCC will accept an embarrassingly small C subset and
that is correct. Breadth is the easy direction to grow.

---

## D-005 — Prove on the **host** first; the OS is the final judge, not the first

**Decided:** 2026-07-19 (inherited practice). **Status:** firm.

EmbCC development runs host-side (compare against gcc/TCC output, `readelf`,
`objdump`, cross-`ld`); on-OS boots confirm.

**Why.** Inherited from EmbLinkOS and repeatedly vindicated: the TCC header
incompatibility, the static-GOT bug, and the shared-library import gap were each
found on the host in seconds or minutes, where a boot cycle costs minutes and a
QEMU/TCG guest runs at a fraction of wall speed. The rule that emerged:
**reproduce on the host, confirm on the metal.**

---

## D-006 — Adoption is earned by capability, not authorship

**Decided:** 2026-07-19 (design). **Status:** firm.

EmbLinkOS switches from TCC to EmbCC only when EmbCC is *better for a stated
reason* — not because it is ours.

**Why.** THE RULE, applied to ourselves: a claim is only honest if the capability
is genuinely there. "We wrote it" is not a capability. Legitimate reasons would
be: it clears a wall TCC cannot (C++, TLS/`__thread`, codegen quality), it
enables a language that fits the OS's typed model, or the total-loop property
(the system reproducing its own toolchain) becomes a goal in itself.

**Where that stands (2026-09-08).** Two of the three legitimate reasons are now
met by evidence rather than intent: EmbCC clears a wall TCC cannot — it builds
and boots the **kernel**, with freestanding codegen, a full inline-asm
assembler, and an optimizer (D-007, revised) — and the total-loop property is
real, with the self-hosting fixed point holding on the OS over 16 sources.

**This still is not adoption.** Being able to build the kernel is a capability;
choosing EmbCC over TCC for the OS's official build is a separate decision, and
this one governs it. The OS ships TCC until someone has a concrete reason to
switch. What has changed is that the case would now be argued from what the
compiler demonstrably does.

---

## D-007 — The kernel stays out of scope

**Decided:** 2026-07-19 (design). **REVISED 2026-09-08 — the reopen condition
was met.**

**The original decision.** EmbCC targets **userland**. The EmbLinkOS kernel is
built by the cross gcc.

**Why (as written in 2026-07).** "Rebuild-self" in EmbLinkOS has always meant
the userland, stated honestly. The kernel uses freestanding/`-mcmodel=kernel`
codegen, custom linker scripts, and inline asm that a young compiler has no
business attempting. Pretending otherwise would be the exact overclaim the
project's docs exist to prevent.

**Reopens if:** EmbCC ever becomes a serious optimizing compiler with proven
freestanding support — a decision for a much later year.

### The revision

That reopen condition is satisfied, and it was satisfied by building each of the
three named obstacles rather than by lowering the bar:

- **Freestanding codegen** — `-mno-sse`, `-mno-red-zone`, `-mcmodel=kernel` and
  the rest, with a float op in SSE-off mode refused loudly rather than emitted.
- **Inline asm** — a real extended-asm assembler covering the kernel's full
  hardware vocabulary, every encoding byte-verified against objdump.
- **Linker scripts** — not needed: EmbLD auto-provides the end-of-image and
  bracket symbols a script would define, plus higher-half LMA (`p_paddr`).
- **The assembler** — EmbAS assembles the kernel's hand-written `.asm`
  byte-identically to nasm.
- **An optimizer worth the name** — SSA mem2reg, inlining, SCCP, global CSE and
  Chaitin-Briggs register allocation, with `-O0`/`-O1`/`-O2` kernels all booting.

**The result:** all 89 kernel C translation units compile under `embcc`, the 6
`.asm` assemble under `embas`, `embld` links the image, and it boots to the home
desktop behaviourally identical to the gcc build — 193 lines of boot output, 0
faults. **No kernel C was changed to achieve this.**

**What has NOT changed.** The kernel is still *EmbLinkOS's* code, and kernel
design questions (the debugging contract, `CAP_DEBUG`, syscall numbering) remain
out of EmbCC's scope — that half of D-007 stands. The OS's official kernel build
also still uses the cross gcc; being *able* to build the kernel is not adoption,
which D-006 governs. What died is the claim that EmbCC *cannot* and should not
try.

---

## D-008 — Target languages: **C, then C++.** No language of our own is planned

**Decided:** 2026-07-20. **Status:** current intent; C++ is unscheduled.

EmbCC's languages are **C** (the M1–M4 path) and, in the long term, **C++**.
The novel-language direction that VISION.md §4.2 called "the most interesting
long-term direction" — EmbLink's typed values (records, tables, SQL-nulls) as
first-class types — is **not planned**. It is demoted from "interesting future"
to "possible if it ever earns itself"; D-002's ten-lines gate remains the only
door back in, and nobody is expected to walk through it.

**Why.**
1. What the OS actually needs is the ability to build the software that exists,
   and that software is C and C++. The ports story already proved the demand:
   C++/libstdc++ was ported *before* any native compiler work began, and C++
   is the wall TCC will never clear — making it the clearest D-006-legitimate
   capability EmbCC could ever deliver.
2. A novel language multiplies every cost in this repo — testability against
   existing compilers disappears, self-hosting gains a second bootstrap
   problem, and adoption requires rewriting working programs. The payoff was
   always speculative; stating "not planned" is more honest than leaving it
   glowing in the vision docs as an implied someday.

**Order still holds:** C++ comes after the C compiler closes the M4 loop, not
alongside it. It is a frontend-and-sema project of a different size (name
mangling, overloading, templates, EH/unwinding, a C++ runtime against newlib's
C-only world) and it gets its own decision record when it becomes concrete.

**Reopens if:** the ten-lines test (D-002) passes convincingly for the typed
values language — the gate is unchanged, only the expectation is.


---

## D-009 — Own libc: **emlibc**, non-POSIX, EmbLink-shaped

**Decided:** 2026-07-23. **Status:** REALIZED (2026-07-26). emlibc is implemented
(`myos/user/emlibc/`), EmbCC compiles it on the OS **floating point included**
(real `fdlibm` math), and it self-hosts and ships as EMBX — the closed loop the
last paragraph below names, reached: `test emlibc math selfhost` has the OS
build the whole libc with EmbCC + EmbLD and run it. Requirements:
`myos/docs/EMLIBC_Requirements.md`.

EmbCC's link target is **emlibc**, EmbLinkOS's own C library, not
newlib (nor musl/glibc). The requirements doc is the OS-side artifact; EmbCC
consumes its contract, the same relationship it has with the EMBX format spec.

**Why.** The ownership loop D-003 and D-008 point at closes only if the *library*
is owned too: language + compiler + libc + format + loader + OS, one system.
musl/glibc were weighed and declined — both are written against the Linux
syscall ABI and would drag the system toward host-the-world, the pole
EmbLinkOS deliberately sits opposite. The OS's author is "only half okay" with
depending on a ported POSIX libc forever.

**What keeps it tractable** (and why it is not "rewrite a libc"): most of a libc
is OS-agnostic (string/math/malloc/printf number-formatting) and may be **lifted**
from a permissive source — D-006 applied to ourselves, a from-scratch `cosf` is
authorship without capability. POSIX lives only in the thin OS-facing rim
(I/O, process, time, entropy), and EmbLinkOS **already owns that rim**
(`crt0.c`, `syscalls.c`, the errno map). So emlibc is incremental: own the rim,
grow the agnostic bulk header by header, expose the capability-aware surface
newlib cannot (`getcaps`, spawn+file-actions instead of `fork`), and eventually
be **compiled by EmbCC itself** (the closed loop) and shipped as **EMBX**.

**Rejected:** a POSIX-compatible libc (musl/glibc port), because POSIX
compatibility is exactly the thing the OS's non-POSIX model refuses. picolibc
remains a legitimate *interim* upgrade over newlib if raw completeness is wanted
before emlibc exists — but as a stopgap, never the destination.

**Order:** after the C compiler is real (M1–M3) and alongside/after C++ (D-008);
emlibc is not the opening move. It gets its own milestones when it becomes
concrete.

**Reopens if:** the interim (picolibc) proves good enough that owning the libc
never earns itself under D-006 — the same earn-by-capability gate everything
here answers to.

---

## D-010 — Debug info: **DWARF as the bridge, native `.embdbg` derived last**

**Decided:** 2026-07-24. **Status:** landing as predicted. Requirements written
(`docs/tools/embdbg.md`); the OS side carries the byte-exact format AND
the kernel debugging contract (`myos/docs/EMBDBG_Specification.md`) — the
consumer and invariants this decision said `.embdbg` must be derived from.
**Realized 2026-07-26:** EmbCC emits **DWARF line info** (`-g`, the host bridge),
and **EmbLD emits the native `.embdbg`** at link time (`emit_embdbg`) — exactly
the split this decision predicted: the LINKER, not the compiler, produces the
absolute-addressed sidecar, because EmbCC's ET_REL objects carry only
*relocatable* line info (the EMBX finding, one channel over). DWARF stays the
bridge; the `.embdbg` byte layout was still derived from what EmbDBG actually
needs, not ahead of it.

EmbCC's first debug output was **minimal DWARF line info**, because it is
debuggable by tools that already exist (gdb/lldb) on the host the day it lands —
no EmbDBG required. *(2026-09-08: EmbDBG now exists too — `tools/embdbg/` reads
the DWARF back with no gdb in the loop, and the native `.embdbg` was derived
from its real needs, exactly in the order this decision set.)* A **native `.embdbg`** sidecar is the eventual owned form,
but its byte layout is derived **later**, from what EmbDBG (which did not exist
when this was decided) actually needs.

**Why.** This is the DECISIONS D-003 fork again — own-the-stack vs
meet-the-world — and it resolves the same way, for the same reason. A byte-exact `.embdbg` *at the time of this decision* would have been designed
against a producer that emitted nothing (EmbCC put no line/local/type info in its
objects) and a consumer that did not exist.
That is the exact inversion D-003 was reopened *with eyes open about*: derive the
on-disk shape last, from invariants. DWARF line info sidesteps it entirely — its
consumer (gdb) is real, so "prove it on the host first" (D-005) applies to
debugging exactly as it did to codegen. And EmbCC is unusually well-placed to
emit it: it already knows `file:line` at every node (diagnostics use it), every
local lives in a fixed stack slot (one `DW_OP_fbreg`, DWARF's trivial case), and
`rbp` is kept as a frame pointer, so unwinding is a pointer walk with no CFI.

**The dual-form stance, stated so it is not re-litigated:** DWARF is the bridge
for host debugging and stays for as long as that is useful; `.embdbg` is the
native form EmbDBG consumes, mirroring EMBX's ELF-for-porting /
native-for-the-owned-world split (D-003) and EMBKFS's FAT32 / native split. The
EMBX spec already reserved the slot — `EMBX_COMPAT_DEBUG_SIDECAR`, a *compat* bit
(a loader that does not understand debug info ignores it), and `EMBX_F_STRIPPED`
for its absence.

**Rejected:** a byte-exact `.embdbg` format now. Designing the container before
there is a producer or a consumer is D-003's mistake at a smaller scale, and
`EMBDBG_Requirements.md` §6 refuses it explicitly.

**Order:** after M3 — *and that is how it went* (ARCHITECTURE §8 lists DWARF among the deliberate early
non-goals; VISION_LONGTERM gates debug info on a debugger existing to consume
it). The one honest exception is DWARF line info, cheap enough and useful enough
— it would help debug the self-hosting compiler *through* M3 — that it is the
one step plausibly worth pulling earlier.

**Reopens if:** EmbDBG's real needs turn out not to fit a DWARF-derived model,
or the kernel debugging contract (the open question that gates a native
debugger, D-007) lands and dictates a shape.

## D-011 — A second architecture: **aarch64**, as a peer backend, not a fork

**Decided:** 2026-09-10. **Status:** landed for the C EmbLinkOS userland
compiles; the kernel's needs (inline asm, atomics) are open. **2026-09-18:**
inline asm landed (`src/asm/asm_arm64.c`), sized to the vocabulary the ARM
kernel was MEASURED to use rather than to a general assembler; the ARM
kernel's C went from 60 to 120 of 131 files compiling, and what remains is
front-end work both targets share (`__atomic_*` above all) plus `va_start`.
**2026-09-18, later:** the atomics, `va_start` / `va_arg`, HFA arguments and
the corrected large-composite rule landed; **all 131 C files of the ARM
kernel compile**, and `make test-arm64` passes in full.

EmbLinkOS is two architectures now. `myos/docs/ARM64.md` closed its A0–A9
campaign: the whole shared kernel links and runs on aarch64 under QEMU `virt`,
four cores, the real syscall table, the full 52-program userland — built by
`aarch64-elf-gcc`. A compiler whose whole reason to exist is that the OS
should not need someone else's toolchain cannot answer "except on ARM".

**The target is chosen at run time by `--target=`, not at build time.** One
`embcc` binary emits for both machines. The alternative — two binaries, or a
compile-time `#ifdef` — would have made the on-OS build (M4) pick an
architecture when it was *built* rather than when it is *used*, which is
exactly backwards for a compiler that is meant to be hosted on the OS it
compiles for.

**Only the phases that genuinely differ know about the target.** The lexer,
parser and the bulk of sema are machine-neutral and stay that way; the seam is
`src/target/` (a small enum plus a machine-neutral relocation *kind*), a second
`src/asm/emit_arm64.c` and a second `src/codegen/codegen_arm64.c`. The
relocation indirection is the part that had to be invented rather than copied:
taking a symbol's address costs ONE relocation on x86-64 (a RIP-relative `lea`)
and TWO on aarch64 (`adrp`/`add`), so codegen records a kind and the driver
turns (kind, target) into an ELF type. Without that seam the driver would have
had to know which machine it was writing for at every relocation site.

**The AAPCS64 argument classification is recomputed in the backend, not read
from the IR.** irgen fills `ir_arg` with the *SysV* classification, and the two
ABIs disagree — eight integer argument registers against six; a composite
larger than 16 bytes passed as a POINTER to a copy the caller makes (stage
B.3) rather than SysV's by-value MEMORY class; a struct of one to four
same-typed floats (a Homogeneous Floating-point Aggregate) in consecutive v
registers, whatever its size; no back-filling once a register file is spent.
Reusing the SysV numbers would have been a silent miscompile of every call
with more than six arguments. The IR keeps carrying the SysV fields for the
x86 backend and now also each argument's TYPE (`ir_arg.ty`), from which one
classifier, `a64_place`, places a call's arguments and a function's
parameters alike — so the two sides cannot disagree.

*(Corrected 2026-09-18: this paragraph first said large composites went "by
value on the stack", and the backend did exactly that — consistent with
itself, wrong against gcc, and invisible to every test in which EmbCC called
EmbCC. The cross-ABI test against gcc, blocked until HFAs existed, is what
settles it; it and tests/golden/cross-varargs.sh now pass on both targets.)*

**The naive backend was rebuilt, not shared.** `codegen_arm64.c` starts where
`codegen.c` started: every vreg in a stack slot, every operation through one
accumulator. Its slot coalescing, RAX residency cache and Chaitin-Briggs
allocator are all x86-shaped in their current form, and D-005's "prove it
first" applies to a second backend as much as it did to the first. What the
two DO share is the IR, the optimizer, the ELF writer and the driver — which
is the split that matters.

**What is refused loudly rather than emitted wrong** (THE RULE): `-g`, whose
DWARF describes x86 frame offsets. (Inline asm, `va_start`, the atomics and
HFA arguments were on this list at decision time and have since landed; see
the status notes above.)

**Reopen if:** the two backends start duplicating real algorithms — a register
allocator written twice is the signal that the shared layer is in the wrong
place, and the answer then is to lift the machine-independent half out of
`codegen.c`, deriving the shared shape from two WORKING backends rather than
inventing it from one (the discipline `myos` ARM64.md §2.3 used for the HAL).

## D-012 — Each architecture in its own directory

**Decided (2026-09-18).** Everything that depends on the target machine lives
under `src/arch/`: the shared pieces at its top (`target.c` for selection and
relocation kinds, `backend.h` for the contract a backend implements,
`code.c` for the machine-code buffer, `predef.c` for choosing the macro
table), then one directory per architecture — `x86_64/` and `aarch64/` — each
holding its backend (`codegen.c`), encoder (`emit.c`), its share of IR
generation (`irgen.c`: `va_arg` and extended asm), and its predefined macros;
x86-64 also its file-scope asm and EmbAS, aarch64 its inline-asm assembler.
The rest of `src/` never names a machine. The tests follow the same rule:
`tests/golden/` and `tests/exec/` run for every target, `tests/golden/<arch>/`
and `tests/exec/<arch>/` for one. What each target supports is one document,
docs/language/compatibility.md.

**Why.** Two machines had grown into the tree by accretion — `codegen.c` next
to `codegen_arm64.c`, `emit.c` next to `emit_arm64.c`, both targets' `va_arg`
and inline asm inside `irgen.c` — so "what does aarch64 have?" was a grep,
and adding a third target would have meant finding every such seam again. Now
a target is a directory with a known set of files, and its gaps are a table.

**How it was done, and what proves it.** Pure moves (`git mv`, so history
follows) plus one split: the target-specific parts of `irgen.c` moved to
`src/arch/<arch>/irgen.c`, reaching irgen's helpers through the internal
header `src/ir/irgen_int.h`, and the byte buffer both encoders used moved out
of the x86 encoder into `src/arch/code.c`. No generated code changed:
`tools/x86-identity.sh` found all 748 x86-64 compiles byte-identical to the
previous tree, and the same comparison for aarch64 (every exec test at `-O0`
and `-O2`, and the whole ARM kernel) found 293 of 293 identical.

**Not done, on purpose:** the backends still share no code generation, per
D-011's reopen condition — the layout makes that sharing a later, visible
step (a `src/arch/` file both use) rather than a precondition.

## D-013 — C++: **C++20 and libstdc++, on both architectures, lowered through C**

**Decided (2026-09-18).** D-008's "C, then C++" becomes concrete. The
target is **C++20** with **libstdc++** as the standard library — GNU's, the
one EmbLinkOS already runs through a ported g++ — on **x86-64 and aarch64**
alike, with the **Itanium C++ ABI** on both (what g++ uses, so EmbCC objects
link with g++-built C++ and with libstdc++ itself). It must work over newlib
first and over emlibc after, and it must eventually run on the OS: C++
compiled on EmbLinkOS is the capability TCC can never deliver (myos
PORTS.md).

**Architecture: a C++ front-end that lowers to C** — the shape EDG's
C-generating back end proved can carry full ISO C++, and cfront before it.
`src/cxx/` parses C++ with semantic analysis interleaved (C++ cannot be
parsed without knowing which names are types and templates), and emits C:
classes become structs laid out by the Itanium rules, member functions
mangled free functions with an explicit `this`, references pointers,
constructors/destructors/cleanups explicit calls on every scope exit, virtual
calls vtable loads, templates instantiated by replaying their tokens with the
parameters bound. That C goes through the existing pipeline — C sema, IR,
optimizer, both backends, DWARF (through `#` line markers, so diagnostics and
debug info point into the `.cc`). One front-end, both machines, from the
first line.

What plain C cannot say becomes a small internal extension of EmbCC's own C
(we own both sides): the aarch64 `x8` result pointer for classes returned in
memory, and — for exceptions — calls with landing pads and the unwind tables
(`.eh_frame` CFI, the LSDA) the Itanium personality routine reads.

**Library strategy.** libstdc++ is not rewritten; EmbCC grows until it
compiles it. Until then, EmbCC-compiled C++ links against a g++-built
libstdc++ (tools/build-ref-gxx.sh builds the reference g++ and a hosted
libstdc++ against the harness's newlib, per target) — which is also how the
ABI is proven: every C++ test is built by EmbCC and by the reference g++ and
must agree, and cross-ABI tests link halves from each. The runtime pieces that
touch the OS (operator new, `__cxa_atexit`, guards, the unwinder) come from
libsupc++/libgcc first and are owned later where emlibc needs it.

**Milestones** (docs/language/cpp-levels.md): CX1 C++ as a better C (namespaces, classes,
ctors/dtors, overloading and mangling, new/delete, static initialization);
CX2 operators, conversions, copy/move; CX3 inheritance, virtual functions,
RTTI; CX4 templates; CX5 exceptions; CX6 the modern core (auto, lambdas,
constexpr, range-for, ...); CX7 C++20 (concepts, `<=>`, consteval,
coroutines); CX8 libstdc++ compiled by EmbCC; CX9 C++ on the OS, over emlibc.
Each is proven by running programs on both targets under QEMU, agreeing with
g++.

**Rejected:** extending the C parser in place (its parse-then-check shape
cannot resolve C++'s type/expression ambiguities, and C must stay
byte-identical while C++ grows); writing our own standard library (a
complete one is a multi-year authoring project, and conformance would be ours
alone to prove); libc++ (not binary-compatible with the g++-built C++ already
on the OS).


---

## D-014 — **Host operating systems as targets**: Linux, macOS, Windows

**Decided:** 2026-09-21 (design; no code yet). **Status:** the ADR the
vision's first non-goal named. Supersedes that non-goal.

`docs/design/vision.md` Non-goals §1 said it plainly: *"EmbCC does not aim
to produce Mach-O or PE/COFF binaries for macOS/Windows **until an ADR says
otherwise**. Running on those hosts is a goal; targeting them is not."*
This is that ADR. Targeting them is now a goal, and the aim is the whole
thing — objects that link, programs that run, the C and C++ standard
libraries working — not format coverage.

**The reason the non-goal existed is gone.** It was written to keep the
vision achievable while the compiler did not yet have a second
architecture, an optimizer, exceptions, or a C++ front end. It has all of
those, aarch64 proved the target seam works (D-011), and the remaining
distance to a hosted platform is mostly *format and convention*, not
*compiler*.

### It is five pieces, not one

Saying "support macOS" hides the fact that an OS target is five
independent things, and their costs differ by an order of magnitude:

| | Linux | macOS | Windows |
|---|---|---|---|
| object format | ELF — **have** | Mach-O | PE/COFF |
| calling convention | SysV — **have** | SysV / AAPCS64 — **have*** | **Microsoft x64** |
| C++ exceptions | Itanium + DWARF — **have** | Itanium + DWARF — **have** | SEH, or DWARF via MinGW |
| triple and predefines | — | — | — |
| libc, startup, linking | glibc, crt1, `ld` | libSystem, `ld64` | UCRT or MinGW, `lld-link` |

\* Apple's arm64 varargs differ from AAPCS64; a real but contained quirk.

Linux is most of the way there because three of its five rows already
exist. Windows shares none of them.

**The order is Linux, then macOS, then Windows**, and it is chosen by what
each one *teaches*. Linux exercises the new triple machinery against an
object format and an ABI that already work, so a failure there is a
failure of the refactor and nothing else. macOS adds exactly one new
thing, a second object format. Windows adds three at once — a format, a
calling convention, and an unwinder — and is the only one that can be
attempted with two of them already proven.

### EmbLinkOS is one of the operating systems, and the first draft forgot it

*(Added 2026-09-21, the same day, after the omission was caught in
review.)* The table above lists Linux, macOS and Windows, and the first
implementation of the triple put EmbLinkOS in `TGT_OS_NONE` alongside
bare metal. That is wrong, and wrong in the direction that matters: the
OS this compiler exists for is the **primary product target** (vision
§5.2 names `x86_64-emblink`), it has syscalls, a libc and a process to
start, and "no operating system" describes none of that.

`x86_64-emblink` and `aarch64-emblink` are triples in their own right,
and code built for the OS can ask `__emblink__` rather than infer the
platform from the *absence* of `__linux__`. Nothing in the OS tree read
such a macro before — the platform was selected by which `backend.c` the
Makefile compiled — so this names it for the first time rather than
matching an existing convention.

**It forces a distinction that would otherwise have stayed hidden.**
"Has an operating system" and "the platform owns the toolchain" are not
the same question, and only EmbLinkOS separates them. On Linux the right
answer is glibc's headers, `crt1.o` and `ld`, because they are there and
they are what every other program on the machine links against. On
EmbLinkOS the right answer is `lib/libc` over `os/emblinkos/backend.c`
and EmbLD, because those **are** the platform's — we wrote them (D-009).
A single `target_is_hosted()` meaning "has an OS" would have sent the
primary product target looking for a glibc that does not exist. So there
are two predicates, `target_has_os()` and `target_is_hosted()`, and
EmbLinkOS is the only target that answers them differently.

### EMBX is not a fourth object format

`enum target_fmt` has ELF, Mach-O and COFF and deliberately no EMBX,
which looks like the same omission and is not. EMBX is a **link**
output: `embld --embx` writes a native, capability-carrying image
instead of an ELF executable (D-003), and the objects that go into it
are ELF like any others. The format dimension is the container an
*object* goes in, so EMBX would be a category error there — and worse,
it would tell the compiler it had a fourth object writer to build when
it has three.

### The architectural consequence: the target becomes a triple

Today `enum target_arch` has two values and **no operating-system
dimension at all**, and `target_elf_machine()` assumes the object format.
That is the foundation this rests on and it is built first, before any
platform work: a target becomes **architecture × OS × object format**,
with `--target=` parsing the full triple.

What does *not* change is D-011's rule, which this extends rather than
revises: only the phases that genuinely differ may know. The lexer,
parser and the bulk of sema stay machine- and OS-neutral. The relocation
seam already generalises — codegen records a machine-neutral
`enum reloc_kind` and the driver maps (kind, target) to a concrete type —
and that mapping becomes (kind, arch, format).

### Microsoft x64 is a third calling convention on an architecture that
### already has one

This is the part with no precedent in the tree. aarch64's AAPCS64 arrived
*with* a new backend, so "one architecture, one convention" held. Win64 is
a different convention on x86-64: four argument registers rather than six,
32 bytes of shadow space the caller reserves, every composite larger than
eight bytes passed by reference, and a different varargs shape. The
classification cannot live in `irgen` keyed on architecture, as the SysV
numbers do now (D-011 already had to recompute AAPCS64 in the backend for
the same reason). It becomes a property of the *target*, asked for by
name.

### MinGW before MSVC, and possibly instead of it

C++ exceptions are the decisive cost on Windows. Our runtime implements
the **Itanium** C++ ABI — `__cxa_*`, `__gxx_personality_v0`, DWARF
unwinding — which is native on Linux and macOS alike. MSVC's scheme shares
nothing with it: different mangling, different EH tables, different
personality. MinGW-w64 uses the Itanium ABI and DWARF or SEH unwinding, so
`x86_64-windows-gnu` reuses the C++ runtime that exists while
`x86_64-windows-msvc` requires a second one.

So Windows means **MinGW first**. MSVC compatibility is a separate
decision, deliberately not taken here.

### The system's libc, not ours — and D-009 stands

D-009 chose to own a libc because EmbLinkOS is not POSIX and nobody else's
fits it. A hosted target is the opposite case: the platform's libc is
already there, already correct, and already what every other program on
the machine links against. Hosted targets use **the system's** headers,
startup objects and libc. `lib/libc` remains what it is — the freestanding
library for EmbLinkOS and bare metal — and neither replaces the other.

### The system's linker, not EmbLD, to begin with

EmbLD emits ET_EXEC ELF and EMBX. Teaching it Mach-O and PE is a second
linker project, and it is not on the path to a running program: `ld`,
`ld64` and `lld-link` are present on the machines these targets run on.
The driver invokes the system linker for hosted targets, as gcc and clang
do. Owning the link for hosted platforms is a later decision, made against
a working toolchain rather than instead of one.

### What is refused loudly rather than emitted wrong (THE RULE)

Every capability absent for a given triple is an error naming the triple,
never a silent fallback to another platform's behaviour. Specifically, at
decision time: a Windows target refuses C++ exceptions until SEH or the
MinGW DWARF path lands; any hosted target refuses `-g` until its debug
format is proven against the platform's own debugger; and a triple whose
object writer does not exist is refused by the driver rather than falling
back to ELF.

**Rejected:**

- **Emitting only object files and calling it done.** Valid `.o` files
  that nobody can link into a running program prove the writer and nothing
  else. The gate is a program that runs on the platform.
- **A separate binary per platform.** Same reasoning as D-011: one `embcc`
  chooses its target at run time, or the on-OS build picks a platform when
  it is built rather than when it is used.
- **Doing Windows first** because it is the most different. The most
  different target is the worst place to debug a new triple abstraction,
  because every failure has three possible causes.
- **MSVC ABI compatibility as part of this decision.** It is a second C++
  runtime, and bundling it here would make a large decision unfalsifiable.
- **Retiring `lib/libc`.** It is for the targets that have no libc. That
  is still most of them.

**Reopens if:** the triple abstraction starts leaking into the front end —
if the parser or sema acquire OS knowledge, the seam is in the wrong
place, and the answer is to move it rather than to spread it. Also if
MinGW's Itanium EH turns out not to work on Windows in practice, since
that assumption is what makes Windows C++ affordable at all.
