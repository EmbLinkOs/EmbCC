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

*(Paths, 2026-09-22: every file this record names moved when **D-012** put
each architecture in its own directory, three days after this was decided.
`src/target/` → `src/arch/target.c`, `src/asm/emit_arm64.c` →
`src/arch/aarch64/emit.c`, `src/codegen/codegen_arm64.c` →
`src/arch/aarch64/codegen.c`, `src/asm/asm_arm64.c` →
`src/arch/aarch64/asm.c`, and `codegen.c` → `src/arch/x86_64/codegen.c`.
The names are left as they were written — an ADR records what was decided,
not where the code sits today — but a reader following them to the tree
would find nothing, so the mapping is here.)*

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

*(That step was taken on 2026-09-22, and the reopen condition is what
justified it. `src/arch/regalloc.c` is the register allocator, lifted
out of `src/arch/x86_64/codegen.c` unchanged: real backward liveness, a
precise interference graph, Chaitin-Briggs simplify ordering and
colouring with move-coalescing preferences. A machine now supplies a
`struct ra_target` — which registers may be handed out and in what
order, which survive a call, whether a narrow load is a plain move —
and nothing else.*

*The discipline was D-012's own: a pure move, verified by comparing
EMITTED BYTES rather than test results. `tools/x86-identity.sh` found
808 objects byte-identical and none different, which says nothing
changed at all, where a green suite would only say nothing it covers
changed.*

*Derived from a WORKING backend, as D-011 asked, rather than invented
for a second one: the allocator had been in service on x86-64 for a
while before it was made shareable, so the shape is one that already
earned its keep.)*

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

**Decided:** 2026-09-21 (design). **Status:** implemented, 2026-09-21/22
— all three object writers exist and two of the three targets run.
Supersedes the vision's first non-goal, which this ADR was written to
replace.

| | object format | ABI | runs |
|---|---|---|---|
| Linux | ELF — **done** | System V — **done** | **yes**, on a real kernel |
| macOS | Mach-O — **done** | SysV / AAPCS64 + Apple varargs — **done** | **yes**, native |
| Windows | COFF — **done** | Microsoft x64 — **done** | **no**: nothing has executed |

What each still refuses, by name rather than by guessing: `-g` on both
new targets, C++ exceptions and `__thread` and constructors on Windows,
`__thread` on Darwin, and `__int128`/`long double` in a Windows
signature. Darwin's typed `catch` is broken (todo.md). And the Linux
target has no compiler runtime and no unwinder, so `__int128` and C++
exceptions do not LINK there — see the amendment below, which states
what "self-sufficient" was and was not checked to mean.

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
| libc, startup, linking | ~~glibc, crt1, `ld`~~ → **ours, static — have** | libSystem, `ld64` — **have** | UCRT or MinGW, `lld-link` |

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

### Amended: on Linux it is OUR libc, static, straight onto the kernel

*(Added 2026-09-21, when the Linux target was built. The paragraph above
is what was decided before writing it; this is what writing it changed,
and the two are left side by side because the reasoning matters more
than the conclusion.)*

Linux does not use glibc. `lib/libc/os/linux/backend.c` issues syscalls,
`lib/libc/os/linux/start.c` is the entry point, and the result is a
**static** image containing our printf, our malloc and our strtod, with
no glibc, no musl, no dynamic loader and no crt from anyone else. `nm -u`
on it prints nothing — *for the programs it was tried on. That sentence
was written after checking `hello.c`, and it is not a property of the
target; see the second amendment below, which is about the programs where
it is false.*

Four things pushed it there:

1. **The seam is shaped for a kernel.** `lib/libc/os/backend.h` asks for
   write, read, sbrk, a clock and exit. Those are syscalls. A backend
   that forwarded `__os_write` to glibc's `write` would put our stdio
   buffering on top of theirs and give the image two errno variables and
   two heaps — and the seam exists precisely so that does not happen.
2. **The "sit on the host libc" backend already exists.** That is what
   `os/posixlike` is. Writing a second one of those for Linux would have
   added a target and no capability; writing the other kind added the
   first backend to reach a mainstream kernel directly, which is the
   thing EmbLinkOS's backend could not prove generalises.
3. **It removes the sysroot from the problem.** No distro headers, no
   glibc symbol versioning, no version skew between the machine that
   builds and the machine that runs. A static image built against a
   syscall ABI runs on any kernel of that architecture.
4. **It can be built where there is no glibc**, which is the machine this
   was written on. The glibc path could not have been compiled at all
   here, let alone checked.

What it costs, stated plainly: no dynamic linking or PIE; `statx` puts a
floor of Linux 4.11 under the filesystem group; and the clocks enter the
kernel because nothing reads the vDSO yet. *(Threads were on this list —
"`clone` needs a per-architecture assembly entry, and
`__os_thread_create` returns ENOSYS rather than shipping one written
blind" — and landed on 2026-09-21, with TLS: `lib/libc/os/linux/thread.c`
and `tls.c`. The entry stubs were not written blind in the end; they are
assembled by a real assembler and read back as bytes.)*

This does **not** foreclose a glibc-hosted Linux mode. Because the
difference is one file under `os/`, adding one later is a backend, not a
redesign — and `os/posixlike` is most of it already.

The catch this creates is that every number in that file belongs to the
kernel and none of them can be checked against a header here. Two things
answer it, and both are in `tests/golden/linux.sh`: on Linux it compiles
our `syscall.h` together with the kernel's own headers and makes the
compiler assert that every pair agrees, and everywhere else
`tests/harness/linux/run.sh` boots a real kernel under QEMU and runs the
image as PID 1. The second one is not a convenience. A hand-written
syscall stub would implement the same numbers this backend calls and
agree with them by construction; the first thing a real kernel said was
that `O_DIRECTORY` is `0200000` on x86-64 and `040000` on aarch64, which
the one hardcoded value had silently got wrong on one of the two.

### Amended again: "self-sufficient" was measured on the wrong program

*(Added 2026-09-22.)* The claim above — that the Linux image needs
nothing — was checked against `hello.c` and generalised. It does not
generalise. Two libraries are missing, and both of them are libraries the
platform supplies everywhere else, which is exactly why their absence was
invisible:

- **The compiler runtime.** `__multi3`, `__ashlti3`, `__lshrti3`,
  `__mulxc3`, `__muldc3`, `__powidf2` — the routines a backend calls for
  operations the machine has no instruction for. `__int128` multiply or
  divide, and complex multiplication, emit a call to one of these.
- **The unwinder.** `_Unwind_RaiseException` and its family, plus the
  `.eh_frame_hdr` section and `dl_iterate_phdr` used to find tables. C++
  `throw` emits a call to one of these.

On macOS these come from the system (`libSystem`, `libgcc_s`); on
EmbLinkOS from the ported toolchain. The Linux target, by deciding to
link against nothing, decided to link against these too — without
noticing, because nothing in the test suite compiled `__int128` division
or a `throw` *for Linux*.

**Decided: scope the claim down, do not ship the libraries yet.** Writing
a compiler runtime is a day's work and writing an unwinder is not; a
DWARF CFI interpreter that gets a corner wrong does not fail visibly, it
unwinds into the wrong frame. Neither belongs in the same change as the
target that revealed them. So:

1. **The claim is narrowed wherever it appears** — here, in the target
   matrix, in `--version` and in the roadmap. Linux runs C. It does not
   run `__int128` multiply/divide or C++ exceptions.
2. **The failure is made legible.** It surfaces at the link as an
   undefined symbol, which names a routine nobody has heard of.
   `missing_runtime_note()` in `src/link/link.c` recognises both families
   by name and prints what the routine is, why it is missing and which
   targets have it. A gap you can read is a different thing from a gap
   that looks like a linker bug.
3. **Shipping them is its own work**, in this order: the integer runtime
   first (small, testable against gcc's libgcc output value by value),
   the unwinder second and only against a real differential oracle.

*(Both done, 2026-09-22, and the ordering held. `lib/rt` is the
compiler runtime -- a separate archive from libc, because libc
implements what a program asks for by name and nothing in a program
ever writes `__multi3` -- and `lib/rt/unwind.c` is the unwinder. The
Linux targets now link every routine their backends can emit, and
`embcc prog.cc -o prog` compiles, links and runs a C++ program that
throws, with no flags.*

*The unwinder's oracle is the part worth recording, because getting one
took a detour. libgcc's unwinder cannot simply be linked into a static
image of ours: it finds its tables through a `__register_frame_info`
that a crtbegin normally calls, and ours does not, so it links and then
finds nothing. But the bare-metal harness already runs C++ exceptions
on libgcc's unwinder, because `tests/harness/crt.c` registers the
tables by hand -- which is what a bare-metal image has to do. So one
source is built twice, freestanding and Linux, with the same compiler,
the same C++ runtime and the same libc; the unwinder is the only
difference, and the two print 78 identical lines over ten exception
cases.*

*Refused rather than approximated, in the same spirit as the rest:
`DW_CFA_def_cfa_expression` and its siblings, which gcc emits where the
CFA is not a register plus a constant. An unwinder that guesses at a
rule it cannot evaluate jumps to an address it invented.)*

*(Done, 2026-09-22, for the first of the two. `lib/rt` is the compiler
runtime, a separate archive from libc because it is a different job --
libc implements what a program asks for by name and nothing in a program
ever writes `__multi3`. The driver puts `librt.a` on the link line after
`libc.a` the way it already found `crt1.o`, so `embcc prog.c -o prog`
links `__int128` multiply and divide with no flags.*

*What it holds: the 128-bit integer operations and the three shifts; the
conversions between 128-bit integers and float and double; the complex
multiply and divide that C99 Annex G requires to be library routines;
and, on x86-64 only, the same two at x87 width. The constraint that
shapes all of it is that none of these routines may use the operation it
implements -- `__multi3` cannot multiply two `__int128`s -- so a 128-bit
value is only ever split and rejoined through a union and everything
between is 64-bit arithmetic.*

*What it does NOT hold, and why: the aarch64 `long double` family
(`__addtf3`, `__multc3`, `__fixtfti` and neighbours). There `long
double` is IEEE binary128 with no instruction behind it, so this is a
soft-float implementation rather than a file, and a soft-float that gets
a corner wrong fails in the last bit where nothing casual sees it. Same
judgement as the unwinder, at a smaller scale. x86-64 Linux now links
every runtime routine its backend can emit; aarch64 Linux links all of
them except those seven.*

*Checked in `tests/golden/rt.sh` against two oracles: the integer half
byte-identical to the host's own runtime over 4343 lines, and the
complex half against gcc's libgcc -- byte-identical for multiply on all
10683 lines including every one of the 2401 combinations of zero,
infinity and NaN, and for division within one ulp, which is all C
requires once the special values (also identical) are right. Writing
that test settled a question worth recording: clang's compiler-rt uses a
different algorithm and differs from libgcc on 605 of the same lines, so
"agrees with a compiler" is not one property but several, and the one
worth having is agreement with the implementation whose NAMES are being
used.)*

The general lesson, recorded because it has now happened twice in this
ADR: *a self-sufficiency claim is only as strong as the program it was
measured on.* `hello.c` exercises none of the paths that call into a
runtime library, and a target that has never compiled `__int128` cannot
discover that it has no `__multi3`.

### The system's linker, not EmbLD, to begin with

EmbLD emits ET_EXEC ELF and EMBX. Teaching it Mach-O and PE is a second
linker project, and it is not on the path to a running program: `ld`,
`ld64` and `lld-link` are present on the machines these targets run on.
The driver invokes the system linker for hosted targets, as gcc and clang
do. Owning the link for hosted platforms is a later decision, made against
a working toolchain rather than instead of one.

*(Amended 2026-09-21: Linux is the exception, and it is the exception for
a reason that does not extend to the other two. EmbLD already reads and
writes x86-64 ELF — that is its native format, not a port — so linking
the Linux target cost nothing, and it is what makes an image with no
glibc under it possible at all: a system `ld` would have wanted a
sysroot. macOS and Windows still use `ld64` and `lld-link`.)*

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

## D-015 — A third architecture: **ARMv7-M (Cortex-M)**, and the first 32-bit target

**Decided:** 2026-09-25. **Status:** the front end, the backend, the
register allocator and inline assembly are in — the triple, the data
model, the predefined macros, ELF32 objects, and Thumb-2 code for the
32-bit scalar language, at 3.7x clang's size. What it cannot lower it
refuses by name.

**2026-09-25, later:** the backend landed for the 32-bit scalar subset.
`src/arch/thumb/emit.c` encodes Thumb-2 and `src/arch/thumb/codegen.c`
lowers to it, naively — every vreg in a stack slot, every operation
through r12 — which is where both other backends started and what D-005's
"prove it first" asks of a third. An object is ELF32 now: the ELF writer
builds every object in the 64-bit structures and converts at the one
place that serialises them, because a second set threaded through the
writer would be a second set of places to get a field order wrong.

Three things about this target had to be found rather than assumed, and
each is the kind that links cleanly and faults at run time:

  * a Thumb function symbol's `st_value` carries BIT 0 SET, which is not
    part of the address — it tells `blx` which instruction set to switch
    to, and an object that leaves it clear branches into ARM state on the
    first indirect call;
  * `$t` mapping symbols say where Thumb code begins, and a consumer that
    finds none disassembles the section as ARM;
  * `e_flags` must carry EF_ARM_EABI_VER5, which is 0 on an object that
    forgot it.

And one thing about the IR: **`fn->outgoing_bytes` is the SysV answer.**
SysV has six integer argument registers and AAPCS32 has four, so a
six-argument call reserves nothing there and needs eight bytes. The
Thumb backend computes its own outgoing area; believing the IR's number
compiles, links, and writes the fifth argument over the first local.

Making the front end ILP32 turned up three more places where 8 meant
"pointer" and now says so: the address arithmetic in irgen, the walking
pointers strength reduction creates, and a call's result width — which
is the RETURN REGISTER's width, not the type's, and that register is four
bytes here.

**2026-09-25, later still:** the toolchain closed. `embld` reads and
writes ELF32 ARM, so a firmware image is built end to end by EmbCC's own
compiler and linker — no `arm-none-eabi-*` anywhere, and no assembler,
because a Cortex-M fetches its initial SP and PC from the vector table
in hardware and its startup is therefore ordinary C.

Three things the linker had to learn. `.vectors` is an output section of
its own placed AHEAD of `.text`, since the processor does not look the
table up but reads address zero. `-Tdata` is a FIRMWARE layout, where
the writable segment is addressed in RAM and stored after the text in
flash — which `lma_offset` could not express, because that shifts every
segment by one constant and here the two differ; the linker then
provides `__data_load`/`__data_start`/`__data_end`/`__bss_start` so the
startup needs no linker script to stay in step with. And `-Ttext 0` is a
real request: zero is where flash begins, so "0 means the default" made
the one base this target needs the one it could not ask for.

The fourth is the one that mattered: **the ARM EABI specifies `SHT_REL`**,
so every object a real ARM toolchain produces carries `.rel.text` and
not `.rela.text`. A linker that skips those relocates nothing, links
without complaint, and produces an image that does nothing at all —
which is exactly what happened to the first clang-built reference. With
it, the implicit addend: the field of an unresolved `bl` is a branch to
ITSELF (`f7ff fffe`, displacement −4), because the displacement is
measured from P+4 and the ABI's addend from P. Taking that −4 literally
puts every call one halfword early.

`tests/golden/thumb-exec.sh` is now the real test: EmbCC compiles,
`embld` links, QEMU's Cortex-M3 runs, and the output must equal what
clang produces for the same source through the same linker on the same
board — at -O0, -O1, -O2 and -Os.

**2026-09-25, later again:** 64-bit integers landed, in the BACKEND
rather than as a legalisation pass over the IR. The IR has no carry:
expressing `adds`/`adcs` in EmbIR would take a compare and a branch per
addition, and adding carry-carrying opcodes would put two operations
into the shared operand switches that only one target ever emits, which
is how an opcode rots. A 64-bit value is an eight-byte slot and a
register pair; divide and remainder are the only calls, into
`lib/rt/int64.c` under libgcc's names. Bitfields work as a consequence,
since irgen assembles a field's storage unit in a 64-bit accumulator.

Two things in the SHARED front end were wrong and had been invisible
while every register was 64 bits. `arith_common` returned
`ty_base(TY_LONG, uns)` for the wide case, which is a silent NARROWING
where `long` is four bytes: `a + b` on two long longs came out as a
32-bit add. And the two merge MOVs a `?:` emits carried no width at all,
so `neg ? -q : q` returned half of a long long — which is how
`__divdi3` came back carrying its own dividend's high word. The backend
also propagates width through copies to a fixpoint, because a MOV is
not required to carry one and several do not.

The other lesson is about the reference. For 64-bit arithmetic it is the
HOST compiler, not clang for thumbv7m: `long long` has one answer
whatever the register width, and clang emits `__aeabi_ldivmod` for a
divide where EmbCC emits `__divdi3` — a routine that returns quotient
and remainder in four registers at once and therefore cannot be written
in C, so the two cannot share a runtime.

**2026-09-25, floating point:** `float` and `double` work, and the
results are BIT-IDENTICAL to hardware. ARMv7-M has no FPU, so every
operation is a call into `lib/rt/softfp.c` under libgcc's names, and
AAPCS's soft-float variant passes the operands in the core registers —
which means the IR's existing integer paths already carry the value and
only the arithmetic needed lowering.

Only binary64 is implemented. A binary32 operation widens both operands,
does it in binary64, and rounds back, which gives the SAME answer as
computing in binary32 directly because 53 significand bits is at least
2p+2 for p = 24 (Figueroa). One core, half the code, and no double
rounding to reason about.

Three bugs, and what each says:

  * the backend's wide map skipped every `flt` instruction — a leftover
    from when floats were refused — so a double-returning call got a
    four-byte slot and the next temporary landed on its high word.
    `__addsf3` added the wrong numbers while every routine it called was
    exact.
  * irgen converts an `unsigned int` to floating point by asking for a
    SIGNED 64-bit conversion, on the grounds that "a 32-bit operation
    zero-extends its result into the eight-byte slot". True of a
    register write on both other targets; false of a four-byte stack
    slot, where the next four bytes are another temporary.
  * `__extendsfdf2` derived a denormal's exponent from SIGBIT and got
    -94 where the answer is -126 and nothing else.

The reference is the host again, and the comparison is of BIT PATTERNS:
a result that prints the same to fifteen digits can still be a rounding
off. The core is additionally swept against 400,000 random bit patterns
on the host, which is where the denormals, the huge exponents and the
near-cancellations that no hand-written list contains actually live.
NaN payloads are compared by CLASS rather than bit-for-bit, because
which payload and which sign a NaN carries out of an operation is
unspecified and x86 and ARM already disagree.

**2026-09-26, aggregates:** structs pass and return by value, and the
check is against another TOOLCHAIN rather than against the host: the
caller and the callee are compiled by different compilers, in both
directions, and linked together, so a disagreement about which register
a composite starts in shows up as a wrong number rather than as nothing.

AAPCS32 differs from AAPCS64 in ways the IR's SysV classification does
not carry, so the backend places arguments itself. A composite of four
bytes or fewer returns in r0 and a larger one through a hidden pointer
in r0 — which shifts the real arguments to r1. A composite may be SPLIT
across r3 and the stack; an eight-byte SCALAR may not, because its
alignment rounds the register number up to even first and that leaves
two registers or none. And the base standard has no
homogeneous-aggregate rule at all: a struct of floats is an ordinary
composite, where AAPCS64 would put it in v registers.

The bug worth recording: the callee decided it had a hidden result
pointer from the return's SIZE, so every function returning a `long
long` treated r0 as a buffer address and read its first parameter out of
r1. Only a COMPOSITE returns in memory; eight bytes of scalar come back
in r0:r1.

**2026-09-26, varargs:** `printf`-shaped functions work. AAPCS32 passes
a variadic argument exactly as it passes a named one, so a `va_list` is
a bare pointer at the next one — the Darwin shape, and the opposite of
SysV's and AAPCS64's, which point AT a record. The prologue's half is
that a variadic function pushes r0-r3 immediately BELOW the caller's
stack arguments, so a single pointer walks from the registers into them
and `va_start` is one address computation.

Two consequences. `va_copy` is a pointer assignment: copying the 24
bytes SysV's tag needs would copy the ARGUMENTS, and both lists would
then walk a snapshot. And a variadic function returns through `lr`
rather than popping into `pc`, because the save area sits above the
saved registers and has to come off first.

The cross-compiled pairing covers these too: the caller and the callee
are built by different compilers and the register save area has to line
up with where the other one left the arguments.

**2026-09-26, the register allocator — LANDED, on the second attempt.**
ARMv7-M is 3.7x clang now rather than 5.4x, and RISC-V, which went
through the same sequence first, reached 1.7x at both widths.

The first attempt is still on `thumb-regalloc-wip` and still fails
`thumb-float` at -O2. What made the difference was not persistence but
ORDER: `ra_parallel_move` was written and proven over 3910 shapes BEFORE
anything used it, and the three sites below then had one routine to call
instead of three open-codings of the same ordering. Two of the four
hazards were fixed in shared code and never had to be found twice.

The hazards, as the first attempt recorded them, with what each turned
out to be:

  * **The call's argument setup is a PARALLEL MOVE.** Loading r0-r3 in
    order destroys a later argument whose value happens to live in an
    earlier one's destination — `unpack(d2u(y), &b)` was handed a
    pointer that y's own low word had overwritten.
  * **So is the prologue's.** `round_pack(int sign, int exp, u64 sig)`
    had `sign` allocated to r2, and `mov r2, r0` at the top of the
    function destroyed the low half of `sig`, which arrives in r2:r3.
  * **An indirect call's target must be read BEFORE the arguments**, and
    into a register that is not one of them.
  * **A copy's two ends must agree about their WIDTH.** A `?:` whose
    arms are an eight-byte value and a four-byte constant had the merge
    copying eight bytes out of a slot the constant never used, because
    the constant had been given a register instead. The width map has to
    propagate both ways through IR_MOV and IR_SELECT, and the backend
    has to decide 64-bitness from that map rather than from `i->w`.

The first three were one problem wearing three hats, and the second
attempt started by writing the parallel move — which is what made it a
morning's work rather than a session's. The THIRD turned out to be
solved in the shared layer all along: `ra_allocate` pins an indirect
call's target to memory itself, because the clobber set is not visible
to it. The FOURTH needed one change, and in one direction rather than
two: a copy is as wide as it SAYS, so `mov.4s` from an eight-byte value
takes its low word and does not make its destination wide. A width-less
copy still propagates.

Two hazards the first attempt did NOT record, both found the second
time:

  * **A soft-float helper is not an `IR_CALL`.** `call_int_arg_in_reg`
    keeps a real call's arguments in memory, but `__ltdf2(a, b)` has no
    IR_CALL, so its operands are ordinary values the allocator puts in
    registers — and the setup then does `mov r0, r1` and loses b before
    reading it. On a target with no FPU that is EVERY float operation,
    and it is almost certainly what failed `thumb-float` on the branch.
  * **The frame moves when the allocator takes a callee-saved
    register.** They go in the same `push` as the fixed four — a mask
    costs no extra instruction — so sp drops four bytes further per
    register, and `base`, which locates the caller's stack arguments,
    has to know. A stack-arriving parameter that the allocator gave a
    register also has to be LOADED into it; `udivmod64(u64, u64, u64 *,
    u64 *)` spends r0-r3 on its first two arguments, so `q` arrives on
    the stack.

**2026-09-26, inline assembly.** `asm()` works here now, with the CMSIS
core set: mrs/msr over all fourteen special registers, cpsid/cpsie, the
barriers, the wait hints, ldrex/strex, and the arithmetic a
hand-written sequence mixes in. Thirty instructions went into emit.c
first so thumbcheck round-trips them, and tests/golden/thumb-asm.sh
compares the whole vocabulary against llvm-mc — which caught `adds`
mapped onto `add` with the flag bit clear, an instruction that
assembles, runs, and takes the wrong branch.

**What it still does not do, all refused by name:** atomics, VLAs,
computed goto, exceptions and `-g`.


EmbLinkOS is meant to carry embedded tooling, and a compiler for embedded
systems that stops at 64-bit application cores is not one. `thumbv7m-none-eabi`
is the Cortex-M line — M3, M4, M7 — which executes Thumb-2 and nothing else.
It is the first target here with no operating system underneath it by
construction: there is no `thumbv7m-linux` row to add later, and no hosted
spelling of this target that would mean anything.

**The interesting part is not the third backend, it is the first ILP32
target.** x86-64 and aarch64 are both LP64 and their data models differ in
exactly two places — whether plain `char` is signed and which 16-byte format
`long double` uses. So `target_get() == TARGET_AARCH64` had become a
serviceable stand-in for half a dozen different questions, asked at 35 sites
across the lexer, sema, the C++ front end and the debug writer. Every one of
those would have taken the x86-64 answer for a target that is not aarch64,
silently, and most of them would have been wrong.

They are now separate questions with names: `target_ptr_size`,
`target_long_size`, `target_ldouble_size`, `target_char_unsigned`,
`target_wchar_unsigned`, `target_has_int128`, each a column of one table in
`src/arch/target.c`. A fourth architecture is a row, and the compiler will not
build until every column of it is filled in — which is the property the
`== TARGET_AARCH64` test did not have.

**`long long` became a type of its own.** It had been folded into `TY_LONG`,
which cost nothing while every target was LP64 and they were the same width.
On ILP32 they are four bytes and eight. The spelling now survives into the
type (`type.is_llong`), the three integer-literal paths in the lexer promote
past a 32-bit `long` when the magnitude needs it, and `ty_int_of_size()` is
how a caller asks for "the integer type eight bytes wide" instead of assuming
that means `long`.

`ty_equal()` deliberately still lets `long` and `long long` interchange where
they are the same width. Making them distinct everywhere is correct C and a
separate change with its own fallout; mixing it into the one that adds a
32-bit target would have put a pile of new diagnostics between a real
regression and a bisect. It IS enforced where ignoring it is unsound — when
the two spellings are different widths, as they are here.

**The predefined-macro table comes from clang, not gcc.** `tools/gen-predef.sh`
has always taken a target's table from a production compiler's own `-dM -E`
rather than deriving it by hand (ARCHITECTURE.md §5), and that discipline is
what matters, not which compiler. clang carries every target in one binary
where `arm-none-eabi-gcc` is a separate toolchain download; the generated
file's header records which one it was, and `EMBCC_REF_GCC_THUMB` switches it
to a real cross gcc. `__clang__` and `__llvm__` joined the exclusion list for
the same reason `__GNUC__` was already on it.

**What is refused loudly rather than emitted wrong** (THE RULE): the whole
back end. `--target=thumbv7m-none-eabi -c` says there is no code generator and
stops, because handing the unit to the x86-64 backend on the grounds that it
is "not aarch64" would write an object full of x86 instructions under an
EM_ARM header — the exact failure a default case exists to prevent. `__int128`
is refused by name in the parser, since a 32-bit target has no register pair
to carry one and libgcc's 32-bit multilib has none of the `__*ti3` routines.

**What the backend will have to face that neither existing one did:** a Thumb
16-bit data-processing instruction always sets the flags. `and r0, r1` is four
bytes; `ands r0, r1` is two. So on this target small code and flag liveness
are the same problem, and the plan is to emit the 32-bit `.w` forms first —
uniform, flag-preserving, correct — then narrow where the flags are provably
dead and the registers are low, measuring against `clang -target
thumbv7m-none-eabi -Os` the way the other two are measured against gcc.

**Reopen if:** a fourth data model appears that the table cannot express —
a target where `int` is not 4 bytes, or one with a 16-bit `char` — at which
point the columns are the wrong shape and the sizes belong in the table
wholesale rather than as exceptions to a fixed set.

---

## D-016 — RISC-V, as **two targets and one backend**

**Decided:** 2026-09-26. **Status:** done at both widths — the triples,
the data models, the predefined macros, the `long double` format, a code
generator, the register allocator, inline assembly, and images that run
under QEMU. 1.7x clang's code size at both widths. What it cannot lower
it refuses by name.

RV32 and RV64 are two entries in `enum target_arch` and not one entry with
a width knob beside it, because the enum keys the DATA MODEL and the two
data models genuinely differ: RV32 is ILP32, RV64 is LP64, `__int128`
exists only at the wider one. A single value could not answer
`target_ptr_size()` for both, and every place that asks would have to ask
something else as well.

The instruction set does not differ nearly as much, so when the backend
lands it will be ONE directory (`src/arch/riscv/`) parameterised by
`target_xlen()`, not two. That is the opposite of D-012's per-architecture
split and deliberately so: D-012 separates ISAs, and these two are the
same ISA at two widths — `add` and `addw` differ by a bit, the register
file and the calling convention are the same shape, and two copies would
drift. `src/arch/riscv32/` and `src/arch/riscv64/` hold only what really
is per-target: the generated macro tables.

**Why the macro tables are generated twice** rather than once with
`__riscv_xlen` patched: the widths disagree about far more than the
pointer. The `int_fast*` types, the lock-free atomic set and
`__SIZEOF_INT128__` all move, and a generated file has no business being
hand-edited into a parameterised one (ARCHITECTURE.md §5). They come from
clang, as ARMv7-M's does and for the same reason — clang carries every
target in one binary — and with an explicit `-march=rv32im -mabi=ilp32`.
The `-march` is not a detail: the default `rv32imafdc` would define
`__riscv_flen` and claim a hardware FPU this compiler cannot emit for
(THE RULE). One macro is filtered out on top of that, `__riscv_v_intrinsic`,
which clang defines for plain `rv32im` with no `__riscv_v` beside it — the
version of a vector intrinsics API for a vector unit that is not in the
`-march`.

Two front-end facts this target settled:

  * **`wchar_t` is SIGNED here and `char` is unsigned.** That combination
    is why those are two columns in `src/arch/target.c` and not one; every
    target before this one had them agree.
  * **`long double` is IEEE binary128, and x87 is the exception.**
    `ldf_target_fmt()` used to name the targets that were ordinary and let
    x86-64 fall through; it now asks for the odd one out. Written the old
    way, both RISC-V widths would have silently folded `long double` in the
    80-bit x87 format while advertising `__LDBL_MANT_DIG__ 113`.

`-dumpmachine` was a chain of ternaries — a second list of targets to keep
in step with the real one, and it had already fallen behind: it printed
`x86_64-elf` for ARMv7-M. It reads `target_triple_now()` now, so the table
that parses a triple is the table that prints it.

**2026-09-26, later:** the backend landed, for both widths at once, and
one directory was the right call — `src/arch/riscv/` is a single
`codegen.c` reading `target_xlen()`, and the width shows up in three
places rather than everywhere: a `long long` is a register pair at RV32
and a register at RV64, a 32-bit operation at RV64 must use the `w`
instruction forms to keep its result sign-extended, and `ld`/`sd`/`lwu`
do not exist at RV32.

Running everything TWICE is what the suite is for, not a doubling of it.
Three of the bugs below passed at one width and failed at the other.

**The one that decided the code model.** `lui` SIGN-EXTENDS bit 31, so
the absolute `lui`+`addi` pair cannot name an RV64 address between
0x80000000 and 0xffffffff7fffffff — and a firmware image lives at
0x80000000, which is where QEMU's `virt` board and most RISC-V hardware
put RAM. Every global's address came out sign-extended and the first
store through one faulted. The answer is PC-relative `auipc`+`addi` at
BOTH widths (what `-mcmodel=medany` gives), which has no hole and is
position-independent besides; clang needs the same flag to build the
reference. The low half's relocation names the AUIPC rather than the
target, because the two halves must agree about the +0x800 rounding and
only the high one saw the whole displacement.

**The stack pointer, which C cannot write.** A Cortex-M fetches its
initial sp from the first word of the image, which is why the ARMv7-M
harness is pure C. RISC-V has nothing equivalent — every register is
zero at reset — so `embld -Tstack ADDR` emits the four instructions that
set sp and jump to the entry, from the same encoder the compiler uses.
That is the software half of what the other target gets in hardware, and
it keeps a firmware image buildable by this toolchain alone.

Four more, each of which compiled, linked and ran:

  * **`target_xlen()` returned BYTES.** XLEN is the ISA manual's name for
    the register width in BITS; the backend divided by 8 to get bytes,
    got 1, and made every value one byte wide.
  * **`place_arg` numbers argument registers 0..7, and a0 is x10.** On
    ARM the index and the register number coincide; here the index was
    used directly, so a function read its second parameter out of `ra`.
  * **the far-offset scratch was chosen, not reserved.** For a frame
    deeper than 2047 bytes the address register was "whichever scratch is
    not the one being moved", which picked B_LO while loading B_HI — so
    reading the high half of a register pair destroyed the low half. Only
    functions with enough locals hit it, so every small test passed.
  * **irgen converts an `unsigned int` by asking for a SIGNED 64-bit
    conversion**, relying on "a 32-bit operation zero-extends its result
    into the eight-byte slot". True of a register write on x86-64 and
    aarch64; false at RV32, where the slot is four bytes, and false at
    RV64, where a slot load sign-extends. `(float)(unsigned)k` came back
    as a constant 4.7e18 whatever k was. The Thumb backend had already
    found and written this down; the note is what made it a ten-minute
    fix rather than an hour.

Two things outside the backend turned out to be wrong for everyone, not
just for RISC-V:

  * **`__data_load` was 0 unless `-Tdata` was given.** It is a real symbol
    in every link, and the startup that copies `.data` from it — the same
    startup a firmware build uses — read from address 0. It is now equal
    to `__data_start` when the two are the same place, which makes the
    copy a correct no-op.
  * **`lib/rt/softfp.c` was guarded on POINTER WIDTH.** RV64 is a 64-bit
    machine with no FPU under `-march=rv64im`, and the guard compiled the
    whole file away. It now asks whether the target has hardware floating
    point, which is the actual question.

**Still to check:** `va_copy` special-cases the targets whose `va_list` is
a bare pointer, and RISC-V is now among them. Darwin's arm64 has a
bare-pointer `va_list` too (`irg_va_arg_darwin`) and is NOT in that list,
so it appears to copy 32 bytes of arguments where it should copy a
pointer. Nothing here exercises that target, so it is written down rather
than changed blind.

**2026-09-26, later still: the register allocator, and inline assembly.**
Both widths reached 1.7x clang, from 5.4x and 7.1x. The sequence that
worked, and the order matters more than any of the steps:

  1. `ra_parallel_move` in the shared layer, PROVEN over 3910 shapes
     before anything used it — because the three sites that need it
     (a call's arguments, a prologue's parameters, an indirect call's
     target) had produced three separate bugs when each open-coded the
     ordering on ARMv7-M.
  2. The allocator on with all three capability flags at 0, so calls,
     returns and memcpy still read from slots. Correctness first.
  3. The straight-line operations converted to compute in the allocated
     register rather than through a scratch.
  4. The flags on, one at a time, full matrix each.

The three bugs on the way, each found by a technique the ARMv7-M
write-up had recorded rather than by guessing: a soft-float helper's
arguments are a parallel move the IR cannot see (there is no IR_CALL, so
nothing marks its operands); a copy is as wide as it SAYS; and a
variadic function's named parameters come from the spill area, not from
their argument registers.

And one that was mine alone: **`wide` means two different things.** To
the backend it is "needs a register PAIR"; to `ra_allocate` it is "too
large for any register". Identical at RV32 and OPPOSITE at RV64, where
passing the same map marked every pointer ineligible — 294 memory
operations against RV32's 41 for the same source. The map had once been
built only at RV32, which made the conflation harmless; the guard came
off so the float conversions could use it, and the comment asserting
"at RV64 that map is empty" was true when written and false by the time
it mattered.

Inline assembly landed with it: the CSR instructions, the fences and the
system instructions, all encoded through `emit.c` so compiled code and
inline asm cannot disagree, and all compared against llvm-mc. That
comparison caught a bare `fence` emitted as `fence rw, rw` — which
orders memory but not device I/O, so a barrier written around an MMIO
register would not have ordered it — and four CSRs that exist only at
RV32. The vocabulary is derived from the privileged ISA rather than
MEASURED from a corpus, as aarch64's was from the ARM kernel's 67
templates; `asm.h` says so, and says to prune it when RISC-V code exists
here to measure.

**Reopen if:** the backend turns out to want separate directories after
all — most likely if RV32 and RV64 end up needing different lowering for
64-bit integers, which is where the widths stop being the same machine.
It has not happened: the register-pair code is still the only RV32-only
part, and the allocator's one width-dependent line (whether `wide`
reaches `ra_allocate`) is marked as such.


## D-017 — A **configured default target**, compiled in, with every backend still there

**Decided:** 2026-09-26. **Status:** done.
`make DEFAULT_TARGET=riscv32-unknown-elf` builds an EmbCC that compiles
for that machine when the command line names none;
`EMBCC_DEFAULT_TARGET` overrides it for one shell and `--target=`
overrides both.

D-014 made every machine reachable from one binary, which was the right
shape and left one thing unsolved: a person whose work is one board still
typed `--target=` on every line, and a Makefile that inherited no
environment still compiled for x86-64. A cross toolchain that has to be
asked, every time, to be a cross toolchain is not one.

**Why a build-time knob and not only an environment variable.** An
installed compiler should behave the same for everyone who runs it —
including cron, including a build server, including a Makefile that
scrubs the environment. An environment variable alone puts the target in
the *caller's* hands, which is exactly where a cross build should not
keep it. So the compiled-in value is the durable one and the variable is
the temporary override, which is also the order GCC users already expect
from `./configure --target=`.

**What is deliberately NOT copied from GCC.** There, a cross build is a
different binary that can only emit for one machine. Here only the
DEFAULT string is compiled in; the binary still contains x86-64,
aarch64, ARMv7-M and both RISC-V widths, and `--target=` reaches all of
them. A cross-configured install is the same compiler with a different
starting point — which is what makes it safe to configure one, and is
D-014's property preserved rather than traded away.

**Why the variable is `EMBCC_DEFAULT_TARGET` and not `EMBCC_TARGET`.**
The short name was taken: `tests/lib.sh` uses it for which target the
SUITE is exercising, and `tests/run.sh` exports it around every golden
test. A driver that honoured it would have retargeted, under
`make test-arm64`, every test that relies on the default — and the
failure would have looked like a miscompile rather than a naming
mistake. Two unrelated settings behind one name is a bug with a delay
fuse; `tests/golden/default-target.sh` asserts the separation so nobody
tidies the two names into one.

**A bad default is refused at startup**, by name, saying which of the two
places it came from (THE RULE). Falling back to `x86_64-elf` would be the
worst possible behaviour here: the objects link, and then the board does
not run.

## D-018 — AVR, the first **8-bit** target, and what a byte-wide register file changes

**Decided:** 2026-09-27. **Status:** the triple, the data model, the
predefined macros, the instruction encoder, a code generator, an assembler,
interrupt handlers, inline `__asm__`, 64-bit integers, varargs, aggregates
by value, and software binary32 — all of it running on an ATmega328P under
QEMU and judged against the host. What it cannot lower it still refuses by
name; what remains is `__flash`/PROGMEM and a byte swap. No register
allocator yet, so every value lives in a frame slot.

AVR is first in the EmbLinkRTOS requirements document's target order
(ATmega328P, the Nano profile), and it is the first target here that is not
a flat 32- or 64-bit register machine. Four things about it are unlike
every other target, and each cost a design decision rather than a port.

**Nothing fits in a register.** An `int` is two registers and a `long` is
four, so every operation is a carry chain over bytes. On the other four
targets a scalar fits and the awkward case was the one that did not (a
`long long` on ARMv7-M); here the awkward case is everything. The IR's
width class is only ever 4, 8 or 16 — there is no `w == 2` — so a two-byte
`int` arrives as a value already extended to four, and computing at four
bytes is the CORRECT reading rather than a shortcut. Narrowing it would
take a width analysis over the whole function.

**There is no sp-relative addressing.** Memory is reachable only through
X, Y or Z, and only Y and Z take a displacement — of six bits. So a frame
pointer is not an optimisation, it is the only way to name a local, and a
frame past 63 bytes needs a computed pointer for its upper reaches. That
is why `layout()` here produces offsets from Y starting at 1, and why the
slot helpers have a near and a far path at all.

**Four bytes per temporary does not fit in 2 KB.** This is the decision
that separates this target from the others in practice. The test program's
`run` wanted 1910 bytes of frame for its temporaries once the inliner had
been through it, against the part's 2048 bytes of SRAM, and the symptom was
not a diagnostic — it was a program that printed nothing, because the frame
ran off the bottom of RAM into the register file. Temporaries therefore
SHARE slots by live range, which at -O0 is almost all of them: each
subexpression gets its own vreg and nearly every one dies at the next
instruction. 1296 bytes down to 304 at -O0, 1910 down to 418 at -O2.

The interval is [definition, last use] in instruction order, STRETCHED over
any loop it touches, to a fixpoint. The stretching is not conservatism for
its own sake: without it a value defined inside a loop and used after it
shares with one defined earlier in the same loop — disjoint in the listing,
but the back edge re-executes the earlier definition. At -O2 that is not a
corner case, because mem2reg turns every promotable local into a vreg.

**Program space is a separate address space, addressed in words.** Two
consequences, and neither faults when it is wrong.

  * A function pointer holds half a byte address. `&f` therefore goes
    through the `_GS` relocations and a function pointer in data is
    `R_AVR_16_PM`; using the data forms would produce a pointer to twice as
    far into flash, landing on a real instruction.
  * `.rodata` cannot stay in flash. `ld`/`lds` reach only the data space, so
    a string literal left there is not slow to read — it is unreadable by
    any instruction this compiler emits. On a Harvard target it joins the
    WRITABLE segment and gets a RAM address with a flash load address, and
    the startup copies it across with `lpm`, which is what avr-gcc's linker
    script does and why `const char *s = "hi"` costs RAM there.

**Why no register allocator yet.** The shared Chaitin-Briggs allocator
hands out single registers. AVR needs RUNS of one, two or four consecutive
registers, even-aligned for `movw` and `adiw`, out of a pool whose halves
are not interchangeable — the immediate instructions (`ldi`, `subi`,
`andi`, `cpi`) reach only r16-r31, and `adiw` only four pairs. Teaching the
shared layer that is a change to code five working targets depend on, and
it is not the first thing to do on a machine that had never run an
instruction from this compiler. D-005 applies to a fifth backend as it did
to the second.

**Why multiply and divide are refusals rather than inline sequences.**
AVR's `mul` is 8x8 into r1:r0 and DESTROYS r1, the machine's zero register
that every other lowering reads. A 32-bit product is ten partial products
with a carry chain threaded through them — about seventy instructions, 140
bytes of flash at every site on a part that has 32 KB. Every AVR toolchain
calls a helper instead (libgcc's `__mulsi3`, `__udivmodsi4`), so this is
`lib/rt` work and a helper-call convention, not an instruction selection.

Multiply by a CONSTANT is implemented, and had to be: at -O0 an array index
scales by its element size with an `IR_MUL`, so refusing it would refuse
`tab[1].x`. Double-and-add from the top bit down — one shift for a power of
two. That also required the backend to work constants out for itself rather
than trusting `imm_b`, which is the optimizer's answer and absent at -O0.

**Two things this target settled elsewhere:**

  * **A call's scalar return type is now in the IR** (`ret_tybytes`,
    `ret_tysign`). AVR's return value is a run of byte registers sized by
    the type, and the ABI leaves everything above it undefined, so the
    CALLER must extend — which avr-gcc's callers also do. The IR kept the
    return type only for structs, so `signed char sc(void)` yielded an
    `ext.4:2s` reading a byte the callee never wrote. The same gap is
    latent on x86-64, where convention masks it.
  * **`elf32` is `target_ptr_size() <= 4`, not `== 4`.** An AVR pointer is
    two bytes and its objects are ELFCLASS32 like every other small
    machine's.

**The referee, and the hole in it.** The instruction encoder is checked
against llvm-mc form by form, as the RISC-V and VFP vocabularies are, and
for a stronger reason: AVR's operand fields are SPLIT, so a mistake encodes
a DIFFERENT VALID instruction rather than an invalid one. It found two bugs
hand-checking had passed (the `adiw`/`sbiw` `KKdd KKKK` layout, and `ld rd,
Y`/`Z` being the displaced form at q=0 where nibble 0 of the 0x9000 group
is `lds`).

It then missed a third, and the reason is worth recording. The conditional
branches were not in the vocabulary, because llvm-mc leaves an
`R_AVR_7_PCREL` relocation even on a branch to a label in its own section —
its bytes are a placeholder, so a byte comparison would grade nothing.
`enum avr_cond` numbered its flags in mnemonic order rather than by SREG
bit, so `breq` tested CARRY and `brlt` tested overflow. Every encoding was
self-consistent, which is all a comparison against itself can establish.
`while (*s)` walked past its NUL and printed 1700 bytes of RAM.

So those forms are refereed in the OTHER direction: `llvm-mc -disassemble`
decodes our bytes and the text is compared against what each form was meant
to be. The general rule this leaves behind is that a referee must grade the
MEANING of an operand and not only its packing — and that an encoder
function absent from the vocabulary is unchecked however many forms the
vocabulary reports.

**Floating point, and the three things it broke elsewhere.** `float` and
`double` are BOTH four-byte binary32 on this target — avr-gcc's documented
default — so there is no binary64 anywhere and `lib/rt/softfp.c` cannot be
reused: that file implements binary32 by widening to binary64, doing the work
there and rounding back, which is exact (53 >= 2p+2 for p = 24) and needs a
type this machine does not have. binary32 is therefore implemented natively,
in `lib/rt/avrfp*.c`.

Nothing in it is wider than 32 bits, and that is a size decision rather than a
style one. The first version carried significands in `unsigned long long`,
which reads better — a 24×24 product is 48 bits and wants a type that holds it
— and came to 51 KB of text on a part with 32768 bytes of flash, because every
64-bit operation in this backend is a byte-at-a-time chain through frame slots.
`round_pack` alone was 10.7 KB and `addsub` 14.7 KB. Rewritten at 32 bits they
are 6.5 KB and 8.2 KB. The one value that genuinely does not fit is the
multiply's product, carried as two 32-bit words split at bit 24.

It still does not fit as one object, so it is EIGHT: `avrfp.c` holds only
unpack and round, and add, multiply, divide, compare, and each direction of
each integer width is its own object. The boundaries were put where -O0 put
them — `addsub` left the core when its image was 9 KB over, and the 64-bit
conversions split by direction when the two together came to 19 KB — because
-O0 is the level someone debugging builds at. `tests/golden/avr-float.sh` is
one image per object, so a dependency a group should not have fails its link by
name; that is how the multiply's call to `__mulsi3` surfaced.

**`-Os` was not a size mode.** It differed from `-O2` only by turning off
vectorization (x86-64 only) and unrolling, and this file is what showed the
cost: 41392 bytes against `-O1`'s 32856. `pack` and `mul24` had been copied
into every caller and deleted, `addsub` growing 6600 → 9850 to hold them. The
inliner's budget is 24 IR instructions, which is a proxy for BYTES — and an IR
op is one or two instructions on x86-64 and six to ten here, where an `int` add
is four and every value lives in a frame slot. So the same budget that admits a
one-liner on a 64-bit machine admits a 450-byte function on an 8-bit one.
`-Os` now has its own budget (`INLINE_SIZE_CALLEE`), and keeps
`INLINE_SOLE_CALLEE` because a body with one caller MOVES and duplicates
nothing. `-O2` is unchanged, so no speed measurement moves.

**Unsigned conversions went through 64 bits for no reason.** The front end
widened an unsigned 32-bit integer to 64 before converting to or from floating
point, and split an unsigned 64-bit one into halves to add — both correct, and
both devices for a signed-only INSTRUCTION. x86-64's `cvtsi2sd` and `cvttsd2si`
have no unsigned form; libgcc's `__floatunsisf`, `__fixunssfsi`,
`__floatundisf` and `__fixunssfdi` do, and the thumb, riscv and avr backends
all already emitted those names — nothing reached them. On AVR the widening
turned a 32-bit software conversion into a 64-bit one and pulled 8.5 KB of
64-bit conversion code into any image containing an unsigned cast.
`target_widen_unsigned_fp_cvt()` now says which targets want it: x86-64
because it must, aarch64 because it always has and a register widening there
is free.

**`IR_SELECT` had no lowering here, and if-conversion creates them.** `long
pick(int c, long a, long b) { return c ? a : b; }` compiled at -O0 and -O1 and
was REFUSED at -O2 — the same source, the same target, the optimisation level
deciding whether it builds. Two-byte values never showed it, because
if-conversion asks for a 4- or 8-byte arm and an `int` here is two, so the hole
was open for exactly the widths a program is most likely to write. There is no
conditional move on this machine, so the lowering is the branch the pass just
removed; it is still required, because the pass runs for every target. Written
the obvious way it tripped its own range check at -O2 (a `br` reaches ±63 words
and an eight-byte copy out of a far slot is more than that), so it is an
inverted branch over an `rjmp`, which is the shape `jump_if` already had.

A fourth bug came out of the same work but belongs to the optimizer, not this
target: `sel_width` answered 8 for any parameter — "a parameter, full width" —
which is harmless where a select is a register move and wrong anywhere else. It
now reads the declared width.

**What the float code cost to get right.** Four rounding bugs were found on
the host, driving the same source against native float over 3338 cases, before
any of it ran on the part: the exponent recomputed after shifting into
subnormal position, exact cancellation taking the magnitude path and producing
−0, a divide that did not maintain `rem < den` (3.0f / 1.0f = 2.0f), and a
divide that did not normalise a subnormal significand. 477 → 266 → 77 → 6 → 0
mismatches. A fifth was found on the part: the multiply folded the low word of
its product into the sticky bit on the assumption the high word held bits
47..24, which is false when an operand is subnormal — 7.0f × 1.4e-45f came out
zero. Normalising both significands before the product makes the shift down to
27 bits a constant 21 places with no loop and nothing guessed at.

**And a note on the disassembler, because it cost a wrong hypothesis.**
`llvm-objdump --triple=avr` drops the high three bits of an `ldd`/`std`
six-bit displacement, silently: it prints `std Y+24, r18` as `std Y+0, r18`
and `std Y+17` as `std Y+1`. Reading a frame copy off it, the last store of a
four-byte move appeared to go to offset 0 instead of 8, which looked exactly
like an off-by-one in `layout()` and was not — `llvm-mc -disassemble` on the
same bytes is correct, and the real fault was elsewhere entirely (the slot
width above). The referee lesson recorded earlier in this entry has a
companion: a disassembler is a referee too, and this one is wrong about the
one field an AVR frame is made of.

**Frame slots are placed by use, and that was worth a quarter of all AVR
code.** Measured over the backend's own output (the embedded-*.c programs and
lib/rt/avr*.c, 54730 instructions), the far-slot path -- a slot past `ldd`'s
63-byte reach, addressed as `movw Z,Y / subi / sbci / ld` with SREG saved
around it where a carry must survive -- was roughly HALF of everything
emitted, and the program's own arithmetic barely registered. Offsets had been
handed out in discovery order, so a loop counter declared after a buffer was
out of reach for its whole life. Every local and shared temporary is now an
object weighed by the bytes of access the code makes to it, placed densest
first. 102324 bytes to 78160 over that corpus (7.2x clang to 5.5x), with no
change to what any instruction does. The jump shortening that looked obvious
was measured first and was worth 0.7%.

**The calling convention is checked against the RULE, not a compiler.** Two
silent miscompiles surfaced the first time the shared cross-target programs
ran on the part: va_copy (an inline target list in irgen that nobody extended
for AVR, now `target_va_list_is_pointer()`, a switch with no default) and
every call to a function returning a struct wider than eight bytes, which
lost its first argument -- the caller placed it in r25:r24 and then wrote the
hidden pointer on top. Both sides of that call were EmbCC's, which is why
EmbCC-against-EmbCC tests could not see it. Cross-checking against clang then
showed a third -- five- and six-byte structs returned in r20, where avr-gcc
pads a returned size to a POWER OF TWO and uses r18 -- and also that clang is
not a usable oracle here: its struct ARGUMENTS put the first field in the
highest registers, against "allocated left to right", and Rust's AVR backend
documents clang's convention as not binary-compatible with avr-gcc. So
tests/golden/avr-abi.sh encodes avr-libc's FAQ rules directly, with
hand-written assembly as the other side in both directions.

**`sym+N` in AVR assembly meant `sym`.** Every symbol form read the identifier,
stopped at the `+`, and never looked at what followed, and the contract
between src/as/gas.c and a target's assembler had no addend to carry --
`lds r24, buf+5` linked to buf[0], silently. The C compiler never takes that
path; hand-written .S -- startup and context switching, which the
requirements put in assembly -- takes nothing else. The contract carries an
addend now, and an operand followed by anything its form does not expect is
refused rather than shortened.
