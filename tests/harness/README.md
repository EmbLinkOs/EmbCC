# tests/harness

The aarch64 proving ground.

## Why this exists

The exec suite used to compile a test with `embcc`, link it with the host
`cc`, and run it. That was a real end-to-end proof on the Linux x86-64 host
it was written for, because there the host WAS the target. It proves nothing
on a machine that is neither: an arm64 Mac cannot link, let alone run, an
x86-64 ELF object.

So `--target=aarch64-elf` runs its tests where they belong — on the
architecture they were compiled for, on QEMU's `virt` machine, which is the
same machine EmbLinkOS's own aarch64 kernel targets. About 25 ms per test.

## The pieces

- `qrun.sh` — runs QEMU under a hard timeout. macOS ships no `timeout`, and
  `perl -e 'alarm'; exec` does not work here: QEMU installs its own SIGALRM
  handler and swallows it, so a hung guest hangs the whole suite.
- `aarch64/start.S` — the entry stub. Longer than a `mov sp` because QEMU
  hands an ELF the CPU at EL1 with **SP undefined, `.bss` uninitialised,
  FP/SIMD trapped and the MMU off**, and every one of those four is here
  because leaving it out produced a hang. The MMU is the subtle one: with it
  off, every access is Device-nGnRnE memory, where unaligned accesses fault
  unconditionally — so newlib's `memset`, which stores 8 bytes at a 4-byte
  aligned address, takes an alignment Data Abort into a `VBAR_EL1` that is
  still zero. Mapping RAM as Normal memory is what makes ordinary C legal.
- `aarch64/link.ld` — a flat image at 0x40080000 with the heap and stack
  carved out of it, and the level-1 page table inside `.bss` so the zeroing
  loop clears the descriptors that are never written by hand.
- `aarch64/semihost.c` — the syscall floor: newlib's bare POSIX names over
  ARM semihosting. This is the same layer EmbLinkOS's `user/lib/syscalls.c`
  is, with `hlt #0xf000` where the OS has `svc #0`. Stock `librdimon` does
  not fit: the aarch64 newlib in `~/cross` was configured for EmbLinkOS's
  userland, which supplies the bare names, while librdimon defines the
  underscore-prefixed ones.
- `aarch64/link.sh` — links one embcc object into a runnable image.

- `aarch64/start.S`'s vector table and `harness_fault` — sixteen entries
  that all land in one place, and a handler that prints `ESR`, `FAR` and
  `ELR` and exits 125. Before it existed `VBAR_EL1` stayed zero, so every
  trap spun on an unmapped vector page until the twenty-second timeout
  killed QEMU, and a null dereference, an alignment abort and an illegal
  instruction were all reported as "the test hung".

Everything here is built with `aarch64-elf-gcc`. It is scaffolding, not the
thing under test: only the object under test came from `embcc`.

## Address zero is not mapped, on purpose

Both bare-metal harnesses used to map page zero, and a bare-metal image
has no kernel to object: `*(int *)0 = 1` simply wrote, read back, and the
test passed. On x86-64 the low 1 GiB was identity-mapped with 2 MiB
pages; on aarch64 the whole first gigabyte was one Device block "for
virt's MMIO window", which nothing here ever touches because all I/O goes
through semihosting.

So **no test in `tests/exec` or `tests/cxx` could detect a null
dereference**, on either target. That is not a theoretical gap: it hid a
real one. `std::cerr` was dynamically initialized, every translation unit
including `<iostream>` runs its own `__ios_init` constructor, and the one
that ran first stored through a `cerr` that was still null. The suite was
green. Unmapping the page turned it into `tests/golden/libcxx-std.sh`
failing on the spot.

x86-64 now splits the first 2 MiB into 4 KiB pages and leaves entry 0
absent — the image is linked at 1 MiB, so nothing else changes. The fault
escalates to a triple fault with no IDT installed, `-no-reboot` turns that
into QEMU exiting, and `run.sh` reports 125. aarch64 leaves level-1 entry
0 invalid, which the vector table above turns into a printed `ESR`/`FAR`
and the same 125. `tests/golden/static-init.sh` checks that a null
dereference still faults, so this cannot quietly come back.

## `linux/` — the one that boots somebody else's kernel

The harnesses above are *bare metal*: they start our code on a CPU and
supply the handful of services it needs themselves. `linux/run.sh` does
the opposite. It boots a **real Linux kernel** and runs a static image
built for `--target=*-linux-gnu` as PID 1, with nothing else in the
initramfs — no shell, no busybox, no libraries. That is only possible
because the image is complete: `nm -u` on it prints nothing.

It exists because the thing being checked is exactly the thing a shim
cannot check. `lib/libc/os/linux` writes down syscall numbers, `errno`
values, open flags and the layout of `statx` and `getdents64`, and none
of those can be read out of a header on a macOS build machine. A
hand-written harness would implement the same numbers this backend
calls, agree with them by construction, and go on agreeing if every one
of them were wrong. A kernel does not.

It has already earned that. `O_DIRECTORY` is `0200000` on x86-64 and
`040000` on aarch64 — the two architectures swap it with `O_DIRECT` —
and the single hardcoded value was right for one of them. On the wrong
one, `opendir` returned `EINVAL`, which reads like a bad argument rather
than like the wrong constant. Nothing short of a kernel would have said
so.

The exit status comes from the kernel rather than from the program. PID
1 is not allowed to exit, so when it does the panic message carries what
it returned — `Attempted to kill init! exitcode=0x00002a00` is 42 — and
that is a better witness than anything the program could print about
itself.

No kernel is committed: it is ten megabytes of somebody else's build, and
`.gitignore` keeps it out. `run.sh` says where to get one, looks for
`linux/vmlinuz-x86_64` or `linux/vmlinuz-aarch64` (or
`EMBCC_LINUX_KERNEL_*`), and exits 126 when there is none so the caller
skips instead of failing. On an Apple Silicon machine the aarch64 boot
uses `hvf` and takes about a twentieth of a second; x86-64 is emulated
and takes under two.
