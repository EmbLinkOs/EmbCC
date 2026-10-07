# embmap — where an image's flash and RAM go

`embmap` reads a linked ELF image and reports how much flash and RAM it
uses: section by section, per memory region, per symbol, and per input
file. It also shows how two builds differ, and it can fail a build that no
longer fits. It reads images from EmbLD, GNU ld or lld: 32- or 64-bit,
either byte order, any machine. This page is the command reference.

## Synopsis

```text
embmap IMAGE [--region NAME:ORIGIN:LENGTH]... [--top N]
             [--map FILE.map] [--diff OLD] [--json]
             [--max-flash SIZE] [--max-ram SIZE]
```

Sizes and addresses are decimal or `0x` hex. A `K`, `M` or `G` suffix
multiplies by 1024, 1024² or 1024³: `256K`, `0x08000000`.

## What it reports

```text
$ embmap fw.elf --region FLASH:0:256K --region RAM:0x20000000:64K --top 5
fw.elf: ELF32 little-endian, machine 40

section                  kind         size            address               load
.isr_vector              rodata         64                  0
.text                    text          668               0x40
.rodata                  rodata         44              0x2e0
cmds                     rodata         16              0x30c
.init_array              data            4              0x31c
.data                    data           24         0x20000000              0x320
.bss                     bss           404         0x20000018
._user_heap_stack        bss          1540         0x200001ac

flash 820 bytes, RAM 1972 bytes (text 668, rodata 124, data 28, bss 1944)

Memory region       Used Size  Region Size  %age Used
FLASH                   824 B       256 KB      0.31%
RAM                    1968 B        64 KB      3.00%

      size  kind     section              symbol
       408  function .text                main
       114  function .text                Reset_Handler
        88  function .text                putn
        64  object   .isr_vector          g_pfnVectors
        24  function .text                puts_
        18  -        .text                [bytes no symbol covers]
        44  -        .rodata              [bytes no symbol covers]
       ...
```

- **Sections.** Every allocated section with its kind, size and run
  address. When initialised data is stored somewhere other than where it
  runs, the section also shows its load address, read from the program
  headers.
- **Kinds.** A section's kind comes from its ELF flags:
  - `text` is executable;
  - `rodata` is read-only data;
  - `data` is writable and has contents;
  - `bss` takes no space in the file.
- **Flash and RAM.** These follow the formula `arm-none-eabi-size` users
  know:
  - flash is what the image stores: text, read-only data and initialised
    data;
  - RAM is initialised data plus `.bss`.

  A writable section kept in flash, such as `.init_array` above, counts as
  data. Use `--region` for exact placement.
- **Regions (`--region`).** Each region is named as a linker script's
  `MEMORY` block names it. A region counts as used from its origin to the
  end of the last byte placed in it, by run address, and also by load
  address for data stored elsewhere. Alignment padding between sections is
  therefore used space. This is how GNU ld and EmbLD's
  `--print-memory-usage` count, and the numbers agree: FLASH is 824 here,
  4 more than the sections' sum, because `.rodata` is aligned.
- **Symbols (`--top N`).** The `N` largest functions and objects, with the
  default 10 when no other report is asked for. Two names for the same
  bytes, such as an alias or a weak handler, are one entry. Each section's
  bytes that no symbol covers are listed too: literal pools, alignment,
  string literals and an unnamed heap area. Every byte of the image is
  accounted for.

## By input file: `--map`

Given the map from the link (EmbLD's `-Map` writes GNU ld's format, and GNU
ld's own works too), `embmap` attributes every input section to the object
file or library member it came from. It sorts them by size:

```text
$ embld -T stm32.ld startup.o prog.o io.o -o fw.elf -Map fw.map
$ embmap fw.elf --map fw.map
      text     rodata       data        bss  input file
       424         43         25        404  prog.o
       118         64          0          0  startup.o
       124          0          0          0  io.o
```

A library member is named as the map names it, `lib/libc.a(printf.o)`. Link
with `-ffunction-sections -fdata-sections` to see each function's input
section. EmbMap reads the map's wrapped lines, where a long section name
puts its address on the next line, as GNU ld writes them.

## Two builds: `--diff OLD`

```text
$ embmap fw.elf --diff fw-old.elf
against fw-old.elf: flash +24 bytes (796 -> 820), RAM +0 bytes (1972 -> 1972)

       old        new      delta  section
       644        668        +24  .text

       old        new      delta  symbol
       376        408        +32  main
```

Sections and symbols are matched by name and sorted by how much they
changed. A symbol's size is the total over every symbol of that name, so
two `static` helpers in different files are compared together. A symbol
only one build has is marked `(new)` or `(gone)`.

## Budgets

`--max-flash SIZE` and `--max-ram SIZE` make `embmap` exit with status 1
when the image's flash or RAM total exceeds the size. It also exits 1 when
any `--region` overflows. Each overflow is explained on standard error:

```text
$ embmap fw.elf --max-flash 512
embmap: fw.elf needs 820 bytes of flash and the budget is 512 (--max-flash)
$ echo $?
1
```

A size exactly at the budget passes. In a build, put it after the link:

```make
fw.elf: $(OBJS)
	embld -T part.ld $(OBJS) -o $@ -Map fw.map
	embmap $@ --region FLASH:0x08000000:512K --region RAM:0x20000000:128K
```

## JSON: `--json`

`--json` writes the same report as one JSON object: `image`, `elf`,
`byte_order`, `machine`, `flash`, `ram`, the four kind totals,
`sections` (each with `unattributed`), `regions`, and `top`, `files` and
`diff` when those were asked for. The text form is for people; scripts
should read the JSON.

## Exit status

| Status | Meaning |
|---|---|
| 0 | The report was written, and every budget and region fits. |
| 1 | A `--max-flash`, `--max-ram` or `--region` limit is exceeded. |
| 2 | The image cannot be read, or an option is malformed. |

## Notes

- **Stripped images.** An image without section headers is refused. A
  linked image keeps its section headers unless `strip` removed them.
- **Symbols.** Only functions and objects with a size are listed.
  Assembly that defines labels without `.size` shows up in the uncovered
  bytes instead.
- **Thumb.** A Thumb function's address has its low bit set in the
  symbol table; `embmap` clears it, as the bytes start one lower.
- **Objects.** An object file (`.o`) can be read too. Its sections all
  sit at address 0, so read its symbols and section sizes rather than
  regions.

`tests/golden/embmap.sh` checks every number against an independent tool:

- the regions against EmbLD's `--print-memory-usage` for the same STM32
  linker script;
- every section and symbol against `llvm-readelf` and `llvm-nm`, on
  Cortex-M, RV64 (ELF64) and big-endian MIPS images;
- the per-file breakdown, `--diff` (including two same-named statics) and
  the budgets.
