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
# aarch64/ (src/arch/README.md; docs/language/compatibility.md for what each supports).
SRCS := \
	src/platform/platform_posix.c \
	src/driver/main.c \
	src/driver/util.c \
	src/driver/diag.c \
	src/driver/remark.c \
	src/driver/inspect.c \
	src/driver/asmout.c \
	src/driver/iface.c \
	src/driver/explain.c \
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
	src/cxx/traits.c \
	src/cxx/concepts.c \
	src/cxx/coro.c \
	src/cxx/access.c \
	src/cxx/mangle.c \
	src/cxx/emit.c \
	src/sema/sema.c \
	src/sema/type.c \
	src/sema/ldfloat.c \
	src/sema/w128.c \
	src/sema/uninit.c \
	src/sema/format.c \
	src/ir/irgen.c \
	src/ir/irprint.c \
	src/ir/irparse.c \
	src/opt/opt.c \
	src/debug/dwarf.c \
	src/debug/eh.c \
	src/elf/write.c \
	src/macho/write.c \
	src/coff/write.c \
	src/arch/target.c \
	src/arch/code.c \
	src/arch/predef.c \
	src/arch/x86_64/irgen.c \
	src/arch/x86_64/codegen.c \
	src/arch/x86_64/emit.c \
	src/arch/x86_64/topasm.c \
	src/arch/x86_64/as.c \
	src/arch/x86_64/disasm.c \
	src/arch/x86_64/predef.c \
	src/arch/x86_64/predef_cxx.c \
	src/arch/aarch64/irgen.c \
	src/arch/aarch64/codegen.c \
	src/arch/aarch64/emit.c \
	src/arch/aarch64/asm.c \
	src/arch/aarch64/predef.c \
	src/arch/aarch64/predef_cxx.c

OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)

all: embcc embread embld embas embls embidx

embcc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# embas — the standalone NASM/Intel-syntax assembler (A1, ARCHITECTURE §4). Reads
# the kernel's hand-written .asm and emits ELF objects the same writer (src/elf)
# the compiler uses produces, so the toolchain owns the whole build (drops nasm).
embas: tools/embas/embas.c src/arch/x86_64/as.c src/arch/x86_64/as.h \
       src/elf/write.c src/elf/elf.h src/driver/util.c src/driver/diag.c \
       src/platform/platform_posix.c src/platform/platform.h
	$(CC) $(CFLAGS) -o $@ tools/embas/embas.c src/arch/x86_64/as.c \
	    src/elf/write.c src/driver/util.c src/driver/diag.c \
	    src/platform/platform_posix.c

# embld — the integrated linker (ARCHITECTURE §6, WORKPLAN stream B), as
# a standalone tool for host development. The link library also gets
# wired into embcc so `embcc prog.c -o prog` links in-process.
# embld also links the EmbDBG core (compiled -DEMBDBG_NO_MAIN, so no CLI main)
# so the linker can emit a native .embdbg at link time through the SAME format
# writer the embdbg tool uses — one implementation, not two.
embld: tools/embld/embld.c tools/embld/doctor.c src/link/link.c \
       src/driver/util.c src/driver/diag.c src/driver/explain.c \
       src/link/link.h src/elf/elf.h src/embx/embx.c src/embx/embx.h \
       tools/embdbg/embdbg.c tools/embdbg/embdbg_core.h \
       src/platform/platform_posix.c src/platform/platform.h
	$(CC) $(CFLAGS) -DEMBDBG_NO_MAIN -Wno-unused-function -o $@ \
	    tools/embld/embld.c tools/embld/doctor.c src/link/link.c \
	    src/driver/util.c src/driver/diag.c src/driver/explain.c \
	    src/embx/embx.c tools/embdbg/embdbg.c src/platform/platform_posix.c \
	    src/arch/x86_64/disasm.c

# embls — the language server (docs/tools/diagnostics.md T5). It links EmbCC's own
# preprocessor and parser, so what an editor is told about a file comes from
# the compiler that will compile it; diagnostics it gets by running embcc
# itself. The parse runs in a forked child, because a front end ends the
# process where it cannot continue and a server must not.
EMBLS_SRCS = tools/embls/embls.c src/platform/platform_posix.c src/cpp/cpp.c src/lex/lex.c \
             src/parse/parse.c src/sema/type.c src/sema/ldfloat.c \
             src/sema/w128.c src/sema/uninit.c src/sema/format.c \
             src/driver/util.c src/driver/diag.c src/driver/remark.c \
             src/driver/inspect.c src/driver/asmout.c src/driver/iface.c \
             src/driver/explain.c \
             src/arch/target.c src/arch/predef.c src/arch/x86_64/predef.c \
             src/arch/aarch64/predef.c src/arch/x86_64/predef_cxx.c \
             src/arch/aarch64/predef_cxx.c \
             $(filter src/cxx/%,$(SRCS)) src/sema/sema.c src/ir/irgen.c \
             src/ir/irprint.c src/ir/irparse.c \
             src/opt/opt.c src/debug/dwarf.c src/debug/eh.c src/elf/write.c \
             src/arch/code.c src/arch/x86_64/irgen.c src/arch/x86_64/codegen.c \
             src/arch/x86_64/emit.c src/arch/x86_64/topasm.c \
             src/arch/x86_64/as.c src/arch/x86_64/disasm.c src/arch/aarch64/irgen.c \
             src/arch/aarch64/codegen.c src/arch/aarch64/emit.c \
             src/arch/aarch64/asm.c
embls: $(EMBLS_SRCS)
	$(CC) $(CFLAGS) -o $@ $(EMBLS_SRCS)

# embidx — the cross-TU index (vision 8.2). It links none of the
# compiler: it runs `embcc --emit-interfaces` and stores what comes back,
# so the TEXT is the contract between them and an external tool can take
# the same input.
embidx: tools/embidx/embidx.c
	$(CC) $(CFLAGS) -o $@ tools/embidx/embidx.c

# embread — the EMBX dumper/verifier (EMBX spec §9). A separate binary,
# not part of embcc: it reads images, it does not compile. The EMBX
# container definition it shares with the future linker lives in
# src/embx/, exactly as src/elf/ is shared by asm and link.
embread: tools/embread/embread.c src/embx/embx.c src/embx/embx.h
	$(CC) $(CFLAGS) -o $@ tools/embread/embread.c src/embx/embx.c

# embdbg — EmbDBG v0, the debug-info reader/symbolizer (EMBDBG step 1's
# consumer). Standalone like embread: it reads the DWARF EmbCC emits, it does
# not compile. The live-control half is gated on the kernel debug contract.
# embdbg reads the x86-64 decoder from src/arch/x86_64 (disasm.c) rather
# than carrying its own: the compiler needs it for -S, and one decoder is
# the same discipline as one encoder (R1).
embdbg: tools/embdbg/embdbg.c src/elf/elf.h src/arch/x86_64/disasm.c \
        src/arch/x86_64/disasm.h
	$(CC) $(CFLAGS) -o $@ tools/embdbg/embdbg.c src/arch/x86_64/disasm.c

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The tree is small; every object depending on every header is honest
# enough and cannot go stale (CONTRIBUTING lie #1).
$(OBJS): $(wildcard src/*/*.h src/arch/*/*.h)

# The libc and libcxx goldens run against the BUILT archives, so the suite
# depends on them: without this a stale archive is silently what gets
# tested, and a fix made in the library is reported as still broken (or,
# worse, a break is reported as fixed).
test: embcc embread embld embdbg embls libc-x86_64 libcxx-x86_64 \
      libc-linux-x86_64 libcxx-linux-x86_64
	tests/run.sh

# The aarch64 suite: compile for the second architecture and RUN the result
# under qemu-system-aarch64 (tests/harness/aarch64). Separate from `test`
# because it needs the cross newlib and QEMU, which `test` does not.
test-arm64: embcc libc-aarch64 libcxx-aarch64 libc-linux-aarch64 \
            libcxx-linux-aarch64
	tests/run.sh --target=aarch64-elf

# The C++ suites wholly on EmbCC's library: libstdc++ and libsupc++ built
# from GCC's sources by EmbCC, every tests/libstdcxx and tests/cxx program
# linked with it and compared with g++'s build — both targets. Opt-in (the
# library takes minutes to build): tests/golden/cxx-libstdcxx-embcc.sh.
test-libstdcxx: embcc
	EMBCC=$(CURDIR)/embcc EMBCC_TARGET=x86_64-elf EMBCC_LIBSTDCXX=1 \
	    sh tests/golden/cxx-libstdcxx-embcc.sh
	EMBCC=$(CURDIR)/embcc EMBCC_TARGET=aarch64-elf EMBCC_LIBSTDCXX=1 \
	    sh tests/golden/cxx-libstdcxx-embcc.sh

# ---- our C library (lib/libc) ------------------------------------------
# One implementation, ported to an OS by one small backend. Built with
# EmbCC itself, per target -- which is also the widest test the compiler
# gets outside its own sources.
# Everything above the seam, which is every target's library verbatim. The
# backend is appended per target -- that difference IS the port.
LIBC_SRCS_PORTABLE := $(wildcard lib/libc/src/*/*.c) \
                      $(wildcard lib/libc/src/math/fdlibm/*.c)
LIBC_SRCS := $(LIBC_SRCS_PORTABLE) lib/libc/os/posixlike/backend.c
LIBC_INC  := -Ilib/libc/include -Ilib/libc/src/math

libc-x86_64: embcc
	@mkdir -p $(BUILD)/libc/x86_64
	@for f in $(LIBC_SRCS); do \
	    o=$(BUILD)/libc/x86_64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=x86_64-elf -c -O1 $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/x86_64/libc.a
	@$${EMBCC_X86_AR:-x86_64-elf-ar} rcs $(BUILD)/libc/x86_64/libc.a $(BUILD)/libc/x86_64/*.o
	@echo "libc: $(BUILD)/libc/x86_64/libc.a"

libc-aarch64: embcc
	@mkdir -p $(BUILD)/libc/aarch64
	@for f in $(LIBC_SRCS); do \
	    o=$(BUILD)/libc/aarch64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=aarch64-elf -c -O1 $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/aarch64/libc.a
	@$${EMBCC_AARCH64_AR:-aarch64-elf-ar} rcs $(BUILD)/libc/aarch64/libc.a $(BUILD)/libc/aarch64/*.o
	@echo "libc: $(BUILD)/libc/aarch64/libc.a"

# EmbLinkOS is a backend, not a second library: same sources, one different
# file at the bottom. It needs that OS's ABI headers, so it is opt-in.
#   make libc-emblinkos EMBLINKOS=$HOME/EmbLinkOs
EMBLINKOS ?= $(HOME)/EmbLinkOs

libc-emblinkos: embcc
	@[ -f "$(EMBLINKOS)/user/lib/embk.h" ] || { \
	    echo "libc-emblinkos: no $(EMBLINKOS)/user/lib/embk.h"; \
	    echo "  set EMBLINKOS=/path/to/EmbLinkOs"; exit 1; }
	@mkdir -p $(BUILD)/libc/emblinkos
	@for f in $(LIBC_SRCS_PORTABLE) lib/libc/os/emblinkos/backend.c; do \
	    o=$(BUILD)/libc/emblinkos/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc -c -O2 $(LIBC_INC) -I$(EMBLINKOS)/user/lib $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/emblinkos/libc.a
	@$${EMBCC_X86_AR:-x86_64-elf-ar} rcs $(BUILD)/libc/emblinkos/libc.a \
	    $(BUILD)/libc/emblinkos/*.o
	@echo "libc: $(BUILD)/libc/emblinkos/libc.a"

# Linux, which is the first target this library reaches with NO other C
# library underneath it: lib/libc/os/linux/backend.c issues syscalls, so
# a program linked against this archive needs no glibc, no musl and no
# dynamic loader. That is also why there is a crt1.o here and not for
# the targets above -- on those, somebody else supplied the entry point.
#
# crt1.o stays OUT of the archive on purpose. _start must be defined
# exactly once in an image, and a definition that arrives by archive
# member is a definition that arrives by accident.
#   make libc-linux-x86_64   /   make libc-linux-aarch64
LIBC_SRCS_LINUX := $(LIBC_SRCS_PORTABLE) lib/libc/os/linux/backend.c \
                   lib/libc/os/linux/thread.c lib/libc/os/linux/tls.c

libc-linux-x86_64: embcc
	@mkdir -p $(BUILD)/libc/linux-x86_64
	@for f in $(LIBC_SRCS_LINUX); do \
	    o=$(BUILD)/libc/linux-x86_64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=x86_64-linux-gnu -c -O1 $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-x86_64/libc.a
	@$${EMBCC_X86_AR:-x86_64-elf-ar} rcs $(BUILD)/libc/linux-x86_64/libc.a \
	    $(BUILD)/libc/linux-x86_64/*.o
	@./embcc --target=x86_64-linux-gnu -c -O1 $(LIBC_INC) \
	    lib/libc/os/linux/start.c -o $(BUILD)/libc/linux-x86_64/crt1.o
	@echo "libc: $(BUILD)/libc/linux-x86_64/libc.a + crt1.o"

libc-linux-aarch64: embcc
	@mkdir -p $(BUILD)/libc/linux-aarch64
	@for f in $(LIBC_SRCS_LINUX); do \
	    o=$(BUILD)/libc/linux-aarch64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=aarch64-linux-gnu -c -O1 $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-aarch64/libc.a
	@$${EMBCC_AARCH64_AR:-aarch64-elf-ar} rcs \
	    $(BUILD)/libc/linux-aarch64/libc.a $(BUILD)/libc/linux-aarch64/*.o
	@./embcc --target=aarch64-linux-gnu -c -O1 $(LIBC_INC) \
	    lib/libc/os/linux/start.c -o $(BUILD)/libc/linux-aarch64/crt1.o
	@echo "libc: $(BUILD)/libc/linux-aarch64/libc.a + crt1.o"

libc-linux: libc-linux-x86_64 libc-linux-aarch64

libc: libc-x86_64 libc-aarch64

# ---- our C++ runtime (lib/libcxx) --------------------------------------
# The Itanium C++ ABI on top of libgcc's unwinder: operator new, static-
# local guards, the type_info hierarchy, dynamic_cast, and the personality
# routine. This is what libsupc++ is; the standard library sits above it.
LIBCXX_SRCS := $(wildcard lib/libcxx/src/*.cc)
LIBCXX_INC  := -Ilib/libcxx/include -Ilib/libc/include

libcxx-x86_64: embcc
	@mkdir -p $(BUILD)/libcxx/x86_64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/x86_64/$$(basename $$f .cc).o; \
	    ./embcc -c -O2 -x c++ $(LIBCXX_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/x86_64/libcxx.a
	@$${EMBCC_X86_AR:-x86_64-elf-ar} rcs $(BUILD)/libcxx/x86_64/libcxx.a \
	    $(BUILD)/libcxx/x86_64/*.o
	@echo "libcxx: $(BUILD)/libcxx/x86_64/libcxx.a"

libcxx-aarch64: embcc
	@mkdir -p $(BUILD)/libcxx/aarch64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/aarch64/$$(basename $$f .cc).o; \
	    ./embcc -c -O2 -x c++ --target=aarch64-elf $(LIBCXX_INC) $$f -o $$o \
	        || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/aarch64/libcxx.a
	@$${EMBCC_AARCH64_AR:-aarch64-elf-ar} rcs \
	    $(BUILD)/libcxx/aarch64/libcxx.a $(BUILD)/libcxx/aarch64/*.o
	@echo "libcxx: $(BUILD)/libcxx/aarch64/libcxx.a"

# The same runtime for the Linux triples, so std::thread, std::mutex and
# the exception machinery can be exercised on a real kernel rather than
# only on the freestanding harness.
libcxx-linux-x86_64: embcc
	@mkdir -p $(BUILD)/libcxx/linux-x86_64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/linux-x86_64/$$(basename $$f .cc).o; \
	    ./embcc --target=x86_64-linux-gnu -c -O2 -x c++ $(LIBCXX_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/linux-x86_64/libcxx.a
	@$${EMBCC_X86_AR:-x86_64-elf-ar} rcs $(BUILD)/libcxx/linux-x86_64/libcxx.a \
	    $(BUILD)/libcxx/linux-x86_64/*.o
	@echo "libcxx: $(BUILD)/libcxx/linux-x86_64/libcxx.a"

libcxx-linux-aarch64: embcc
	@mkdir -p $(BUILD)/libcxx/linux-aarch64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/linux-aarch64/$$(basename $$f .cc).o; \
	    ./embcc --target=aarch64-linux-gnu -c -O2 -x c++ $(LIBCXX_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/linux-aarch64/libcxx.a
	@$${EMBCC_AARCH64_AR:-aarch64-elf-ar} rcs $(BUILD)/libcxx/linux-aarch64/libcxx.a \
	    $(BUILD)/libcxx/linux-aarch64/*.o
	@echo "libcxx: $(BUILD)/libcxx/linux-aarch64/libcxx.a"

libcxx: libcxx-x86_64 libcxx-aarch64

clean:
	rm -rf $(BUILD) embcc embread embld embdbg embas embls

.PHONY: all test test-arm64 test-libstdcxx libc libc-x86_64 libc-aarch64 \
        libc-emblinkos libc-linux libc-linux-x86_64 libc-linux-aarch64 \
        libcxx libcxx-x86_64 libcxx-aarch64 \
        libcxx-linux-x86_64 libcxx-linux-aarch64 clean
