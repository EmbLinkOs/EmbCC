# EmbCC scoreboard

f5bde5bd Merge pull request #183 from EmbLinkOs/perf4 -- 2026-11-08 -- reference: Homebrew clang version 23.1.5

Every ratio is EmbCC / clang: below 1.00, EmbCC is ahead.

## 1. Speed (estimated cycles, tools/bench)

(skipped: --quick)

## 2. Code size (lib/libc at -Os)

Per object: every function of each file both compilers build. Per
function: the functions both define, which a static EmbCC inlines into
its one caller skews (see the script).

| Target | per object | without math | per function | without math | objects |
|---|---|---|---|---|---|
| thumbv7em-none-eabi | 1.035 | 0.991 | 1.147 | 1.252 | 197 |
| thumbv6m-none-eabi | 1.103 | 1.068 | 1.225 | 1.347 | 197 |
| riscv32-unknown-elf | 1.121 | 0.998 | 1.226 | 1.230 | 197 |
| riscv64-unknown-elf | 1.149 | 0.995 | 1.268 | 1.221 | 197 |
| x86_64-elf | 1.491 | 1.255 | 1.686 | 1.546 | 198 |
| aarch64-elf | 1.319 | 1.153 | 1.506 | 1.420 | 198 |
| mipsel-none-elf | 1.156 | 1.086 | 1.290 | 1.365 | 197 |
| powerpc-none-eabi | 1.163 | 1.054 | 1.296 | 1.303 | 196 |
| sparc-none-elf | 1.161 | 1.053 | 1.289 | 1.311 | 197 |
| loongarch64-unknown-elf | 1.057 | 1.008 | 1.168 | 1.226 | 197 |
| avr | 1.346 | 1.261 | 1.536 | 1.565 | 165 |

## 3. Compile speed (x86_64-elf at -O2, one file at a time, wall clock)

| Corpus | files | EmbCC | clang | ratio |
|---|---|---|---|---|
| lib/libc | 229 | 2.9 s | 9.2 s | 0.31 |
| EmbCC's src/ | 216 | 25.0 s | 23.3 s | 1.08 |

## 4. Limitations (distinct refusals in src/)

| Backend refusals | Front-end and driver refusals |
|---|---|
| 110 | 42 |

Backend refusals by target:

- aarch64: 0
- avr: 11
- coldfire: 16
- loongarch: 23
- mips: 19
- ppc: 16
- riscv: 23
- rx: 15
- sparc: 20
- thumb: 16
- tricore: 15
- x86_64: 0
- xtensa: 17
