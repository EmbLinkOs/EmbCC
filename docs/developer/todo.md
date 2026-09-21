# TODO — completeness gaps (evidence-backed corpus audit)

*How complete is EmbCC? It self-hosts and compiles all of fdlibm + emlibc, so the
architecture is done — what's left is a short, concrete feature list. This list
is ranked by a corpus run of `embcc -c` over TinyCC 0.9.27 (24 files) and newlib
libc string/stdlib/stdio (~500 files), with header-integration noise separated
from real language gaps.*

*Repro (host), from the EmbCC dir:*

```
NL="/home/motsou/cross/newlib-c99/x86_64-elf/include"
./embcc -c FILE.c -I include -I "$NL" -o /tmp/x.o
```

---

## Kernel self-host — the next corpus (the hardest yet)

*New goal: EmbCC compiles the EmbLinkOS **kernel** (so the OS can rebuild itself
entirely, not just its userland). The kernel is freestanding, higher-half,
hardware C — a materially harder corpus than TinyCC/newlib. Repro from `myos/`:*

```
for f in $(find kernel -name '*.c'); do /home/motsou/EmbCC/embcc -c "$f" -Ikernel -o /tmp/x.o; done
```

***89 / 89 kernel TUs compile clean (was 22) — the whole kernel builds under
EmbCC.*** K1–K9 and every one-off below are closed. No kernel C was changed.

### K1 — inline-asm assembler — DONE
EmbCC's extended-asm assembler (irgen.c `asm_assemble`) grew from
`int`/`cpuid`/`rdrand`/`setc` to the kernel's hardware vocabulary, every
encoding byte-verified against objdump: fixed-form (`cli`/`sti`/`hlt`/`nop`/
`pause`/`mfence`/`lfence`/`sfence`/`wbinvd`/`rdtsc`/`rdmsr`/`wrmsr`/`fninit`/
`pushfq`/`popfq`); port I/O (`outb`/`w`/`l`, `inb`/`w`/`l`); reg operands
(`pop`/`push`/`popq`/`pushq` `%N`, `str`/`ltr` `%N`); control registers
(`mov %reg,%%crN` / `mov %%crN,%reg`); memory operands (`lgdt`/`lidt %N`,
`invlpg (%N)`, `movdqa` ↔ `%%xmm0`). Constraint grammar already covered
`"=r"`/`"r"`/`"=a"`/`"a"`/`"=m"`/`"m"`/`"N"`/`"x"`, `"memory"`/`"cc"` clobbers,
`%N` refs, and `volatile`. Small parse helpers read the operand forms; unknown
mnemonics still refuse loudly. Golden test: tests/golden/x86_64/inline-asm-kernel.sh.

### One-offs — ALL DONE
The ten distinct remaining gaps, each closed and gcc-verified:
- GCC **statement expressions** `({ ... })` — EXPR_STMTEXPR (selftests.c).
- GNU **array-range designators** `[lo ... hi] = v` (font_8x16.c).
- **`__attribute__((noreturn))`** honored in the return-path check (syscall.c).
- **`__builtin_va_list`** accepted as `char *` (kprintf.c).
- **`&global` at a constant offset** in a static initializer — `&arr[i]`,
  `&g.field`, `p+n` — via a recursive resolve_addr filling greloc's addend
  (keyboard.c).
- **`sizeof(EXPR)`** folded in an ICE when the type is resolvable
  (`sizeof(((T*)0)->f)`, embkfs.c) and **`sizeof(local/global var)`** (fd.c).
- freestanding **`<string.h>`** (fd.c); **char\*/unsigned char\*** signedness.
- inline asm the three low-level TUs need: `mov` reg/imm↔GPR and segment
  registers, `pushq $imm`, `iretq`/`lretq`, a local-label RIP-relative `leaq`
  (the gdt trampoline), and named `%[operand]`s with the "i" constraint
  (process.c, gdt.c, usermode.c). **CRITICAL fix along the way:** the
  per-instruction operand-skip stopped only at `;`, silently dropping every
  instruction after the first in a `\n`-separated template — now stops at `\n`.

### K2 — GCC builtins — DONE
`__builtin_bswap16/32/64` (IR_BSWAP), `__sync_synchronize` (mfence),
`__builtin_unreachable` (ud2 + noreturn in the return-path check),
`__builtin_expect` (becomes its first arg), `__atomic_load_n`/`store_n`
(load / store+mfence) and `__atomic_exchange_n` (locked `xchg`, IR_XCHG).
`__builtin_memcpy/memset` already resolved to the libc names. Verified vs gcc
at -O0 and -O1.

### K3 — `sizeof` / `offsetof` as an integer-constant-expression — DONE
`sizeof(type)` already folded; the gap was `offsetof`. Added
`__builtin_offsetof(type, designator)` folded to a size_t constant at parse
time (descending `.field`/`[index]`), and pointed `<stddef.h>`'s offsetof at
it — so `_Static_assert(offsetof(...) == N)` works.

### K4 — raise the parameter / argument cap — DONE
12 → 32. MAX_PARAMS moved to type.h so `struct type`'s `ptypes[]` grows with
it (the cap in ast.h alone left ptypes[12] to overflow — a heap smash on a
>12-param function type, found by ASAN).

### K5 — char/string escape sequences — DONE
A shared `scan_escape` now handles the full C set: `\a \b \f \v \?`, GNU `\e`,
hex `\xH...`, and octal `\NNN`, for both char and string literals.

### K6 — `&array` — DONE
`&arr` yields `T(*)[N]` whose value is the array's address (gen_addr already
produces it). The pointer-to-array `int (*p)[N]` declarator (and its abstract
`int (*)[N]`) now parse too — DONE (commit 719d93a).

### C11 alignment — DONE
`_Alignof(type)`/`__alignof__(expr)` fold to `ty_align` like sizeof; `_Alignas(N)`
/`_Alignas(type)` reuse the aligned-attribute layout on struct members and locals
(over-16 local alignment still refused loudly). Commit 27f57c0.

### K7 — use-before-declaration of a `static` function — DONE
The call and value paths test `seq <= cur_body_seq` (what the ordered-walk
`declared` flag encoded) so it also holds during static-initializer lowering —
a function pointer in a vtable resolves. This closed the "&func in a static
initializer" seam too: `greloc` grew an `ftarget` the driver relocates.

### K8 — `__attribute__` after a declarator — DONE
A trailing `__attribute__` on a local declarator is accepted (alignment not
yet honored on a stack slot).

### K9 — DONE
An empty translation unit yields a valid empty object.
*(A freestanding `<string.h>` on the path is an integration matter.)*

### K10 — `-mno-sse` codegen — **DONE** (kernel now compiles + links + boots + runs)

**RESOLVED (branch Teo).** EmbCC gained a `-mno-sse` mode (also spelled
`-mno-sse2` / `-mgeneral-regs-only`; `-mno-mmx` / `-mno-red-zone` / `-mno-80387`
/ `-mcmodel=` accepted as no-ops). Under it the varargs prologue skips the
`xmm0..7` register-save spill entirely, and any float op / `IR_I2F` / `IR_F2I`
/ `IR_F2F` is **refused loudly** (THE RULE — no silent SSE). Verified:
the `#UD` site `kprintf` disassembles to **0 SSE**; whole-kernel codegen under
`-mno-sse` emits **0 SSE** (the only 2 `movdqa` kernel-wide are the kernel's own
deliberate SSE-context-switch selftest inline asm in `process.c`, which runs
*after* `fpu_init` sets `CR4.OSFXSR`); integer varargs still run correctly;
default (no-flag) path is byte-identical (self-host holds, suite 87/87). The
2 `movdqa` in the original triage were miscounted as a struct-copy lowering —
they were always the kernel's own inline asm, not codegen.

<details><summary>original triage (kept for context)</summary>

*Empirically confirmed: all 88 kernel TUs compile, LINK with the kernel linker
script into a valid higher-half `EXEC` (entry `0xffffffff8037bde0`), and it BOOTS
and runs ring-0 higher-half code — so `-mcmodel=kernel` already works, no
relocation or code-model problems. It dies with a `#UD` → triple fault at the
first varargs call:*

```
v=06 (#UD) at kprintf+0x20:  f2 0f 11 85 ... movsd %xmm0,-0x130(%rbp)
```

The kernel is `-mno-sse -mno-mmx`, and SSE is not enabled in CR4 until `fpu_init`
runs — so any SSE instruction before that faults. EmbCC emits SSE where GCC (with
`-mno-sse`) does not. Whole-kernel disassembly shows this is **tiny and targeted
— 18 SSE instructions total, zero float math**:
- **16 `movsd`**: the System V varargs prologue spilling `xmm0..7` into the
  register-save area (in `kprintf` and one other varargs fn). Under `-mno-sse`
  this whole XMM save area is skipped (and callers must not set `AL`=xmm-count).
- **2 `movdqa`**: a 16-byte aligned move (a struct/`memcpy` lowering) — should use
  general-purpose `mov`s under `-mno-sse`.

**Fix: a `-mno-sse` mode** (a) no XMM spill in the varargs prologue, (b) never
lower struct copies / anything to SSE. That's the last thing between EmbCC and a
booting self-compiled kernel. (`-mno-red-zone` not yet exercised — the `#UD`
comes first; worth confirming once SSE is off, since the kernel takes interrupts.)

*Repro (from `myos/`): compile every `KERNEL_SRC` with `embcc -c … -Ikernel`,
link `x86_64-elf-ld -T kernel/linker.ld` with the nasm objects, boot.*

</details>

### K11 — honor `__attribute__((aligned(N)))` in LAYOUT — **DONE**

**RESOLVED (branch Teo).** `aligned(N)` is now applied to layout, not just
parsed: (a) a struct **member**'s offset rounds up to `N` and (b) the struct's
own align/size rise to a multiple of `N` (per-member `user_align` threads
through `ty_struct_layout`; it overrides `packed`, which only lowers the
default); (c) a **stack local** carrying the attribute gets its frame slot
rounded so its rbp-relative base is `N`-aligned (rbp is 16-aligned on entry, so
`N<=16` is honored; `N>16` would need dynamic stack realignment and is **refused
loudly** — THE RULE). Verified against the REAL kernel header: `struct thread`
(the `fxsave` target) lays out byte-identical to gcc — `fpu_state` at offset 80
(16-aligned) and `sizeof == 848` (16-multiple); the minimal repro static-asserts
pass; a local `observed[16] aligned(16)` lands on a 16-aligned slot; suite
87/87, self-host holds, kernel 89/89. This was the last thing between the
self-compiled kernel and the desktop.

<details><summary>original triage (kept for context)</summary>

*With `-mno-sse` in, the self-compiled kernel now boots much further — PMM (512 MB),
VMM direct map, ACPI, EMBKFS mounted, VFS at `/`, ksym loaded — then takes a
`#GP` at the **first context switch**:*

```
Vector 0x0D (#GP)  RIP 0xFFFFFFFF8037BC90 = kernel_ctx_switch:  fxsave (%rdx)
```

`fxsave` **#GPs unless its memory operand is 16-byte aligned**. The buffer is the
process struct's FPU save area:

```c
/* kernel/process/process.h:153 — the comment says "aligned(16) is load-bearing" */
unsigned char fpu_state[512] __attribute__((aligned(16)));
```

EmbCC **parses** `aligned(N)` (K8) but does not **apply** it to layout, so
`fpu_state` lands at a non-16 offset and `fxsave` faults. Minimal host repro —
this `_Static_assert` **fails** under embcc, passes under gcc:

```c
struct s { char c; char buf[512] __attribute__((aligned(16))); };
_Static_assert(__builtin_offsetof(struct s, buf) % 16 == 0, "buf 16-aligned");
```

**Fix: apply `aligned(N)` to layout** — (a) round a struct **field**'s offset up
to `N`, (b) raise the **struct's** own alignment/size to a multiple of `N`, and
(c) align **stack slots** for locals carrying the attribute (e.g.
`uint8_t observed[16] __attribute__((aligned(16)))`). This is the one thing
between the self-compiled kernel and reaching the desktop.

</details>

### K12 — inline-asm operand allocator must EXCLUDE clobbered registers — **DONE**

**RESOLVED (branch Teo).** The `"r"` operand allocator now removes from its free
set (a) every register in the **clobber list** (kept in the AST now, no longer
discarded) and (b) every hard register the **template writes/reads as `%%reg`**
(any width — `rdi`/`edi`/`di`/`dil`…, mapped by `asm_phys_reg`; conservative,
a read-only mention is excluded too — always sound). So no allocatable operand
can land in a register the asm destroys. Verified on the REAL kernel: both
`iretq` trampolines in process.c now load argc/argv/envp into rdi/rsi/rdx via
r9/r10/r11 and push operands from rax/rcx/rbx/r8 — none template-written;
gcc-refereed exec test `tests/exec/x86_64/asm-clobber.c` (clobber-list AND
template-written paths) passes. Suite 88/88, self-host holds, kernel 89/89.
Alongside, K11 got a refinement: a local whose *type* is over-aligned (a struct
with an aligned member) now also gets its stack slot rounded to the type's
natural alignment (`ty_align`), not just the declarator attribute.

<details><summary>original triage (kept for context)</summary>

*With aligned(N) honored (K11), the self-compiled kernel boots even further —
past the first context switch — then `#GP`s on the `iretq` that launches the
first ring-3 process (`process_trampoline`), i.e. right as it would start
`init`/`home`.*

Root cause: an inline-asm `"r"` operand is allocated to a register named in the
**clobber list**, and the asm's own instructions destroy it before it's used.
`process_trampoline` builds the iret frame with 7 `"r"` operands and clobbers
`rdi`,`rdx`:

```c
"movq %6, %%rdx\n"     /* envp -> rdx (rdx is clobbered) */
"pushq %2\n"           /* cs=0x23 ... but %2 was allocated to rdx! */
"iretq\n"
: : ... "r"((uint64_t)(0x20|3)) /*=%2 cs*/ ... "r"(envp) /*=%6*/
: "rdi", "rdx", "memory"
```

EmbCC put `%2` (cs) in `rdx`; the `movq %6,%%rdx` overwrites it with `envp`, so
`push %2` pushes `envp` as **CS** → `iretq` faults. Minimal host repro (7 ops,
clobber `rdi`/`rdx`) — EmbCC emits `push %rdx` for the operand despite the
clobber; gcc never does:

```c
void f(unsigned long o0,unsigned long o1,unsigned long o2,unsigned long o3,
       unsigned long o4,unsigned long o5,unsigned long o6){
  __asm__ volatile("movq %4,%%rdi\n movq %5,%%rsi\n movq %6,%%rdx\n"
                   "pushq %0\n pushq %1\n pushq %2\n pushq %3\n"
   : : "r"(o0),"r"(o1),"r"(o2),"r"(o3),"r"(o4),"r"(o5),"r"(o6)
   : "rdi","rdx","memory"); }        /* embcc: 'push %rdx' for %2 — the bug */
```

**Fix:** remove clobber-list registers (and any register the template writes
explicitly, e.g. `%%rdi`/`%%rsi`/`%%rdx` here) from the operand allocator's free
set, so no `"r"` operand is ever placed in one. This is the last thing between
the self-compiled kernel and userspace / the desktop.

*(K1 follow-ups spotted alongside — **now DONE**: inline-asm memory operands
`movq disp(%base), %dst` and the store reverse `movq %src, disp(%base)` (incl.
rbp/rsp/r12/r13 SIB / forced-disp bases), plus the ALU ops add/sub/and/or/xor/
cmp in `%src,%dst` and `$imm,%dst` (imm8/imm32, 64- and 32-bit) forms. Every
encoding byte-compared to gas; gcc-refereed exec test `tests/exec/x86_64/asm-mem-alu.c`
+ extended `tests/golden/x86_64/inline-asm-kernel.sh`. Remaining known gap, NOT on the
boot path: a `+r` read-write operand's read side isn't wired — sema accepts `+`
but treats it as output-only, so the initial value isn't loaded. The kernel
uses no `+` constraints; deferred.)*

</details>

### K13 — stack usage: `-O0` frames ~18× GCC's — **DONE** (temp slot coalescing)

**RESOLVED (branch Teo).** EmbCC gave every temporary (vreg ≥ nvars) its own
8-byte slot, never reused — so a deep call chain overflowed the 16 KiB kernel
stack. Added **temporary stack-slot coalescing** in codegen (`coalesce_temps`):
temps whose live ranges don't overlap share one slot. A temp is coalescable only
when its whole live range lies in ONE basic block; such temps are packed by a
linear scan over their `[first,last]` appearance interval (sound even across
loop back-edges — each is reborn in its block per iteration). Temps that cross a
block boundary (`?:`/`&&`/`||` results) keep a unique slot. The interval is the
span of EVERY operand appearance via a blind field scan — over-counting only
reduces reuse, never makes it unsound, so there's no per-op operand table to get
wrong. Deterministic (self-host fixed point holds).

Results: **`ata_read_dma` 1760 B → 144 B** (gcc: 96 B), **frames > 1 KB:
331 → 46** (the rest are real local buffers — e.g. `sys_chan_recv`'s
`uint8_t kbuf[CHAN_MSG_MAX_BYTES]` — which gcc sizes identically and coalescing
correctly leaves alone). Deep overflow path now tiny: `vfs_read` 160 B,
`ata_read_dma` 144 B. Suite 90/90 (incl. new gcc-refereed `slot-reuse.c` stress
test at -O0 and -O1), self-host deterministic, kernel 89/89 compile clean.

🎉 **END-TO-END CONFIRMED (myos):** the self-compiled kernel now boots to the
**home desktop in the STOCK 16 KiB kernel stack** — `home.elf` pid 4, compositor
window, Clock widget first frame — with **zero kernel changes** (the diagnostic
KSTACK bump was reverted). *"Self-hosting for C, but the kernel wants GCC"* is
retired: the OS's own compiler compiles the OS's kernel and it runs.

*Residual tail (NOT on the boot path — desktop is fine): a few mega-functions
carried EmbCC frames 3–5× GCC's. **Halved by scope-based local-slot coalescing**
(this session): locals in DISJOINT lexical scopes now share a stack slot — sound
because a stack pointer used past its scope is UB (gcc's own model). irgen
records each local's block instruction-range (`ir_func.var_scope_lo/hi`), and
codegen interval-colours the slots (`coalesce_locals`), sized/aligned to the
strictest occupant; `-g` disables it (distinct DWARF locations). Result:
`shell_handle_process_command` **84 KB → 45 KB**. gcc-refereed by
`tests/exec/scope-slots.c` (disjoint sibling arrays coalesce; nested/overlapping
ones must not — validated address-taken). The remaining gap to gcc (16.5 KB) is
gcc ALSO coalescing SAME-scope locals by liveness — safe for non-address-taken,
but for the address-taken arrays here it relies on the escaping-pointer UB;
deferred as riskier for a kernel. Not blocking; boot-to-desktop runs stock.*

<details><summary>original triage (kept for context)</summary>

*With K12 in, the self-compiled kernel boots ALL the way through init, the
dynamic linker runs, and **userspace launches** (`home: launched
/system/bin/home.elf as pid 4`) — then a **Double Fault** at a plain
`mov %rax,-0x70(%rbp)` in `ata_read_dma`, with a garbled backtrace: the classic
**kernel-stack-overflow** signature.*

EmbCC at `-O0` spills every local to the stack (no register allocation, no
slot reuse), so frames are far larger than GCC's, and a deep kernel call chain
(`syscall → vfs → embkfs → block → ata_read_dma → …`) overflows the **16 KiB**
per-thread kernel stack (`KSTACK_SIZE`, myos `process.h:37`):

| function | GCC frame | EmbCC frame |
|---|---|---|
| `ata_read_dma` | 96 B (`sub $0x60`) | **1760 B** (`sub $0x6e0`) |
| kernel-wide | — | **331 functions > 1 KB**, biggest ~4 KB |

The code is *correct* — it's just too stack-hungry. This is the first item where
"compiles + is correct" isn't enough; it's a **codegen-quality** gap.

**Fix (EmbCC side):** cut stack usage — real **register allocation** (the started
optimizer, `src/opt/opt.c`) so hot locals live in registers, and/or **reuse
stack slots** for locals whose live ranges don't overlap (today each gets its own
slot). Getting close to GCC's frame sizes lets the self-compiled kernel run in
the same 16 KiB the GCC kernel uses.

*(Confirmed by a diagnostic-only KSTACK_SIZE bump on the myos side — NOT
committed, per "don't change the kernel for an EmbCC gap": with a larger stack
the self-compiled kernel runs past this. So this is the last codegen item; once
EmbCC's stack usage drops, no kernel change is needed.)*

</details>

### K14 — register allocation (`-O2`) — **DONE**

**RESOLVED (branch Teo).** EmbCC gained a real register allocator at a new
`-O2`, on top of K13's slot coalescing. Eligible vregs (temps, and scalar
int/long/pointer locals & params of size 4/8) live in the five callee-saved GPRs
(rbx, r12–r15) instead of memory, so their loads/stores vanish. Design:

- **Callee-saved only** — such a value survives a call untouched, so there is no
  spill-around-call machinery; the function saves/restores the regs it uses in
  dedicated frame slots. Params are synced slot→reg once in the prologue.
- **Real liveness + graph colouring** — a backward dataflow
  (`compute_live_intervals`) gives per-instruction live-in/live-out sets; the
  PRECISE interference graph (two vregs interfere only when live at the same
  program point — entry or exit — so ranges that overlap but are never
  simultaneously live can share a register) is greedily coloured with the 5
  registers, spilling the rest. (Two subtleties, each caught by the gcc
  differential: appearance intervals are unsound across loop back-edges — a
  loop-carried value's register got clobbered mid-loop; and interference from
  live-OUT alone misses a value whose only appearance is a last use, e.g. a
  param consumed once — two such params collided in one register. Both fixed by
  using real liveness and including live-IN.)
- **Conservative eligibility** — temps, and scalar int/long/pointer/char/short
  locals & params, are candidates; scalar-integer CALL ARGUMENTS are allocated
  too (moved straight into their arg register / stack slot). Any vreg touching
  an "opaque" raw-slot site (a float op, address-of, atomic/memcpy/store-address/
  va_start, a struct/float call arg, or inline asm — or live across an asm)
  stays in memory. Missing an exclusion would read a stale slot, so the
  allocator errs toward memory.
- **`-O0`/`-O1` untouched** — everything is gated on the regalloc flag, so their
  output stays byte-identical (the RAX residency cache is disabled at `-O2`,
  where register-resident values would make its tracking stale).

Verified: all 60 exec programs match gcc's exit + stdout at `-O2`
(`tests/golden/regalloc-O2.sh`); a stress corpus (values live across calls,
recursion, register pressure > 5 forcing spills, 8-param calls, div/mod/shift,
pointers/structs/narrow types) agrees with gcc at `-O0`/`-O1`/`-O2`; codegen is
deterministic; EmbCC self-compiles at `-O2` and links (and the `-O2` compiler is
*smaller* — fewer load/stores); the kernel compiles 89/89 at `-O2`. Memory
traffic drops materially — a counted loop went 35→15 rbp accesses, an 8-param
call 68→40, a value-live-across-calls function 30→12. Suite 91/91.

**Follow-up (same session) — move coalescing + a stronger residency cache.**
Three additions on top: (a) the interference graph is now split PRECISELY into
live-in and live-out groups (a value dying at an instruction and one born there
never interfere), which both improves allocation and enables (b) MOVE
COALESCING — a copy `dst = a` (IR_MOV / IR_STVAR, and a plain non-extending
IR_LDVAR) records a preference edge, biased colouring gives the pair one
register, and codegen drops the now-identical self-move; and (c) the RAX
residency cache runs at `-O2` alongside the allocator (keyed on vreg so it also
elides reloads of register-resident values), with a zero-extend-aware relaxation
so a 4-byte store then 8-byte reload (the `IR_MOV` round-trip) is elided.
Result: whole-kernel `-O2` .text is **23% smaller than `-O0`** (2.42 MB →
1.84 MB); a copy-heavy function fell 39→24 movs. Still 91/91, deterministic,
`-O0`/`-O1` byte-identical, kernel 89/89.

**Follow-up 2 (same session) — the deferred optimizer list, ALL done:**
- **`volatile` tracking** (a latent correctness fix — EmbCC ignored it, so ANY
  memory optimization would have broken MMIO): `type.is_volatile` → IR
  load/store/ldvar/stvar `vol`; a member of a volatile struct is itself volatile
  (ehci/ohci register structs). `ty_volatile` copies the type; struct identity
  equality follows a `canon` back-pointer.
- **Local value numbering (CSE)**, `pass_lvn`: pure ops, address arithmetic,
  constants, and non-volatile loads are numbered within a basic block (loads
  memory-versioned so a store forces a reload; volatile never numbered).
- **Strength reduction**: `x*2^k`→`x<<k`, unsigned `/2^k`→`>>k`, `%2^k`→`&(2^k-1)`
  (signed div stays idiv).
- **Same-scope local coalescing by liveness**: a non-address-taken local uses
  its precise liveness range (address-taken stay scope-bounded), so same-scope
  disjoint-lifetime locals share a slot.
- **Chaitin-Briggs optimistic register colouring** (simplify/select, spill the
  most-constrained node).

Net effect: `a[i]+a[i]`→one load+multiply; `poly()` 119→80 insns; **whole-kernel
`-O2` .text 2.42 MB → 1.47 MB (39% under `-O0`)**; `shell_handle_process_command`
frame 84 KB → 30.6 KB (gcc 16.5 KB). Correctness: gcc-refereed tests
(`cse.c`/`strength.c`/`same-scope.c`), the stress corpus, volatile-MMIO
preservation, suite **95/95**, self-host deterministic, kernel 89/89 at -O0/-O2.
*(Remaining gap to gcc on leaf compute loops is codegen-architectural: EmbCC
hardcodes rax/rcx/rdx as scratch, so only the 5 callee-saved regs are
allocatable where gcc uses all 15; and same-scope ADDRESS-TAKEN arrays still need
alias analysis to coalesce like gcc.)*

---

## EmbLD — linking the kernel (the LINK side of self-host)

*With the compile side done (K1–K13), the next step is linking the kernel with
**EmbLD** instead of `x86_64-elf-ld`, so the whole toolchain is owned. Tried it —
and it very nearly just works.*

***🎉 Proven: an EmbCC-compiled, EmbLD-linked kernel boots to the home desktop***
*(EMBKFS mounted, `home.elf` pid 4, compositor window, Clock first frame). No GCC,
no `ld` in the loop.* EmbLD already produces a correct higher-half kernel: the
`.text` LOAD at vaddr `0xffffffff80100000`, a page-aligned second LOAD for
`.data`/`.bss`, the entry point from `-e _start`, and program headers the
bootloader loads. Invocation:

```
embld -e _start -Ttext 0xFFFFFFFF80100000 -o kernel.elf <all .o> <nasm .o>
```

### L1 — linker-defined symbols (`kernel_end`) — DONE
*It was the one blocker; option (b) below is what shipped.* EmbLD now
auto-defines the end-of-image family — `kernel_end`, `_end`, `end`, `__bss_end`,
`__kernel_end` — at the true end of `.bss`, only when referenced and otherwise
undefined, so a real definition still wins and ordinary programs are untouched.
The diagnostic stub is gone and **the kernel links with zero external tools.**

*The problem, as it stood:* the kernel's linker script ends with
`kernel_end = .;` and `pmm.c` places the PMM bitmap at `kernel_end`. EmbLD had
no `-T`/symbol-assignment, so:

```
embld: undefined symbol 'kernel_end' (referenced by …/pmm.c.o)
```

It was the **only** thing missing — a diagnostic stub (`kernel_end equ <addr
past the image>`) let the link succeed and the kernel boot to the desktop. **Fix
options were:** (a) minimal `-T` linker-script support handling `SYM = .`
assignments (the general answer — also subsumes `-Ttext`/`-e`/`ENTRY()`); or
(b) implicitly define end-of-image symbols. **(b) shipped** — narrower, and it
turned out `-T` was never needed for the kernel. Full linker scripts remain
unimplemented, and no corpus has asked for them.

### L2 — cosmetic: `AT()` LMA (p_paddr) — DONE (`--lma-offset`)
EmbLD sets `p_paddr = p_vaddr` (no `AT()` load-address split), so the LOAD
segments report a higher-half `PhysAddr`. Both loaders (stage2 and the UEFI
loader) derive the physical destination from `p_vaddr − KERNEL_VIRTUAL_BASE` and
ignore `p_paddr`, so it boots fine — but a correct `p_paddr` (real LMA `0x100000`)
would be nicer for a general kernel ELF. Low priority.

---

## Toolchain frontier — owning the whole build (consolidated)

*Added 2026-07-29 from the OS side (myos `docs/BUILD.md` §12 + `docs/PACKAGING_AND_SDK.md`).
Compile is done (K1–K14); these are the remaining pieces to a build with **zero
external tools**, and then to being the **SDK's producer**. OS-side context lives in
those two myos docs; only the EmbCC/EmbLD/assembler work is tracked here.*

| # | Gap | State | Unlocks |
|---|-----|-------|---------|
| **L1** | EmbLD linker-defined symbols (`kernel_end`) | **DONE** — `define_end_symbols` auto-provides `kernel_end`/`_end`/`end`/`__bss_end` at the image end (only if referenced; a real def wins; a genuinely-undefined symbol still errors) | kernel links with **no `ld` and no stub** |
| **A1** | EmbCC standalone `.asm` assembler (NASM/Intel front-end) | **DONE** — `embas` + `embcc -c foo.asm`; all 6 kernel ELF `.asm` assemble **byte-identical to nasm** in code, symbols, and relocations | drops **nasm** for the kernel ELF objects — EmbCC = compiler+assembler; `EmbBuild`-builds-the-kernel (KM1) |
| **L2** | EmbLD `AT()` LMA (`p_paddr`) | **DONE** — `embld --lma-offset OFFSET` sets `p_paddr = p_vaddr - OFFSET` (higher-half kernel LMA); default keeps `p_paddr == p_vaddr` | cosmetically-correct kernel ELF |
| **X1** | EMBX emission driven by a build manifest (+ inline namespace, `build_id`) | forward-looking (packaging is design-only) | EmbCC/EmbLD become the **SDK producer** (packaging PK2) |

### A1 — a standalone assembler (grow EmbCC into compiler+assembler) — DONE

The last external tool in the kernel build was **nasm**: the 6 kernel `.asm`
are NASM syntax, and the K1 inline-asm assembler is AT&T/operand-resolved, so it
could not read them. Rather than port nasm, EmbCC grew its own assembler
front-end — `src/arch/x86_64/as.c`, reachable two ways:

- **`embas -f elf64 foo.asm -o foo.o`** — the standalone tool (`tools/embas/`).
- **`embcc -c foo.asm -o foo.o`** — the driver dispatches a `.asm` input to the
  same assembler, exactly as gcc dispatches `.s` (`embcc` = compiler+assembler).

**Shipped (all built and tested):** an Intel-syntax two-pass assembler with jump
relaxation (rel8/rel32 fixpoint, matching nasm's short-jump choices), local-label
scoping (`.halt` → `parent.halt`), `%macro` expansion (`isr0..255`), the
directives `global`/`extern`, `section`, `align`, `db`/`dw`/`dd`/`dq`, `resb`,
`incbin`, and ELF-object emission via the shared writer (`src/elf/write.c`) with
`R_X86_64_64`/`R_X86_64_PC32` relocations.

**Correctness bar met — byte-identical to nasm.** All 6 kernel ELF `.asm`
(`kentry`, `syscall_entry`, `kcontext`, `isr`, `ap_entry`, `ap_trampoline_blob`)
assemble to objects whose **code (`.text`/`.rodata`/`.data`), symbol table, and
relocations are byte-identical to `nasm -f elf64`** — including nasm's own
conventions: section-appearance ordering, a leading `STT_FILE` symbol,
`STT_SECTION` symbols, a definition-sequence-ordered symtab, and relocations
against the section symbol + addend for locally-defined targets. The only
residual differences are the linker-invisible internal layout of the ELF
container (the `.strtab` string order and the section-header-table placement,
both in the shared writer used by `embcc` too); a real link of an
`embas` object produces byte-identical relocated `.text` to the nasm object.
Covered by `tests/golden/x86_64/assembler.sh` (skips honestly without nasm). `as.c` is
in the self-host source set — the fixed point holds at **16 sources**.

**Out of scope (deliberately):** `-f bin` flat-binary mode for the boot stages
and AP trampoline (`boot/stage1`, `boot/stage2`, `ap_trampoline.asm`) is *not*
implemented — those are 16/32-bit real-mode code with `org`/`BITS 16`/`DEFAULT
ABS`, a separate encoder frontier from the 64-bit kernel ELF corpus. `embas -f
bin` reports "not yet implemented" rather than miscompiling. The kernel's own
ELF objects — the A1 goal — no longer need nasm.

### X1 — EMBX emission as the SDK producer (forward-looking, not yet needed)

EmbCC/EmbLD already emit EMBX with a declared capability set
(`embld --embx --cap NAME`). The OS's packaging + SDK design
(myos `docs/PACKAGING_AND_SDK.md`, phase **PK2**) wants the toolchain to be the
single producer of an app's declared authority — *when packaging work starts*, not
now (packaging is design-only). The EmbCC-side asks:

1. **Drive the EMBX capability table from the build manifest** (`build.ebm` package
   stanza) instead of only command-line `--cap`, so declaring authority is part of
   building.
2. **Optional inline namespace section** in EMBX beside the capability table, so an
   EMBX binary can carry its own namespace declaration (today ELF apps ship a
   sidecar `<name>.ns`; this is the EMBX-native equivalent noted in the packaging
   doc §4 / userspace UP4).
3. **Fill the header fields the packager verifies** — **already done.** EmbLD
   stamps `abi_version`, `build_id[32]` (SHA-256 over the whole image, computed
   with `build_id`/`header_checksum` zeroed), and `header_checksum` (CRC32C over
   the header body), plus per-segment CRC32C — see `src/link/link.c` (~L847–889),
   in the checksum order EMBX §3.4 fixes. So `pkg install` can already verify a
   bundle by content; nothing outstanding here.

So of X1, only (1) manifest-driven capabilities and (2) the inline-namespace EMBX
section remain, and both are gated on OS-side packaging. **No action until PK1/PK2
begin on the OS side; recorded so the producer end is scoped when it does.**

### Codegen — verify the large-frame tail is closed

K14 (register allocation, `-O2`) is done; confirm it closed the residual K13 tail
(a few userspace **mega-functions** were 3–5× GCC frames — e.g. the shell's
`shell_handle_process_command` at ~82 KB vs 16.5 KB — which would overflow a
16 KiB stack; the *boot path* was already fine). Not a new gap if `-O2` covers it —
just a check to retire the note.

---

## Tier 1 — blocks ordinary real C; do these first (small, high-leverage)

*Status: both landed. On the TinyCC 0.9.27 corpus (24 files) the block-scope
`extern`/`typedef` and `redefined-differently` first-errors dropped to **zero**;
every remaining blocker is now `cannot find include file "dlfcn.h"` (a missing
host header for dynamic linking — integration, not a language gap) or a file's
own `#error`. The front-end runs the whole preprocessor + language layer before
stopping. Full suite 74/74; self-host fixed point holds.*

- [x] **1. Block-scope `extern` / `typedef` declarations.**
  `int f(void){ extern int errno; ... }` and `... { typedef int T; ... }`.
  Error: `expected a statement, got 'extern'` / `'typedef'`. Confirmed on ~33
  newlib files. `parse_stmt` accepts a type or `static` (and the register-asm
  form) as the leading token of a local declaration, but not `extern` or a local
  `typedef`. Fix: recognize `extern`/`typedef` (and `extern`+type combos) at the
  start of a block-scope declaration. Very common in real C; contained fix.

- [x] **2. Macro redefinition: warn, don't fatal.**
  A macro redefined with a DIFFERENT body is a fatal `error: macro 'X' redefined
  differently` in EmbCC; gcc issues a WARNING and takes the new definition.
  Blocked ~190 corpus files (mostly `SEEK_SET`, `ARG_MAX` — largely a two-header-
  set artifact, but the fatal-vs-warn behavior is a real, gcc-incompatible
  strictness). Fix: downgrade a benign redefinition to a diagnostic that does not
  stop the compile (match gcc), keeping the LAST definition. (Identical
  redefinition already accepted.)

## Tier 2 — genuine ISO features still missing (confirmed by direct probe)

Lower corpus frequency only because Tier-1 fails hit first; each is real C.

- [x] **3. Bitfields.** `struct F { unsigned a:3, b:5; };` — DONE.
  Little-endian gcc-compatible layout (fields don't cross a storage-unit
  boundary; anonymous padding fields; `:0` separators; `packed`), signed and
  unsigned extraction via the two-shift trick, read/assign/`+=`/`++`, access
  through pointers, and `long` (>32-bit) fields. Verified against gcc for
  values, `sizeof` layout, and by-value SysV ABI (incl. a cross-ABI
  embcc→gcc link). `&bitfield` is refused. Braced initialization of bitfields
  also works — static, local, and designated — by carrying `(bit_off,
  bit_width)` on `initelem` and masking/merging in both the static-byte and
  local lowerings.
- [x] **4. Designated ARRAY initializers.** `int a[5] = { [2]=9, [4]=1 };` —
  DONE. File-scope and local, unsized arrays sized to the highest index
  reached, gaps zero-filled, a `[i]=` designator repositions the running
  index with positional elements continuing after it, later writes to a slot
  win. Verified against gcc; index-past-end is refused.
- [x] **5. Compound literals.** `&(struct P){ .x = 5 }` — DONE. `(type){init}`
  becomes an unnamed object with automatic storage (a synthesized local slot),
  initialized like a declared aggregate (zero-fill, designators, last-write-
  wins). It is an lvalue: address-of, member access, array decay + indexing,
  by-value passing, scalar literals, and initializing a local all work and
  match gcc. File-scope (static-storage) literals work too: a direct
  `T g = (T){...}` (or one nested in a static initializer) unwraps to its brace
  initializer, and `&(T){...}` becomes an anonymous global the pointer
  relocates to.
- [x] **6. A real `_Bool` type.** DONE. `_Bool` is a keyword and a distinct
  1-byte unsigned type (TY_BOOL); a store normalizes any nonzero scalar
  (integer, pointer, or float) to 1. `<stdbool.h>` now maps `bool` to it, so
  `sizeof(bool)==1`. Verified against gcc.
- [x] **7. C11 niceties.** All DONE.
  - [x] `_Static_assert(expr, "msg")` — evaluated at parse time; legal at file
    scope, in a struct/union body, and in a block; the message is optional
    (C23). A false assertion is a fatal error naming the message.
    (`size_fold` also learned `&&`/`||`.)
  - [x] `_Generic(ctrl, T: e, ..., default: e)` — the arm whose type matches
    the controlling expression (after its lvalue conversion) is selected at
    compile time; the controlling expression is not evaluated; no match and
    no default is an error. Verified against gcc.
  - [x] Anonymous struct/union members — a nameless `struct{...};`/`union{...};`
    member's fields are reached through the enclosing object (member lookup
    descends into them with cumulative offsets). Verified against gcc.

## Tier 3 — integration / ergonomics (NOT compiler gaps)

- [x] **8. Accept `-isystem`** — DONE. `-isystem DIR` and `-isystemDIR` are
  accepted as an include directory (EmbCC keeps one search path), so build
  scripts that pass it work.
- **"cannot find include file"** = a missing/inconsistent libc header set on the
  path — an integration matter, not a language gap.
- **Implicit-declaration errors** = CORRECT C99 strictness; the fix is prototypes,
  not a lax mode. Do not "fix".

## Confirmed NOT gaps (work today)

`register`, `const`/`volatile` (accepted), variadic macros, `#`/`##`, function
pointers + arrays of them, structs/unions/enums, `goto`, varargs incl. float,
static/hex float, u64<->double, weak undefined refs.

---

## Acceptance

Tier 1 both land + a corpus re-run shows real C getting materially further before
hitting a Tier-2 feature; full test suite + self-host fixed point stay green.

## Status

*(2026-09-08.)* All of Tier 1, Tier 2, and the Tier-3 `-isystem` item are DONE —
each landed with a gcc-refereed exec test and the self-host fixed point holding.
The suite is now **102/102**. Since that pass the C surface also gained C11
`_Alignof`/`_Alignas` and the `_Atomic` qualifier, pointer-to-array declarators
`int (*p)[N]`, GNU `typeof`, wide/prefixed string and character literals, and
GNU computed `goto`.

**What is still genuinely missing:** nothing in the C language itself.
**`_Complex`** landed on 2026-09-18 — float, double and long double, lowered
to their parts in sema, `*` and `/` through libgcc as gcc does, the struct
ABI both targets use for it (plus x86-64's st0/st1 return for long double
_Complex), newlib's `<complex.h>` (tests/exec/complex.c,
tests/golden/complex-abi.sh).
**VLAs** landed on 2026-09-18 (both targets; tests/exec/vla.c), and so did
**`long double`** — which, correcting this list's earlier claim, had never
been refused: it was compiled as `double` (sizeof 8 against the ABI's 16).
It is now x87 extended on x86-64 and binary128 on aarch64, with exact
constants (sema/ldfloat.c) and gcc-compatible calls (tests/golden/
ldouble-abi.sh). Found on the way and fixed: a float `++`/`--` added 1 to the
bit pattern, and a float condition tested bits, so `-0.0` was true. The two initializer seams that were once refused loudly
are now implemented too: braced initialization of **bitfields** (static/local/
designated) and **file-scope compound literals** (direct value, nested, and
`&(T){...}` via an anonymous global). The remaining Tier-3 entries are
integration notes, not compiler work.

## Closed: C++ access control (2026-09-21)

The entry that stood here said it "wants to be done once, properly,
with the existing corpus as the regression test -- not slipped in".
That is what happened, and the corpus is what made it possible: it
caught four wrong refusals that reading the code had not.

Enforced at five places: a member named through an object, a member
named without one (`C::s`), a member function once overload resolution
has chosen it (access belongs to the OVERLOAD, not the name), a
constructor where the construction is built, and the derived-to-base
conversions. Friendship is recorded for classes, functions and class
templates; `template <class U> friend class X;` befriends every
instance, which is the shape shared_ptr and weak_ptr use to reach each
other.

The four the corpus caught:

  - A using-declaration REPUBLISHES a base member at a new access.
    libstdc++'s vector is `protected _Vector_base` with
    `using _Base::get_allocator;` public, and internal_file_clock
    republishes a protected _S_to_sys.
  - A base's accessibility is about a CONVERSION, not about the object
    adjustment that reaching an inherited member needs. to_base serves
    both; checking there refuses the case above.
  - A nested class is a member of its enclosing class and reaches its
    privates, while being derived from nothing.
  - Checks must not fire under SFINAE. An error there does not reach
    anyone -- it unwinds and turns a viable overload into a non-viable
    one. With them running, one test program's generated C differed by
    5856 lines, compiled, and behaved differently. That is the one
    failure mode a diagnostic-only feature must not have.

-fno-access-control turns it off, as g++ spells it.
tests/golden/cxx-access.sh: ten ill-formed programs refused with a
located diagnostic and accepted again under the flag, twelve correct
ones still compiling.

Remaining, all missed diagnostics rather than wrong refusals: nothing
is checked under SFINAE, so is_constructible answers as though
everything were public; [class.protected]'s "through an object of the
derived class" narrowing is not enforced; a member republished more
permissively stops being checked everywhere, since overload resolution
works on the original rather than the alias; a befriended function
template or specialization makes its class befriend everything; and a
nested type carries no access of its own.

## Closed: two ties-to-even bugs, and a `>` inside decltype (2026-09-21)

All three found by writing headers, not by reading the compiler.

**nearbyint was floor(x + 0.5)**, which is round() under another name.
Its entire job is to follow the CURRENT rounding direction, and the
default is ties to even: nearbyint(2.5) is 2 and round(2.5) is 3. Now
reads fegetround() and honours all four modes -- which only became
possible when <fenv.h> was written.

**remainder had no tie rule.** fmod truncates the quotient; IEEE's
remainder rounds it to nearest with ties to EVEN. remainder(7, 2) was 1
and must be -1, because the nearest multiple of 2 to 7 is 8. This is
the case argument reduction depends on: reducing an angle by pi/2 lands
on a tie at every odd multiple of pi/4. The whole family now matches a
known-correct libm byte for byte.

**About sixty math variants were missing.** C11 requires all three
widths for every function in <math.h>; the gap was invisible until
<tgmath.h> made a missing one a compile error. The ones that MOVE or
INSPECT a value are now written in long double throughout, because
narrowing them loses exactly the bits a long double was chosen for.

**A `>` inside decltype's parentheses was read as the closing angle
bracket** of an enclosing template argument list, so
`is_same_v<decltype(a > 2), bool>` did not parse. Call arguments and
casts already cleared that state; decltype did not.

## Closed: the asm "m" constraint, both directions (2026-09-21)

Found by writing `<fenv.h>`, which has to reach a hardware register and
so is the first code in this tree to use an `"=m"` output on a scalar.

**An `"m"` INPUT passed the operand's VALUE where the template needs its
ADDRESS.** It happened to work for a struct -- whose "value" in this IR
already is an address -- and read from a garbage address for a scalar.
`lgdt %0` with `"m"(*(char *)p)` was the only existing use and it is a
char lvalue, so the golden checked the instruction's own bytes and never
noticed what surrounded them.

**An `"=m"` OUTPUT was stored over.** The lowering loads inputs, emits
the template, then stores each output register through the lvalue's
address -- which is right for `"=r"` and destroys an `"=m"` result,
because the template already wrote that memory. `stmxcsr %0` read MXCSR
into the local and the next instruction overwrote it with a register.

Both fixed on both targets, with a `mem` flag on the operand.

On the way: `stmxcsr`/`ldmxcsr` were not in the x86-64 inline
assembler at all, so nothing could reach the SSE rounding mode or the
sticky exception flags. Encodings verified byte for byte against the
cross assembler.

## Closed: five front-end gaps the standard library found (2026-09-20)

None of these was found by reading the compiler. Each was found by
writing a header that a real C++ program would include, which is the
point of writing the library at all — a corpus that exercises overload
resolution and name lookup the way library code does, rather than the
way test cases do.

**A defaulted `operator<=>` did not implicitly declare `operator==`**
([class.compare.default]/2). `a != b` rewrites through `==` and *never*
through `<=>`, so a class written with the one line
`auto operator<=>(const T &) const = default;` could be ordered and
could not be compared for equality. The declaration is now made in
`src/cxx/class.c` beside the other implicit members, defaulted and with
the same access, so it is defined only if it is used.

**A nested braced list was scored one element at a time.** Choosing an
overload for `vector<vector<int>> v{{1, 2, 3}}` offered the inner
`{1, 2, 3}` to `vector<int>`'s constructors as *three arguments* and
never as one `initializer_list`, so it found no three-argument
constructor and the whole outer call was reported as having no viable
candidate. The initialization itself was right all along — this rejected
valid programs rather than miscompiling them, which is why `{{1, 2}}`
worked (`vector(n, value)` happens to take two) and `{{1, 2, 3}}` did
not. `ics_of` now tries the initializer-list constructors first, the
order `construct` already used.

**Two lambdas in two different blocks of one function mangled the
same.** An unnamed type's number was counted per *block* scope, so the
first lambda in every `{ }` came out `Ut_`. Invisible until such a type
reaches a template argument — and then `v | filter(a)` in one block and
`v | filter(b)` in another give two distinct `filter_view`
instantiations with identical mangled names, which the emitted C rejects
as a redefinition. The counter belongs to the enclosing function.

**A conversion function inherited from a base was invisible to the
built-in operators.** `a == 7` on a class whose `operator int()` comes
from a base found no candidate at all, because `class_to_builtin`
scanned only the class's own scope. `std::atomic<int>`, whose conversion
lives in `__atomic_base`, could not be compared with anything.

**A using-declared base member did not get the derived class's implicit
object parameter** ([over.match.funcs]/4). `a = 3` where `operator=(T)`
came through a using-declaration was *ambiguous* against the implicit
copy assignment: the first wanted a derived-to-base conversion for its
object and an exact match for its argument, the second the reverse, and
neither was better in every argument.

Two more, not front-end: the generic atomic builtins now take any object
of a workable size rather than only an integer or pointer (they pass by
pointer and copy bytes, so `std::atomic<double>` works), and printf's
floating conversion is exact — see `docs/language/libc.md`.

The library grew with them, and has kept growing: `lib/libcxx/include`
is now 97 headers and `lib/libc/include` has every C11 header. With
`<filesystem>` in, the C++ list is down to `<locale>` alone, which is
left out on purpose — it is the one header whose entire subject is the
host's cultural data, and a freestanding target has none.

`tss_create` and `thread_local` are absent for a reason of their own --
they need thread-local storage, which EmbCC does not have, and a key
every thread shared would be a global under another name.

## Closed: atomic wait/notify (2026-09-21)

The last entry in this file that said "absent on purpose". The futex
primitive had been in the seam for a while; what was missing was the
library above it.

The futex word is NOT the atomic object, for two reasons and the second
decides it: the object may be one byte or sixteen while the primitive
takes a 32-bit word, and a word per object would put four extra bytes
in every std::atomic for an operation most never use. Waiters share a
64-entry table indexed by a hash of the address. Collisions cost a
wakeup that finds nothing changed, which the standard explicitly
allows -- every caller re-checks and loops.

The protocol is the correctness: the notifier bumps the counter BEFORE
waking, the waiter reads it BEFORE checking the value. Either one
backwards loses a notification that lands in the window between a
waiter's check and its sleep.

tests/golden/libcxx-std/atomicwait.cc supplies the seam's own weak
futex_wait/futex_wake, so the "other thread" runs at a chosen instant
inside the sleep rather than by luck -- one thread, no timing. Swapping
the two lines in the notifier makes it fail, which was verified rather
than assumed. The waiter's half is not covered and the file says so:
telling its two orders apart needs a notification delivered between two
inline loads, with no seam between them to hang one on. A racing second
thread would be the kind of test that passes for a year and then does
not.

Comparison is of the OBJECT REPRESENTATION, not operator==, as
compare_exchange already was: 0.0 and -0.0 compare equal and are
different values, and a wait returning on one seeing the other would
report a change that never happened.

## Closed: -Wformat (2026-09-21)

`printf("%d\n", some_long)` compiled with nothing to say and then read
four bytes where eight were passed. Nothing in the language relates a
variadic call's format string to its arguments, but both are at the
call and the types are known.

Which functions to check does NOT live in the compiler.
`__attribute__((format(printf, n, m)))` is how a declaration says so --
GCC's spelling, already on every declaration in a real <stdio.h> -- and
lib/libc/include/stdio.h now carries it. A compiler that knew the name
`printf` would hold that knowledge twice and still know nothing about
anyone's own log().

What is compared is the PROMOTED type, because that is what lands in
the variadic tail: a float arrives as a double and a short as an int,
so %f of a float and %d of a short are both right. scanf is the mirror
-- nothing promotes through a pointer -- so plain %f writes a float and
`sscanf(s, "%f", &a_double)` is caught.

A signedness mismatch is deliberately NOT reported. GCC reports it; the
values have the same size and representation, and noise inside -Wall
trains the reader to skip the category, which costs the warnings that
do matter.

C++ is checked through the LOWERING rather than beside it: the
attribute is carried into the emitted C, so one implementation serves
both front ends. Exactly one thing moves the indices, and it is not
`this` -- GCC defines a non-static member's arguments as counting from
two because of it, so the source already accounts for the parameter
lowering makes explicit. A hidden return slot does move them, being
this compiler's own and coming before `this`.

tests/golden/format-check.sh judges against gcc, one way: every line
EmbCC reports, gcc must report too. Sixteen wrong calls that must all
be caught, twenty correct ones that must all be silent -- and the
second list is what found the bugs. Checking a scanf field's pointee
against a size reported `sscanf(s, "%s", buf)`, since %s takes a
character BUFFER rather than a pointer to one object.

## Our libc on macOS: two things the OS seam did not anticipate (2026-09-21)

`embcc --target=aarch64-apple-darwin` compiles all 63 libc sources, and
a program linked against the resulting archive runs on macOS: printf,
snprintf, the string functions and strtol all correct. Two things had
to be worked around by hand to get there, and both are LIBC work rather
than compiler work.

**A weak undefined symbol cannot be left unresolved at static link
time.** The seam's optional groups (threads, filesystem) are built on
the ELF idiom: declare `fs_chdir` and friends weak, and a target that
does not provide them reads them as zero. Mach-O marks the reference
weak the same way -- our objects are byte-for-byte what clang produces,
"(undefined) weak external" -- but ld still refuses to finish the link,
and CLANG'S OWN OBJECT IS REFUSED THE SAME WAY. It is a platform rule,
not a bug in the writer: on macOS a weak reference may go unmet only
when it comes from a dylib that declares it weak_import.

So the 25 hooks currently need `-Wl,-U,_name` apiece. The real answer
is that macOS is a HOSTED platform with a real filesystem and real
threads, so the optional groups should be implemented rather than left
absent -- `lib/libc/os/darwin/backend.c`, binding them to the system's
own chdir, chmod, opendir and pthreads. That is the honest shape, and
it removes the question rather than answering it.

**The platform's exit is not ours.** libSystem's crt1 calls main and
then libSystem's exit, so the atexit handler our stdio registers to
flush its buffers never runs: a program printed nothing and exited with
the right status. An explicit fflush proves the output was correct all
along. A Darwin backend has to arrange the flush -- either by owning
the entry point or by registering with the platform's atexit rather
than ours.

Neither of these is reached by any test today, because the libc built
for macOS is not built by `make`. That is the next piece: a darwin
backend and a `libc-darwin` target, at which point both findings become
checks instead of notes.

## `strtold` parses through a double (2026-09-21)

`conv()` in `lib/libc/src/stdlib/strtod.c` takes a `wide` flag and then
says `(void)wide;` — `strtold` is `strtod` widened afterwards, and
`parse_hex` returns `(long double)ldexp((double)v, bexp)`, narrowing the
one path that could have been exact for free.

This is the mirror of the printf gap closed below, and it matters for
the same reason: `%La` now writes every bit a long double has, and
reading one back loses them again above double's range. The hex path is
the easy half — it is bit assembly, not decimal arithmetic — and needs
only a 64-bit accumulator for x87, though 128 for an IEEE quad. The
decimal path needs a big-integer decimal-to-binary at long double
width, which is the same order of work as the printf side.

## Closed: %a, and long double converted at its own width (2026-09-21)

Both were listed in `docs/language/libc.md` as "absent rather than
wrong". The long double one was the substantive half: a `long double`
was cast to a `double` before conversion, so `%Lf` of anything beyond
double's range printed `inf` and the smallest one printed `0.000`.

The fix is a decomposition step. A double, an x87 80-bit extended and an
IEEE quad differ only in their bit layout; once decomposed they are the
same thing — an integer significand times a power of two — so
`struct fpval` is what the conversion works on and everything below it
is written once. The x87 is the one format that stores its leading bit
explicitly, and the quad's significand spans both words, which is the
whole of the difference.

The big integer needed to grow fifteen-fold (5^16494 rather than
5^1074), and sizing one buffer for the larger would have put ~17KB of
stack on every printf of a double. So the limbs belong to the CALLER:
`put_double` and `put_ldouble` each declare their own and pass them in.

Two things fell out of it. The output was being built in a fixed
512-byte buffer whose loops stopped at its end, so any precision that
overran it was silently truncated — `%.700f` of a double was wrong
before any long double was involved. The digits are now streamed and
only the LENGTH is computed up front, which is what the width padding
actually needs. And %g's style choice, which depends on the exponent
after rounding, was rounding a full copy of the digit string to find
out; it only ever needed the carry, so it now asks for that directly and
the second buffer is gone.

`%a` needs none of this machinery: four bits of a binary significand are
one hexadecimal digit, so it is exact by construction. Its one real
decision is rounding at a precision, which is ties-to-even in base
sixteen — a tie being an exact eight with nothing after it.

Verifying it needed three referees, because no single one covers the
ground. The system libc refereed **117,819** double conversions, all
byte-identical. It could NOT referee `%a` with a precision, because
macOS truncates there rather than rounding — so **2,997,615** roundings
were checked against an independent implementation of the rule, reading
the exact digits from the default `%a` both agree on. And no library on
the build host has an 80-bit long double at all, so those expectations
were computed in exact rational arithmetic.

Not covered: the IEEE quad path. Nothing currently runs this libc on
aarch64 — `tests/golden/libc.sh` says so itself — so the 113-bit
decomposition is compiled and type-checked but never executed. It
inherits that gap rather than creating one.

## Closed: a rollback that restored a freed pointer (2026-09-21)

`<filesystem>` would not compile: the compiler took SIGSEGV inside
`malloc`, which is never where the bug is. It reproduced only when
writing a real object file — `-o /dev/null` and `--emit-c` both
succeeded — which is the signature of heap corruption rather than a
parse error, since those paths simply allocate differently. ASan named
it in one run: a heap-use-after-free WRITE in `add_pending`, in a call
path with no connection to the one that did the `free`.

The cause is a save/restore pair that saved a POINTER into a growable
array. `parse_save`/`parse_restore` capture the parser's state so a
speculative parse can be unwound, and among the fields they captured
were `pend`, `npend` and `cappend` — the delayed-member array, its
length and its capacity. If anything between the save and the restore
called `add_pending` often enough to grow the array, the `xrealloc`
freed the old block; the restore then put that freed pointer back into
the global, and the next `add_pending` wrote through it. Nothing was
wrong at either site. The bug lived in the assumption that a pointer
is part of a state you can roll back, when a realloc has made it a
pointer to nothing.

What makes it interesting is that the two callers wanted DIFFERENT
things and the one mechanism could not serve both. `class_define_from`
sets `pend = NULL` before parsing a nested class body, deliberately
starting a fresh list so the interrupted parse's queued members are not
run by the inner one — it genuinely needs the pointer put back. Every
other caller goes on appending to the SAME array, and for those
restoring the pointer is the use-after-free. So the fix is not one
change but a separation: `parse_save` keeps only `npend`, the logical
length, which is the correct rollback for a shared buffer that only
grows and whose entries past the mark are dead; and `class_define_from`
does its own explicit three-variable swap, which is safe because the
array it swaps in starts empty and the outer one is never touched.

The first attempt — dropping the pointer from the saved state and
stopping there — turned the use-after-free into a null dereference at
`pend[1]`, because `class_define_from` still cleared `pend` and now
nothing put it back. That failure was useful: it is what pointed at the
second caller and its opposite requirement.

## Closed: dead landing pads, and long double objects (2026-09-20)

Both entries that stood here are fixed, and each turned out to be one
line of MODEL rather than the structural change they were written up as.

**Dead landing pads.** The pads were never the problem. `IR_LANDING` is
the one instruction with two destinations — the exception pointer and
the selector, as the unwinder leaves them — and `def_target()` reports
one, so `compute_defs()` thought the selector undefined and DCE deleted
the landing while keeping the stores that read it. Naming both
definitions in `compute_defs` was enough; `opt_func` no longer has to
refuse the function, so every `noexcept` function that calls nothing is
optimized again. It is deliberately still absent from `writes_temp`,
because an instruction there is one LVN and GCSE may value-number, and
two landing pads are not the same computation however identical they
look.

The same blind spot was in the **inliner**: `remap_ins()` renumbered one
destination, so an inlined pad kept the callee's numbering for its
selector and read a vreg that did not exist in the caller. Inlining any
`noexcept` function reached it — `std::exception::exception()` did,
which is how the verifier found it.

**Long double objects.** `ldf_from_bytes()` is the exact inverse of
`ldf_encode`, so a `constexpr long double` can be stored and read back
in the target's own format. `store` writes all sixteen bytes now, rather
than the double half — which left the object holding a bit pattern that
was not the value stored.
