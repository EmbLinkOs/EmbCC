# EmbCC — host build (ROADMAP M0: EmbCC is a host program at this stage).
#
# Plain make on purpose: the eventual on-OS build goes through EmbBuild
# (ROADMAP M4), so nothing here may grow host-only cleverness the manifest
# could not express.
#
# -std=c99: self-hosting constrains the source to the subset EmbCC will
# implement (ARCHITECTURE.md §7). Keep it buildable by a strict C99 compiler.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -g
BUILD   := build

# The target-neutral compiler, then src/arch: what every target shares
# (selection, the backend contract, the code buffer), then one directory per
# architecture — everything x86-64-only under x86_64/, aarch64-only under
# aarch64/ (src/arch/README.md; docs/COMPATIBILITY.md for what each supports).
SRCS := \
	src/driver/main.c \
	src/driver/util.c \
	src/lex/lex.c \
	src/cpp/cpp.c \
	src/parse/parse.c \
	src/cxx/cxx.c \
	src/cxx/tok.c \
	src/cxx/type.c \
	src/cxx/scope.c \
	src/cxx/parse.c \
	src/cxx/expr.c \
	src/cxx/class.c \
	src/cxx/vtable.c \
	src/cxx/template.c \
	src/cxx/consteval.c \
	src/cxx/mangle.c \
	src/cxx/emit.c \
	src/sema/sema.c \
	src/sema/type.c \
	src/sema/ldfloat.c \
	src/ir/irgen.c \
	src/opt/opt.c \
	src/debug/dwarf.c \
	src/debug/eh.c \
	src/elf/write.c \
	src/arch/target.c \
	src/arch/code.c \
	src/arch/predef.c \
	src/arch/x86_64/irgen.c \
	src/arch/x86_64/codegen.c \
	src/arch/x86_64/emit.c \
	src/arch/x86_64/topasm.c \
	src/arch/x86_64/as.c \
	src/arch/x86_64/predef.c \
	src/arch/x86_64/predef_cxx.c \
	src/arch/aarch64/irgen.c \
	src/arch/aarch64/codegen.c \
	src/arch/aarch64/emit.c \
	src/arch/aarch64/asm.c \
	src/arch/aarch64/predef.c \
	src/arch/aarch64/predef_cxx.c

OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

all: embcc embread embld embas

embcc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# embas — the standalone NASM/Intel-syntax assembler (A1, ARCHITECTURE §4). Reads
# the kernel's hand-written .asm and emits ELF objects the same writer (src/elf)
# the compiler uses produces, so the toolchain owns the whole build (drops nasm).
embas: tools/embas/embas.c src/arch/x86_64/as.c src/arch/x86_64/as.h \
       src/elf/write.c src/elf/elf.h src/driver/util.c
	$(CC) $(CFLAGS) -o $@ tools/embas/embas.c src/arch/x86_64/as.c \
	    src/elf/write.c src/driver/util.c

# embld — the integrated linker (ARCHITECTURE §6, WORKPLAN stream B), as
# a standalone tool for host development. The link library also gets
# wired into embcc so `embcc prog.c -o prog` links in-process.
# embld also links the EmbDBG core (compiled -DEMBDBG_NO_MAIN, so no CLI main)
# so the linker can emit a native .embdbg at link time through the SAME format
# writer the embdbg tool uses — one implementation, not two.
embld: tools/embld/embld.c src/link/link.c src/driver/util.c \
       src/link/link.h src/elf/elf.h src/embx/embx.c src/embx/embx.h \
       tools/embdbg/embdbg.c tools/embdbg/embdbg_core.h
	$(CC) $(CFLAGS) -DEMBDBG_NO_MAIN -Wno-unused-function -o $@ \
	    tools/embld/embld.c src/link/link.c \
	    src/driver/util.c src/embx/embx.c tools/embdbg/embdbg.c

# embread — the EMBX dumper/verifier (EMBX spec §9). A separate binary,
# not part of embcc: it reads images, it does not compile. The EMBX
# container definition it shares with the future linker lives in
# src/embx/, exactly as src/elf/ is shared by asm and link.
embread: tools/embread/embread.c src/embx/embx.c src/embx/embx.h
	$(CC) $(CFLAGS) -o $@ tools/embread/embread.c src/embx/embx.c

# embdbg — EmbDBG v0, the debug-info reader/symbolizer (EMBDBG step 1's
# consumer). Standalone like embread: it reads the DWARF EmbCC emits, it does
# not compile. The live-control half is gated on the kernel debug contract.
embdbg: tools/embdbg/embdbg.c src/elf/elf.h
	$(CC) $(CFLAGS) -o $@ tools/embdbg/embdbg.c

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The tree is small; every object depending on every header is honest
# enough and cannot go stale (CONTRIBUTING lie #1).
$(OBJS): $(wildcard src/*/*.h src/arch/*/*.h)

test: embcc embread embld embdbg
	tests/run.sh

# The aarch64 suite: compile for the second architecture and RUN the result
# under qemu-system-aarch64 (tests/harness/aarch64). Separate from `test`
# because it needs the cross newlib and QEMU, which `test` does not.
test-arm64: embcc
	tests/run.sh --target=aarch64-elf

clean:
	rm -rf $(BUILD) embcc embread embld embdbg embas

.PHONY: all test test-arm64 clean
