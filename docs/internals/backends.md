# Backends

This page describes the code generators in `src/arch/`: the target model
they share (`target.c`), the predefined-macro tables, the contract every
backend implements, and then each backend in turn (x86-64, AArch64,
Thumb, RISC-V and AVR): how it lowers EmbIR, how it lays out a frame,
how it implements its calling conventions, how its instruction encoder
is checked, and what it does specially. It is written for people
changing EmbCC. Register allocation, which all five share, is in
[Register allocation](register-allocation.md); the IR is in
[EmbIR](ir.md); what each target supports from the user's side is in
[Targets](../manual/targets.md).

## Layout of `src/arch/`

| Path | Contents |
|---|---|
| `target.c`, `target.h` | the target model: triples, data model, ABI questions, relocation kinds |
| `backend.h` | the contract a backend implements |
| `code.c`, `code.h` | the machine-code buffer every encoder writes into |
| `predef.c`, `predef.h` | which predefined-macro table the target uses |
| `regalloc.c`, `regalloc.h` | the shared register allocator |
| `x86_64/` | x86-64: `codegen.c`, `emit.c`, `irgen.c` (`va_arg`, extended asm), `topasm.c` (file-scope asm), `as.c` (EmbAS), `disasm.c`, `predef*.c` |
| `aarch64/` | AArch64: `codegen.c`, `emit.c`, `asm.c` (inline-asm assembler), `irgen.c`, `predef*.c` |
| `thumb/` | ARMv7-M and ARMv8-M Mainline: `codegen.c`, `emit.c`, `asm.c`, `attrs.c` (build attributes), `irgen.c`, `predef*.c` |
| `thumbv8m/` | the ARMv8-M predefined-macro tables only |
| `riscv/` | RV32 and RV64, one backend: `codegen.c`, `emit.c`, `asm.c`, `irgen.c` |
| `riscv32/`, `riscv64/` | the two RISC-V predefined-macro tables only |
| `avr/` | AVR (ATmega328P): `codegen.c`, `emit.c`, `asm.c`, `irgen.c`, `predef*.c` |

Each `irgen.c` holds the target's share of IR generation: `va_arg`,
inline-asm operand resolution, and the `*_imm_foldable` predicate the
optimizer asks before folding a constant into an instruction.

## The target model

One process compiles for one target. The driver selects it from
`--target=` before the front end runs, and nothing in the compiler reads
the host's architecture. Everything that depends on the target asks
`src/arch/target.h`.

### Triples

A target has three independent dimensions: the architecture
(`enum target_arch`: `TARGET_X86_64`, `TARGET_AARCH64`, `TARGET_THUMB`,
`TARGET_RISCV32`, `TARGET_RISCV64`, `TARGET_AVR`), the operating system
(`enum target_os`: none, EmbLinkOS, Linux, Darwin, Windows), and the
object format (`enum target_fmt`: ELF, Mach-O, COFF). The accepted
triples are an explicit table, `g_triples[]` in `target.c`, not a cross
product, so a combination that does not exist cannot be accepted by
accident. Each row is marked canonical (the spelling `-dumpmachine` and
diagnostics print) or an alias. A name not in the table is refused.

The Thumb rows also carry the sub-architecture: ARMv7-M, ARMv7E-M
(`thumb_em`), or ARMv8-M Mainline (`g_thumb_arch` 8), and whether the
name was an `-eabihf` one. These are a level on one target, not separate
targets, because the data model and calling convention are the same.

When no `--target=` is given, `target_apply_default()` applies, in order:
`EMBCC_DEFAULT_TARGET` from the environment, the compiled-in
`EMBCC_DEFAULT_TARGET` (`make DEFAULT_TARGET=...`), and `x86_64-elf`.

### The data model

`g_model[]` holds one row per architecture, and each question has a
function:

| Function | x86-64 | AArch64 | Thumb | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|
| `target_ptr_size()` | 8 | 8 | 4 | 4 | 8 | 2 |
| `target_long_size()` | 8 | 8 | 4 | 4 | 8 | 4 |
| `target_int_size()` | 4 | 4 | 4 | 4 | 4 | 2 |
| `target_double_size()` | 8 | 8 | 8 | 8 | 8 | 4 |
| `target_ldouble_size()` | 16 | 16 (8 on Darwin) | 8 | 16 | 16 | 4 |
| `target_char_unsigned()` | no | yes (no on Darwin) | yes | yes | yes | no |
| `target_wchar_unsigned()` | no | yes (no on Darwin) | yes | no | no | no |
| `target_has_int128()` | yes | yes | no | no | yes | no |
| `target_max_scalar_align()` | none | none | none | none | none | 1 |
| `target_stack_align()` | 16 | 16 | 8 | 16 | 16 | 1 |

`-fsigned-char` and `-funsigned-char` override the char column through
`target_set_char_signed()`. `target_xlen()` is the register width in
bits (RISC-V's XLEN).

Other questions are asked by name, each a `switch` with no `default`
so that a new architecture fails to compile until it answers:

| Function | Meaning |
|---|---|
| `target_va_list_is_pointer()` | `va_list` is a bare pointer (Thumb, RISC-V, AVR, Win64, Darwin AArch64) rather than a pointer to a tag (System V, AAPCS64); decides what `va_copy` copies |
| `target_anon_bitfield_aligns()` | an unnamed bit-field raises the struct's alignment (AAPCS, AAPCS64; not Apple arm64, which lays them out as x86-64 does) |
| `target_widen_unsigned_fp_cvt()` | an unsigned 32-bit integer is widened to 64 bits before a floating-point conversion (x86-64, AArch64) |
| `target_jump_tables()` | a dense `switch` may become `IR_SWITCH` (every target but AVR) |
| `target_win64_abi()` | x86-64 uses the Microsoft x64 convention (the Windows triples) |
| `target_op_calls_helper()` | the current backend lowers this IR instruction to a runtime-helper call. The driver registers the backend's predicate with `target_set_calls_helper()` once, for the target finally chosen (from `--target=` or the configured default): `t_op_calls_helper`, `rv_op_calls_helper` or `a64_op_calls_helper`; x86-64 and AVR have none |
| `thumb_imm_foldable()`, `riscv_imm_foldable()`, `a64_imm_foldable()` | whether the optimizer may fold a constant into an instruction's immediate |
| `target_mul_shift_add()` | whether a multiply by `c` is a shifted add or reverse subtract, for AArch64 and Thumb |
| `target_thumb_*()`, `target_pcs_vfp()`, `target_pcs_differs()` | the Thumb level, FPU, float ABI and `pcs` attribute |
| `target_riscv_rvc()` | RISC-V emits compressed instructions; always 1, read by the encoder and the ELF flags |
| `target_insn_len()` | the length of the instruction at a byte pointer, for `-S` grouping; 0 on x86-64, where the disassembler decides |

`target.c` is also linked into tools that carry no backend (the encoding
checkers, `embls`), so it must not name a backend's functions directly;
the backend predicates are declared in `target.h` and defined in the
backends.

### Relocation kinds

A backend records a machine-neutral `enum reloc_kind` at each patch site
(`RK_CALL`, `RK_TAIL`, `RK_PCREL32`, `RK_ADR_HI21`/`RK_ADD_LO12`,
`RK_GOT_PAGE`/`RK_GOT_LO12`, `RK_THM_MOVW`/`RK_THM_MOVT`,
`RK_RISCV_PCREL_HI20`/`RK_RISCV_PCREL_LO12_I`, `RK_RISCV_CALL`, the
`RK_AVR_*` kinds, `RK_ABS64`, `RK_ABS32`, `RK_DATA_PREL32` and the TLS
kinds). The driver turns a kind into a concrete relocation:

- `target_reloc_type(arch, kind)` gives the ELF type (-1 when the kind
  does not apply to that architecture, which is a codegen bug).
  `RK_TAIL` becomes `RK_CALL` except on Thumb (`R_ARM_THM_JUMP24`) and
  AArch64 (`R_AARCH64_JUMP26`).
- `target_reloc_addend(arch, kind, bias)` gives the ELF addend; x86-64's
  PC-relative fields are biased by -4, because they are measured from the
  end of the instruction.
- `target_macho_reloc()` and `target_coff_reloc()` give the Mach-O and
  COFF spellings; both formats store the addend in the patched field, so
  the -4 bias does not apply.

`target_elf_machine()` and `target_elf_flags()` give `e_machine` and
`e_flags` (`EF_ARM_EABI_VER5` on Thumb, `EF_RISCV_RVC` on RISC-V,
`EF_AVR_ARCH_AVR5` on AVR).

### Adding a target

1. A row in `g_model[]`, an answer in every `switch` in `target.c` (the
   build fails until each is given), rows in `g_triples[]`, and the
   relocation mappings.
2. A directory with `codegen.c`, `emit.c` and `irgen.c`, and a
   `codegen_unit_*` entry point the driver calls.
3. A predefined-macro table from `tools/gen-predef.sh` (below).
4. An encoding referee for `emit.c` (see [Encoders and their
   referees](#encoders-and-their-referees)).
5. A QEMU harness under `tests/harness/<arch>/`, and the target in the
   test suite. See [Testing](testing.md).
6. A new source file also has to be added to the build lists, including
   `EMBLS_SRCS`; otherwise `make test` fails at its build step.

## Predefined macros

Each architecture's predefined-macro table is generated from a reference
compiler and checked in, so building EmbCC needs no cross toolchain.
`tools/gen-predef.sh ARCH` regenerates `src/arch/ARCH/predef.c` and
`predef_cxx.c` (`ARCH` is `x86_64`, `aarch64`, `thumb`, `thumbv8m`,
`riscv32`, `riscv64` or `avr`; with no argument, all of them). The files
must not be edited by hand.

| Table | Reference |
|---|---|
| `x86_64` | `x86_64-elf-gcc -dM -E` |
| `aarch64` | `aarch64-elf-gcc -dM -E` |
| `thumb` | `clang -target thumbv7m-none-eabi -ffreestanding` |
| `thumbv8m` | `clang -target thumbv8m.main-none-eabi -mfloat-abi=soft -ffreestanding` |
| `riscv32` | `clang -target riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding` |
| `riscv64` | `clang -target riscv64-unknown-elf -march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding` |
| `avr` | `clang -target avr -mmcu=atmega328p -ffreestanding` |

`EMBCC_REF_GCC_<ARCH>` (for example `EMBCC_REF_GCC_THUMB`) names a
different reference compiler. The C++ tables come from the same
compiler's C++ driver with `-std=gnu++20`. The generated file's header
records the compiler and version used.

The script removes macros that would claim something EmbCC is not or
does not do: the `__GNUC__`, `__VERSION__`, `__clang__` and `__llvm__`
families (EmbCC is neither), `__STDC*` (defined by the preprocessor
itself), `__BITINT_MAXWIDTH__`, `__riscv_v_intrinsic`,
`__ARM_FEATURE_CMSE`, and on RISC-V the
`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_*` sizes the backend refuses. The C++
tables also drop the `__cpp_*` feature-test macros and the types EmbCC's
C++ does not implement.

`src/arch/predef.c` chooses the table at run time and adds what the
generated tables cannot know:

- the operating system's macros (`__emblink__`, `__linux__`,
  `__APPLE__`/`__MACH__`, `_WIN32`/`_WIN64`/`__MINGW32__`/`__MINGW64__`,
  and the `__unix__` family where it applies);
- Apple's arm64 data model, replacing the generated AArch64 values for
  `long double`, `wchar_t` and `wint_t` and dropping `__CHAR_UNSIGNED__`;
- the Thumb FPU macros (`__ARM_FP` 0x4, the `__ARM_VFPV*__` set,
  `__ARM_FPV5__` on ARMv8-M, `__ARM_PCS_VFP` for the hard-float ABI), with
  `__SOFTFP__`, the generated `__ARM_FP` and (for hard float) `__ARM_PCS`
  removed;
- `__ELF__` removed for Mach-O and COFF.

`embcc --dump-predef` prints the table in use.
`tests/golden/predef.sh` compares it with
`tools/gen-predef.sh --reference ARCH` when the reference compiler is
installed.

## The backend contract

`backend.h` declares one entry point per backend, all with the same
signature: `codegen_unit` (x86-64), `codegen_unit_arm64`,
`codegen_unit_thumb`, `codegen_unit_riscv` (both widths) and
`codegen_unit_avr`. Each takes the optimized `struct ir_unit` and
returns:

- the unit's `.text` in a `struct code`, with each function's
  `code_off` and `code_len` filled in;
- `struct extcall` sites: calls to functions not defined in the unit
  (`tail` set for a tail call);
- `struct strsite`, `struct gsite` and `struct fsite` sites: the places
  that need a string's, a global's or a function's address, each with a
  relocation kind (one site on x86-64, two for a two-instruction address
  on AArch64, Thumb and RISC-V, one per byte on AVR).

The driver turns the sites into relocations for the object writer.
Nothing downstream knows which backend produced the image.

The flags passed in are `want_debug` (`-g`: collect line tables and
variable locations), `optimize` (`-O1` and above), `no_sse` (x86-64
`-mno-sse`) and `regalloc` (also `-O1` and above).

Shared helpers:

- `cg_wide_vregs(fn)` marks vregs holding a value too wide for a
  register (a 16-byte `long double`, an `__int128`, a vector).
- `cg_float_vregs(fn)` marks the floats and doubles, for a backend with
  a floating-point register class.
- `cg_resolve_strsites()` turns each string site's index into its
  `.rodata` offset.
- `cg_call_local()` says whether a callee is defined in the unit.
- `code_byte`, `code_u16`, `code_u32`, `code_patch32` and `code_align`
  write and patch the buffer (little-endian). `code_mark_data(c, start,
  end)` records a range of data inside `.text` (a jump table); the ELF
  writer puts ARM mapping symbols around it (`$d`, then `$t` or `$x`) on
  Thumb and AArch64.

### What the backends have in common

All five backends share a shape:

- **Every vreg has one home**: the register the allocator assigned, or
  a frame slot. Instructions read operands through accessor functions
  that return the register a value is in, or load it into a scratch
  register from its slot, and write results the same way. A lowering that
  bypasses the accessors and reads a slot directly is reading memory that
  nothing wrote when the value has a register; x86-64 and AArch64 catch
  that at encoding time (the dead-slot guards below).
- **Scratch registers** outside the allocator's pool carry operands of
  spilled values, addresses and intermediate results. Which ones, and
  why, is listed per backend in
  [Register allocation](register-allocation.md#backend-hooks).
- **Fusions look at neighbouring instructions.** A compare whose only
  use is the next branch becomes a compare-and-branch; an address
  computation whose only use is the next load or store becomes an
  addressing mode. The optimizer's last pass (`pass_sinkaddr`) moves
  address arithmetic next to its access so these fire.
- **No code after an unconditional transfer** up to the next label, and
  no jump to the label that immediately follows.
- **Tail calls** from `-O1` when the call is direct, every argument is in a
  register, and nothing in the frame outlives the call. Each backend adds
  its own conditions. `EMBCC_NO_TAILCALL` turns them off on AArch64,
  Thumb, RISC-V and AVR.
- **Branch relaxation** (all but AArch64) by generating a function more
  than once: each branch starts in one form, and a later pass shrinks or
  grows it according to the distances measured.
- **Refusals.** An operation a backend cannot lower ends the compilation
  with a message naming it, never with wrong code. The embedded backends
  use one format:

  ```text
  embcc: FILE:LINE: error: the ARMv7-M backend cannot lower WHAT yet (function NAME) [OP w=W size=S]
  ```

  with `the RV32 backend`, `the RV64 backend` or `the AVR backend` in
  place of `the ARMv7-M backend`. An encoder asked to encode an operand
  it cannot represent stops with an internal error rather than emit a
  different instruction.

### Encoders and their referees

Each backend assembles its own instructions, so no assembler checks
them. Each encoder is therefore compared with an independent one by a
golden test: a small program built from the encoder emits every form the
backend uses, and the test compares the bytes, or the disassembly of the
bytes, with an LLVM or GNU tool.

| Encoder | Test | Driver program | Reference | What is compared |
|---|---|---|---|---|
| AArch64 `emit.c` | `tests/golden/aarch64/arm64-encoding.sh` | `tools/a64check/a64check.c` | `aarch64-elf-objdump` | disassembly text of each word against the intended instruction |
| AArch64 `asm.c` | `tests/golden/aarch64/arm64-asm.sh` | `tools/a64check/a64asmcheck.c` | `aarch64-elf-as -march=armv8.2-a` | bytes, for the kernel's templates and every vocabulary entry |
| Thumb `emit.c` | `tests/golden/thumb-encoding.sh` | `tools/thumbcheck` | `llvm-objdump` | disassembly text; `--immediates` sweeps every modified immediate |
| Thumb VFP | `tests/golden/thumb-vfp.sh` | `tools/vfpcheck` | `llvm-mc -triple=thumbv7em-none-eabihf -mattr=+vfp4` | bytes |
| Thumb `asm.c` | `tests/golden/thumb-asm.sh` | `tools/tasmcheck` | `llvm-mc` | bytes, for every entry of the assembler's own tables |
| RISC-V `emit.c` | `tests/golden/riscv-encoding.sh` | `tools/riscvcheck` (`--rv32`, `--rv64`) | `llvm-mc --disassemble -mattr=+m` | disassembly text; `--li32`/`--li64` execute `rv_li` sequences in an interpreter; `--refuse` checks the range checks fire |
| RISC-V compression | `tests/golden/riscv-compressed.sh` | `tools/riscvcheck` (`--csweep32`, `--csweep64`) | `llvm-mc -mattr=+m,+c` | bytes of `rv_compress` against llvm-mc's compression of the same instruction |
| RISC-V `asm.c` | `tests/golden/riscv-asm.sh` | `tools/rvasmcheck` | `llvm-mc -mattr=+m` | bytes |
| AVR `emit.c` | `tests/golden/avr-encoding.sh` | `tools/avrcheck` | `llvm-mc -triple=avr -mcpu=atmega328p` | bytes for the vocabulary; PC-relative forms disassembled and compared as text; `--writes` checks the decoder `avr_insn_writes` |
| AVR `asm.c` | `tests/golden/avr-asm.sh` | `tools/avrasmcheck` | `llvm-mc` | bytes; PC-relative forms as text |

The vocabularies are generated from the encoders' and assemblers' own
tables where possible, so a new form cannot escape the check. Comparing
disassembly text, not only bytes, is what catches a condition code or
register field that encodes cleanly but means something else. A form
the driver program does not list is not checked; add each new
instruction form to it. On Thumb the 16-bit branch forms, `cbz`, the IT
form and their patchers are covered only by execution tests
(`thumb-relax.sh`, `thumb-exec.sh`).

x86-64's `emit.c` has no per-instruction referee. Its coverage is the
execution tests, `tools/x86-identity.sh` (byte-identical objects against
a baseline revision, for changes that must not alter x86-64 output), and
the inline-asm tests (`tests/golden/x86_64/inline-asm*.sh`), which check
the separate inline-asm assembler against `objdump`.

`tests/golden/asm-S.sh` and `tests/golden/asmout-roundtrip.sh` check that
`-S` output reassembles to the same object as `-c`. They test the `-S`
writer, not the encoders: `-S` prints the encoded bytes.

The toolchain the tests use can be overridden: `EMBCC_LLVM_MC`,
`EMBCC_LLVM_OBJDUMP`, `EMBCC_LLVM_OBJCOPY`, `EMBCC_AARCH64_OBJDUMP`,
`EMBCC_AARCH64_OBJCOPY`. A test whose tool is missing is skipped.

## x86-64

`src/arch/x86_64/codegen.c`, entry point `codegen_unit`. Targets
`x86_64-elf` (the default), `x86_64-emblink`, `x86_64-linux-gnu`,
`x86_64-apple-darwin` (Mach-O) and `x86_64-windows-gnu` (COFF).

### Lowering

`gen_func` lowers a function; its per-instruction `switch` is
preceded by the `__int128` lowering (`gen_i128`), the x87 `long double`
lowering (`gen_x87`), and the read-modify-write fusion. At `-O0` every
value lives in an `rbp`-relative slot and operations go through rax and
rcx. At `-O1` the backend keeps a residency cache: a value just computed
into rax is not reloaded. With the allocator on, values live in their allocated
registers, and a zero-extended value in rax also serves a wider
zero-extending read of the same temp -- but only when rax holds the whole
value, as after a store of a 4-byte result, whose slot's upper bytes are
then zero. After a narrow load of a wider value rax holds only its low
bytes, and only a read of exactly that width may reuse it.

Instruction selection:

- **Compare and branch** fuse when the compare's only use is the next
  branch. A float compare branches on the flags; `==` and `!=` add a
  `jp` for the unordered case.
- **Addressing modes.** An `ADD` whose only use is the next load or store
  becomes `[base+disp]` or `[base+index*scale]`, with a `shl` by 1 to 3
  as the scale. The address of a local folds into an `rbp` displacement.
- **`lea`** is the three-address add (`d = a + imm`, `d = a + b`);
  multiply by a constant is the three-operand `imul`.
- **Read-modify-write.** A load, an operation and a store back to the
  same address become one instruction with a memory destination
  (`x86_rmw_find`; `EMBCC_NO_RMW` turns it off). Its address folds in
  too (`rmw_addr_load`): `s[sp - 1] += s[sp]` is one
  `add %x, (%base,%index,4)`, the add and the shift emitting nothing and
  the base and the index held, not combined, across the loads of the
  right-hand side. A constant stored to memory is one `mov $imm, mem`.
- **`IR_SELECT`** is `test` of the condition at its own width (`size`,
  4 or 8) and `cmovne`.
- **Division** puts the dividend in rax, extends it into rdx (`cqo`,
  `cdq`, or `xor edx`), and divides; the remainder is read from rdx. A
  variable shift count goes through cl.
- **Block copies and clears** (`IR_MEMCPY`, `IR_MEMZERO`, struct
  arguments) over 256 bytes use `rep movsq` or `rep stosq`; smaller ones
  move 16 bytes at a time through xmm7 (8 bytes through rax under
  `-mno-sse`), then the remainder.
- **Vectors.** The vectorizer's operations (`IR_VLOAD`, `IR_VSTORE`,
  `IR_VBIN`, `IR_VSPLAT`, `IR_VWIDEN`, `IR_VREDADD`) are SSE2 only, with
  xmm7 as the working register and accumulators in xmm8-xmm11.
- **`__int128`**: add, subtract, bitwise operations, negation and
  comparisons are inline (`adc`/`sbb`); multiply, divide, remainder,
  shifts and conversions call libgcc (`__multi3`, `__divti3`,
  `__udivti3`, `__modti3`, `__umodti3`, `__ashlti3`, `__ashrti3`,
  `__lshrti3`, `__floattisf` and the others). `IR_CAS16` is
  `lock cmpxchg16b`.
- **`long double`** is the x87 80-bit format in a 16-byte slot. Values
  are never in registers; each operation loads onto the x87 stack and
  stores back (`fld`/`fstp`, `faddp` and the others, `fucomip`), and
  conversions to integers use `fistp` under a truncating control word.
- **Atomics** work on slots: `xchg`, `lock xadd`, `lock cmpxchg`, and a
  compare-and-swap loop for the other read-modify-write operations.
  `mfence` is the fence.
- **Thread-local storage** is local-exec: `mov %fs:0, reg` and an add of
  an `RK_TPOFF32` offset.

### Frame layout

The frame is `rbp`-based. Below `rbp`, `layout_frame` places, in order:
the callee-saved register saves, the sret pointer slot (when the
function returns a struct in memory), the locals, the shared temp
slots, the 16-byte slots of wide temps, scratch space for struct-return
temporaries and Win64 by-reference copies, the variadic register save
area (176 bytes) and `__va_list_tag` (24 bytes), and the outgoing
argument area at `rsp`. The total is a multiple of 16. There is no red
zone.

Locals share slots when their scopes, or (when their address is not
taken) their live ranges, do not overlap (`coalesce_locals`); this is
off under `-g`, with a computed `goto`, and with exception regions.
Temps share slots through `ra_coalesce_temps`. A local whose slot is not
needed (unreferenced, in a register, built in place, or an incoming
memory-class struct parameter used where it arrives) gets no slot.

A local aligned beyond what the stack guarantees does not reach the
backend as a slot: sema gives such an aggregate storage carved from the
stack at function entry and turns the local into a pointer to it
(`var_indirect`), and refuses such a scalar. `layout_frame` still stops
on a slot needing more than 16-byte alignment (`a local in 'NAME' needs
N-byte alignment, exceeding the 16-byte stack alignment EmbCC can
guarantee`) as a guard.

**Frameless and push-only functions** (from `-O1`, not under `-g`). A
function with an empty frame, no saved registers, no calls other than
tail calls, at most four scalar parameters, and none of `alloca`,
varargs, sret, inline asm, `va_start` or frame-address operations has
no prologue at all. A function that meets the same conditions but makes
calls or saves registers, and has no value in a stack slot, no outgoing
stack arguments and no exception regions, gets only `push` instructions
(plus a pad push when needed for alignment) and no frame pointer. This
is ELF-only (Mach-O's compact unwind expects an `rbp` frame) and not used
under Win64.

**The dead-slot guard.** A slot that should never be accessed (a local
in a register, a temp that appears nowhere, a value with an FP home) is
given the displacement `X86_DEAD_SLOT` (`-0x40000000`). Every
`rbp`-relative ModRM goes through `no_dead_slot`, which stops the
compilation if that displacement is used, or if any frame access is
made in a function with no frame pointer:

```text
internal error: a value was read from a stack slot it does not have -- it lives in a register, and the lowering of `OP` does not know that
internal error: a frame access in a function that has no frame pointer -- rbp is the caller's here, and the lowering of `OP` does not know that
```

`OP` is the IR operation being lowered (`x86_lowering_op`). The guard
also fires on writes.

### Prologue and epilogue

Each function is aligned to 16 bytes with `nop` padding, except at
`-Os`. The prologue is `push rbp; mov rbp, rsp`, the callee-saved
pushes, and `sub rsp, N`. A variadic function then saves rdi-r9 and
(unless `-mno-sse`) xmm0-xmm7 to the save area; under Win64 it spills
rcx-r9 to the home area. Incoming parameters are moved to their
allocated registers with one parallel move per class.

The epilogue restores the callee-saved registers (`lea` to the save area
and `pop`s, or a load when there is one) and returns with `leave; ret`
(or `ret`). With registers to restore and more than one return, every
return jumps to one shared epilogue.

Unwind information is `.eh_frame` (`src/debug/eh.c`) on ELF and
`__compact_unwind` on Mach-O. C++ exceptions are refused for Windows,
because `.pdata`/`.xdata` are not written.

**Tail calls** require a direct call with no struct return, a caller
that is not variadic, has no `alloca`, no exception regions, takes no
address of a local and uses no computed `goto`, no stack or
by-reference arguments, the Win64 ABI not in use, and the call's result
returned directly. The call becomes the epilogue followed by `jmp`.

### Calling conventions

**System V.** Arguments are classified in the shared IR generator
(`src/ir/irgen.c`, using `ty_classify` in `src/sema/type.c`), which
records for each argument its class, register or stack offset, and
whether it is passed by reference. Integer arguments use rdi, rsi, rdx,
rcx, r8, r9; floating-point arguments xmm0-xmm7, each file with its own
counter. A struct goes on the stack whole unless every eightbyte fits in
registers. MEMORY-class results are returned through a hidden pointer in
rdi, which is returned in rax. A variadic call sets al to the number of
vector registers used. `va_list` points at the 24-byte `__va_list_tag`;
`va_arg` is generated by `irg_va_arg_sysv` in `x86_64/irgen.c`. A
structure is classified as an argument would be: when every eightbyte
finds an unread register of its class, the INTEGER eightbytes are read
from the general-register part of the save area and the SSE ones from
the vector part, 16 bytes apart; otherwise, and always for a MEMORY
structure, it is the next run of the overflow area. Its bytes are copied
into a slot semantic analysis gives the `va_arg` expression, as on every
target.

**Microsoft x64** (`target_win64_abi()`). Four argument slots, rcx, rdx,
r8, r9, with one position counter shared by integer and floating-point
arguments; 32 bytes of shadow space are always reserved, so the first
stack argument is at `rbp+48`. Aggregates other than 1, 2, 4 or 8 bytes
are copied by the caller and passed by address. A variadic double is
also passed in the integer register of its slot. A variadic callee spills
rcx-r9 into the home area its caller reserved; `va_list` is a `char *`
that walks 8-byte slots from the first unnamed one, `va_arg` of a
`double` reads its slot, and `va_copy` is an assignment. `va_arg` of a
structure, of `long double` or of `__int128` is refused. EmbCC's
Windows target does not yet preserve rsi, rdi, xmm6 and xmm7 across
calls, and its `long` and `wchar_t` sizes are those of System V; the
`-Wwindows-abi` warning says so on every Windows compile.

**Darwin** uses System V; the backend differences are no push-only
frames, offset jump tables and compact unwind.

### Branches and jump tables

Branch relaxation: `codegen_unit` generates each function repeatedly.
Every branch starts in its short (`rel8`) form; one that does not reach
is marked long and the function is generated again. After 12 rounds
every remaining branch is long. A switch's default branch, a
compare-and-swap loop's branch, jumps to the shared epilogue and tail
jumps are always `rel32`. `tests/golden/branches.sh` checks convergence
and displacements at the limits.

`IR_SWITCH` is `cmp $n, %rax; jae default; lea table(%rip), %rcx`, with
the table in `.text` right after the dispatch:

- **ELF**: `jmp *(%rcx,%rax,8)` through a table of 8-byte absolute
  addresses, each an `RK_ABS64` relocation against the function's own
  symbol with the case label's offset as addend. EmbCC's code is
  position-dependent (`-fPIC` is refused), so the relocations cost
  nothing at run time.
- **Mach-O and COFF**: 4-byte offsets from the table, read with
  `movsxd`, added to the table address, and jumped through; no
  relocations.

The IR generator decides which switches become tables (`switch_dense`
in `src/ir/irgen.c`, the same for every target that has them): at least
4 cases, a range of at most 4096 values, and the range at most `4n+4`
for `n` cases (at `-Os`, at least 6 cases and a range at most `2n`). A
switch on a value wider than a pointer stays a compare tree.

### Inline and file-scope assembly

Extended inline asm (AT&T syntax) is assembled by `x86_64/irgen.c`
(`irg_asm_x86`, `asm_assemble`). Operands are resolved from their
constraints (fixed `a`, `b`, `c`, `d`, `S`, `D`; allocatable `r` from
rax, rcx, rdx, rbx, rsi, rdi, r8-r11; `x` for xmm0-xmm7; `m`; `i`).
Clobbered registers and registers the template names are not used for
operands. A callee-saved register an operand names is marked used, so
the prologue saves it.

File-scope `__asm__` is assembled by `topasm.c` on x86-64 and AArch64:
the directives (`.global`, labels, `.byte`, `.long`, `.quad`) work on
both; the mnemonic half (`and`, `call`, `jmp`, `ret`) is x86-64 only. On
Cortex-M, RISC-V and AVR the driver hands each block to the GNU-syntax
assembler instead (`gas_assemble_block` in `src/as/gas.c`), which
returns its bytes, labels, relocations with their ELF types, alignment
and data ranges in the same `struct topasm`. A naked function becomes
such a block before code generation (`naked_to_blocks` in
`src/driver/main.c`): its label, then its asm statements with their
constant operands written in.

`as.c` is EmbAS, the NASM-syntax assembler behind `embas` and
`embcc -c FILE.asm`; see [embas](../manual/tools/embas.md).
`disasm.c` is the AT&T disassembler used by `-S` and EmbDBG.

## AArch64

`src/arch/aarch64/codegen.c`, entry point `codegen_unit_arm64`. Targets
`aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` and
`aarch64-apple-darwin` (Mach-O). There is no Windows on ARM target.

### Lowering

`gen_func` handles the frame and the per-instruction `switch`; the
binary128 `long double` lowering (`gen_a64_ld`) and the `__int128`
lowering (`gen_a64_i128`) run before it. Operands are read with
`rd`/`rd_ext`/`rd_b` and written with `wr`/`wrote` (and `frd`/`fwr` for
floats); a spilled value is loaded into x9 (or x10 for the second
operand, v16/v17 for floats).

Instruction selection:

- **Immediates.** Constants are built with `movz`/`movk` or `movn`/`movk`,
  whichever is shorter; `add`/`sub`/`cmp` take a 12-bit immediate,
  optionally shifted by 12 (a negative one flips the operation); logical
  operations take bitmask immediates; float constants use `fmov` where
  the 8-bit form fits, `fmov d, xzr` for +0.0, and otherwise -- when
  `movz`/`movk` would take three or more instructions for a double, two
  for a float -- `ldr dN, <literal>` from the function's literal pool:
  each distinct value once, eight-aligned after the epilogue, marked as
  data (`$d`), and the loads patched once it is laid out
  (`a64_lit_load`, `a64_lit_flush`, `a64_fldr_lit`).
- **Compare and branch.** An `==`/`!=` compare with zero whose only use
  is the next branch is `cbz`/`cbnz`; any other such compare is `b.cond`.
- **`IR_SELECT`** is `cmp` of the condition with zero at its own width
  (`size`) and `csel`.
- **Shifted and extended operands.** A shift by a constant whose only use
  is the next add, subtract or logical operation becomes its shifted
  register operand, and an extension feeding an add or subtract becomes
  its extended register operand (`uxtw`, `sxtw` and the others). A
  multiply by `(2^k ± 1) << j` is a shifted add or a shift and subtract.
- **Addressing.** An add whose only use is the next access becomes a
  register-offset address (`[xn, xm]`, or `[xn, xm, lsl #k]` when the
  shift matches the access size); a constant offset folded by
  `ra_fold_memoff` uses the scaled 12-bit form or, for -256..255,
  `ldur`/`stur`.
- **Division** is `sdiv`/`udiv`; the remainder is the quotient in x11
  followed by `msub`.
- **Multiply-accumulate.** A product whose only reader is the add or
  subtract right after it is one `madd` (`c + a*b`) or `msub`
  (`c - a*b`), at 4 or 8 bytes; `a*b - c` stays two instructions.
- **binary128 `long double`** calls libgcc (`__addtf3`, `__subtf3`,
  `__multf3`, `__divtf3`, the `__eqtf2` family, `__floatditf`,
  `__fixtfdi`, the extend and truncate helpers), with values in q0/q1.
- **`__int128`**: add, subtract, bitwise operations, negation and
  comparisons are inline; multiply, divide, remainder, shifts and
  conversions call libgcc. `IR_CAS16` is an `ldxp`/`stxp` loop.
- **Atomics** are `ldxr`/`stxr` retry loops between `dmb ish` barriers;
  `IR_FENCE` is `dmb ish`.
- **Addresses** of strings are `adrp` + `add :lo12:`. A global or
  function that is undefined and either weak or on Darwin is reached
  through the GOT (`adrp` + `ldr`). TLS is local-exec (`mrs tpidr_el0` and two
  `add`s).
- **Vector operations are not lowered.** They reach the `default` case
  and stop with `aarch64 codegen: unhandled IR op N in 'NAME'`; the
  vectorizer is on only for x86-64.
- Under `-mgeneral-regs-only`, any floating-point operation stops with
  `floating point used under -mgeneral-regs-only (in 'NAME')`.

### Frame layout

Slots are addressed from the frame base with non-negative offsets: `sp`,
or x19 in a function with `alloca` (which moves `sp`). From the frame
base upward, `layout_frame` places the outgoing argument area, the
by-reference copy area, struct-return scratch space, the sret slot
(which keeps x8), the saved x19 (with `alloca`), the variadic save areas
(64 bytes for x0-x7, 128 bytes for q0-q7 unless
`-mgeneral-regs-only`, and the 32-byte `va_list` record; not on
Darwin), the locals, the shared temp slots, and the 16-byte slots of
wide temps. The callee-saved save area follows, and the frame record
(x29, x30) sits above it; incoming stack arguments are at `x29+16`.
Over-aligned locals are handled in sema as on x86-64, with the same
guard in `layout_frame`.

An offset that does not fit the scaled 12-bit form or the 9-bit unscaled
form is built in x12 (`mov x12, #off; add x12, base, x12`).

**The dead-slot guard.** A slot that should never be accessed gets the
offset `A64_DEAD_SLOT` (`0x40000000`). `a64_no_dead_slot` checks every
load, store and address computation based on the frame base, x29 or
`sp`, and stops on an offset in `[0x40000000, 0x40010000)` (the range
covers field offsets within the dead slot):

```text
internal error: aarch64: a value was read or written at a stack slot it does not have -- it lives in a register, and some lowering path does not know that
```

**Frameless leaves.** From `-O1` without `-g`, a function with an empty
frame, no saved registers, no `alloca`, no varargs, no exception
regions, no calls other than tail calls, no helper calls, no inline asm
and no stack or by-reference parameters gets no frame record: no
prologue, and `ret` as its epilogue.

### Prologue and epilogue

Each function is aligned to 16 bytes with `nop`, except at `-Os`. The
prologue is `stp x29, x30, [sp, #-16]!`, `mov x29, sp`, and
`sub sp, sp, #N` (through x12 when `N` does not fit). Then the
callee-saved registers are stored (`stp` pairs where the offset allows),
x19 is set up for `alloca`, the variadic registers are saved, x8 is
stored to the sret slot, and parameters are moved to their homes: first
the register-arrived ones (one integer and one floating-point parallel
move), then the stack-arrived ones.

There is one epilogue at the end of the function: restore the
callee-saved registers, reset `sp` (`add sp`, or `mov sp, x29`), `ldp
x29, x30, [sp], #16`, `ret`.

CFI (`.eh_frame`) describes the frame record and each saved register; a
frameless function's FDE has no instructions. Mach-O gets
`__compact_unwind` entries instead.

**Tail calls** (from `-O1`, without `-g`) require a direct, non-variadic
call with no struct or float result, a caller that returns no struct or
float, is not variadic, has no `alloca` and no exception regions, takes
no local's address, does not use `va_start`, and every argument in a
register; and the caller has at most one return, unless its frame is the
frame record alone (no locals, no callee-saved registers), when each
tail call's own teardown is one `ldp` -- or nothing, when every call is
a tail call and the function needs no frame. The call becomes the
callee-saved restores, the frame teardown and `b`.

### Calling conventions

**AAPCS64.** Placement (`a64_place_info`, `a64_place_arg`) is computed
by the backend, shared by call sites, the prologue, `va_start` and the
allocator's hints. Eight x and eight v registers with separate
counters, and no back-filling once one is exhausted. A float takes the
next v register; a homogeneous floating-point aggregate (1 to 4 members
of one floating type) takes one v register per member if all fit; a
composite over 16 bytes that is not an HFA is passed as a pointer to a
caller-made copy; an `__int128` takes an even-numbered x pair; a
composite with 16-byte natural alignment starts at an even register;
other composites take consecutive x registers. Stack slots are at least
8 bytes, aligned by natural alignment. A result over 16 bytes is
returned through x8. Variadic functions fill the 32-byte `va_list`
record (`__stack`, `__gr_top`, `__vr_top`, `__gr_offs`, `__vr_offs`).
`va_arg` of a structure (`va_struct_aapcs`) reads an HFA from the v save
area, one member per 16-byte register slot; a composite over 16 bytes as
the pointer it was passed as; any other from the x save area, starting
at an even register when its natural alignment is 16; and one that no
longer fits in registers from the stack, in whole doublewords.

**Darwin arm64.** Named stack arguments are packed at their own size and
alignment, there is no even-register rounding, every variadic argument
goes on the stack in 8-byte slots (aligned to 16 for a type with
16-byte alignment), and `va_list` is a `char *`, so `va_copy` is an
assignment. The data model differs too (signed `char`, `int` `wchar_t`,
8-byte `long double`), and an unnamed bit-field does not raise a
structure's alignment. `irg_va_arg_darwin` walks the pointer in 8-byte
slots; a structure takes whole doublewords, starts at a 16-byte boundary
when its type is 16-byte aligned, and is read through a pointer when it
is over 16 bytes.

### Branches and jump tables

There is no branch relaxation. `b.cond` and `cbz`/`cbnz` reach ±1 MiB,
`b` and `bl` ±128 MiB; a displacement out of range stops with an
internal error (`aarch64 conditional branch displacement cannot encode
N`). `tbz`/`tbnz` are not used.

`IR_SWITCH` is `cmp`, `b.hs default`, `adr x11, table`, `ldrsw x12,
[x11, wI, uxtw #2]`, `add x12, x11, x12`, `br x12`, with the table of
4-byte offsets from its start inline in `.text` and no relocations. The
table is marked with `code_mark_data`.

### Inline asm

`aarch64/irgen.c` (`irg_asm_arm64`) resolves operands: constraints `r`,
`g`, `i`, `n`, register variables bound with `register ... __asm__("xN")`
(x0-x11 and x13-x15 only), and the template forms `%N`, `%wN`, `%xN`,
`%[name]`. Operand registers come from x9, x10, x11, x13, x14, x15, then
x0-x8, never a callee-saved register; a template that names or clobbers
a callee-saved register is refused. `asm.c` assembles the template text:
hints, barriers, `mrs`/`msr` with a table of system registers, `tlbi`,
`dc`, `ic`, exception-generating instructions, `ldr`/`str`, and `.inst`.
Labels in inline asm are refused.

## Thumb (ARMv7-M and ARMv8-M Mainline)

`src/arch/thumb/codegen.c`, entry point `codegen_unit_thumb`. Targets
`thumbv7m-none-eabi`, `thumbv7em-none-eabi`, `thumbv8m.main-none-eabi`
and their `-eabihf` forms. Code generation is the same at both
architecture levels; ARMv8-M differs in its build attributes, its
predefined-macro table and its FPU (`fpv5-sp-d16`).

### Lowering

`gen_ins` lowers 32-bit operations, `gen_ins64` 64-bit ones, and
`cmp64` 64-bit comparisons. Operands are read with `rdr`/`rd` and
written with `wreg`/`wr`/`wrote`; `rd64`/`wr64` handle register pairs.

- **16-bit forms.** Flags are dead between IR instructions, so the
  encoder uses the 16-bit flag-setting forms (`movs`, `adds`, `subs`,
  register ALU operations, shifts, `muls`) whenever the operands are low
  registers. The low-scratch mechanism (`lo_free`) computes in a free
  low register to make that possible.
- **Constants** are `movs`, `movw`, `mov.w` with a modified immediate,
  or `movw`+`movt`. There are no literal pools. Addresses are always
  `movw`/`movt` (`RK_THM_MOVW`/`RK_THM_MOVT`).
- **Immediates**: add and subtract use the 16-bit forms, then
  `addw`/`subw` (±4095), then a modified immediate; an AND mask of low
  bits that is not a modified immediate becomes `ubfx`.
- **Shifted operands and addressing.** A constant shift with one use
  followed by an add, subtract or logical operation is its shifted
  operand (`rsb` for `(a<<k)-b`); with LSL up to 3 and an add used only
  as the next access's address it becomes `ldr/str rt, [rn, rm, lsl
  #k]`. An add of two registers used only as the next access's address,
  with no folded constant offset, is `ldr/str rt, [rn, rm]` (16-bit in
  low registers). Multiply by `(2^k ± 1) << j` uses `add`/`rsb` with a
  shifted operand.
- **Compare and branch** fuse when the compare's only use is the next
  branch. `IR_BRZ`/`IR_BRNZ` on a low register whose target is 0 to 126
  bytes ahead is `cbz`/`cbnz`.
- **A comparison's 0/1** into a low register is `ite c; mov d, #1; mov
  d, #0` (six bytes). This is the only IT block the backend emits;
  `IR_SELECT` uses a compare and branches, testing the condition at its
  own width (`size`): an 8-byte condition is the OR of its two halves.
- **Divide** is `sdiv`/`udiv`; the remainder is the quotient followed by
  `mls`.
- **Multiply-accumulate.** A 32-bit product whose only reader is the add
  or subtract right after it is one `mla` (`c + a*b`) or `mls`
  (`c - a*b`); `a*b - c` stays two instructions.
- **64-bit integers** use register pairs: `adds`/`adc`,
  `subs`/`sbc`, per-half logic, `umull`/`mla` for multiply, shifts by a
  constant or (branching on count ≥ 32) by a variable. A 64-bit divide
  calls `__divdi3`, `__udivdi3`, `__moddi3` or `__umoddi3`. An AND, OR or
  XOR with a constant takes each half on its own (`logic_half`): all
  ones or zero is a copy, a zero or a `mvn`; a modified immediate or its
  complement is `and`/`orr`/`eor` or `bic`/`orn`; a mask of low bits is
  `ubfx`; anything else is built in r10. A shift by a constant goes from
  the operand's pair straight into the result's, the bits crossing
  between the words being an `orr`'s shifted operand. A 64-bit shift
  right by 32 or more whose only reader is a 32-bit AND with a low mask
  is one `ubfx` of the high word, and so is a 32-bit shift right followed
  by such a mask. A branch on a 64-bit value is one `orrs` of the halves
  where they live. `EMBCC_T_NOWIDEIMM=1` turns these off.
- **Soft float** calls the libgcc names (not `__aeabi_*`):
  `__addsf3`/`__adddf3` and the rest of the arithmetic, the
  `__eqsf2`/`__eqdf2` family followed by a compare of r0 with 0, and the
  `__float*`, `__fix*`, `__extendsfdf2`, `__truncdfsf2` conversions.
  Negation flips the sign bit inline. `__builtin_sqrt` without an FPU is
  refused.
- **With an FPU** (`-mfpu=fpv4-sp-d16` or `fpv5-sp-d16` with
  `-mfloat-abi=softfp` or `hard`, or an `-eabihf` triple),
  single-precision arithmetic, negation, square root, comparisons
  (`vcmp`/`vcmpe` and `vmrs APSR_nzcv`) and int32 conversions run on the
  FPU, on values in s16-s31. Doubles still use the soft-float helpers.
- **Atomics** are `dmb`, an `ldrex`/`strex` loop (status in lr), `dmb`;
  compare-and-swap adds `clrex` on failure. Atomics wider than four bytes
  are refused (`an atomic wider than four bytes (ARMv7-M has no doubleword exclusive; GCC calls libatomic for these)`).
- **`IR_ALLOCA`** (VLAs, `__builtin_alloca`, and locals aligned beyond 8
  bytes) subtracts the size rounded to 8 from `sp`.
- **Byte swaps** arrive as shifts and masks: IR generation emits no
  `IR_BSWAP` for ARMv7-M.
- **Refused**: computed `goto`, exception landing pads, 128-bit values,
  `long double` operations the target lacks, and anything else not
  handled, with the backend's refusal message. `IR_UD2` is `udf #0`.

### Frame layout

The frame is `sp`-relative. A function with `alloca` uses r7 as the
frame base (`mov r7, sp` after the frame is set up). From `sp` upward
(`layout`): the outgoing argument area, the shared 4-byte temp slots,
64-bit temps that did not get a pair, locals of up to 8 bytes, then
larger locals, scratch space, and the sret slot; the total is a
multiple of 8. This order keeps the most-used slots within the reach of
the 16-bit `ldr`/`str rt, [sp, #imm]` forms (1020 bytes). Above the
frame are the pushed registers and, in a variadic function, r0-r3.

Slots beyond an instruction's reach are addressed by building the
address (`add sp`, `addw`, or `movw` and `add`); an access that still
cannot be made is an internal error rather than a dropped access. Block
copies are straight-line up to 4092 bytes and a loop beyond.

### Prologue and epilogue

Functions are halfword-aligned (4-byte aligned when they contain inline
asm), padded with `nop`. The prologue is `push {r0-r3}` in a variadic
function, `push {mask}`, `vpush {s16-...}` with an FPU, `sub sp, #N`,
`mov r7, sp` with `alloca`, the sret pointer stored, and the parameters
placed. The push mask contains the used scratch registers among r9-r11,
the allocator's callee-saved registers, and lr; when the register count
is odd, r3 is added so that `sp` stays 8-byte aligned. `push` and `pop`
use the 16-bit form when the mask allows. The epilogue undoes the frame
and returns with `pop {..., pc}` (`pop`, `add sp, #16`, `bx lr` in a
variadic function).

A leaf that saves nothing, has no frame, is not variadic and has no
`alloca` emits no `push` at all and returns with `bx lr`.

**Tail calls** (from `-O1`, without `-g`) require a direct, non-variadic
call with no struct, float or VFP result, a caller with no struct or
float result, not variadic, no `alloca`, no exception regions, no
`IR_ADDR` and no `va_start`, every argument in registers, and the result
(at most 4 bytes) returned directly. The tail call is emitted as `b.w`
only in a function that pushes nothing.

**Interrupt handlers.** `__attribute__((interrupt))` is accepted and
changes nothing: the Cortex-M hardware stacks r0-r3, r12, lr, pc and
xPSR on exception entry, so an ordinary function is a handler and `bx
lr` with EXC_RETURN is the interrupt return.

### Calling convention

**AAPCS32 core** (`place_arg`): r0-r3, then the stack. A value with
8-byte alignment starts at an even register and an 8-aligned stack
offset. A composite may be split between r3 and the stack only while
nothing is on the stack yet. A composite's alignment is its natural
alignment. A composite over 4 bytes is returned through a hidden pointer
in r0; one of at most 4 bytes in r0.

**AAPCS-VFP** (hard-float ABI, or `pcs("aapcs-vfp")`). Floats, doubles
and homogeneous aggregates of 1 to 4 of them go in s0-s15 (d0-d7),
allocated lowest-first with back-filling; after one spills to the stack,
no further one uses VFP registers. Results go in s0/d0 (s0-s3/d0-d3 for
an aggregate). A variadic call always uses the base convention.
`pcs("aapcs")` selects the base convention for one function;
`pcs("aapcs-vfp")` without an FPU is an error.

**Float ABI selection** (`arm_float_resolve` in the driver):
`-mfloat-abi=soft` (the default) emits no FPU instructions;
`softfp` uses the FPU with the base convention; `hard` uses the FPU and
AAPCS-VFP. `-mfpu=` accepts `fpv4-sp-d16` (ARMv7E-M) and `fpv5-sp-d16`
(ARMv8-M).

Register arguments, pair halves, and the operands of 64-bit and
soft-float helper calls are placed with `ra_parallel_move` (r9 breaks
cycles). `va_list` is a bare pointer; `va_arg` walks it in 4-byte
steps, and an argument with 8-byte alignment (for a structure, its
natural alignment) starts at a doubleword boundary.

### Encoder and build attributes

`emit.c` writes halfwords; a 32-bit instruction is two halfwords. Each
encoder picks the 16-bit form when the operands fit. Modified
immediates are found by searching all encodings. The VFP encoder
handles the opposite register-number splits of singles and doubles.

`attrs.c` writes `.ARM.attributes`: the CPU name and architecture
(`7-M`, `7E-M` or `8-M.MAIN`), the M profile, Thumb-only ISA use, the FP
architecture and `HardFP_use` when there is an FPU, `ABI_VFP_args` for
the hard-float ABI, and the ABI tags (`wchar_t` 4, 8-byte alignment
needed and preserved, `enum_size`, R9 use, unaligned access). EmbLD
checks these when it links Thumb objects.

### Branches and jump tables

Relaxation (from `-O1`) runs up to three passes. Pass 0 emits every
branch in its 32-bit form and saves all of r9-r11; it records which
branches would fit 16 bits (`b`: -2048 to +2046; `b<c>`: -256 to +254)
or a `cbz`. Pass 1 emits those short and saves only the scratch
registers pass 0 used. Pass 2 runs only if pass 1 used a scratch
register it did not save. After allocation, `b<c> L1; b L2; L1:` is
merged into `b<!c> L2` (`invert_last_bcond`); a merged branch is never a
`cbz`. If any conditional branch is beyond ±1 MiB, every conditional
jump becomes `b<!c>` over an unconditional `b` (far mode).
`tests/golden/thumb-relax.sh` and `thumb-far.sh` test the limits.

`IR_SWITCH` is `cmp rI, #n; bhs default`, then:

- when every case label lies ahead: `tbh [pc, rI, lsl #1]` and a table
  of halfword offsets (an entry that cannot reach restarts the function
  without `tbh` for that switch);
- otherwise: `adr.w r11, table; ldr.w r12, [r11, rI, lsl #2]; add r12,
  r11; bx r12`, a `nop` to align, and a table of word offsets with the
  Thumb bit set.

Tables are marked with `code_mark_data`, and the object carries `$t` and
`$d` mapping symbols around them.

### Inline asm

Operands come from r0-r3 and r12; naming or clobbering r4-r11 in a
template is refused. `asm.c` assembles the CMSIS vocabulary: `mrs`/`msr`
with the M-profile special registers, `cpsid`/`cpsie`, barriers, hints,
`bkpt`, ALU and shift instructions, loads and stores, `ldrex`/`strex`,
multiply and divide, `clz`/`rbit`/`rev`, compares, branches,
`push`/`pop`, `movw`/`movt`.

## RISC-V

`src/arch/riscv/codegen.c`, entry point `codegen_unit_riscv`, one
backend for `riscv32-unknown-elf` (ILP32) and `riscv64-unknown-elf`
(LP64). Where the widths differ it reads `target_xlen()`. The ISA is
RV32IMAC or RV64IMAC: integer, multiply and divide, atomics, compressed
instructions. There is no `-march=` option and no hardware floating
point; the ABI is soft-float (`ilp32`, `lp64`).
`.riscv.attributes` records the ISA string (`rv32i2p1_m2p0_a2p1_c2p0`
or the RV64 equivalent) and the 16-byte stack alignment.

### Lowering

`gen_ins` dispatches each instruction: floating-point operations become
helper calls, operations wider than 64 bits are refused, and at RV32
64-bit operations go to `gen_ins64`. At RV64 a 32-bit operation uses the
`W` forms (`addw`, `subw`, `sllw`, `mulw`, `divw` and the others).
Operands are read with `rdr` and written with `wreg`/`wrote`;
`rd64`/`wr64` handle RV32 pairs.

- **Constants** (`rv_li`): `addi` for 12 bits, `lui`+`addi` (or
  `addiw` at RV64) for 32 bits, a shift-and-add chain beyond.
- **Addresses** of strings, globals and functions are always
  `auipc`+`addi` with `R_RISCV_PCREL_HI20`/`R_RISCV_PCREL_LO12_I`, at
  both widths: the medany code model. `lui` sign-extends bit 31 at RV64
  and cannot reach an image at `0x80000000`. The `LO12` relocation names
  the `auipc`'s own address (against the `.text` section symbol), as the
  psABI requires.
- **Calls** to a function in the same unit are `jal ra` while every such
  call reaches (±1 MiB); if one does not, the whole unit is regenerated
  with `auipc`+`jalr`. Calls to other units are `auipc ra; jalr ra` with
  `R_RISCV_CALL_PLT`. `EMBCC_RV_LONG_CALLS` always uses `auipc`+`jalr`,
  and `EMBCC_RV_JAL_RANGE` reduces the assumed `jal` reach, for testing.
- **Compare and branch** fuse when the compare's only use is the next
  branch; a compare with 0 uses x0. A compare whose result is kept as a
  value takes a folded constant that fits 12 bits as an immediate
  (`cmp_imm_to_reg`): `slti`/`sltiu`, `x <= k` as `x < k + 1` and `x > k`
  its inverse, `==` and `!=` an `xori` and a `seqz`/`snez`. Against zero
  that is the `seqz`, `snez` or `slt` alone. `EMBCC_RV_NOCMPIMM=1` loads
  the constant into a register instead.
- **64-bit operations with a constant** at RV32 take each half on its
  own (`logic_half`): all ones or zero is a copy, a zero or an `li -1`; a
  12-bit immediate is `andi`/`ori`/`xori`; a mask of low bits is
  `slli`+`srli`, and one of high bits `srli`+`slli`; anything else is
  built in t4. A shift by a constant goes from the operand's pair
  straight into the result's (`shift64_imm_to`). A branch or select on a
  64-bit value ORs the halves where they live. A 64-bit compare with
  zero whose only reader is the next branch is that OR and a
  `beqz`/`bnez`, or, for a signed `<` or `>=`, a `bltz`/`bgez` of the
  high word. `EMBCC_RV_NOWIDEIMM=1` turns these off.
- **Multiply and divide** use the M extension (`mul`, `div`, `rem` and
  the `W` forms). At RV32 a 64-bit multiply is `mul`/`mulhu`; a 64-bit
  divide calls `__divdi3`, `__udivdi3`, `__moddi3` or `__umoddi3`.
- **Soft float**: arithmetic calls `__addsf3`/`__adddf3` and the rest,
  comparisons the `__eqsf2` family, conversions `__floatsisf`,
  `__fixdfsi` and the others; negation flips the sign bit inline.
  `__builtin_sqrt` and `long double` arithmetic are refused.
- **Atomics** (A extension): `amoswap.aqrl`, `amoadd.aqrl`,
  `amoand`/`amoor`/`amoxor.aqrl`; NAND and compare-and-swap use
  `lr.aq`/`sc.rl` loops. Atomics must be register-width (or 4 bytes at
  RV64); narrower ones are refused. `IR_FENCE` is `fence rw, rw`.
- **`IR_ALLOCA`** (VLAs, `__builtin_alloca`, and locals aligned beyond 16
  bytes) subtracts the size rounded to 16 from `sp`.
- **`IR_SELECT`** loads one arm or the other around a branch on the
  condition, tested at its own width: at RV32 an 8-byte condition is the
  OR of its halves, and at RV64 a 4-byte one is read through `rd32`.
- **Byte swaps** are `IR_BSWAP` only at RV64; at RV32 IR generation
  builds them from shifts and masks.
- **Block copies** are straight-line up to 2040 bytes and a loop beyond.
- **Refused**: computed `goto` and 128-bit values. `IR_UD2` is `unimp`.

**32-bit values at RV64.** The psABI keeps a 32-bit value in a register
as its sign extension, unsigned values included (`0xffffffffu` is all
ones). The `W` forms and `lw` produce that, but EmbIR reads a narrowed
value as the same temp at width 4, and `lwu`, zero-extending reads and
zero extensions do not. Each reader that compares or passes all 64 bits
takes a width-4 operand through `rd32`, which adds `sext.w` unless the
value is already sign-extended: compares and fused compare-and-branch,
`IR_BRZ`/`IR_BRNZ`, an `IR_SELECT` condition of 4 bytes, the bound check
of `IR_SWITCH`, the expected value of a compare-and-swap (compared with
what `lr.w` sign-extended), an `int` converted to floating point, a
4-byte return value, and 4-byte integer arguments in registers and on
the stack, as clang and gcc pass them. `sext_map` decides which values
are already sign-extended, to a fixed point: the `W` arithmetic,
compares, constants that fit 32 bits signed, signed 4-byte and all
narrower loads and extensions, a call returning an integer of at most 4
bytes, `__fix*si` results, an `and` with a constant from 0 to 2^31-1
(whatever its other operand), and `and`/`or`/`xor`/`not`, copies and
selects of such values.

### Frame layout

The frame is `sp`-relative; s0 is the frame base only in a function with
`alloca`. From `sp` upward (`layout`): the outgoing argument area, the
by-reference copy area, the shared temp slots, RV32 64-bit temps without
a pair, locals of up to two registers' width, larger locals (arrays
last), scratch space, the sret slot, the callee-saved saves, ra (absent
in a leaf), and in a variadic function the a0-a7 save area at the top,
next to the caller's stack arguments. The frame is a multiple of 16.
Values with a register home get no slot; a lowering that asks for the
slot of such a value stops with `riscv: NAME: a path addresses vreg N's
slot, and it has none`. Offsets beyond 12 bits are built in t6.

A leaf (no non-tail calls, no inline asm, no helper calls) saves no ra
and, with nothing else in the frame, never moves `sp`.

### Prologue and epilogue

The prologue adjusts `sp`, stores ra and the callee-saved registers,
sets up s0 for `alloca`, spills a0-a7 in a variadic function, and places
parameters with one `ra_parallel_move` (t4 breaks cycles) followed by
loads of stack-arrived parameters. Every return jumps to the single
epilogue, or falls into it when nothing but labels lies between; the
epilogue restores, adjusts `sp` and returns with `ret` (`c.jr ra`). It
is omitted when the function ends in a tail call and nothing jumps to it.
Functions are not padded when the C extension is on (always, at
present).

**Tail calls** (from `-O1`, without `-g`) require a direct, non-variadic
call with no struct or float result, a caller that returns no struct or
float, has at most one return, no `alloca`, no varargs, no exception
regions, no `IR_ADDR` and no `va_start`, and every argument in
registers. They are `jal x0` within the unit or `auipc t1; jalr x0`.

Interrupt handlers are refused on RISC-V
(`__attribute__((interrupt)) is not supported: ...`).

### Calling convention

`place_arg` implements the RISC-V psABI integer convention: a0-a7, then
the stack. A struct larger than two registers is passed by reference to
a caller-made copy. A two-register scalar or aggregate takes two
consecutive registers; only a variadic argument with 2×XLEN alignment is
rounded to an even register (and to 2×XLEN on the stack), a named one is
not. A value split by the end of the register file puts its low word in
a7 and its high word on the stack. A stack argument is aligned to its
type, between XLEN and 16 bytes. Small aggregates are packed by bytes
into registers. A composite over two registers is returned through a
hidden pointer in a0; smaller ones and 64-bit scalars at RV32 come back
in a0/a1. At a call, by-reference copies and stack words come first,
then one parallel move for register arguments, then struct words, and
the sret pointer last. `va_list` is a bare pointer. `va_arg` of a
structure of up to two registers copies its words from the walk; a
larger one was passed by reference, and its pointer is followed.

### Encoder

`emit.c` has one packer per instruction format (R, I, S, B, U, J), each
with range checks that stop with an internal error rather than truncate.
Every instruction passes through `rv_w`, which substitutes the
compressed form from `rv_compress` when one exists and compression is
on (c.addi, c.li, c.mv, c.lui, c.add, c.sub and the other register
forms, c.lw/c.ld/c.sw/c.sd and their `sp` forms, c.addi16sp,
c.addi4spn, c.jr/c.jalr, and others). Compression is suppressed where a
fixed size is needed: branch and jump placeholders, `auipc` pairs (so the
`LO12` relocation's instruction is 4 bytes), call placeholders, jump
table dispatch, and `unimp`.

### Branches and jump tables

Pass 0 emits every jump as `jal x0` and every conditional branch as an
inverted branch over a `jal` (8 bytes). From the measured distances,
pass 1 (when the allocator is on and something can shrink) uses `c.j`
(±2 KiB), `c.beqz`/`c.bnez` (a compare with zero of x8-x15, ±256
bytes), a plain branch (±4 KiB), or keeps the long form. A relaxed
branch that no longer reaches is an internal error.
`tests/golden/riscv-relax.sh` sweeps the limits.

`IR_SWITCH` is a bounds check (`bgeu` to the default), then
`auipc t1, 0; slli t2, rI, 2; add t2, t2, t1; lw t2, off(t2); add t2,
t2, t1; jr t2`, and a 4-byte-aligned table of 32-bit offsets from the
`auipc` in `.text`, resolved by the compiler with no relocations.

### Inline asm

Operands use ABI register names and come from t0-t6, then a0-a7. Naming
or clobbering a callee-saved register is refused. `asm.c` assembles
system instructions (`ecall`, `ebreak`, `mret`, `sret`, `wfi`, fences,
`fence.i`), the CSR instructions with a table of named CSRs (RV32-only
ones refused at RV64), integer, multiply, load, store and branch
instructions with numeric offsets, and the common pseudo-instructions.
AMOs and `lr`/`sc` are not in the vocabulary.

### Relocations

`RK_CALL` and `RK_TAIL` become `R_RISCV_CALL_PLT`; the address pair
`R_RISCV_PCREL_HI20`/`R_RISCV_PCREL_LO12_I`; data `R_RISCV_32` or
`R_RISCV_64`. EmbCC emits no `R_RISCV_RELAX` or `R_RISCV_ALIGN`.

## AVR

`src/arch/avr/codegen.c`, entry point `codegen_unit_avr`. Target `avr`
(the ATmega328P). Registers are 8 bits, pointers 16 bits, `int` 2 bytes,
and `double` and `long double` are the 4-byte float. Code and data are
separate address spaces.

### Lowering

The IR computes at 4 bytes and `int` is 2. IR generation keeps every
`int` value equal to its 16-bit extension (an `IR_EXT` after each
operation that can carry out of 16 bits; see
[EmbIR](ir.md#widths)), so a 4-byte read sees the C value. The backend
computes only the bytes something reads: `avr_demand` propagates,
backward to a fixpoint, how many bytes of each value are needed, and
comparisons of extended values are made at their original width
(`cmp_width`). A compare whose 0/1 is used only by the next branch fuses
with it.

- **Constants** (`ldi4`): a zero byte is `mov r, r1`; registers r16 and
  up take `ldi`; a nonzero byte for r0-r15 goes through r31 (`push r31;
  ldi r31, b; mov r, r31; pop r31`).
- **Multiply** by a constant below 2^17 is shift-and-add in place;
  otherwise it calls `__mulsi3`. The `mul` instruction is not emitted by
  code generation.
- **Divide and remainder** call `__divsi3`, `__udivsi3`, `__modsi3` and
  `__umodsi3` (arguments in r22-r25 and r18-r21, result in r22-r25); the
  64-bit forms `__muldi3`, `__divdi3` and the others.
- **Shifts** by a constant move whole bytes, then shift single bits; by a
  variable they loop.
- **Floats** are all helper calls: `__addsf3`, `__subsf3`, `__mulsf3`,
  `__divsf3`, the `__eqsf2` family, and the `__floatsisf`/`__fixsfsi`
  family. Negation flips the sign bit inline; float `%` is refused.
- **64-bit values** are processed a byte at a time.
- **Loads and stores** through a pointer use Z; constant offsets up to 63
  (folded by `ra_fold_memoff`) go into `ldd`/`std`. Stores write the high
  byte first. Z keeps a pointer between accesses while nothing has
  written it (`z_holds`, checked by decoding the bytes emitted since).
- **`IR_SELECT`** is a branch and copies. The condition's low four
  bytes are ORed and tested whatever its `size`, so the high half of an
  8-byte condition is ignored (a defect; see
  [Status](status.md#known-defects)). **`IR_MEMCPY`/`IR_MEMZERO`** are
  byte loops with Z as the destination and X as the source.
  **`IR_FENCE`** emits nothing; **`IR_UD2`** is `rjmp .-2`.
- **Refused** with the backend's message: 128-bit values, a memory
  access wider than four bytes, a VLA, an exception region, an aligned
  local, returns wider than 8 bytes, and every operation without a
  lowering by its IR name (`switch`, `igoto`, `labeladdr`, `bswap`,
  atomics wider than one byte). IR generation does not emit `IR_BSWAP`
  for AVR; a byte swap arrives as shifts and masks.

The compiler never emits `lpm`: the linker places `.rodata` in the data
segment, and the startup code copies `.data` and `.rodata` from flash.

### Frame layout

Y (r28:r29) is the frame pointer and points one byte below the frame, so
slots are at `Y+1` to `Y+frame`. Y is set up only when the function has
a frame, is variadic, has an sret slot, has stack-passed parameters, or
is an interrupt handler; a frame access in a function without it is an
internal error (`avr: NAME: a frame access in a function whose frame
pointer was not set up`).

The outgoing argument area comes first, at `Y+1`. Locals and temp slots
are then placed densest first: each object is weighted by the bytes of
code that access it, so the most-used ones fall within `ldd`/`std`
reach. Four-byte temps share slots through `ra_coalesce_temps`;
eight-byte temps share by an interval sweep. Incoming stack arguments are
at `Y+frame+5+2*nsave`.

**Far slots.** `ldd`/`std` reach Y+0 to Y+63. A slot ending by Y+127 is
reached by copying Y into Z, `adiw Z, min(off, 63)`, and `ldd`/`std`
with the rest of the offset. Beyond that, Y is copied into a walker
pointer (Z, or X when the value being moved occupies Z), the offset is
added with `subi`/`sbci`, and the bytes are moved with post-increment.
Both clobber SREG, so the `_cc` variants, used where flags must survive,
save SREG in r0 around the access.

`SP` is written from Y with interrupts disabled for the high byte:
`in r0, SREG; cli; out SPH, r29; out SREG, r0; out SPL, r28`.

### Prologue, epilogue and interrupts

The prologue pushes the call-saved pairs the function uses, pushes Y
(when used), reads `SP` into Y, subtracts the frame size, writes `SP`,
then places parameters: those without a home are stored to their slots
and those with a home are moved with one `ra_parallel_move`, emitted
with `movw` where two byte moves form an aligned pair. The epilogue
(`a_teardown`) reverses it and returns with `ret`. Every function also
saves the call-saved registers that loading a call's arguments would
overwrite (r8-r17, down to the lowest argument register used).

**Interrupt handlers.** `__attribute__((signal))` and
`__attribute__((interrupt))` are accepted. The handler saves r0, SREG,
r1, r18-r27, r30, r31 and Y, clears r1, and (for `interrupt`) executes
`sei`; it restores them and returns with `reti`. A handler with
parameters or a return value is refused. The compiler emits no vector
table; the startup code provides one (the test harness's `boot.S` has
weak `__vector_N` entries).

**Tail calls** require a direct, non-variadic, non-float call with no
struct result and every argument in registers; a caller that is not an
interrupt handler, returns no struct or float, is not variadic, has no
`alloca`, no exception regions, no `IR_ADDR` and no `va_start`; and the
result (at most 8 bytes) returned directly. They are made only when the
function allocated at least one home, when no argument register is one
the epilogue pops, and, if the function has other returns, when the
teardown is empty. The sequence is the teardown and `jmp`.

### Calling convention

The avr-gcc convention (`place_arg`). A cursor starts at r26 and moves
down by each argument's size rounded up to even, and the argument
occupies the registers from the new cursor upward: a 2-byte first
argument is in r24:r25, a 4-byte one in r22-r25. The cursor stops at r8;
an argument that does not fit, and every one after it, goes on the
stack, packed at natural size, with no back-filling. A variadic call passes every
argument on the stack. A result's size is padded to a power of two, and
it is in r24 (up to 2 bytes), r22-r25 (up to 4) or r18-r25 (up to 8); a
composite over 8 bytes is returned through
a hidden pointer passed as the first argument and returned in r24:r25;
the callee copies the value into that buffer with the source in Z and
the buffer in X, both walked with post-increment, so the size is not
limited by `ldd`'s reach.
The caller extends a narrow result as far as it is read. Struct
arguments are loaded through Z into registers, or copied to the stack; a
stack copy beyond `std`'s reach walks the source in X and the
destination in Z. All calls are `call` with a relocation, even within
the unit; indirect calls are `icall`. `va_list` is a bare pointer, and
`va_arg` walks it with no padding, structures included.

### Encoder

`emit.c` appends little-endian halfwords; a 32-bit instruction is two.
Operand ranges are checked, and an out-of-range register or immediate
stops with `avr: WHAT (got N) -- a wrong register here encodes a
DIFFERENT one and assembles cleanly`, because AVR's split operand fields
turn a wrong value into a different valid instruction. `avr_insn_writes`
decodes an instruction and reports which registers it writes; the
allocator's proof and the Z cache use it, and the referee tests it.
Function addresses are word addresses (`RK_AVR_LO8_LDI_GS`,
`RK_AVR_HI8_LDI_GS`, `RK_AVR_ABS16_PM`).

### Branches

`jump_to` and `jump_if` measure backward targets directly: a conditional
branch within -64 to +63 words, `rjmp` within ±2048 words, `jmp`
beyond. A forward jump starts long (an inverted branch over `jmp`, or a
bare `jmp`); `jmp` carries an absolute address, relocated with
`RK_AVR_TEXT_CALL` against `.text` plus the label offset.
`gen_relaxed` then regenerates the function up to 16 times, giving each
site a hint (long, `rjmp`, branch, or omitted when it jumps to the next
instruction); a short site that turns out not to reach is pinned long.

### No jump tables

`target_jump_tables()` is false for AVR, so the IR generator lowers
every `switch` as a balanced compare tree (equality tests at each node,
chains of up to four cases at the leaves). An `IR_SWITCH` that reaches
the backend is refused.

### Inline asm

Constraint letters follow avr-gcc: `d` (r16 and up), `a` (r16-r23), `w`
(r24, r26, r30), `e` (X or Z), `b` (Z), `r`, `x`, `z`, and the constant
letters. `y` is refused, because Y is the frame pointer. Operands come
from r18-r27 and r30-r31; r2-r17, r28 and r29 are refused in templates,
operands and clobbers. Modifiers `%A` to `%D` name an operand's bytes and
`%a` names it as a pointer. `asm.c` assembles the AVR5 instruction set
with `lo8`, `hi8`, `pm_lo8`, `pm_hi8` and `gs()` symbol operands.

## Debugging a backend

| Variable | Backend | Effect |
|---|---|---|
| `EMBCC_NO_TAILCALL` | AArch64, Thumb, RISC-V, AVR | no tail calls |
| `EMBCC_NO_MEMOFF` | Thumb, RISC-V, AVR | no constant-offset folding into accesses |
| `EMBCC_NO_RMW` | x86-64 | no read-modify-write fusion |
| `EMBCC_NO_MLA` | AArch64, Thumb | no multiply-accumulate fusion |
| `EMBCC_T_NOLO` | Thumb | no low-register scratch |
| `EMBCC_T_NOREGOFF` | Thumb | no `[rn, rm]` register-offset addressing |
| `EMBCC_T_FPU` | Thumb | override the FPU setting |
| `EMBCC_T_NOWIDEIMM` | Thumb | build a 64-bit AND/OR/XOR constant whole |
| `EMBCC_RV_NOWIDEIMM` | RISC-V | the same at RV32 |
| `EMBCC_RV_NOCMPIMM` | RISC-V | load a value compare's constant into a register |
| `EMBCC_RV_LONG_CALLS` | RISC-V | never use `jal` for calls |
| `EMBCC_RV_JAL_RANGE` | RISC-V | assume a shorter `jal` reach |

The register-allocation knobs (`EMBCC_RA_*`, `EMBCC_T_*`, `EMBCC_RV_*`,
`EMBCC_AVR_*`) are listed in
[Register allocation](register-allocation.md#debugging-and-stress-testing).

`embcc -S` prints the object's bytes as `.byte` directives, one
instruction per line (grouped by `target_insn_len`, or by the
disassembler on x86-64), with each relocation as an explicit `.reloc`;
assembling it reproduces the `-c` object.
