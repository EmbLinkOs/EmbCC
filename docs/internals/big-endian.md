# Big-endian targets

Byte order is a property of the target, like the data model. Every target
EmbCC had was little-endian, so "the low byte comes first" was written into
the compiler, the linker and the libraries without anyone deciding it. The
first big-endian target is `mips-none-elf` (also `mips-unknown-elf`,
`mips-elf`, `mips`): MIPS32r2, o32, soft float, the same backend as
`mipsel-none-elf`, run on QEMU's big-endian malta (`qemu-system-mips`).
The second is `powerpc-none-eabi` (powerpc-plan.md), which is big-endian
only. This page lists every place that depends on the byte order, what it
does big-endian, and the test that notices when it is wrong. A new big-endian
target (PowerPC, SPARC LEON3, m68k/ColdFire, OpenRISC, s390x) should walk
the same list.

## The one question

`target_big_endian()` (src/arch/target.h) answers it. It is set with the
triple (`target_from_triple`; on MIPS the sub-architecture column's 1 means
big-endian, and every PowerPC triple is) and nothing else changes it. `-EB`/`-mbig-endian` and
`-EL`/`-mlittle-endian` are accepted when they agree with the triple and
refused by name when they do not (the triple decides more than the order:
the runtime's directory, the predefined macros). Values go into target
memory through

- `target_put_uint(p, n, v)` / `target_get_uint(p, n)`: an `n`-byte value
  in the target's order;
- `target_byte_shift(off, size, whole)`: where the `size` bytes at `off` of
  a `whole`-byte value sit in it, as a right shift.

The compiler never copies a host integer into an object, an instruction or
an image byte for byte. The host is assumed little-endian, and the ELF
writer refuses to run on a big-endian host rather than write the opposite
order.

## Where it matters

| Place | What big-endian changes | Caught by |
| --- | --- | --- |
| `src/arch/target.c` | the triples; `target_rel_put_addend` stores a REL addend into the field in the target's order | mips-be-data, mips-be-exec |
| `src/arch/predef.c`, `src/arch/mips32eb/` | its own generated table (`tools/gen-predef.sh mips32eb`): `__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__`, `__BIG_ENDIAN__`, `MIPSEB`, `_MIPSEB`, `__MIPSEB`, `__MIPSEB__` | predef |
| `src/elf/write.c` | `EI_DATA = ELFDATA2MSB`; the header, section headers, symbols and relocation entries are swapped field by field once their 32-bit shapes are final. Section contents are not touched: the code generator and the data writers already wrote them in order. A big-endian ELF64 is refused. | mips-be-data (llvm-readelf-equivalent parse), every board test |
| `src/sema/sema.c` `lower_static_bytes` | every scalar, pointer-sized integer, float, double, complex part and `__int128` half of a static initializer through `target_put_uint`; long double through `ldf_encode_target` | mips-be-data |
| `src/sema/sema.c` `merge_bits` | a bit-field's bits into its unit's bytes: the unit read as one big-endian integer, and only the bytes the object owns written | mips-be-data, endian.c |
| `src/sema/sema.c` `flatten_init` | a wide string initializing an array: elements read back with `target_get_uint` | mips-be-data (`d_u16s`, `d_u32s`, `d_wstr`) |
| `src/lex/lex.c` `lit_encode` | `u""`, `U""`, `L""` units in the target's order (every literal, every consumer) | mips-be-data, endian.c |
| `src/sema/ldfloat.c` | `ldf_encode_target`: the format's bytes reversed whole. `ldf_encode` stays little-endian, because `ldf_to_double` and the C++ evaluator read it as host order | mips-be-data (`d_ld`, `d_cd`, `d_cf`) |
| `src/driver/main.c` | a scalar global's `.data`/`.rodata` image from `g->init`; `-EB`/`-EL` | mips-be-data |
| `src/ir/irgen.c` `emit_ldconst` | a long double constant's pool bytes | (no big-endian target has a 16-byte long double yet) |
| `src/sema/type.c` bit-field layout | `bit_off` is the field's shift from the least significant end of its unit AS LOADED; the positions in memory order are the same in both orders (gcc's layout), and `ty_bf_mempos` converts between them. Big-endian the first field is at the unit's high end. Union members and packed straddling fields likewise. | mips-be-data (six bit-field structs, a union), endian.c |
| `src/ir/irgen.c` bit-fields | the unit loads and stores need nothing (they shift from the loaded value's LSB); the byte-at-a-time forms of a packed field across its unit put byte k at `8(n-1-k)`; the 128-bit byte forms are refused big-endian | endian.c, mips-be-exec (packed-bitfields, refereed) |
| `src/debug/dwarf.c`, `src/debug/eh.c` | every multi-byte field through `target_put_uint`, the backpatched unit and header lengths included; `DW_AT_data_bit_offset` is the memory-order position | mips-be-data (`llvm-dwarfdump --verify`, clang's bit offsets) |
| `src/as/gas.c` `emit_int` | `.word`/`.half`/`.quad` data in the target's order | mips-be-gas, mips-be-exc |
| `src/driver/asmout.c` | a relocated MIPS instruction is decoded from its bytes in order for `-S` | mips-be-asm (-S reassembled by llvm-mc) |
| `src/opt/opt.c` `ro_bytes` | a load from a constant global folded from its image: the first byte is the most significant | ro-globals, endian.c |
| `src/opt/opt.c` `pass_punfwd` | the word at +4 of an 8-byte store is the LOW half (fdlibm's GET_HIGH_WORD) | fp-bits, store-forward (refereed), endian.c |
| `src/opt/opt.c` `pass_storefwd`, `pass_mem2reg` | a read of a local narrower than the local is its first bytes -- the value's high end -- so neither forwards across differing sizes big-endian (the IR does not produce them; this keeps it that way) | -- |
| `src/arch/mips/emit.c` | instruction words in order (`mips_set_big_endian`, set by the driver for the compiler and by EmbLD from its objects -- the linker links this file without target.c) | mips-encoding (bytes in memory order against llvm-mc's), every board test |
| `src/arch/mips/codegen.c` | see below | mips-be-exec, mips-be-abi |
| `src/link/link.c` | see below | every board test, mips-be-abi (clang's objects) |
| `lib/libc/src/math/fdlibm/fdlibm.h` | `__IEEE_BIG_ENDIAN` from `__BYTE_ORDER__`: which word of a double is first | libc-embedded (mips-none-elf against x86-64), mips-be-exec |
| `tests/harness/mips/run.sh` | the board is chosen by the image's `EI_DATA` | -- |
| `src/arch/ppc/emit.c` | instruction words always big-endian (`ppc_put_word`), there being no other order | ppc-encoding (bytes in memory order against llvm-mc's) |
| `src/arch/ppc/codegen.c` | pairs high word first (r3:r4, PHI = the first register), as memory and the SVR4 ABI have them; a narrow variable at the end of its word home (`obj_slot`), and one aligned beyond its word with the object -- not the word -- on the alignment; small composites returned right-justified in r3:r4 | ppc-exec, ppc-abi, ppc-data |
| `src/link/link.c` `apply_ppc` | every field written big-endian through `ppc_put_word` and a big-endian halfword for @ha/@l; a little-endian PowerPC object refused | ppc-abi (clang's objects), ppc-refuse |

### The MIPS backend

o32 makes a register pair MIRROR memory: the word at the lower address of
the argument block travels in the lower-numbered register. So big-endian a
`long long` or soft `double` argument in a0:a1 has its HIGH word in a0, a
result comes back high word in v0, and a composite shorter than a word is
LEFT-justified in its register (clang: `int k(struct {char c;} s)` is
`sra v0, a0, 24`). The backend follows the same rule internally: a pair is
named by its first register r and holds the low word in `PLO(r)` and the
high in `PHI(r)`; in memory the low word is at `WLO` (+4 big-endian) and the
high at `WHI`. Every place that took "the low word is in loc, at slot+0":

- `rd64`/`wr64`, `src64`/`dst64`, 64-bit loads and stores, the narrow
  high-word shift (`nshr`), the 64-bit stack argument;
- calls: each argument word to its register (`sh_`), helper operands
  (`args64x2`, the divide's folded constant), results (`wr64(V0..)`), a
  32-bit result read into a 64-bit value (v0 either way), `IR_RET`;
- a 32-bit read or write of a 64-bit vreg is its low word (`reg_of`,
  `slot32`) -- copy propagation lets any operation read one at its width;
- the prologue: a pair parameter's words, a composite's partial last word
  (`store_tail`), `pack_tail` for an argument;
- a narrow integer VARIABLE's object is at the end of its four-byte home
  (`obj_slot`): the home is also read and written as a whole word (`rd`,
  the incoming argument register), and big-endian the value's low bytes
  are the word's last;
- unaligned accesses: `lwl` at `off` and `lwr` at `off+3` (the reverse of
  little-endian), a halfword's high byte first;
- the jump table's words and the delay-slot filler's re-read of the last
  instruction; `.MIPS.abiflags`' flags1 word.

### EmbLD

A big-endian ELF32 object is read by turning its descriptions -- header,
section headers, symbols, relocation entries -- into host order in place,
once (`be_normalise`); its section contents stay big-endian. Every MIPS
relocation reads and writes its field as a word through `mips_get_word`
(HI16/LO16/PC16 change the word's low half), so the AHL rule and the REL
addends work unchanged. The image is written as before and its headers,
program headers, section headers and symbol table are swapped back
(`be_image`); a linker script's `BYTE`/`SHORT`/`LONG`/`QUAD` go out in the
target's order (`FILL` is big-endian in either, as GNU ld writes it). The
`-Tstack` stub is encoded in the objects' order. Objects of both orders in
one link are refused by name.

## Refused big-endian, by name

- a packed bit-field across more than 8 bytes, or a packed `__int128`
  bit-field (the 128-bit byte forms are little-endian, and no big-endian
  target has `__int128`);
- an x87 long double (`ldf_encode_target`), a big-endian ELF64 object, and
  a big-endian host;
- `--embx` (EMBX images are EmbLinkOS's, little-endian);
- the `.embdbg` sidecar (EmbDBG reads little-endian objects; the image's
  own DWARF is correct) -- a note, not an error;
- C++ (refused on every ILP32 target already; its constant evaluator
  models memory little-endian, so a 64-bit big-endian target must refuse
  it too until that changes).

## Tests

- `tests/golden/mips-be-exec.sh`: the exec corpus at -O0..-Os on the board.
  The programs whose expected value assumes little-endian layout (a union
  read through another member, a double's `{lsw, msw}`) are judged against
  clang's result for the same triple, as the LP64 ones are.
- `tests/golden/mips-be-abi.sh`: EmbCC and clang objects calling each other
  (small structs, long long and double pairs, variadics).
- `tests/golden/mips-be-data.sh`: `tests/golden/be-data.c`'s every object,
  bytes and relocations, against clang's; and the same values printed on
  the board.
- `tests/exec/endian.c`, on every board: finds the order at run time and
  checks unions, memcpy, static images, bit-fields through bytes, wide
  strings and network order against it.
- `tests/golden/predef.sh`: the `mips32eb` table against clang's.
- `mips-be-asm`, `mips-be-link`, `mips-be-gas`, `mips-be-exc`,
  `mips-be-switch`, `mips-be-slots`, `mips-be-access`: the little-endian
  MIPS goldens run for mips-none-elf (`MIPS_BE=1`) against llvm-mc and
  clang for mips-unknown-elf and on qemu-system-mips; `mips-encoding`
  checks the encoder's big-endian bytes; `mips-refuse` the triples, the
  header and the `-EB`/`-EL` rules; `libc-embedded` lib/libc on the board
  against x86-64.
- `tests/exec/llong-bitfield.c`: a `long long` bit-field's unit is eight
  bytes on an ILP32 target (it was loaded as a `long`, on every 32-bit
  target, little-endian too -- found by mips-be-data's program).
- Little-endian byte identity: lib/libc, lib/rt and tests/exec built for
  thumbv7em, riscv32 and mipsel at -O0, -O2 and -Os (and tests/exec at -O1
  -g) by the compiler before and after are identical object files, but for
  tests/exec/llong-bitfield.c, whose fix is the point of it; EmbLD's
  mipsel images of the corpus are identical too.

Each was shown to fail against a mutant of the code it guards.

## Adding the next big-endian target

PowerPC (powerpc-plan.md) walked this list; what it found is in the table
above.

1. A triple that sets `g_big_endian` (target.c), and a generated predef
   table from the reference compiler.
2. The backend: its instruction encoder writes words through an order it
   is told; pairs and memory halves as its ABI lays them out (check what
   the ABI does with a sub-word composite in a register -- o32 and the
   PowerPC SysV ABI both left-justify); unaligned access sequences.
3. The linker: its relocation patcher reads and writes fields through the
   machine's own accessors, not `put32`.
4. Its ELF class: the writer swaps ELF32 only today; ELF64 needs the same
   (and C++'s evaluator, before C++ is allowed).
5. lib/: anything `#if`-ing on the architecture for word order (fdlibm
   keys on `__BYTE_ORDER__` and needs nothing).
6. The board, and the three goldens above with its triple.
