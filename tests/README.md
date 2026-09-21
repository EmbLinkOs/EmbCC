# tests

Laid out like `src/`: what applies to every target at the top of each
directory, what is about one machine in a subdirectory named for it.

```
tests/
  run.sh              the runner: tests/run.sh [--target=TRIPLE] [--exec-only]
  lib.sh              shared by the .sh tests: host tool paths, link/run a program
  exec/               C programs compiled AND RUN on every target
  exec/x86_64/        ... on x86-64 only (AT&T inline asm)
  compile/            must compile, or must FAIL with a named diagnostic
  golden/             checks run for every target (they compile --target=$TARGET)
  golden/x86_64/      x86-64 only: embas vs nasm, embld, embdbg, EMBX, the
                      self-host fixed point, the kernel build and boot
  golden/aarch64/     aarch64 only: the encoder and inline-asm referees
  harness/<arch>/     the bare-metal QEMU harness that runs a target program
```

    tests/run.sh                        # x86_64-elf (the default): make test
    tests/run.sh --target=aarch64-elf   # make test-arm64
    tests/run.sh --exec-only            # just the compiled-and-RUN corpus
    EMBCC_KM1=1 sh tests/golden/x86_64/embbuild-kernel.sh   # opt-in, ~70 s

- **exec/** — programs COMPILED AND RUN, asserting the exit code (`// expect-exit:
  N`). These are the ones that count (CONTRIBUTING: "a change is not done
  because it compiles"). Every one is also built with the target's gcc and
  must exit the same way (`golden/agrees-with-gcc.sh`, strict `-std=c11`), so
  the corpus referees EmbCC's C against gcc's on each machine. Neither target
  runs on the host: x86-64 programs boot a Multiboot image under
  `qemu-system-x86_64` (natively only on an x86-64 Linux host), aarch64 ones
  a bare-metal image under `qemu-system-aarch64 -M virt` — see
  `harness/README.md`.
- **compile/** — refusal tests: an unsupported construct must fail loudly with
  a diagnostic naming it, never be silently miscompiled (THE RULE).
- **golden/** — host-side checks against a reference: gcc's output and ABI
  (programs half built by EmbCC and half by gcc, calling each other), a real
  gdb reading EmbCC's DWARF on a running program, the predefined macros, the
  optimizer's output. In `golden/<arch>/`, the machine-specific ones: nasm is
  the referee for embas, objdump and `aarch64-elf-as` for the aarch64 encoder
  and inline-asm assembler, QEMU booting the kernel for the whole toolchain.

What each target supports, and which of these suites covers it, is tabulated
in [docs/language/compatibility.md](../docs/language/compatibility.md).
