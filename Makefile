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
	src/driver/paths.c \
	src/link/link.c \
	src/embx/embx.c \
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
	src/arch/target.c src/arch/regalloc.c \
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

# One unit that is not under src/. src/link/link.c writes the EMBX
# image hash and the .embdbg sidecar through the SAME reader the embdbg
# tool uses -- R1, one implementation -- so every build that links the
# link library needs it: the driver (which links in-process now), the
# EmbBuild manifest, and the self-host. It is listed separately because
# the OBJS rule maps src/%.c and because it is compiled without its CLI
# main, which the driver already has.
SRCS_TOOLCORE := tools/embdbg/embdbg.c
TOOLCORE_CFLAGS := -DEMBDBG_NO_MAIN -Wno-unused-function
EMBDBG_CORE := $(BUILD)/embdbg_core.o

$(EMBDBG_CORE): tools/embdbg/embdbg.c tools/embdbg/embdbg_core.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TOOLCORE_CFLAGS) -c -o $@ $<

all: embcc embread embld embas embls embidx

embcc: $(OBJS) $(EMBDBG_CORE)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(EMBDBG_CORE)

# The same compiler, linked INSIDE the object directory rather than at
# ./embcc. tests/golden/host-agnostic.sh builds EmbCC twice, with two
# different host compilers, and it runs in parallel with every other
# test -- so it must not replace the ./embcc those tests are running.
# With BUILD= pointing somewhere private, this target is entirely its
# own: `make CC=clang BUILD=/tmp/x /tmp/x/embcc`.
$(BUILD)/embcc: $(OBJS) $(EMBDBG_CORE)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(EMBDBG_CORE)

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
             src/arch/code.c src/arch/regalloc.c \
             src/arch/x86_64/irgen.c src/arch/x86_64/codegen.c \
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
# The fast loop: every program in tests/exec and tests/cxx, compiled,
# linked and RUN -- 217 of them in about 27 seconds. It skips the
# goldens, which is where the time goes: three of them recompile the
# whole corpus a second time with gcc and diff the results, and one
# builds GCC's libstdc++ from source.
#
# This is what to run while changing something. It catches essentially
# every codegen and front-end regression, because those show up as a
# program printing the wrong answer -- which is what it checks.
#
# It is NOT a substitute for `make test` before a commit. What it does
# not check is exactly what the goldens exist for: that the answers
# agree with gcc's, that -S and -c build the same program, that the
# object format is what the platform's tools expect.
check: embcc libc-x86_64 libcxx-x86_64
	tests/run.sh --exec-only

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

# lib/rt -- the COMPILER runtime, which is a different library from the C
# one: libc implements what a program asks for by name, and no program
# ever writes __multi3. It is a separate archive so the freestanding
# targets, which link somebody else's libgcc, are not given two
# definitions of the same routine.
#
# It ships as librt.a beside libc.a, and the driver puts it on the link
# line AFTER libc, because a libc routine can call into it (a printf of
# a 128-bit value) and an archive is searched once.
RT_SRCS := $(wildcard lib/rt/*.c)

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
	@mkdir -p $(BUILD)/libc/linux-x86_64/rt
	@for f in $(RT_SRCS); do \
	    o=$(BUILD)/libc/linux-x86_64/rt/$$(basename $$f .c).o; \
	    ./embcc --target=x86_64-linux-gnu -c -O1 $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-x86_64/librt.a
	@$${EMBCC_X86_AR:-x86_64-elf-ar} rcs $(BUILD)/libc/linux-x86_64/librt.a \
	    $(BUILD)/libc/linux-x86_64/rt/*.o
	@echo "libc: $(BUILD)/libc/linux-x86_64/libc.a + librt.a + crt1.o"

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
	@mkdir -p $(BUILD)/libc/linux-aarch64/rt
	@for f in $(RT_SRCS); do \
	    o=$(BUILD)/libc/linux-aarch64/rt/$$(basename $$f .c).o; \
	    ./embcc --target=aarch64-linux-gnu -c -O1 $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-aarch64/librt.a
	@$${EMBCC_AARCH64_AR:-aarch64-elf-ar} rcs \
	    $(BUILD)/libc/linux-aarch64/librt.a $(BUILD)/libc/linux-aarch64/rt/*.o
	@echo "libc: $(BUILD)/libc/linux-aarch64/libc.a + librt.a + crt1.o"

libc-linux: libc-linux-x86_64 libc-linux-aarch64

# ---- installation ------------------------------------------------------
#
# EmbCC finds its own files relative to the binary (src/driver/paths.c),
# so nothing here is compiled in and the result can be moved afterwards
# or unpacked anywhere. PREFIX is where it will LIVE; DESTDIR is where
# to stage it now, which is what a package build sets and what the
# golden test uses.
#
# The version comes out of src/driver/version.h rather than being
# written again here: the directory this creates and the directory the
# compiler looks in have to be the same one, and two copies of a
# version string are two chances to ship an upgrade that reads the old
# version's headers.
PREFIX  ?= /usr/local
DESTDIR ?=
VERSION := $(shell sed -n 's/.*EMBCC_VERSION "\(.*\)".*/\1/p' \
                   src/driver/version.h)
LIBROOT  = $(DESTDIR)$(PREFIX)/lib/embcc/$(VERSION)

# Each installed target directory, and where the build put its pieces.
# The triples are what --target= accepts, so `ls` next to a failure
# answers "is that target installed?" directly.
# Split in two on purpose. `install` is what a person runs and builds
# what it needs first; `install-files` only COPIES, and is what
# tests/golden/install.sh uses -- a test that ran the first would be a
# test that rebuilds the compiler while the rest of the suite is using
# it, which is the one thing the suite must never do to itself.
install: all libc libcxx libc-linux libcxx-linux-x86_64 \
         libcxx-linux-aarch64 install-files

install-files:
	@echo "installing EmbCC $(VERSION) into $(DESTDIR)$(PREFIX)"
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	@for t in embcc embld embas embread embdbg embls embidx; do \
	    cp $$t $(DESTDIR)$(PREFIX)/bin/$$t; \
	    chmod 755 $(DESTDIR)$(PREFIX)/bin/$$t; \
	done
	@mkdir -p $(LIBROOT)/include $(LIBROOT)/include/c++ \
	          $(LIBROOT)/freestanding
	@cp -R lib/libc/include/. $(LIBROOT)/include/
	@cp -R lib/libcxx/include/. $(LIBROOT)/include/c++/
	@cp -R include/. $(LIBROOT)/freestanding/
	@for pair in "x86_64-elf:x86_64" "aarch64-elf:aarch64" \
	             "x86_64-linux-gnu:linux-x86_64" \
	             "aarch64-linux-gnu:linux-aarch64"; do \
	    triple=$${pair%%:*}; dir=$${pair#*:}; \
	    mkdir -p $(LIBROOT)/$$triple; \
	    for f in libc.a librt.a crt1.o; do \
	        [ -f $(BUILD)/libc/$$dir/$$f ] && \
	            cp $(BUILD)/libc/$$dir/$$f $(LIBROOT)/$$triple/$$f; \
	    done; \
	    [ -f $(BUILD)/libcxx/$$dir/libcxx.a ] && \
	        cp $(BUILD)/libcxx/$$dir/libcxx.a $(LIBROOT)/$$triple/libcxx.a; \
	    case $$triple in *-linux-gnu) \
	        cp lib/libc/os/linux/link.ld $(LIBROOT)/$$triple/link.ld ;; \
	    esac; \
	    true; \
	done
	@echo "installed: $(DESTDIR)$(PREFIX)/bin/embcc"
	@echo "           $(LIBROOT)/"
	@echo "check it with: $(DESTDIR)$(PREFIX)/bin/embcc --print-search-dirs"

# Removes exactly what install wrote, and the versioned directory with
# it -- never $(PREFIX)/lib/embcc itself, which may hold another version.
uninstall:
	@for t in embcc embld embas embread embdbg embls embidx; do \
	    rm -f $(DESTDIR)$(PREFIX)/bin/$$t; \
	done
	@rm -rf $(LIBROOT)
	@echo "removed EmbCC $(VERSION) from $(DESTDIR)$(PREFIX)"

libc-linux-all: libc-linux libcxx-linux-x86_64 libcxx-linux-aarch64

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

.PHONY: all check test test-arm64 test-libstdcxx libc libc-x86_64 libc-aarch64 \
        libc-emblinkos libc-linux libc-linux-x86_64 libc-linux-aarch64 \
        libcxx libcxx-x86_64 libcxx-aarch64 \
        libcxx-linux-x86_64 libcxx-linux-aarch64 \
        install install-files uninstall libc-linux-all clean
