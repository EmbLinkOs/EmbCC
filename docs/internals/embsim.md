# EmbSim's internals

This page is for anyone who changes EmbSim (`tools/embsim`): how it is
put together, the interfaces between its parts, and how to add a core, a
peripheral or a board. The command reference is
[the manual page](../manual/tools/embsim.md).

EmbSim is ISO C99 with no dependencies beyond libm, built warning-free
with the project's flags (`-std=c99 -Wall -Wextra -Werror`), and each
file but the network one compiles with EmbCC itself. Everything is in
`tools/embsim/`, but for the SVD reader it shares with embsvd
(`tools/embsvd/svd.c`), so the two directories move together.

## The parts

| File | What it is |
|---|---|
| `sim.h` | the shared types, and every interface below |
| `main.c` | the command line |
| `run.c` | building the machine, the run loop, time, the run's end, the console |
| `bus.c` | memory regions, bit-band aliases, devices by address, watchpoints |
| `boards.c` | the boards as data, and the catalogues of cores and device types |
| `loader.c` | the ELF image |
| `semihost.c` | ARM semihosting |
| `trace.c` | `--trace`, `--count`, `--stats` |
| `gdb.c` | the GDB remote server |
| `net.h`, `net-posix.c`, `net-none.c` | the server's connection: POSIX sockets, or none |
| `cortexm.h` | the Cortex-M's state, shared by its files |
| `cortexm.c` | the Cortex-M core: memory access, exceptions, step, reset, its CPU interface |
| `cortexm-thumb.c` | the Thumb instruction set |
| `cortexm-fpu.c` | the floating-point unit |
| `scs.c` | the system control space: the NVIC's and the SCB's registers |
| `systick.c`, `dwt.c` | SysTick, DWT's cycle counter |
| `uart-pl011.c`, `uart-cmsdk.c`, `uart-nrf51.c` | the boards' UARTs |
| `riscv.h` | the RISC-V core's state, shared by its files |
| `riscv.c` | the RISC-V core: memory, traps, the CSRs, the integer, atomic and compressed instructions, step, reset, its CPU interface |
| `riscv-fpu.c` | F and D, on IEEE arithmetic of its own |
| `clint.c` | the RISC-V core's CLINT: msip, mtimecmp, mtime |
| `virt-rom.c`, `sifive-test.c`, `uart-16550.c` | virt's reset ROM, test device and UART |
| `avr.h` | the AVR core's state, and the interrupt-source interface of its peripherals |
| `avr.c` | the AVR core: the instruction set, SREG, the data space, interrupts, SLEEP, step, reset, its CPU interface |
| `avr-io.c`, `avr-usart.c`, `avr-timer16.c` | the ATmega328P's I/O registers, USART0, Timer/Counter1 |
| `svd-map.h`, `svd-map.c` | an SVD file's peripherals as a register file on the bus (`--svd`), `--trace-periph`, `--svd-map`, and the interface of the models over it |
| `../embsvd/svd.h`, `../embsvd/svd.c` | the CMSIS-SVD reader, embsvd's |
| `stm32-rcc.c`, `stm32-gpio.c`, `stm32-usart.c`, `stm32-tim.c` | the STM32's peripheral models, over its SVD's registers |
| `devices.h` | the create functions `boards.c` builds boards from |
| `image.h`, `image.c` | the image's symbols, code ranges, DWARF line table and `.debug_frame`, for the analyses |
| `analysis.h`, `analysis.c` | the step the analyses watch, and the shadow stack of the program's calls |
| `coverage.c` | `--coverage`: the counts per instruction, the report and lcov's tracefile |
| `profile.c` | `--profile`: the counts per call path, the report and the collapsed stacks |

The cost of each instruction comes from `tools/bench/cost.h`, the table
the bench uses in QEMU, so the two estimates cannot drift (`arm_cost`,
`rv_cost`).

## How a run goes

`main.c` parses the options and calls, in order:

1. `sim_init`: the board's core (its `create` function, which also puts
   the core's own devices on the bus), then the board's devices and
   bit-band aliases. With an SVD file (`--svd`, or the board's own,
   found in `EMBSIM_SVD_PATH`), the file is read and its register file
   goes on the bus before the board's space that reads as zero, and the
   board's models are put over its peripherals.
2. `trace_open`, for `--trace`.
3. `sim_load`: the board's memory regions, the image (`load_elf`), the
   flash made read-only to the core, and the core's power-on reset.
4. `sim_run`, or `gdb_serve` with `--gdb`.
5. `trace_report`: `--count`'s file and the `--stats` line.

`sim_run` calls the core's `step` until `s->state` is not `RUN`, checking
`--max-insns` after each. Anything can end the run with `sim_end(s,
STATE, "why")`: the core (a lockup, an idle loop), a device (the SCS for
SYSRESETREQ), semihosting (an exit), or the console (`--until`). The
reason is what `--stats` prints.

**Time** is `s->cycles`, which the core advances by each instruction's
estimated cost. Devices that keep time are told after each instruction
(`sim_advance`), but only while one has said its clock is running
(`sim_clock`), so a stopped SysTick costs nothing. A WFI asks
`sim_next_event` when a device will next raise an interrupt and skips
ahead to it; when none will, the run is idle. Nothing reads the host's
clock: a run gives the same counts every time, under a debugger too.

**The analyses** (`--coverage` and the rest) watch the run through
`an_step`, which `sim_run` and `sim_step` call instead of the core's
step when `s->an` is set, so a run without them is the loop it always
was. `an_step` notes the pc and the counts, steps the core, and gives
each analysis the instruction that ran (`s->insns` moved) and what it
cost (the change in `s->cycles`). It follows the program's calls and
returns on a shadow stack, recognizing them by the instruction as the
core executes it (`analysis.c` says which), and the cores tell it what
no instruction shows: `an_exc_entry` when an exception or interrupt is
entered, `an_exc_return` when the handler returns. Each is one test of
`s->an` in the core, where it was 0 before.

The shadow stack's frames each name a call path (`struct an_node`): a
function, reached along its parent's path. Each step's instructions and
cycles go to the path the pc is on before the step makes its call or
return -- the top frame's, or a path below it when the pc has left the
frame's function (a tail call) -- so every count is in exactly one path,
and the profile's totals are `--stats`' by construction.

`image.c` reads the ELF a second time, apart from the loader, for what
the analyses need: the functions (STT_FUNC, and labels in the code for
assembly), the line table as address ranges, and the call frame
information. None of it is read without an analysis.

## The interfaces

### A CPU: `struct cpu_ops`

```c
struct cpu_ops {
    const char *name;               /* "cortex-m" */
    int elf_machine;                /* the e_machine of its images */
    const char *elf_name;           /* "an ARM image" */
    void (*reset)(struct cpu *c);   /* power-on reset */
    void (*step)(struct cpu *c);    /* one instruction, or one exception entry */
    u32 (*pc)(struct cpu *c);
    int (*reg_read)(struct cpu *c, int n, u8 *buf);       /* by GDB number */
    int (*reg_write)(struct cpu *c, int n, const u8 *buf);
    const int *(*gdb_g_regs)(struct cpu *c);   /* the g packet's, then -1 */
    const char *(*gdb_xml)(struct cpu *c, const char *annex);
    int bp_kind;                    /* Z0's kind: the breakpoint's size */
    void (*interrupt)(struct cpu *c, int n);   /* make exception n pending */
    int pc_regnum;                  /* the pc's GDB number: `c ADDR` sets it */
    int elf_classes;                /* 1: ELFCLASS32, 2: ELFCLASS64, 3: both */
};
```

A core's state is a struct whose first member is `struct cpu` (the
pointer to its ops), so the run loop and the GDB server hold a `struct
cpu *` and know nothing else about it. `step` counts what it runs into
`s->insns` and `s->cycles`, writes the `--trace` line (`trace_insn`), and
calls `sim_advance` with the instruction's cost.

The loader records the image's entry point, class (`s->elf64`) and
e_flags in `struct sim` before the core's `reset`, which is where a core
with more than one width (RISC-V) takes its width from the image.

The Cortex-M's state is `struct cm_state` (`cortexm.h`). Its files reach
it through `cs`, which every entry point of the interface sets, so the
instruction code reads `cs->R[n]` and two cores would each have their
own. Memory access is `cm_ld` and `cm_st`, inline: plain memory directly,
and everything else -- bit-band, devices, a bus error, and every access
while a watchpoint is set -- through the bus. A fault is
`cm_fault_raise`; the first wins, the accesses after it are suppressed,
and `step` puts the registers back and takes the exception.

### Memory and devices: `struct bus`, `struct dev_ops`

An access goes to the first that answers at its address:

1. a memory region (`bus_add_region`) that holds all its bytes. A flash
   region ignores the core's stores once the image is loaded; a
   debugger's go in.
2. a bit-band alias (`bus_add_alias`): one word per bit of the target.
3. a device, in the order they were added. A device inside a wider one
   is added first: SysTick before the system control space, a UART
   before the peripheral space that reads as zero.

Nothing answering is `-1` with `bus->fail_addr`, which the core turns
into its bus fault.

```c
struct dev_ops {
    const char *type;
    u32 (*read)(void *ctx, u32 off, int n);        /* masked to n bytes by the bus */
    void (*write)(void *ctx, u32 off, int n, u32 v);
    void (*reset)(void *ctx);
    void (*tick)(void *ctx, u32 cycles);           /* while its clock runs */
    int (*next_event)(void *ctx, u32 *cycles);     /* its next interrupt */
};
```

`off` is the offset from the device's base and `n` the access's size.
A hook that cannot take the access calls `bus_fault(bus)`, and the core
takes the bus fault it takes where nothing answers. `bus->debug` is set
while the access is a debugger's, which a device with read side effects
leaves alone.
What a narrower access does to a word register is the device's choice:
the system control space and SysTick read the word and shift it, and
write the word with the other bytes zero, except the byte-accessible
priority registers. Any hook may be 0.

`bus_debug_read` and `bus_debug_write` are the debugger's accesses:
no watchpoints, no exclusive monitor, and a write to flash goes in.

### Boards: `struct board_desc`

```c
struct board_desc {
    const char *name, *core, *cpu;  /* "lm3s6965evb", "cortex-m", "cortex-m3" */
    int prio_bits;                  /* NVIC priority bits */
    struct mem_desc mem[4];         /* base, size, MEM_RAM or MEM_FLASH, main_ram */
    struct dev_desc dev[6];         /* type, base, size */
    struct alias bitband[2];        /* base, size, target */
    const char *svd;                /* "STM32F405.svd", or 0 */
    const struct model_desc *models;   /* { "USART*", "stm32-usart" }, ... */
};
```

`main_ram` marks the region `--ram-size` resizes. `MEM_FLASH` is read-only
to the core after loading, and is flash in the memory map the GDB server
gives (gdb then programs it with `load`). `MEM_ALIAS` is a second address
for the region at `target`: the STM32's flash at 0.

The RISC-V core's is `struct rv_state` (`riscv.h`), reached through
`rs` the same way. Registers hold XLEN bits (an RV32 value zero-extended
in its u64; `rv_xl` and `rv_sx` take a value to the width, unsigned and
signed). An instruction raises a trap with `rv_trap` and does nothing
more: every register write (`rv_wx`) and store comes after the access
that could fault, so there is nothing to undo. A compressed instruction
is expanded (`rvc_expand`) to the instruction it stands for and run as
that. A debugger's watchpoint raises `TRAP_WATCH` before the access: the
instruction is not run and not counted, and gdb steps it.

The AVR core's is `struct avr_state` (`avr.h`). Its program counter
counts words; `pc` in the CPU interface is the byte address, as gdb
has it. The data space is on the bus at `AVR_DATA` (0x800000) plus the
data address: the core answers for the registers and SP and SREG itself
(and puts them on the bus for a debugger's accesses), and the rest is
the board's. A peripheral's interrupt is a source,
`avr_irq_source(s, vector, pending, ack, ctx)`: the core asks `pending`
which vectors are requested, and `ack` is the hardware clearing the
flag as the vector is entered. A peripheral calls `avr_irq_changed`
when one of its flags or enables changes, and the core takes the lowest
vector requested. The exact cycles are the table (`avr_cost`) plus what
`step` adds for a taken branch, a skip and an interrupt's entry; an
instruction a skip passes over goes into `s->skipped` (a core that
skips sets `s->count_skips`, and --count's file gets a third line).

## Adding a peripheral

1. Write `tools/embsim/NAME.c`: a context struct, the `struct dev_ops`
   (`uart-cmsdk.c` is a small one to start from) and a create function
   `void *NAME_create(struct sim *s, const struct dev_desc *d)` that
   returns the context. Output goes through `sim_out`; an interrupt is
   `s->cpu->ops->interrupt(s->cpu, 16 + IRQ)` on a Cortex-M.
2. A device that keeps time has a `tick` hook, calls
   `sim_clock(s, &running, on)` when it starts and stops counting, and
   a `next_event` hook if it raises interrupts, so a WFI can skip ahead
   to it.
3. Declare the two in `devices.h`, add a line to `dev_types[]` in
   `boards.c`, and the file to `EMBSIM_SRCS` in the Makefile.
4. Put it on a board: an entry in that board's `dev` list.

A device that belongs to a core -- the Cortex-M's system control space
and SysTick, the RISC-V core's CLINT -- includes the core's header and
works on its state; its create function checks the board's core is its
own (`riscv_is`).

## The SVD's register file, and adding a peripheral model

`svd-map.c` reads an SVD file with embsvd's reader and makes each
register of each peripheral a `struct rf_reg`: its address and size, its
value and reset value, and its fields with their access,
modifiedWriteValues and readAction (`struct rf_field`). The registers
are sorted by address in one array, which an access finds by binary
search; a register at an address an earlier one has is left out (an
alternate). One device on the bus covers the span; it finds the register
an access touches, and sends the access to that register's peripheral
(`struct rf_periph`): to its model's `read` and `write` hooks if it has
one, else to the register file's own. A clock gate, set by a model on
another peripheral (`p->gate`, the RCC's enable bit), stops the access
before either.

A model is a `struct dev_ops` over one peripheral. It does not replace
the register file; it wraps it:

```c
static void usart_write(void *ctx, u32 off, int n, u32 v)
{
    struct usart *u = ctx;
    rf_write(u->p, off, n, v);                  /* as the SVD says */
    if (off == rf_off(u->dr) && (u->cr1->value & u->ue) &&
        (u->cr1->value & u->te)) {
        sim_out(u->s, (int)(v & 0xff));         /* what the part does */
        rf_hw(u->sr, u->sr->value | u->txe | u->tc);
    }
    update_irq(u);
}
```

1. Write `tools/embsim/NAME.c` (`stm32-usart.c` is a small one to start
   from): a context, the `struct dev_ops`, and `void *NAME_create(struct
   sim *s, struct rf_periph *p)`. The create function finds its registers
   and fields by the SVD's names (`rf_reg(p, "SR")`, `rf_mask(r, "TXE")`)
   and returns 0 when they are not there: a peripheral of another layout
   that the board's name pattern also matches keeps the plain register
   file. It may correct the file: `rf_set_reset` for a wrong reset value,
   `rf_set_mwv` for flags the file calls read-write that are rc_w0 or
   rc_w1.
2. The hooks: `read` and `write` call `rf_read` and `rf_write`, then do
   what the hardware does. A change the hardware makes goes through
   `rf_hw(reg, value)`, so `--trace-periph` shows it as an `H` line; a
   counter that changes every cycle may set `reg->value` directly (the
   timer's CNT), and is then not traced. `reset` sets the model's own
   state; the registers are back at their reset values before it is
   called. The interrupt is `rf_irq(p, "_UP")` (the SVD's, by name) and
   `rf_interrupt(p, irq)`.
3. A model that keeps time has `tick` and `next_event` hooks, and calls
   `rf_clock(p, &running, on)` when its clock starts and stops; only a
   running model is ticked, so fourteen stopped timers cost nothing.
4. Declare it in `devices.h`, add it to `model_types[]` in `boards.c` and
   to `EMBSIM_SRCS` in the Makefile, and to a board's `models`: the SVD's
   peripheral names it applies to, with `*` for any characters. Models
   are created in the list's order, so one that others depend on (RCC,
   which sets the clock gates) comes first.
5. Test it as `tests/golden/embsim-svd.sh` tests the STM32F405's: a
   firmware that prints the registers, run on QEMU where QEMU models the
   peripheral, and held to the reference manual where it does not.

## Adding a board

An entry in `boards[]` in `boards.c`: the name, the core and its model,
the NVIC's priority bits (0 for a core without one), the memory, the
devices and the bit-band aliases. Add it to `--help`'s list in `main.c` and to the manual's table.
If QEMU models the board, give `tests/golden/embsim.sh` a configuration
for it, so QEMU referees the new board on the whole exec corpus.

## Adding a core

1. A state struct whose first member is `struct cpu`, and the
   `struct cpu_ops`. `step` executes one instruction (or takes one
   exception), counts it into `s->insns` and `s->cycles` with the core's
   cost table, writes `trace_insn` when `s->trace` is set, calls
   `sim_advance` while `s->counting`, and ends the run with `sim_end`
   when it must. Memory is `bus_region` for the fast path and
   `bus_read`/`bus_write` for the rest; before an instruction's access,
   `bus_watch_check` says whether a debugger's watchpoint stops it.
2. The GDB register numbers, the `g` packet and the target description
   of the architecture's GDB feature names (gdb's `features/` directory
   lists them), numbered as QEMU's stub numbers them, so the transcript
   test can referee the server against QEMU.
3. A create function `struct cpu *NAME_create(struct sim *s, const char
   *model, const struct board_desc *bd)` that returns 0 for a model that
   is not its own, sets `s->cpu`, and adds the core's own devices.
4. A line in `cores[]` in `boards.c`, its boards, and its files in the
   Makefile. The ELF loader takes the core's `elf_machine`; semihosting
   (`semihost`) takes the core's view of memory through `struct semi_ops`,
   so a RISC-V core can reuse it with its own trap.

## The GDB server

`gdb_serve` listens (`net.h`), and either waits for a debugger
(`--gdb-wait`) or runs the image, looking for one every 16384
instructions. A session reads packets and answers them; `c`, `s` and
`vCont` call `resume`, the run loop with a debugger:

- before each instruction, the breakpoints (a list in the server; the
  image is never written);
- `sim_step`, and then the watchpoint the step hit, the end of the run,
  and, every 16384 instructions, the socket for the interrupt byte;
- an idle core (`END_IDLE`) is not the end: the server waits for the
  debugger, as a sleeping part does.

A watchpoint stops before the access: `bus_watch_check` says the access
is watched, the core raises `FAULT_WATCH` for it, and `step` undoes the
instruction as it does for a fault, and takes back its count. The
debugger then steps it with the watchpoint removed, which is what gdb
expects of ARM, and what QEMU's stub does.

The answers follow QEMU's stub wherever both have one, so
`tests/golden/embsim-gdb.sh` can hold the two transcripts to the same
text. What is EmbSim's own: the memory map, the `monitor` commands,
QStartNoAckMode, and the stop on a lockup.

`net-posix.c` is the only file that needs an operating system. A host
without BSD sockets builds with `make EMBSIM_NET=tools/embsim/net-none.c`,
or with its own file for the six functions of `net.h`.

## Testing a change

- `tests/golden/embsim.sh`: the exec corpus on QEMU and EmbSim on five
  cores; output, status, and (four of them) the instruction and cycle
  counts must be the same. The exception and instruction-edge programs.
- `tests/golden/embsim-riscv.sh`: the same for the RISC-V core on virt,
  RV32 and RV64, soft- and hard-float, 2425 programs to the instruction
  and the cycle; `rv-isa.c`'s instruction edges against QEMU; the ends
  of a run.
- `tests/golden/embsim-avr.sh`: the AVR core on uno: some 580 corpus
  programs to the instruction, `avr-isa`'s edges against QEMU,
  `avr-cycles.S`'s cycles against the datasheet, the ends of a run.
- `tests/golden/embsim-coverage.sh`: `--coverage` against a program
  whose lines carry the counts they must get, on the three cores and
  from clang's DWARF 5.
- `tests/golden/embsim-profile.sh`: `--profile` against a program of
  known calls, an interrupt among them, and the `--trace` of its run.
- `tests/golden/embsim-gdb.sh`: the GDB server against QEMU's stub, on
  the Cortex-M, on RISC-V and on the AVR.
- `tests/golden/embsim-svd.sh`: the register file over a test SVD,
  against the specification's values; the STM32F405's models against
  QEMU's netduinoplus2 and RM0090. `embsim-svd-all.sh`: every SVD in
  `$EMBREF/svd` mapped, each register where `svdref.py` puts it.
- `tests/golden/svd-*.sh`: embsvd, whose reader EmbSim shares.
- A change that must not change behaviour (a refactor, a speed-up)
  should also compare the binary before and after on every corpus image
  and every board: stdout, stderr with `--stats`, the exit status, the
  `--count` file and a `--trace`. The split into these files was checked
  that way: 1684 runs, no difference.
