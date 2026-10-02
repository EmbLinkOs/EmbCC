# EmbIR

This page is the reference for EmbIR, the intermediate representation
between EmbCC's front end and its backends. It describes the data
structures in `src/ir/ir.h`, every opcode, the textual form that
`embcc inspect ir` prints and reads back, and the invariants the
optimizer and the backends rely on. It is written for people who change
IR generation, add a pass, or teach a backend a new operation.

## Overview

EmbIR is linear three-address code over virtual registers ("vregs").
One `struct ir_unit` holds one translation unit. Each function is an
array of `struct ir_ins` in program order; control flow is expressed
with labels and branches inside that array. There is no separate
basic-block structure: the optimizer derives blocks and a control-flow
graph when it needs them.

EmbIR is not in SSA form. Expression temporaries are written once in
most cases, and the optimizer builds SSA on demand for `mem2reg` and
takes it down again before the next pass
(see [The optimizer](optimizer.md)).

Values are typed by width and a few flags rather than by a type graph:
an instruction says how many bytes it operates on, whether the
operation is signed, and whether it is a floating-point operation. The
ABI decisions that need C types (how an argument is passed, how a
structure is returned) are made during IR generation and recorded on the
instruction, so backends never consult the AST for them.

| File | Contents |
|---|---|
| `src/ir/ir.h` | The data structures and the public functions |
| `src/ir/irgen.c` | IR generation from the checked AST (`irgen`) |
| `src/ir/irgen_int.h` | The helpers the per-target halves of IR generation use |
| `src/arch/<arch>/irgen.c` | Target-specific lowering: `va_arg` and extended `asm` |
| `src/ir/irprint.c` | The textual form (`ir_print_unit`, `ir_opname`) |
| `src/ir/irparse.c` | The reader for the textual form (`ir_parse`) |
| `src/opt/opt.c` | The optimizer and the IR verifier (`verify_func`) |

## Where EmbIR sits in the pipeline

The driver calls `irgen(u)` on the unit that semantic analysis has
checked, then `opt_run(iu, level)`, then one backend's `codegen_unit*`
function. The DWARF emitter (`src/debug/dwarf.c`) and the unwind-table
emitter (`src/debug/eh.c`) read the same `struct ir_unit` after code
generation. See [Architecture](architecture.md#the-compilation-pipeline).

IR generation emits a function only if it is defined in the unit and is
either external, referenced (`used`), or named `main`. At `-O1` and
above, the driver removes static functions that are unreachable after
optimization before code generation; `inspect ir` reports the unit after
that step.

## Units, symbols and strings

### `struct ir_unit`

| Field | Meaning |
|---|---|
| `src` | The AST unit it was generated from. `NULL` in a unit read by `ir_parse`. |
| `syms`, `nsyms` | The symbol table: every function and object the IR names. |
| `funcs`, `nfuncs` | The functions, as an array of `struct ir_func`. |
| `strs`, `nstrs` | The string-literal pool. |
| `rodata_len` | The size of the pool in bytes. |

### `struct ir_sym`

Instructions refer to functions and objects by index into
`ir_unit::syms` (`callee_sym`, `glob_sym`). Symbols are interned by name
and kind, so a function named by several instructions has one entry.
`ir_sym_func` and `ir_sym_global` intern a symbol from an AST node and
return its index.

| Field | Meaning |
|---|---|
| `name` | The symbol name. |
| `is_func` | 1 for a function, 0 for an object. |
| `defined` | A definition exists in this unit. |
| `is_weak` | Declared weak. |
| `is_varargs` | Functions: declared with `...`. |
| `sret_first` | Functions: parameter 0 is the indirect-result pointer. |
| `is_nothrow` | Functions: no exception propagates out of it. |

The instructions also keep the AST pointers `callee` (a `struct func *`)
and `glob` (a `struct global *`) beside the indices. Backends and the
driver use the pointers to reach symbol-table indices and section
offsets; the textual form uses the indices.

### `struct ir_str`

Each string literal is one entry with its bytes, its length including
the terminating NUL, and `off`, its offset in `.rodata`. Offsets are
assigned in order as strings are interned and become section offsets
unchanged. `ir_intern_string` adds a string and returns its index; the
driver uses it for strings that global initializers point at.
`IR_STRADDR` names a string by index.

## Functions

### `struct ir_func`

| Field | Meaning |
|---|---|
| `name`, `file`, `line` | The function's name and where it is defined. |
| `is_static`, `is_varargs` | Linkage and variadic-ness. |
| `nparams` | Number of parameters. |
| `nvars` | Number of frame slots, parameters included. |
| `param_abi` | One `struct ir_arg` per parameter: how the prologue receives it. |
| `locals` | One `struct ir_local` per frame slot (`nvars` entries). |
| `ret_abi` | How the function returns its value. |
| `pcs` | ARM only: the function's `pcs` attribute. |
| `src` | The AST function. Holds `code_off`/`code_len`, which code generation fills, and the types the DWARF emitter needs. |
| `nvregs` | Number of vregs; all vreg numbers are below it. |
| `nlabels` | Number of labels; all label numbers are below it. |
| `scratch_bytes` | Caller-side storage for structure return values and, for Microsoft x64, by-reference argument copies. |
| `outgoing_bytes` | The largest stack-argument area any call in the function needs. |
| `has_i128` | The function computes with `__int128` values. |
| `has_alloca` | The function contains `IR_ALLOCA`; the stack pointer moves at run time. |
| `ins`, `nins`, `cap` | The instruction array. |
| `jt`, `njt` | The jump tables that `IR_SWITCH` instructions refer to. |
| `lines`, `nlines` | `-g`: (code offset, line) pairs, filled by code generation. |
| `dbgvars`, `ndbgvars` | `-g`: the source-level parameters and locals, filled by IR generation. |
| `var_off` | `-g`: each vreg's frame offset, filled by code generation. |
| `var_scope_lo`, `var_scope_hi` | Each local's lexical scope as a half-open range of instruction indices. Backends give locals with disjoint scopes one stack slot. |
| `eh`, `neh` | Exception regions (C++). |
| `eh_types`, `neh_types` | The catch types a landing pad's selector numbers, 1-based. |
| `csites`, `ncsites` | Code generation: the code range and region of every call in a function with exception regions. |

### Frame slots: `struct ir_local`

Each frame slot carries the facts about its C type that the optimizer
and the backends need, decided at IR generation. `ir_locals_fill`
computes them from the function's slot types; the inliner calls it
again after it adds slots.

| Field | Meaning |
|---|---|
| `size`, `align` | Size and alignment in bytes. |
| `user_align` | `__attribute__((aligned(N)))` on the variable; 0 when absent. |
| `is_volatile` | The slot is `volatile`. |
| `is_ldouble` | A `long double` wider than `double` (x87 or binary128). |
| `is_int128` | An `__int128`. |
| `is_int_or_ptr` | An integer or a pointer of any width. |
| `is_scalar_int_or_ptr` | An integer or pointer of 4 or 8 bytes. |
| `is_scalar_float` | A `float` or `double` of 4 or 8 bytes (not a wide `long double`). |

### Exception regions

`struct ir_eh` describes one exception region, produced from the C
front end's `STMT_EHREGION` (which the C++ lowering writes). `lo` and
`hi` are the region's instruction range, `lp_label` is the landing pad's
label, `parent` the enclosing region or -1, and `acts`/`nacts` the catch
and cleanup actions. A call inside a region records `1 + region` in
`eh_region`. Code generation fills `lp_off` and the call sites
(`ir_add_csite`); `src/debug/eh.c` turns them into `.gcc_except_table`.

## Virtual registers

Vregs are numbered from 0 to `nvregs - 1` in three ranges:

| Range | Meaning |
|---|---|
| `0 .. nparams-1` | The parameters, in order. |
| `nparams .. nvars-1` | The other frame slots: locals in declaration order and the hidden slots semantic analysis creates (compound literals, VLA sizes, saved stack pointers). |
| `nvars .. nvregs-1` | Temporaries. |

A vreg below `nvars` is a frame slot. `IR_LDVAR`, `IR_STVAR` and
`IR_ADDR` take a slot number, printed `vN`. A slot number used as an
ordinary operand (`%N` with `N < nvars`) names the value in that slot;
after `mem2reg`, a parameter that is never assigned is read this way,
as the value the prologue stored there.

Temporaries are values. Most are written by exactly one instruction.
The exceptions are deliberate:

- A *merge temp* receives a value on each path into a join. IR
  generation writes one for `?:`, `&&`, `||` and the register/stack
  choice in `va_arg`, with an `IR_MOV` or `IR_CONST` on each path.
- After optimization, phi copies from `mem2reg`, partial-redundancy
  elimination and loop transformations write some temps more than once.

Code that treats a temp as a known value must first check that it has
exactly one definition.

### Widths

Temporaries hold *promoted* values. An instruction's `w` field is the
width of the value it operates on or produces:

| `w` | Values |
|---|---|
| 4 | `int` class: everything narrower than 8 bytes |
| 8 | 8-byte integers and pointers, and `double` |
| 16 | `__int128`, a `long double` wider than `double`, and a 128-bit vector |

`w` is the IR's width class, not the target's register width: on AVR an
`int` operation still has `w` 4, and the backend narrows it. Pointer
arithmetic uses the target's pointer size (8 on the LP64 targets, 4 on
ARMv7-M and RV32).

Variables live in memory at their true size. `IR_LDVAR` and `IR_LOAD`
read `size` bytes and extend to `w` (sign-extending when `sign` is set);
`IR_STVAR` and `IR_STORE` truncate to `size` bytes.

## Instructions

### `struct ir_ins`

| Field | Meaning |
|---|---|
| `op` | The opcode. |
| `line`, `col` | Source location; `col` is 0 when unknown. |
| `synth` | The compiler invented the instruction; it corresponds to no source construct. |
| `dst`, `a`, `b`, `c` | Destination and operands: vreg numbers, slot numbers or -1. |
| `w` | Operation width class (4, 8 or 16). |
| `size` | Memory or source width in bytes, for the operations that have one. |
| `sign` | Signed variant: signed division, arithmetic shift, signed comparison, sign extension. |
| `flt` | Floating-point operation at width `w`. |
| `vol` | `LOAD`/`STORE`/`LDVAR`/`STVAR`: a `volatile` or atomic access. The optimizer never removes, merges or reorders it. |
| `imm` | `IR_CONST`'s value; the folded constant when `imm_b` is set; the operator of `IR_ARMW` and `IR_VBIN`. |
| `imm_b` | Operand `b` is the constant `imm`, not a vreg. Set only by the optimizer's immediate-folding pass, on `ADD`, `SUB`, `MUL`, `AND`, `OR`, `XOR`, `CMP`, `SHL` and `SHR`, never on a floating-point or 16-byte operation. |
| `pred` | `IR_CMP`'s predicate: `B_EQ`, `B_NE`, `B_LT`, `B_LE`, `B_GT` or `B_GE`. |
| `label` | The label of `IR_LABEL`, `IR_JMP`, `IR_BRZ`, `IR_BRNZ` and `IR_LABELADDR`; `IR_SWITCH`'s default; `IR_STRADDR`'s string index. |
| `jt` | `IR_SWITCH`: index into `ir_func::jt`. |
| `callee`, `callee_sym` | `IR_CALL` (direct) and `IR_FADDR`: the function. |
| `glob`, `glob_sym` | `IR_GADDR`: the object. |
| `indirect` | `IR_CALL` through a function pointer held in `a`. |
| `sret_first` | `IR_CALL`: argument 0 is the indirect-result pointer. |
| `call_varargs`, `call_nfixed` | `IR_CALL`: the callee is variadic, and how many parameters are named. |
| `call_pcs` | `IR_CALL`: the callee's ARM `pcs` attribute. |
| `memoff` | `LOAD`/`STORE`: constant byte offset added to the address. Set only by a backend's own pre-codegen pass (`ra_fold_memoff`); 0 everywhere else. |
| `natural` | `LOAD`/`STORE`: the address is known to be aligned for the access. 0 means not known. |
| `argv`, `nargs` | `IR_CALL`: the arguments, each a `struct ir_arg`. |
| `retsize`, `rety`, `retnclass`, `retcls`, `ret_hfa_n`, `ret_hfa_size`, `ret_byref`, `ret_x87`, `scratch` | `IR_CALL` returning a structure: its size, type and classification under each ABI, and the frame offset of the caller-side copy. |
| `ret_tybytes`, `ret_tysign` | `IR_CALL` returning a scalar: the return type's own size and signedness. `w` describes the return register instead. |
| `asm_ir` | `IR_ASM`: the assembled template and its operands. |
| `eh_region` | `IR_CALL`: 1 + the innermost exception region, or 0. |

`emit()` in `src/ir/irgen.c` creates an instruction with `dst`, `a`,
`b` and `label` set to -1, `w` and `size` set to 4 and `sign` set to 1,
and stamps it with the current source location. Fields that do not
apply to an opcode keep those defaults.

### Call arguments: `struct ir_arg`

Each argument of an `IR_CALL`, each entry of `param_abi`, and `ret_abi`
is classified during IR generation.

| Field | Meaning |
|---|---|
| `vreg` | The value, or the address of the value when `is_struct`. |
| `is_struct`, `size` | Aggregate or scalar, and its size in bytes. |
| `nclass`, `cls[2]` | System V classification by eightbyte (`CLASS_INTEGER`, `CLASS_SSE`, `CLASS_NONE`); `nclass` 0 means MEMORY. |
| `on_stack`, `stk_off` | The argument goes in the outgoing stack area, at this offset. |
| `align`, `nat_align` | Alignment, and the ARM procedure-call standards' natural alignment (`ty_natural_align`). |
| `is_float`, `is_int128` | Scalar kind. |
| `hfa_n`, `hfa_size` | AAPCS64 homogeneous floating-point aggregate: member count and member size. |
| `byref` | Passed as a pointer to a copy (AAPCS64 stage B.3, or Microsoft x64). |
| `copy_off` | Microsoft x64: offset of the caller's copy in its scratch area. |
| `ty` | The C type, for the classifications that read members. |

### Opcodes

The enumeration `enum ir_op` has 63 opcodes, followed by `IR_OPCOUNT`,
which is not an opcode. The order of the enumeration does not group
opcodes by kind; test opcodes by name, never by numeric range.
`ir_opname` returns an opcode's mnemonic and `ir_op_from_name` the
reverse.

In the tables, *Form* is the textual form (see
[The textual form](#the-textual-form)), `W` is `w`, `S` is `size`, and
`[s]`, `[f]`, `[v]` are the flags.

#### Values and arithmetic

| Opcode | Form | Semantics |
|---|---|---|
| `IR_CONST` | `%d = const.W IMM` | `dst = imm`. |
| `IR_MOV` | `%d = mov.W %a` | `dst = a`, a full copy of the temp. |
| `IR_ADD` | `%d = add.W %a, %b` | `dst = a + b`. |
| `IR_SUB` | `%d = sub.W %a, %b` | `dst = a - b`. |
| `IR_MUL` | `%d = mul.W %a, %b` | `dst = a * b`. |
| `IR_DIV` | `%d = div.W %a, %b` | `dst = a / b`, signed when `sign`. |
| `IR_MOD` | `%d = mod.W %a, %b` | `dst = a % b`, signed when `sign`. |
| `IR_AND` | `%d = and.W %a, %b` | `dst = a & b`. |
| `IR_OR` | `%d = or.W %a, %b` | `dst = a \| b`. |
| `IR_XOR` | `%d = xor.W %a, %b` | `dst = a ^ b`. |
| `IR_SHL` | `%d = shl.W %a, %b` | `dst = a << b`. |
| `IR_SHR` | `%d = shr.W %a, %b` | `dst = a >> b`, arithmetic when `sign`, logical otherwise. |
| `IR_NEG` | `%d = neg.W %a` | `dst = -a`. |
| `IR_BNOT` | `%d = bnot.W %a` | `dst = ~a`. |
| `IR_CMP` | `%d = cmp.W PRED %a, %b` | `dst = (a PRED b)`, 0 or 1. `sign` selects a signed integer comparison. With `flt`, `w` is the width of the floating-point operands. |
| `IR_SELECT` | `%d = select.W %a ? %b : %c` | `dst = a ? b : c`. Both arms are already-computed values. Produced only by if-conversion in the optimizer. |
| `IR_BSWAP` | `%d = bswap.W %a` | `dst` is `a` with its `size` bytes reversed (2, 4 or 8); `__builtin_bswapN`. |
| `IR_SQRT` | `%d = sqrt.W[f] %a` | `dst = sqrt(a)` at float width `w` (4 or 8); `__builtin_sqrt*`. |

With `flt` set, `ADD`, `SUB`, `MUL`, `DIV`, `MOV` and `CMP` operate on
floating-point values of width `w`; `w` 16 with `flt` is a wide
`long double`. IR generation negates a `float` or `double` by flipping
its sign bit with an integer `IR_XOR`. A binary operation's `b` operand
is printed `#IMM` when `imm_b` is set.

#### Conversions

| Opcode | Form | Semantics |
|---|---|---|
| `IR_EXT` | `%d = ext.W:S %a` | The low `S` bytes of `a`, extended to width `W`; sign extension when `sign`. Also used to truncate: `ext.4:1` keeps one byte. |
| `IR_I2F` | `%d = i2f.W:S %a` | Integer of `S` bytes (signed when `sign`) to a floating-point value of width `W`. |
| `IR_F2I` | `%d = f2i.W:S %a` | Floating-point value of width `S` to an integer of width `W`, signed when `sign`. |
| `IR_F2F` | `%d = f2f.W:S %a` | Floating-point value of width `S` to width `W`. |
| `IR_BITCAST` | `%d = bitcast.W:S %a` | The bits of `a` at the same width (`W == S`). `sign` set: the destination is an integer (float bits out); clear: the destination is a float. IR generation lowers `fabs`, `copysign`, `signbit`, `isnan`, `isinf`, `isfinite` and `isnormal` builtins to integer operations around it. |

#### Frame slots and memory

| Opcode | Form | Semantics |
|---|---|---|
| `IR_LDVAR` | `%d = ldvar.W:S vN` | Read `S` bytes of slot `a`, extend to `W`. |
| `IR_STVAR` | `stvar:S vN, %a` | Write the low `S` bytes of `a` to slot `dst`. |
| `IR_ADDR` | `%d = addr vN` | `dst` = the address of slot `a`. Taking a slot's address keeps it in memory. |
| `IR_LOAD` | `%d = load.W:S [%a]` | Read `S` bytes at address `a` (plus `memoff`), extend to `W`. |
| `IR_STORE` | `store:S [%a], %b` | Write the low `S` bytes of `b` at address `a` (plus `memoff`). |
| `IR_MEMCPY` | `memcpy:S [%a], [%b]` | Copy `S` bytes from address `b` to address `a`. |
| `IR_MEMZERO` | `memzero:S [%a]` | Zero `S` bytes at address `a`. |
| `IR_ALLOCA` | `%d = alloca %a` | `dst` = a fresh 16-byte-aligned block of `a` bytes on the stack, above the outgoing-argument area. Used for VLAs and `__builtin_alloca`; sets `has_alloca`. |
| `IR_SPSAVE` | `%d = spsave` | `dst` = the stack pointer. |
| `IR_SPRESTORE` | `sprestore %a` | Set the stack pointer to `a`, releasing every `IR_ALLOCA` since the `IR_SPSAVE` that produced `a`. |
| `IR_FRAMEADDR` | `%d = frameaddr` | `dst` = this function's frame pointer (`rbp`, `x29`), which points at the saved frame pointer and the return address. The base of `__builtin_frame_address` and `__builtin_return_address`. Lowered by the x86-64 and AArch64 backends only. |

#### Addresses of symbols

| Opcode | Form | Semantics |
|---|---|---|
| `IR_STRADDR` | `%d = straddr strN` | `dst` = the address of string `label` in `.rodata`. |
| `IR_GADDR` | `%d = gaddr @NAME` | `dst` = the address of the object `glob`. |
| `IR_FADDR` | `%d = faddr @NAME` | `dst` = the address of the function `callee`. |
| `IR_LABELADDR` | `%d = labeladdr LN` | `dst` = the address of label `label` (GNU `&&label`). |

#### Calls and returns

| Opcode | Form | Semantics |
|---|---|---|
| `IR_CALL` | `%d = call @NAME(%x, ...)` or `%d = call [%a](%x, ...)` | Call `callee`, or the function pointer in `a` when `indirect`. Arguments are in `argv`. The result is in `dst`; IR generation allocates a `dst` for every call, including one that returns `void`. A structure result is written to the caller's scratch area at `scratch`. `w` is the width of the return register and `flt` marks a floating-point result. |
| `IR_RET` | `ret %a` or `ret` | Return `a`; `a` is -1 for a `void` return. For a structure, `a` holds the structure's address and `size` its size; the function's `ret_abi` says how it travels. |

The end of the instruction array also returns: every backend emits an
epilogue after the last instruction, so a `void` function may end
without `IR_RET`.

#### Control flow

| Opcode | Form | Semantics |
|---|---|---|
| `IR_LABEL` | `LN:` | Marks a position. Label numbers are below `nlabels`. |
| `IR_JMP` | `jmp LN` | Jump to `label`. |
| `IR_BRZ` | `brz.W %a -> LN` | Jump to `label` if `a` is zero; otherwise fall through. |
| `IR_BRNZ` | `brnz.W %a -> LN` | Jump to `label` if `a` is nonzero; otherwise fall through. |
| `IR_SWITCH` | `switch.W %a -> LDEF [N] L0 L1 ...` | If `a`, compared unsigned, is below the table's length `N`, jump to entry `a` of table `jt`; otherwise jump to the default `label`. `a` is the case value minus the lowest case. |
| `IR_IGOTO` | `igoto [%a]` | Jump to the address in `a` (GNU computed `goto`). |
| `IR_UD2` | `ud2` | The target's undefined instruction (`ud2`, `udf #0`, `unimp`). Used for `__builtin_trap`, `__builtin_unreachable` and failed `-fsanitize` checks. |

A basic block starts at the first instruction, at every `IR_LABEL`, and
after every `IR_JMP`, `IR_BRZ`, `IR_BRNZ`, `IR_RET`, `IR_UD2`,
`IR_IGOTO` and `IR_SWITCH`. `IR_RET` and `IR_UD2` have no successor;
`IR_JMP` has its label; `IR_BRZ` and `IR_BRNZ` have their label and the
next block; `IR_SWITCH` has its default and every table entry;
`IR_IGOTO` has every label whose address an `IR_LABELADDR` in the
function takes. Any other final instruction falls through to the next
block (`build_cfg` in `src/opt/opt.c`).

A jump table (`struct ir_jt`) holds `n` label numbers. It belongs to the
function and is referred to by index, so copies of an `IR_SWITCH`
(made by the inliner or by switch threading) share one table.
`ir_jt_add` creates a table with every entry -1; `ir_jt_clone` copies
one with its labels renumbered. A dense `switch` becomes `IR_SWITCH`
only where `target_jump_tables()` allows it (not on AVR); under `-Os`
the density threshold is higher.

#### Atomics and barriers

| Opcode | Form | Semantics |
|---|---|---|
| `IR_FENCE` | `fence` | A full memory barrier. |
| `IR_XCHG` | `%d = xchg.W:S [%a], %b` | Atomically: `dst = *a; *a = b`. |
| `IR_XADD` | `%d = xadd.W:S [%a], %b` | Atomically: `dst = *a; *a += b`. Also used for subtraction with a negated operand. |
| `IR_ARMW` | `%d = armw.W:S [%a], %b op 'X'` | Atomically: `dst = *a; *a = dst X b`, where `imm` holds `X`: `&`, `\|`, `^`, or `n` for NAND (`~(dst & b)`). |
| `IR_CAS` | `%d = cas.W:S [%a], %b, %c` | Atomically: `dst = *a; if (dst == b) *a = c`. The result is the value seen (the `__sync_*_compare_and_swap` shape). |
| `IR_CMPXCHG` | `%d = cmpxchg.W:S [%a], [%b], %c` | Atomically compare `*a` with `*b`; on a match store `c`; otherwise store the value seen to `*b`. `dst` is 1 on a match and 0 otherwise (the `__atomic_compare_exchange` shape). |
| `IR_CAS16` | `%d = cas16.W:S [%a], %b, %c` | `IR_CAS` on 16 bytes (`__int128`). IR generation builds the other 16-byte atomics as loops around it. |

Atomic loads and stores are ordinary `IR_LOAD` and `IR_STORE` with
`vol` set, with `IR_FENCE` placed around them by IR generation.

#### Variadic functions, inline assembly, exceptions

| Opcode | Form | Semantics |
|---|---|---|
| `IR_VA_START` | `va_start [%a]` | Initialize the `va_list` whose address is in `a`. On System V x86-64 and AAPCS64 the backend fills a record in the frame (System V's 24-byte `__va_list_tag`, AAPCS64's 32-byte record) and stores its address in the `va_list`. Where `va_list` is a plain pointer (`target_va_list_is_pointer()`: ARMv7-M, RISC-V, AVR, Apple arm64, Microsoft x64) it stores the address of the first variadic argument. `va_arg` itself is lowered to ordinary IR by the target's `irgen.c`. |
| `IR_ASM` | `asm N bytes, I in, O out` | Extended `asm`. `asm_ir` holds the template already assembled to bytes and the operand list. The backend loads each input's value into its fixed register, places the bytes, and stores each output register through the output's address. |
| `IR_LANDING` | `%d, %b = landing` | The entry of a landing pad: `dst` receives the exception pointer and `b` the selector, from the registers the unwinder sets. Defines two temps. |

`struct ir_asm` holds `code`/`codelen` (the bytes) and the inputs and
outputs as `struct ir_asm_op`: `temp` (the input's value, or the
output's address), `reg` (the fixed register), `size`, `inout` (a `+`
operand whose register must hold the current value first) and `mem` (an
`m` operand: the register holds the address and the template performs
the access).

#### Vectors

Vector operations work on 128-bit values. `S` is the element width in
bytes, so a vector has `16 / S` lanes. A vector temp occupies a 16-byte
slot and is never given an integer register. These opcodes are produced
only by the optimizer's vectorizer, which runs only for x86-64.

| Opcode | Form | Semantics |
|---|---|---|
| `IR_VLOAD` | `%d = vload.W:S [%a]` | `dst` = the 16 bytes at address `a`. |
| `IR_VSTORE` | `vstore.W:S [%a], %b` | Store vector `b` at address `a`. |
| `IR_VBIN` | `%d = vbin.W:S OP %a, %b` | Lane-wise `a OP b`; `imm` holds `+`, `-`, `&`, `\|` or `^`. For `<` (shift left) and `>` (shift right) the count is the constant `c`, printed `#N`, and `sign` selects an arithmetic right shift. There is no multiply. |
| `IR_VSPLAT` | `%d = vsplat.W:S %a` | Every lane set to scalar `a`. |
| `IR_VREDADD` | `%d = vredadd.W:S %a` | The sum of the lanes of `a`, as a scalar of width `W`. |
| `IR_VWIDEN` | `%d = vwiden.W:S lo\|hi %a` | Half of the lanes of `a` (`c` 0 the low half, 1 the high half), each widened to twice `S`; sign extension when `sign`. |

### Producers and consumers

IR generation (`irgen`, with `src/arch/<arch>/irgen.c` for `va_arg` and
`asm`) produces every opcode except `IR_SELECT` and the vector opcodes,
which only the optimizer creates. The optimizer also sets `imm_b`, and
a backend's pre-codegen pass may set `memoff`.

A backend refuses an operation it cannot lower with a diagnostic that
names the instruction, for example:

```text
embcc: fa.c:1: error: the ARMv7-M backend cannot lower this operation at 64 bits yet (function fa) [frameaddr w=8 size=4]
```

The operand and control-flow knowledge of each opcode is spread over
switches in the optimizer, the register allocator and each backend. The
list of places to update when an opcode is added is in
[The optimizer](optimizer.md#adding-an-ir-operation).

## Source locations

Every instruction carries the line and column of the expression or
statement it was generated from, or `synth` when it corresponds to no
source construct (a prologue store, a landing pad's entry, the zero
`mem2reg` gives a variable read before any write). IR generation stamps
the location in `emit()`; a pass that replaces an instruction copies
`line`, `col` and `synth` from the instruction it replaces. Code
generation turns locations into the `-g` line table (`ir_func::lines`).

## The textual form

`embcc inspect ir FILE.c [options]` prints the IR of a C or C++ file as
it stands after optimization at the `-O` level given, so the same
command shows IR generation's output at `-O0` and the optimizer's at
`-O2`. `inspect` must be the first argument; the rest is an ordinary
command line (`--target=`, `-I`, `-D`, `-O`, `-f<pass>`).

`embcc inspect ir FILE.ir` reads a file in the textual form with
`ir_parse` and prints it again. A `.ir` file is accepted only by
`inspect ir`; other modes treat it as C source.

```sh
embcc inspect ir sum.c -O2 -fno-vectorize -fno-unroll > sum.ir
embcc inspect ir sum.ir | cmp - sum.ir
```

### Example

```c
int sum(int *a, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s;
}
```

At `-O0`:

```text
; EmbIR
func @sum nparams=2 nvars=4 vregs=23 labels=3 {
  local v0 size=8 align=8 intptr scalar
  local v1 size=4 align=4 intptr scalar
  local v2 size=4 align=4 intptr scalar
  local v3 size=4 align=4 intptr scalar
  %4 = const.4s 0	; 3:13
  stvar:4s v2, %4	; 3:9
  %5 = const.4s 0	; 4:18
  stvar:4s v3, %5	; 4:14
L0:
  %6 = ldvar.4:4s v3	; 4:21
  %7 = ldvar.4:4s v1	; 4:25
  %8 = cmp.4s lt %6, %7	; 4:21
  brz.4s %8 -> L2	; 4:14
  %9 = ldvar.4:4s v2	; 5:9
  %10 = ldvar.8:8 v0	; 5:14
  %11 = ldvar.4:4s v3	; 5:16
  %12 = ext.8:4s %11	; line 5
  %13 = const.8s 4	; 5:14
  %14 = mul.8s %12, %13	; 5:14
  %15 = add.8s %10, %14	; 5:14
  %16 = load.4:4s [%15]	; 5:14
  %17 = add.4s %9, %16	; 5:9
  stvar:4s v2, %17	; 5:9
L1:
  %18 = ldvar.4:4s v3	; line 4
  %19 = mov.4s %18	; line 4
  %20 = const.4s 1	; line 4
  %21 = add.4s %18, %20	; line 4
  stvar:4s v3, %21	; line 4
  jmp L0	; 5:9
L2:
  %22 = ldvar.4:4s v2	; 6:12
  ret %22	; 6:5
}
```

At `-O2 -fno-vectorize -fno-unroll` (x86-64): the locals are promoted
to temps, the parameters are read directly as `%0` and `%1`, the loop
is rotated and walks a pointer, and `%23`, `%24` and `%28` are merge
temps with several definitions.

```text
; EmbIR
func @sum nparams=2 nvars=4 vregs=30 labels=5 {
  local v0 size=8 align=8 intptr scalar
  local v1 size=4 align=4 intptr scalar
  local v2 size=4 align=4 intptr scalar
  local v3 size=4 align=4 intptr scalar
  %25 = const.4 0	; compiler-synthesized
  %23 = mov.4 %25	; 4:14
  %24 = mov.4 %25	; 4:14
  %8 = cmp.4s lt %24, %1	; 4:21
  brz.4s %8 -> L2	; 4:14
  %28 = mov.8 %0	; line 5
L3:
  %16 = load.4:4s [%28]	; 5:14
  %17 = add.4s %16, %23	; 5:9
  %21 = add.4s %24, #1	; line 4
  %23 = mov.4 %17	; 5:9
  %24 = mov.4 %21	; 5:9
  %28 = add.8 %28, #4	; line 4
  %27 = cmp.4s lt %24, %1	; 4:21
  brnz.4s %27 -> L3	; 4:14
L2:
  ret %23	; 6:5
}
```

### Layout of a unit

The printer writes, in order:

1. The line `; EmbIR`.
2. One line per string literal: `strN = "BYTES"`, with `\n`, `\t`, `"`
   and `\` escaped and the terminating NUL omitted. A blank line
   follows when there are strings.
3. One line per symbol:
   `func @NAME [defined] [weak] [varargs] [sret] [nothrow] ;decl` or
   `data @NAME [defined] [weak] ;decl`. A blank line follows when there
   are symbols.
4. Each function, separated by blank lines:

   ```text
   func @NAME [static] [varargs] [alloca] [i128] nparams=N nvars=N vregs=N labels=N [scratch=N] [outgoing=N] {
     local vI size=N align=N [user_align=N] [volatile] [ldouble] [int128] [intptr] [scalar]
     ...one instruction per line...
   }
   ```

   `scratch=` and `outgoing=` are printed only when nonzero. There is
   one `local` line per frame slot; `intptr` is `is_int_or_ptr` and
   `scalar` is `is_scalar_int_or_ptr`.

### Instructions

A label is printed at column 0 as `LN:`. Every other instruction is
indented two spaces and printed in the form given in the opcode tables,
followed by a tab and its location:

| Trailer | Meaning |
|---|---|
| `; LINE:COL` | Line and column known. |
| `; line LINE` | Column unknown. |
| `; compiler-synthesized` | `synth` set and no line. |

Operands are spelled:

| Spelling | Meaning |
|---|---|
| `%N` | Vreg `N`. |
| `vN` | Frame slot `N`. |
| `[%N]` | The address held in vreg `N`. |
| `#N` | An immediate operand (`imm_b`), or `IR_VBIN`'s shift count. |
| `LN` | Label `N`. |
| `@NAME` | A symbol, declared earlier in the unit. |
| `strN` | String `N`. |

The mnemonic carries a suffix. For operations without a memory width it
is `.W` followed by the flags; for those with one (`ldvar`, `stvar`,
`load`, `store`, `ext`, the conversions, the atomics and the vector
opcodes) it is `.W:S` followed by the flags. `stvar` and `store` omit
`.W`. The flags are `s` (`sign`), `f` (`flt`) and `v` (`vol`), in that
order. So `ldvar.8:4s` is a sign-extending 4-byte load into an 8-byte
value, and `load.4:1v` a volatile byte load. A width or flag that is
not set is not printed. Because IR generation sets `sign` by default,
`s` also appears on operations whose result does not depend on it
(`add.4s`, `brz.4s`, `store:8s`).

`call` prints ` varargs(N)` when the callee is variadic, with `N` the
number of named parameters, and ` sret` when argument 0 is the result
pointer.

### Reading it back

`ir_parse(file, text)` reads the form line by line and modifies `text`
in place. It builds the unit's symbol table from the `func ... ;decl`
and `data ... ;decl` lines, so a symbol must be declared before an
instruction names it. A line that begins with `;` is a comment. A `func`
line without `;decl` opens a function; `}` closes it.

An error stops the read with the file and line:

```text
embcc: bad.ir:3: error: EmbIR: expected a known opcode
```

The round trip is tested by `tests/golden/ir-roundtrip.sh`: printing a
parsed unit must reproduce the text it was parsed from, byte for byte,
at `-O0`, `-O1` and `-O2`, and for EmbCC's own sources at `-O0` and
`-O2`.

### What the text does not carry

The textual form encodes what passes need to be tested text-in,
text-out. It does not carry:

- the classification of call arguments and return values (`argv`
  beyond the vreg, `ret*`, `scratch`, `call_pcs`), the function's
  `param_abi`, `ret_abi` and `pcs`;
- the width and float flag of `ret` and of `call`, and `ret`'s structure
  size;
- `natural`, `memoff` and `eh_region`;
- exception regions, debug variables and variable scopes;
- `is_scalar_float` of a frame slot;
- an `asm` statement's bytes and operands.

A parsed unit therefore has `src` set to `NULL` and is used only to be
printed: the driver does not optimize or generate code from it. Known
limits of the reader:

- An `asm` instruction cannot be read back; the reader stops with
  `EmbIR: 'asm' has no operand form here — teach irparse.c about it`.
- The operator of `armw` (`op '&'`) is not read back; the parsed
  instruction has `imm` 0.
- A line is copied into a 2048-byte buffer before its strings and
  `key=value` fields are read, so a longer line (a long string literal)
  is truncated.

## Invariants and the verifier

The following must hold for any IR handed to the optimizer or a
backend:

- Every vreg is below `nvregs` and every label below `nlabels`.
- Every temp an instruction reads has a definition in the function.
  Frame slots (`< nvars`) may be read before they are written.
- Every label an `IR_SWITCH` names, including its default, is placed by
  an `IR_LABEL` in the same function.
- `var_scope_lo` and `var_scope_hi` index the current instruction
  array. A pass that renumbers instructions remaps or frees them.
- Every instruction has `line` set or is marked `synth`.
- `IR_LANDING` defines two temps, `dst` and `b`.

When the environment variable `EMBCC_VERIFY` is set (to any value), the
optimizer checks the first four of these and the location rule for each
function, once on the IR from IR generation and once after
optimization. A failure is fatal and names the function, the check and
when it failed (`after irgen` or `after opt`):

```text
internal: NAME reads temp %N with no definition (after TAG) — an optimizer pass dropped a value that is still used
```

The verifier runs inside the optimizer, so it runs only at `-O1` and
above. The test suite sets `EMBCC_VERIFY`. The full list of messages is
in [The optimizer](optimizer.md#the-ir-verifier).

## Related pages

- [Architecture](architecture.md): where IR generation and the optimizer
  sit in the compiler.
- [The front end](front-end.md): the AST and type system IR generation
  reads.
- [The optimizer](optimizer.md): the passes over EmbIR.
- [Backends](backends.md): how each target lowers EmbIR to machine code.
