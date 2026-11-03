# Coverage and profile-guided optimization

EmbCC counts what a program runs, in the files GCC's gcov reads, and
optimizes a second build by those counts. It is one mechanism with two
uses:

- **Coverage** (`--coverage`): which lines, branches and functions a test
  run executed, read with `gcov`, `llvm-cov gcov`, `lcov` or `gcovr`.
- **Profile-guided optimization** (`--profile-generate`, then
  `--profile-use`): the code the profile says is hot is laid out in a
  straight line and inlined more, and the code it never saw run is moved
  out of the way and kept small.

Both work on every target, hosted or bare metal. A board with no file
system prints its counts on the console, and `embcc --gcov-extract` turns
the console log back into the files.

## Coverage

```sh
embcc --coverage -c parse.c -o parse.o     # parse.gcno beside the object
embcc --coverage parse.o main.c -o tool    # links libgcov
./tool input.txt                           # writes parse.gcda, main.gcda
gcov parse.c                               # parse.c.gcov: each line's count
```

A compile with `--coverage` writes the notes file, `NAME.gcno`, next to
the object. It holds the shape of each function's control flow and the
line each block came from. When the program exits, or calls
`__gcov_dump()`, each unit's counts go into `NAME.gcda`. A second run
adds to the counts already in the file, as with GCC. Remove the `.gcda`
files to start again.

When one command compiles and links, the files are named after the image
as well as the source, as GCC and clang name them: `embcc --coverage t.c
-o x` writes `x-t.gcno` and, at run time, `x-t.gcda`.

| Option | GCC's spelling | Effect |
|---|---|---|
| `--coverage` | `-coverage`, `-fprofile-arcs -ftest-coverage` | Counts every arc of the control flow and writes the notes; links libgcov |
| `--coverage-files=REGEX` | `-fprofile-filter-files=` | Only the source files these regular expressions match (separate several with `;`) |
| `--coverage-exclude-files=REGEX` | `-fprofile-exclude-files=` | All but the files these match |
| `--coverage-update=WAY` | `-fprofile-update=` | `single` (the default): a plain add. `atomic`: an atomic add, for a program whose threads or interrupt handlers run instrumented code at once. `prefer-atomic`: atomic where the target has the instruction |
| `--coverage-version=VERSION` | | The files' format generation. `gcc` (the default) is GCC 12 and later, which GCC 16's gcov reads. `llvm` is the generation `llvm-cov gcov` reads. A GCC version string such as `B62*` is also taken |
| `--prefix-map=OLD=NEW` | `-fprofile-prefix-map=` | The paths in the notes, as with the other prefix maps; see [Reproducible builds](reproducible-builds.md) |

Counting is placed only where it has to be. A spanning tree of each
function's flow graph is left uncounted, and gcov solves those arcs from
the rest, as GCC's instrumentation does. A loop's counter therefore sits
on its exit, not on every trip.

### Where the files go

At run time, `GCOV_PREFIX` and `GCOV_PREFIX_STRIP` move the `.gcda`
files, as with GCC. The first `GCOV_PREFIX_STRIP` directories of the
recorded path are dropped, and `GCOV_PREFIX` is put in front of what is
left:

```sh
GCOV_PREFIX=/tmp/cov GCOV_PREFIX_STRIP=3 ./tool
```

### Coverage on a board

A board's C library has no files to open. There, libgcov prints each
`.gcda` file on the console as hex between two marker lines:

```text
@gcda /home/me/fw/build/motor.gcda 412
6164636742363226000000000c0000000000000001000000...
@end
```

Capture the console, from a UART, semihosting, a debugger's log or
EmbSim, and give the log to the compiler on the host:

```sh
embsim fw.elf > console.log              # or the board's serial log
embcc --gcov-extract console.log         # writes build/motor.gcda, ...
gcov -o build motor.c
```

`--gcov-extract` reads `-` as standard input. Text before, between and
after the blocks is skipped, so a log with the program's own output works
as it is. When the file already exists, the extracted counts are added
to it. Several runs on the board then add up the same way as on a host.

The console is `putchar`, through `__embcc_gcov_putc(int c)`. A program
may define that function to send the bytes elsewhere, a second UART or a
debugger's channel. A program that wants to send the files itself
defines `__embcc_gcov_output` instead. On AVR, whose C library has no
files at all, the counts are streamed byte by byte rather than built in
RAM.

On a board, call `__gcov_dump()` where the program stops. Firmware
rarely returns from `main`, and the counts are only written at exit or
on that call.

### Reading the counts

Any gcov reader works. GCC's `gcov` 12 and later reads the default
format. For `llvm-cov gcov`, build with `--coverage-version=llvm`. `lcov`
and `gcovr` call one of the two, so name the right one (`gcovr
--gcov-executable "llvm-cov gcov"`).

Line counts, branch counts (`gcov -b`), function counts and the
`#####` marks of lines that never ran all come from the same counts.
Blocks the compiler proved unreachable are left out of the notes, so
they are not reported as missed lines.

## Profile-guided optimization

```sh
embcc -O2 --profile-generate -c *.c        # instrumented objects
embcc -O2 --profile-generate *.o -o app
./app typical-workload                     # writes the .gcda profile
embcc -O2 --profile-use -c *.c             # optimized by the profile
embcc -O2 *.o -o app
```

A `--profile-generate` build counts the same way as `--coverage` does,
but writes no notes. Its profile is the `.gcda` files. A `--profile-use`
build of the same sources reads them, finds every arc's count, and acts
on them:

- **Block placement.** The blocks the profile never saw run are moved to
  the end of their function, and the branch that skipped them is
  inverted, so the path that runs is one straight line and its branches
  fall through.
- **Function placement.** A function that never ran goes to
  `.text.unlikely`, and one that ran often goes to `.text.hot`. A linker
  script that keeps those sections together keeps the hot code within
  one cache or flash-accelerator region.
- **Size where it does not matter.** A function the profile says never
  ran is compiled for size, whatever `-O` says.
- **Inlining.** A call on a path that never ran is left alone
  (`cold-call-site`), and so is a call of a function that never ran
  (`callee-is-cold`). A call on a hot path gets a larger budget.
  `-fremarks` names each decision.

| Option | GCC's spelling | Effect |
|---|---|---|
| `--profile-generate` | `-fprofile-generate` | Counts, for a later `--profile-use` |
| `--profile-generate=DIR` | `-fprofile-generate=DIR` | Writes the profile under `DIR` |
| `--profile-use` | `-fprofile-use`, `-fbranch-probabilities` | Optimizes by the profile found next to each object |
| `--profile-use=PATH` | `-fprofile-use=PATH` | Reads the profile from `PATH`, a `.gcda` file or a directory |
| `--profile-dir=DIR` | `-fprofile-dir=DIR` | Where the profile is written and read |

clang's own profile formats, `-fprofile-instr-generate` and
`-fprofile-instr-use`, and its source-based coverage,
`-fcoverage-mapping`, are refused by name. EmbCC writes GCC's format
only.

Two warnings report a profile that does not fit:

- [`--warn=missing-profile`](warnings.md): `--profile-use` found no
  profile for a unit, which is then optimized as without one.
- [`--warn=coverage-mismatch`](warnings.md): the profile is for a
  different version of a function, whose control flow no longer matches.
  That function's counts are ignored, and the rest are used.

A profile is only as good as the run that made it. Record it with a
workload like the one the product runs. An error path that never ran in
the profiling run is treated as cold, and that is usually what you want.

### Without a profile

Three sources of the same information need no profiling run:

- `__builtin_expect(cond, value)` marks the other arm of an `if` cold.
- A path that calls a `noreturn` function, or a function marked `cold`,
  is cold.
- `__attribute__((hot))` and `__attribute__((cold))` on a function put
  it in `.text.hot` or `.text.unlikely`, and a `cold` one is compiled
  for size.

All three feed the same block placement as a profile does.
`__attribute__((no_profile_instrument_function))` leaves a function
uncounted by `--coverage` and `--profile-generate`.
