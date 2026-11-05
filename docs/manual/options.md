# Option reference

Every option `embcc` takes, generated from the driver's own table (`src/driver/options.def`) by `embcc --reference=options`, so it lists exactly what the driver accepts.

EmbCC's own spellings come first. Where GCC spells the same thing differently, its spelling is accepted too and is shown beside it (with EmbCC's own older GCC-style ones), so a build written for GCC keeps working; [Migrating from GCC](migrating-from-gcc.md) covers the rest.

## What to build

| Option | What it does | Also accepted |
|---|---|---|
| `-c` | compile to an object; do not link |  |
| `-S` | write assembly (NAME.s) instead of an object |  |
| `-E` | preprocess only, to standard output |  |
| `--no-line-markers` | with -E: no # N "file" lines in the output | `-P` |
| `-o FILE` | where to write the result |  |
| `-j N` | compile N sources at once (-j alone: one per processor) |  |
| `--syntax-only` | check the source; write nothing | `-fsyntax-only` |
| `--lang=LANG` | read the inputs as c, c++, assembler or assembler-with-cpp | `-xLANG` |
| `--keep-temps` | keep the preprocessed source and the assembly | `-save-temps` |
| `--keep-temps=WHERE` | ...beside the object (obj) or in the current directory (cwd) | `-save-temps=WHERE` |
| `--emit-c` | print the C a C++ unit lowers to |  |
| `--emit-interfaces` | write the unit's interface record instead of an object |  |
| `--emit-empty-object FILE` | write an empty object for this target (it must come first) |  |
| `--prefix-map=OLD=NEW` | write a path under OLD as under NEW, in __FILE__, the DWARF and the gcov notes (reproducible builds) | `-ffile-prefix-map=OLD=NEW` |
| `--sbom` | record in the object its source and headers, with their hashes, for a link's --sbom= |  |
| `--sbom=FILE` | write the image's software bill of materials (SPDX 2.3 JSON; --sbom-format=cyclonedx) |  |
| `--sbom-format=FORMAT` | spdx (the default) or cyclonedx |  |
| `--sbom-from` | IMAGE [-o FILE]: an SBOM from a linked image built with --sbom |  |

## The language

| Option | What it does | Also accepted |
|---|---|---|
| `--std=STD` | the language standard: c89 to c2y (gnu17 by default), or for C++ c++98 to c++26 (gnu++20 by default) | `-std=STD` |
| `-D NAME[=VALUE]` | define a macro |  |
| `-U NAME` | undefine a macro |  |
| `-I DIR` | search DIR for headers |  |
| `--system-include=DIR` | search DIR for headers, as a system directory | `-isystem DIR` |
| `--include=FILE` | include FILE before the source | `-include FILE` |
| `--no-std-include` | do not search EmbCC's own headers | `-nostdinc` |
| `--freestanding` | a freestanding program: __STDC_HOSTED__ is 0 (the code is the same) | `-ffreestanding` |
| `--no-builtins` | accepted: only the __builtin_ names are built-ins | `-fno-builtin` |
| `--no-builtin=NAME` | accepted, for the same reason | `-fno-builtin-NAME` |
| `--char=signed` | plain char is signed | `-fsigned-char` |
| `--char=unsigned` | plain char is unsigned | `-funsigned-char` |
| `--wchar=short` | wchar_t is 16 bits | `-fshort-wchar` |
| `--wchar=int` | wchar_t is the target's default | `-fno-short-wchar` |
| `--module-dir=DIR` | C++ modules: write interfaces (NAME.ebmi) to DIR and look for them there | `-fmodule-dir=DIR` |
| `--permissive` | old C: implicit int and implicit function declarations, warned | `-fpermissive` |
| `--enums=short` | an enum is the smallest type its values fit | `-fshort-enums` |
| `--enums=int` | an enum is int-sized | `-fno-short-enums` |
| `--enums=neutral` | objects that link with either enum size (for libraries) | `-fenum-size-neutral` |
| `--wrap-overflow` | signed overflow wraps (EmbCC's default; stated) | `-fwrapv` |
| `--common` | tentative definitions are COMMON symbols (C, ELF) | `-fcommon` |
| `--no-common` | tentative definitions are definitions | `-fno-common` |
| `--float-constants=single` | an unsuffixed 1.0 is a float (C) | `-fsingle-precision-constant` |
| `--float-constants=double` | an unsuffixed 1.0 is a double | `-fno-single-precision-constant` |
| `--exceptions` | C++ exceptions (the default) | `-fexceptions` |
| `--no-exceptions` | C++ without exceptions | `-fno-exceptions` |
| `--rtti` | C++ run-time type information (the default) | `-frtti` |
| `--no-rtti` | C++ without run-time type information | `-fno-rtti` |
| `--access-control` | enforce private and protected (the default) | `-faccess-control` |
| `--no-access-control` | do not enforce private and protected | `-fno-access-control` |
| `--char8` | C++: define __cpp_char8_t before C++20 | `-fchar8_t` |
| `--strict-aliasing` | accepted; EmbCC does not optimize by type-based aliasing | `-fstrict-aliasing` |
| `--no-strict-aliasing` | accepted; nothing to turn off | `-fno-strict-aliasing` |

## The target

| Option | What it does | Also accepted |
|---|---|---|
| `--target=TARGET` | what to compile for (an unknown name lists them all) |  |
| `--board=BOARD` | a development board: its target, part and clock, its startup and runtime, and <board.h> (uno, nano, mega2560, esp32-devkit; --reference=boards lists them) |  |
| `--cpu=CPU` | the processor: cortex-m4, cortex-m0plus, ... | `-mcpu=CPU` |
| `--fpu=FPU` | the floating-point unit: fpv4-sp-d16, fpv5-d16, ... | `-mfpu=FPU` |
| `--float-abi=ABI` | soft, softfp or hard | `-mfloat-abi=ABI` |
| `--mcu=PART` | the part: an AVR (atmega328p, ...) or a PIC32 on mipsel (32MZ2048EFH100, ...; XC32's -mprocessor=) | `-mmcu=PART` |
| `--dfp=DIR` | a PIC32's device pack (DFP), as MPLAB X installs it; found under ~/.mchp_packs when not named | `-mdfp=DIR` |
| `--isa=thumb` | Thumb instructions (ARM) | `-mthumb` |
| `--isa=arm` | ARM (A32) instructions | `-marm` |
| `--isa=ISA` | the instruction set: rv32imac, rv64gc, armv7-a, mips32r2, isaa ... | `-march=ISA` |
| `--tune=CPU` | the core to schedule for (ARM): accepted, EmbCC does not schedule | `-mtune=CPU` |
| `--abi=ABI` | the calling convention: ilp32, ilp32f, lp64d, 32 ... | `-mabi=ABI` |
| `--cmse` | Armv8-M Security Extension entry points | `-mcmse` |
| `--endian=little` | little-endian | `-mlittle-endian` |
| `--endian=big` | big-endian | `-mbig-endian` |
| `--unaligned-access` | the core takes unaligned loads and stores | `-munaligned-access` |
| `--no-unaligned-access` | it does not | `-mno-unaligned-access` |
| `--no-sse` | x86-64: no SSE (kernels) | `-mno-sse` |
| `--no-red-zone` | x86-64: no red zone (kernels) | `-mno-red-zone` |
| `--general-regs-only` | AArch64: no floating-point or SIMD registers | `-mgeneral-regs-only` |
| `--code-model=MODEL` | x86-64: small or kernel | `-mcmodel=MODEL` |
| `--macos-min=VERSION` | the oldest macOS the program runs on | `-mmacos-version-min=VERSION` |
| `--print-target` | print the target's name and stop | `-dumpmachine` |

## Optimization

| Option | What it does | Also accepted |
|---|---|---|
| `--lto` | link-time optimization: an -flto link compiles the units again as one program | `-flto` |
| `-O LEVEL` | 0, 1, 2, 3, s, z or g: none, basic, speed, max, size, min-size or debug |  |
| `--opt=none` | no optimization: each statement as written | `-O0` |
| `--opt=basic` | the passes that only remove work: never larger code | `-O1` |
| `--opt=debug` | --opt=basic, with every variable kept where a debugger reads it | `-Og` |
| `--opt=speed` | fast code: --opt=basic and the passes that trade size for speed | `-O2` |
| `--opt=max` | the same as --opt=speed | `-O3` |
| `--opt=size` | small code: no transform that grows it | `-Os` |
| `--opt=min-size` | the same as --opt=size | `-Oz` |
| `--pass=NAME` | turn one optimizer pass on | `-fNAME` |
| `--no-pass=NAME` | turn one optimizer pass off | `-fno-NAME` |
| `--sections=functions` | each function in its own section, for the linker to drop | `-ffunction-sections` |
| `--sections=data` | each object in its own section | `-fdata-sections` |
| `--sections=functions,data` | both | `-ffunction-sections -fdata-sections` |
| `--sections=data,functions` | both | `-ffunction-sections -fdata-sections` |
| `--inline=declared` | inline only what is declared inline | `-fno-inline-functions` |
| `--inline=auto` | inline what pays (the default from -O2) | `-finline-functions` |
| `--unroll` | unroll loops | `-funroll-loops` |
| `--no-unroll` | do not | `-fno-unroll-loops` |
| `--jump-tables` | a dense switch may be a table (the default) | `-fjump-tables` |
| `--no-jump-tables` | no switch through a table of addresses | `-fno-jump-tables` |
| `--frame-pointer` | keep a frame pointer in every function | `-fno-omit-frame-pointer` |
| `--no-frame-pointer` | only where one is needed | `-fomit-frame-pointer` |
| `--fp-contract=MODE` | off, on or fast: may a*b+c become one fused operation | `-ffp-contract=MODE` |
| `--fast-math` | floating point without IEEE's exceptions and signed zeros | `-ffast-math` |
| `--no-math-errno` | the math functions need not set errno | `-fno-math-errno` |
| `--unwind-tables` | unwind tables for every function | `-funwind-tables` |
| `--no-unwind-tables` | none where not needed | `-fno-unwind-tables` |
| `--instrument-functions` | call the profiling hooks at every entry and exit | `-finstrument-functions` |
| `--no-instrument-functions` | do not | `-fno-instrument-functions` |
| `--instrument-exclude-files=LIST` | ...except in these files (comma-separated) | `-finstrument-functions-exclude-file-list=LIST` |
| `--instrument-exclude-functions=LIST` | ...except these functions | `-finstrument-functions-exclude-function-list=LIST` |
| `--stack-protector` | a canary below the return address of a function with a character array | `-fstack-protector` |
| `--stack-protector=strong` | ...of one with any array or a local whose address is taken | `-fstack-protector-strong` |
| `--stack-protector=all` | ...of every function | `-fstack-protector-all` |
| `--stack-protector=explicit` | ...of a function marked stack_protect | `-fstack-protector-explicit` |
| `--no-stack-protector` | no canaries | `-fno-stack-protector` |
| `--stack-clash-protection` | no stack-pointer step of more than a page, and each page touched | `-fstack-clash-protection` |
| `--bounds-safety` | every subscript of an array, or of a counted_by member, checked: a trap when out of bounds | `-fbounds-safety` |
| `--no-bounds-safety` | not | `-fno-bounds-safety` |
| `--branch-protection=KIND` | AArch64 and ARMv8.1-M: standard, bti, pac-ret[+leaf][+b-key], none | `-mbranch-protection=KIND` |
| `--cf-protection=KIND` | x86-64: full, branch, return, none | `-fcf-protection=KIND` |
| `--profile-generate` | a build that records its profile, for --profile-use | `-fprofile-generate` |
| `--profile-generate=DIR` | ...the profile in DIR | `-fprofile-generate=DIR` |
| `--profile-use` | optimize by the recorded profile: hot and cold code, branches, inlining | `-fprofile-use` |
| `--profile-use=PATH` | ...the profile in PATH (a .gcda, or a directory) | `-fprofile-use=PATH` |
| `--profile-dir=DIR` | where profiles are written and read | `-fprofile-dir=DIR` |

## Warnings and diagnostics

| Option | What it does | Also accepted |
|---|---|---|
| `--warn=all` | the warnings most code wants | `-Wall` |
| `--warn=extra` | ...and the stricter ones | `-Wextra` |
| `--warn=NAME` | this warning (--help-warnings lists them) | `-WNAME` |
| `--no-warn=NAME` | not this warning | `-Wno-NAME` |
| `--warnings-are-errors` | every warning is an error | `-Werror` |
| `--no-warnings-are-errors` | they are not | `-Wno-error` |
| `--error=NAME` | this warning is an error | `-Werror=NAME` |
| `--no-error=NAME` | this one is a warning again | `-Wno-error=NAME` |
| `--no-warnings` | no warnings at all | `-w` |
| `--pedantic` | warn for what the --std standard does not have | `-pedantic` |
| `--pedantic-errors` | ...as errors | `-pedantic-errors` |
| `--warn-in-system-headers` | warn in system headers too | `-Wsystem-headers` |
| `--diagnostics=FORMAT` | text, or json for tools | `-fdiagnostics-format=FORMAT` |
| `--color=WHEN` | auto, always or never | `-fdiagnostics-color=WHEN` |
| `--max-errors=N` | stop after N errors | `-fmax-errors=N` |
| `--explain=ID` | what a diagnostic means, and the fix |  |
| `--fix` | apply the fix-its EmbCC proposes |  |
| `--analyze` | the static analyzer: what interrupt handlers share unsafely, double free, use after free, leaks | `-fanalyzer` |
| `--no-analyze` | not | `-fno-analyzer` |
| `--misra` | check the MISRA C:2023 rules a compiler can decide (each one --warn=misra-N.M) |  |
| `--misra=YEAR` | ...2023 (or 2012, the same numbers) |  |
| `--no-misra` | none of them |  |
| `--cert` | check the CERT C rules a compiler can decide (each one --warn=cert-XXXNN) |  |
| `--no-cert` | none of them |  |
| `--remarks` | say what the optimizer did and did not do | `-fremarks` |
| `--remarks=json` | ...as JSON | `-fremarks=json` |

## Debugging and reports

| Option | What it does | Also accepted |
|---|---|---|
| `-g` | debug information (DWARF) |  |
| `--debug` | the same | `-g` |
| `--stack-usage` | write FILE.su: each function's frame | `-fstack-usage` |
| `--stack-info` | put each function's frame, calls and worst-case cycles in the object, for --stack-report |  |
| `--stack-report` | IMAGE [--stack-limit=N]: the program's worst-case stack and cycles, from a --stack-info build |  |
| `--call-graph` | write FILE.ci: the call graph | `-fcallgraph-info` |
| `--call-graph=su` | ...with each function's stack | `-fcallgraph-info=su` |
| `--time-report` | how long each phase took | `-ftime-report` |
| `--sanitize=LIST` | address, thread, memory (with their runtimes); undefined, ... (checks that trap) | `-fsanitize=LIST` |
| `--sanitize-recover=LIST` | report and go on: address, undefined (and its checks), all | `-fsanitize-recover=LIST` |
| `--no-sanitize=LIST` | not these checks | `-fno-sanitize=LIST` |
| `--coverage` | count every line and branch: FILE.gcno now, FILE.gcda when the program runs, for gcov |  |
| `--coverage-files=REGEX` | ...only in the files these match (;-separated) | `-fprofile-filter-files=REGEX` |
| `--coverage-exclude-files=REGEX` | ...except the files these match | `-fprofile-exclude-files=REGEX` |
| `--coverage-update=WAY` | how a counter is added to: single, atomic, prefer-atomic | `-fprofile-update=WAY` |
| `--coverage-version=VERSION` | the files' gcov generation: gcc (GCC 12 on, the default), llvm (what llvm-cov reads), or a GCC version string |  |
| `--gcov-extract` | write the .gcda files a board printed (a console log: the files, or -) |  |

## Dependencies

| Option | What it does | Also accepted |
|---|---|---|
| `--deps=FILE` | write the make rule for the user headers to FILE, and compile | `-MMD -MF FILE` |
| `--deps-all=FILE` | ...system headers included | `-MD -MF FILE` |
| `--deps-only` | write the make rule instead of compiling | `-MM` |
| `--deps-target=TARGET` | the rule's target | `-MT TARGET` |
| `--deps-phony` | a phony rule for each header | `-MP` |

## Linking

| Option | What it does | Also accepted |
|---|---|---|
| `-l NAME` | link libNAME.a |  |
| `-L DIR` | search DIR for libraries |  |
| `--link-script=FILE` | the linker script (a GNU ld script) | `-T FILE` |
| `--entry=SYMBOL` | the entry symbol | `-e SYMBOL` |
| `--undefined=SYMBOL` | pull SYMBOL from the archives | `-u SYMBOL` |
| `--no-stdlib` | no start files, no C library, no runtime | `-nostdlib` |
| `--no-start-files` | no start files | `-nostartfiles` |
| `--no-default-libs` | no C library, no runtime | `-nodefaultlibs` |
| `--static` | a static program | `-static` |
| `--linker-arg=ARG` | pass ARG to EmbLD | `-Xlinker ARG` |
| `--libc=LIBC` | embcc (EmbCC's own C library) or system (a Linux target's glibc: its headers, libraries and linker) |  |
| `--sysroot=DIR` | the target system's root, for --libc=system (default /) |  |
| `--linker=PATH` | the system's linker, for --libc=system (default: TRIPLE-ld, ld.lld or ld) | `--ld-path=PATH` |
| `--assembler-arg=ARG` | pass ARG to the assembler | `-Wa,ARG` |

## Asking EmbCC

| Option | What it does | Also accepted |
|---|---|---|
| `--help` | this list |  |
| `--help=gcc` | the other spellings EmbCC accepts: GCC's, and EmbCC's for each |  |
| `--reference=PAGE` | a reference page: options, warnings or targets in Markdown, or the man page |  |
| `--help=spellings` | every spelling, one per line (for tests and tools) |  |
| `--help-warnings` | every warning, and the sets |  |
| `--version` | the version and the default target |  |
| `--dump-predef` | the predefined macros for this target |  |
| `--print-search-dirs` | where EmbCC found its own files |  |

## GCC spellings with no EmbCC name

GCC flags EmbCC reads, either because they change nothing here or so as to refuse them by name.

| GCC spelling | Notes |
|---|---|
| `-ansi` | --std=c89 for C, and --std=c++98's macros for C++ |
| `-x` | the language, as a separate argument (--lang=) |
| `-pipe` | accepted: EmbCC writes no temporary files |
| `-dumpbase` | accepted with -save-temps |
| `-specs=*` | a GCC specs file |
| `--specs=*` | a GCC specs file |
| `-fdump-*` | GCC's internal dumps |
| `-fverbose-asm` | accepted |
| `-h` | --help |
| `--explain` | --explain=, with the ID as the next argument |
| `--save-temps` | --keep-temps |
| `-dM` | with -E: print the macros |
| `-isysroot` | macOS: the SDK's root |
| `-framework` | macOS: link a framework |
| `-arch` | macOS: the architecture |
| `-fgnu89-inline` | GNU89's meaning of inline |
| `-fno-gnu89-inline` | C99's meaning of inline |
| `-fgnuc-version=*` | the __GNUC__ to claim |
| `-fno-zero-initialized-in-bss` | zero-initialized objects in .data |
| `-fzero-initialized-in-bss` | the default |
| `-fstrict-volatile-bitfields` | volatile bit-fields by their declared type's width |
| `-fno-strict-volatile-bitfields` | the default |
| `-mthumb-interwork` | accepted: every target EmbCC has interworks |
| `-mno-thumb-interwork` | accepted |
| `-mslow-flash-data` | accepted |
| `-mno-sse2` | x86-64 |
| `-mno-mmx` | x86-64 |
| `-mno-80387` | x86-64 |
| `-mmacosx-version-min=*` | --macos-min= |
| `-fno-pic` | position-dependent code (the default) |
| `-fno-PIC` | accepted |
| `-fno-pie` | accepted |
| `-fno-PIE` | accepted |
| `-fpic` | position-independent code, for a shared object (x86-64, AArch64, RISC-V) |
| `-fPIC` | position-independent code, for a shared object (x86-64, AArch64, RISC-V) |
| `-fpie` | a position-independent executable |
| `-fPIE` | a position-independent executable |
| `-fno-plt` | accepted |
| `-shared` | a shared library (--libc=system's linker) |
| `-pie` | a position-independent executable |
| `-no-pie` | an ordinary executable |
| `-nopie` | -no-pie |
| `-static-pie` | a static position-independent executable |
| `-pthread` | POSIX threads: with --libc=system, glibc's (_REENTRANT, -lpthread) |
| `-fuse-ld=*` | the system's linker for --libc=system: bfd, gold or lld (ld.NAME) |
| `-fno-ident` | accepted |
| `-fident` | accepted |
| `-fno-reorder-functions` | accepted |
| `-freorder-functions` | accepted |
| `-funroll-all-loops` | --unroll |
| `-finline-small-functions` | accepted |
| `-fno-inline-small-functions` | accepted |
| `-finline-limit=*` | accepted |
| `-funsafe-math-optimizations` | part of --fast-math |
| `-fno-signed-zeros` | part of --fast-math |
| `-fno-trapping-math` | part of --fast-math |
| `-ffinite-math-only` | part of --fast-math |
| `-fassociative-math` | part of --fast-math |
| `-freciprocal-math` | part of --fast-math |
| `-fmerge-constants` | accepted |
| `-fno-strict-overflow` | accepted: overflow wraps |
| `-fno-delete-null-pointer-checks` | accepted |
| `-fno-tree-loop-distribute-patterns` | accepted |
| `-fno-isolate-erroneous-paths-*` | accepted |
| `-fno-move-loop-invariants` | accepted |
| `-fno-ipa-sra` | accepted |
| `-fno-lto` | accepted |
| `-flto` | link-time optimization |
| `-flto=*` | --lto (thin, full, auto, N: one whole-program merge) |
| `-flto-partition=*` | accepted: the merge is one partition |
| `-flto-compression-level=*` | accepted |
| `-ffat-lto-objects` | accepted: an --lto object always has its code too |
| `-fno-fat-lto-objects` | accepted |
| `-fuse-linker-plugin` | accepted: the merge needs no plugin |
| `-fno-use-linker-plugin` | accepted |
| `-fstack-protector*` | --stack-protector (-strong, -all, -explicit) |
| `-fno-stack-protector` | no canaries (the default) |
| `--param=ssp-buffer-size=*` | the smallest character array -fstack-protector guards (8) |
| `--param` | with ssp-buffer-size=N |
| `-fdebug-prefix-map=*` | --prefix-map=, for the DWARF only |
| `-fmacro-prefix-map=*` | --prefix-map=, for __FILE__ only |
| `-fprofile-prefix-map=*` | --prefix-map=, for the gcov notes only |
| `-frandom-seed*` | accepted: EmbCC's output has no randomness |
| `-mstack-protector-guard*` | accepted: the guard is __stack_chk_guard |
| `-fstack-clash-protection` | --stack-clash-protection |
| `-fno-stack-clash-protection` | no probes (the default) |
| `-fcf-protection*` | x86-64 CET: endbr64 landing pads and the shadow-stack note (full, branch, return, none) |
| `-fasynchronous-unwind-tables` | --unwind-tables |
| `-fno-asynchronous-unwind-tables` | --no-unwind-tables |
| `-fno-function-sections` | the default |
| `-fno-data-sections` | the default |
| `-ffp-contract=*` | --fp-contract= |
| `-fmodules` | accepted: C++20 modules are on from -std=c++20 |
| `-fmodules-ts` | accepted |
| `-fprebuilt-module-path=*` | --module-dir=, for reading only |
| `-W` | -Wextra |
| `-Wl,*` | --linker-arg= (comma-separated) |
| `-Wa,*` | --assembler-arg= (comma-separated) |
| `-Werror-implicit-function-declaration` | --error=implicit-function-declaration |
| `-Wformat=*` | the format warnings' level |
| `-fdiagnostics-color` | --color=always |
| `-fno-diagnostics-color` | --color=never |
| `-fdiagnostics-parseable-fixits` | fix-its for an IDE to apply |
| `-fmessage-length=*` | accepted |
| `-fanalyzer` | GCC's static analyzer |
| `-ggdb` | -g |
| `-g1` | -g |
| `-g2` | -g |
| `-g3` | -g |
| `-gdwarf` | -g |
| `-gdwarf-2` | DWARF 4, which a version 2 reader of it also takes |
| `-gdwarf-3` | DWARF 4, which a version 3 reader of it also takes |
| `-gdwarf-4` | DWARF 4 (-g writes DWARF 5) |
| `-gdwarf-5` | DWARF 5, as -g |
| `-gsplit-dwarf` | split DWARF: most of it in FILE.dwo |
| `-gz` | zlib-compressed debug sections (-gz=zlib, -gz=none) |
| `-gz=*` | -gz=zlib compresses, -gz=none does not |
| `-fcallgraph-info*` | --call-graph |
| `-fsanitize*` | the sanitizers |
| `-fno-sanitize=*` | accepted |
| `-fno-sanitize-recover=*` | stop at the first report (the default) |
| `-fno-sanitize-address-use-after-scope` | accepted |
| `-fsanitize-memory-track-origins*` | accepted: a report says where the value is used |
| `-static-libasan` | accepted: the runtimes are archives |
| `-static-libtsan` | accepted |
| `-static-libsan` | accepted |
| `-shared-libsan` | accepted |
| `-fprofile*` | the gcov profile options; -fprofile-instr-* (clang's format) refused |
| `-fno-profile*` | accepted |
| `-ftest-coverage` | the .gcno |
| `-fno-test-coverage` | accepted |
| `-coverage` | --coverage |
| `-lgcov` | libgcov, for --coverage |
| `-fbranch-probabilities` | accepted: --profile-use |
| `-fno-branch-probabilities` | accepted |
| `-fvpt` | accepted |
| `-fno-vpt` | accepted |
| `-fcoverage-mapping` | clang's coverage format: refused (--coverage) |
| `-pg` | gprof profiling |
| `-M` | the make rule, system headers included, instead of compiling |
| `-MD` | --deps-all= |
| `-MMD` | --deps= |
| `-MF` | the rule's file |
| `-MQ` | the rule's target, quoted |
| `-Xlinker` | --linker-arg= |
| `-e` | --entry= |
| `-u` | --undefined= |
| `-T` | --link-script= |
