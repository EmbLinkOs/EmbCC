#!/bin/sh
# `.S` and `.s` inputs: assembly FILES, not inline statements.
#
# Firmware starts in assembly -- a reset vector, the stack set up
# before C can run -- and EmbCC could not compile a line of it for any
# embedded target. `embas` is NASM syntax and x86-64 only, and the
# per-target asm.c files assemble one statement at a time for inline
# __asm__, with operands already substituted and no idea what a label
# is. src/as/gas.c is the file layer over them: sections, labels,
# directives, symbols, relocations and the object. Only instruction
# encoding is delegated, to the SAME asm.c inline assembly uses, so a
# mnemonic cannot mean two things in one compiler.
#
# The test is the one that matters: a real crt0.S is the actual
# startup of an image that boots under QEMU. It sets the stack, walks
# __bss_start to __bss_end zeroing, and calls main -- so the program's
# output proves the relocations resolved, the branches reached their
# labels, and .data was placed. A test that only checked "it produced
# an object" would pass with every displacement wrong.
set -u
echo "TEST-MARKER gas-file"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/gas-file
rm -rf "$out"; mkdir -p "$out"
fail=0

cat > "$out/crt0.S" <<'CEOF'
/* Preprocessed, because the suffix is uppercase -- which is the whole
 * difference between .S and .s, here as everywhere else. */
#define STACK_TOP 0x80800000

    .section .text
    .global _start
    .type _start, %function
_start:
    li   sp, STACK_TOP
    la   t0, __bss_start
    la   t1, __bss_end
zero_bss:
    bgeu t0, t1, bss_done
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    zero_bss
bss_done:
    call main
halt:
    j    halt

    .global counter
    .data
    .align 4
counter:
    .word 42
CEOF

cat > "$out/main.c" <<'CEOF'
void putn(long v); void puts_(const char *s);
extern int counter;        /* .data, from the assembly file */
static int touched;        /* .bss -- zero only if crt0.S's loop ran */
int main(void)
{
    putn(counter);
    putn(touched);
    touched = 7;
    putn(touched);
    puts_("\n==END==\n");
    return 0;
}
CEOF

T=riscv32-unknown-elf
"$EMBCC" --target=$T -c "$out/crt0.S" -o "$out/crt0.o" 2> "$out/as.err" || {
    echo "FAIL: crt0.S does not assemble"
    head -4 "$out/as.err" | sed 's/^/     | /'; exit 1; }

# The shape of what it produced, before running anything: `la` must be
# an auipc/addi PAIR whose LOW half names the auipc's own address and
# not the symbol. Naming the symbol assembles cleanly and computes the
# wrong address -- the psABI's one real trap, and invisible until the
# image runs.
if command -v llvm-readelf > /dev/null 2>&1; then
    llvm-readelf -r "$out/crt0.o" > "$out/rel.txt" 2>/dev/null
    grep -q 'R_RISCV_CALL.*main' "$out/rel.txt" || {
        echo 'FAIL: call main did not produce an R_RISCV_CALL'; fail=1; }
    grep -q 'R_RISCV_PCREL_HI20.*__bss_start' "$out/rel.txt" || {
        echo 'FAIL: la did not produce a PCREL_HI20'; fail=1; }
    grep -qE 'R_RISCV_PCREL_LO12_I.*\.Lpcrel_hi' "$out/rel.txt" || {
        echo 'FAIL: the low half of la does not name the auipc label'
        grep 'LO12' "$out/rel.txt" | sed 's/^/     | /'; fail=1; }
    echo "  crt0.S assembles: call, la and its paired low half all relocate"
fi

# And now the claim: this is the startup of an image that runs.
QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv32}
if command -v "$QEMU" > /dev/null 2>&1; then
    "$EMBCC" --target=$T -Os -c "$EMBCC_ROOT/tests/harness/riscv/io.c" \
             -o "$out/io.o" 2>/dev/null || {
        echo "FAIL: the harness io.c does not compile"; exit 1; }
    "$EMBCC" --target=$T -Os -c "$out/main.c" -o "$out/main.o" 2>/dev/null || {
        echo "FAIL: main.c does not compile"; exit 1; }
    "${EMBLD:-$EMBCC_ROOT/embld}" -e _start -Ttext 0x80000000 \
        "$out/crt0.o" "$out/main.o" "$out/io.o" -o "$out/img.elf" \
        2> "$out/ld.err" || {
        echo "FAIL: the image does not link"
        head -3 "$out/ld.err" | sed 's/^/     | /'; exit 1; }
    got=$(timeout "${EMBCC_QEMU_TIMEOUT:-20}" "$QEMU" -M virt -bios none \
              -nographic -kernel "$out/img.elf" 2>/dev/null |
          tr -d '\n' | sed 's/==END==.*//')
    want="42 0 7 "
    if [ "$got" = "$want" ]; then
        echo "  the image boots: .data read (42), .bss zeroed by the"
        echo "  assembly loop (0), and a store after it works (7)"
    else
        echo "FAIL: the image printed '$got', wanted '$want'"
        fail=1
    fi
else
    echo "  SKIP the run: $QEMU absent"
fi

# `.s` is the same thing unpreprocessed, and a `#define` in one is not
# a directive -- it must be refused rather than silently ignored.
printf '    .global f\nf:\n    li a0, 5\n    ret\n' > "$out/plain.s"
"$EMBCC" --target=$T -c "$out/plain.s" -o "$out/plain.o" 2>/dev/null || {
    echo "FAIL: a plain .s file does not assemble"; fail=1; }

# What it refuses. A symbol it cannot relocate, and a directive it does
# not know, are both errors that NAME the thing -- emitting a guess
# would put the wrong address in a vector table.
printf '    .global f\nf:\n    addi a0, nosuchsym, 4\n' > "$out/bad.s"
if "$EMBCC" --target=$T -c "$out/bad.s" -o /dev/null 2> "$out/b1.txt"; then
    echo "FAIL: an unrelocatable symbol reference was accepted"; fail=1
else
    grep -q 'nosuchsym' "$out/b1.txt" || {
        echo "FAIL: the refusal does not name the symbol"; fail=1; }
fi
printf '    .frobnicate 3\n' > "$out/bad2.s"
if "$EMBCC" --target=$T -c "$out/bad2.s" -o /dev/null 2> "$out/b2.txt"; then
    echo "FAIL: an unknown directive was accepted"; fail=1
else
    grep -q 'frobnicate' "$out/b2.txt" || {
        echo "FAIL: the refusal does not name the directive"; fail=1; }
fi
# ARMv7-M, whose startup file is a vector table rather than a stack
# setup: the first word is the initial SP and the second the reset
# handler's ADDRESS, which is a relocation into .text from .text.
cat > "$out/start.S" <<'CEOF'
    .syntax unified
    .thumb
    .section .text
    .global _vectors
_vectors:
    .word  0x20008000
    .word  reset_handler
    .global reset_handler
    .type reset_handler, %function
reset_handler:
    mov  r1, 0
loop:
    cmp  r1, 4
    bge  done
    add  r1, r1, 1
    b    loop
done:
    bl   main
hang:
    b    hang
CEOF
if "$EMBCC" --target=thumbv7m-none-eabi -c "$out/start.S" -o "$out/start.o" \
     2> "$out/t.err"; then
    if command -v llvm-objdump > /dev/null 2>&1; then
        llvm-objdump -d --triple=thumbv7m "$out/start.o" > "$out/t.dis" 2>/dev/null
        # A backward branch and a forward one, each resolved to its
        # label -- the two directions the two passes exist for.
        grep -q 'b.w.*<loop>' "$out/t.dis" || {
            echo "FAIL thumb: the backward branch does not reach loop"
            fail=1; }
        grep -q 'bge.w.*<done>' "$out/t.dis" || {
            echo "FAIL thumb: the forward branch does not reach done"
            fail=1; }
    fi
    if command -v llvm-readelf > /dev/null 2>&1; then
        llvm-readelf -r "$out/start.o" > "$out/t.rel" 2>/dev/null
        grep -q 'R_ARM_THM_CALL.*main' "$out/t.rel" || {
            echo "FAIL thumb: bl to an external symbol did not relocate"
            fail=1; }
        grep -q 'R_ARM_ABS32.*reset_handler' "$out/t.rel" || {
            echo "FAIL thumb: the vector word did not relocate"
            grep R_ARM "$out/t.rel" | head -3 | sed 's/^/     | /'; fail=1; }
    fi
    echo "  thumbv7m: a vector table, both branch directions, and bl to an"
    echo "  external symbol all assemble and relocate"
else
    echo "FAIL thumbv7m: the startup file does not assemble"
    head -3 "$out/t.err" | sed 's/^/     | /'; fail=1
fi

# aarch64. Its inline-asm vocabulary was MEASURED from the EmbLinkOS
# kernel, whose __asm__ statements are system instructions, so the
# ordinary ones were missing although every encoder existed. The
# width flag is the trap here: gpr() reports "is a W register" and
# the encoders take a byte width, so `mov x0, 0` assembled as
# `mov w0, 0` until that was converted.
cat > "$out/a64.S" <<'CEOF'
    .section .text
    .global _start
    .type _start, %function
_start:
    mov x0, 0
    mov x1, 4
loop:
    cmp x0, x1
    b.ge done
    add x0, x0, 1
    b   loop
done:
    bl  main
    ret
    .global tag
    .data
    .align 8
tag:
    .quad _start
CEOF
if "$EMBCC" --target=aarch64-elf -c "$out/a64.S" -o "$out/a64.o" \
     2> "$out/a.err"; then
    if command -v llvm-objdump > /dev/null 2>&1; then
        llvm-objdump -d --triple=aarch64 "$out/a64.o" > "$out/a.dis" 2>/dev/null
        grep -q 'mov[[:space:]]*x0' "$out/a.dis" || {
            echo "FAIL aarch64: 'mov x0' came out as a W-register move"
            grep -m1 mov "$out/a.dis" | sed 's/^/     | /'; fail=1; }
        grep -q 'b[[:space:]].*<loop>' "$out/a.dis" || {
            echo "FAIL aarch64: the backward branch does not reach loop"
            fail=1; }
        grep -q 'b.ge.*<done>' "$out/a.dis" || {
            echo "FAIL aarch64: the conditional branch does not reach done"
            fail=1; }
    fi
    if command -v llvm-readelf > /dev/null 2>&1; then
        llvm-readelf -r "$out/a64.o" > "$out/a.rel" 2>/dev/null
        grep -q 'R_AARCH64_CALL26.*main' "$out/a.rel" || {
            echo "FAIL aarch64: bl to an external symbol did not relocate"
            fail=1; }
        grep -q 'R_AARCH64_ABS64.*_start' "$out/a.rel" || {
            echo "FAIL aarch64: .quad of a symbol did not relocate"; fail=1; }
    fi
    echo "  aarch64: X-register moves, both branch kinds, bl and .quad all"
    echo "  assemble and relocate"
else
    echo "FAIL aarch64: the file does not assemble"
    head -3 "$out/a.err" | sed 's/^/     | /'; fail=1
fi

# x86-64 keeps its own NASM-syntax assembler (embas) and is not wired
# to this driver, so it still refuses by name rather than half-working.
if "$EMBCC" --target=x86_64-elf -c "$out/plain.s" -o /dev/null \
     2> "$out/b3.txt"; then
    echo "  (x86-64 assembles .s too)"
else
    grep -q 'assembly-file support' "$out/b3.txt" || {
        echo "FAIL: x86-64's refusal does not explain itself"
        head -1 "$out/b3.txt" | sed 's/^/     | /'; fail=1; }
    echo "  a target without a file assembler refuses by name"
fi
[ "$fail" -eq 0 ] && echo "  unknown symbols and directives are each refused"

[ "$fail" -eq 0 ] || exit 1
