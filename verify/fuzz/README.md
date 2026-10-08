# verify/fuzz -- differential fuzzing

Random C programs, compiled by EmbCC and run on QEMU's boards, against
the same programs compiled by the host's compiler. A program has no
undefined behaviour by construction. It folds everything it computes
into one 64-bit checksum, and returns 42 only when its checksum is the
one built into it.

| Generator | What it adds | Boards | Runner |
|---|---|---|---|
| `gen2.py` | Integers of every width, `float` and `double`, structs by value, unions, bitfields, pointers into arrays and structs, function-pointer tables | x86-64, AArch64, Cortex-M4, RV32 (RV64 on request) | `run.sh` |
| `gen3.py` | gen 2 plus `__int128` | x86-64, AArch64, RV64 | `run.sh -g 3` |
| `gen4.py` | gen 3 plus `long double`, which is binary128 in software on RISC-V | RV32 and RV64; clang's build on the same board is the reference | `run4.sh` |

`gen.py` is the integer-only first version. Its docstring states the
rules that keep every generator free of undefined behaviour.

## Running

From the tree root, after `make embcc embld rt-embedded`. You need
`clang` (or `FUZZ_CC`), `python3` and the QEMU system emulators:

```sh
sh verify/fuzz/run.sh --selftest               # the runner can see a mismatch
sh verify/fuzz/run.sh 1000 1199                # 200 programs, gen 2, -O0 and -O2
sh verify/fuzz/run.sh -g 3 -O "-O0 -O2 -Os" 1 50
sh verify/fuzz/run.sh -b m4,rv32 1 100         # a subset of the boards
sh verify/fuzz/run4.sh 64 1 50                 # long double on RV64
```

Each seed prints `seed N done`. A failure adds a line before it, for
example `seed 7306 x86 -O2: exit 1`, and the program is kept in
`verify/fuzz/out/` (or `$FUZZ_OUT`). A seed whose two host runs disagree
under different stack fillings reads uninitialised memory. That is the
generator's fault: it is reported and skipped. `EMBCC_VERIFY=1` is set,
so the IR verifier runs as it does in the test suite.

Redirect a long run to a log and grep the log. Watching the tail of a
live run has hidden failures before.

## Reducing a failure

`reduce.py FILE.c TEST.sh` deletes lines while `TEST.sh FILE` still exits
0 ("still fails") and the file still parses. It writes the result to
`FILE.c.min.c`. The test is a script of your own. A typical one compiles
the candidate with EmbCC, runs it on the board that failed, and exits 0
when it does not return 42. It must also exit non-zero when the host
build itself misbehaves, so that the reduction cannot drift into
undefined behaviour. Use absolute paths in it: the reducer runs it from
its own directory.

## Before a release

The batch rule: before a batch of changes is merged, run at least 200
gen 2 programs with the verifier on. A seed range no earlier run used
finds what the previous ranges did not.
