#!/bin/sh
# `-S` of file-scope asm blocks and naked functions, on Cortex-M, RISC-V
# and AVR: the assembly must be the object -c writes.
#
# On these targets a block, and a naked function's body, is assembled by
# EmbCC's own assembler into bytes, labels and relocations. -S used to
# write all of it as one run of `.byte`, so the reassembled object lost
# the block's symbols -- a naked function's own name among them -- and
# every relocation in it: `bl other`, `ldr r0, =gvar`, `call`, `la`,
# `.word sym`. It linked to the wrong addresses or not at all.
#
# Each unit below is compiled with -c and with -S, and the -S output is
# assembled twice: by `embcc -c`, and by llvm-mc as an assembler that
# shares nothing with EmbCC. Each reassembled object must have -c's
# symbols, and relocations at the same places of the same types, and
# EmbCC's must have -c's .text byte for byte (llvm-mc's too, except on
# ARM, whose REL relocations keep their addends in the bytes). Then
# each is linked with the same second unit -- which calls into the
# blocks by name -- and the linked images must be identical, bytes and
# symbol addresses.
set -u
echo "TEST-MARKER asm-S-blocks"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
MC=${EMBCC_LLVM_MC:-llvm-mc}
NM=${EMBCC_LLVM_NM:-llvm-nm}
LREADELF=${EMBCC_LLVM_READELF:-llvm-readelf}
LOBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
LOBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=$EMBCC_ROOT/tests/golden/out/asm-S-blocks
rm -rf "$out"; mkdir -p "$out"

for tool in "$NM" "$LREADELF" "$LOBJCOPY" "$LOBJDUMP"; do
    command -v "$tool" > /dev/null 2>&1 ||
        { echo "skipped: $tool not found"; exit 0; }
done
[ -x "$EMBLD" ] || { echo "FAIL: $EMBLD not built (make embld)"; exit 1; }
have_mc=1
command -v "$MC" > /dev/null 2>&1 || { have_mc=0; echo "(no $MC: EmbCC's assembler only)"; }

fail=0
bad() { echo "FAIL $*"; fail=1; }

# ---- the units --------------------------------------------------------------
# Each has a block with a global function label, local labels (named, .L
# and numeric), a call out and a call to C, data references with and
# without an offset, a literal pool on ARM, `.word` of a global, of a
# local function label and of a .L label, and an object label with a
# .size; a second block with a weak label, an untyped global one, a
# branch into the first block, and a local label that shares its name
# with a C function -- which must not clash with it in the -S file; a
# global naked function that calls C, and a static one whose address C
# takes. The data is a multiple of eight bytes, so that the reassembled
# object's eight-aligned .bss (src/as/gas.c's default for it) lands where
# -c's does.
#
# On ARM the C here makes no calls of its own: a call from C keeps a
# placeholder in its field that only a RELA linker reads correctly, which
# is a property of the function code, not of the blocks.
cat > "$out/thumb.c" <<'EOF'
int gvar[2];
extern int other(int);
int cfunc(int x) { return x + gvar[1]; }
__asm__(
    ".globl helper\n"
    ".type helper, %function\n"
    "helper:\n"
    "  push {r4, lr}\n"
    "  bl other\n"
    "  ldr r1, =gvar+4\n"
    "  ldr r2, =loop\n"
    "  ldr r3, =.Lmid\n"
    "  movw r4, #:lower16:gvar\n"
    "  movt r4, #:upper16:gvar\n"
    "  movw r4, #:lower16:gvar+8\n"
    "  movt r4, #:upper16:gvar+8\n"
    "loop:\n"
    "  cmp r0, #0\n"
    "  beq 1f\n"
    "  subs r0, #1\n"
    "  b loop\n"
    "1:\n"
    ".Lmid:\n"
    "  bl cfunc\n"
    "  pop {r4, pc}\n"
    "  .ltorg\n"
    "  .word gvar\n"
    "  .word helper\n"
    "  .word lfun\n"
    "  .word loop+2\n"
    "  .word .Lmid\n"
    ".type lfun, %function\n"
    "lfun:\n"
    "  bx lr\n"
    "  .p2align 2\n"
    ".globl table\n"
    ".type table, %object\n"
    "table: .word cfunc, helper, gvar+8\n"
    ".size table, .-table\n");
__asm__(
    ".weak wfn\n"
    ".type wfn, %function\n"
    "wfn:\n"
    "  b helper\n"
    ".globl plain\n"
    "plain:\n"
    "cfunc: bx lr\n");
__attribute__((naked)) void nk(void) { __asm__("push {lr}\n bl cfunc\n pop {pc}"); }
__attribute__((naked)) static void snk(void) { __asm__("bx lr"); }
void (*fp[2])(void) = { snk, nk };
EOF

cat > "$out/rv.c" <<'EOF'
int gvar[2];
extern int other(int);
static void snk(void) __attribute__((naked));
int cfunc(int x) { snk(); return other(x) + gvar[1]; }
#if __riscv_xlen == 64
#define SX "sd"
#define LX "ld"
#define PTR ".dword"
#else
#define SX "sw"
#define LX "lw"
#define PTR ".word"
#endif
__asm__(
    ".globl helper\n"
    ".type helper, @function\n"
    "helper:\n"
    "  addi sp, sp, -16\n"
    "  " SX " ra, 8(sp)\n"
    "  call other\n"
    "  la a1, gvar\n"
    "  lw a2, gvar\n"
    "  la a3, .Lmid\n"
    "loop:\n"
    "  addi a0, a0, -1\n"
    "  bnez a0, loop\n"
    "1: beqz a0, 1b\n"
    ".Lmid:\n"
    "  call cfunc\n"
    "  la a4, lfun\n"
    "  " LX " ra, 8(sp)\n"
    "  addi sp, sp, 16\n"
    "  ret\n"
    "lfun: ret\n"
    ".globl table\n"
    ".type table, @object\n"
    "table: .word gvar+4\n"
    "  .word helper\n"
    "  .word loop\n"
    "  .word .Lmid\n"
    "  " PTR " cfunc\n"
    ".size table, .-table\n");
__asm__(
    ".weak wfn\n"
    "wfn:\n"
    "  call helper\n"
    ".globl plain\n"
    "plain:\n"
    "cfunc: ret\n");
__attribute__((naked)) void nk(void) { __asm__("call cfunc\n ret"); }
static void snk(void) { __asm__("ret"); }
void (*fp[2])(void) = { snk, nk };
EOF

cat > "$out/avr.c" <<'EOF'
unsigned char gvar[8];
extern void other(void);
static void snk(void) __attribute__((naked));
void cfunc(void) { gvar[1]++; snk(); other(); }
__asm__(
    ".globl helper\n"
    ".type helper, @function\n"
    "helper:\n"
    "  call other\n"
    "  rcall cfunc\n"
    "  ldi r24, lo8(gvar)\n"
    "  ldi r25, hi8(gvar)\n"
    "  ldi r30, lo8(gs(lfun))\n"
    "  ldi r31, hi8(gs(lfun))\n"
    "  lds r24, gvar+1\n"
    "  sts gvar+2, r24\n"
    "loop:\n"
    "  dec r24\n"
    "  brne loop\n"
    "1: dec r25\n"
    "  brne 1b\n"
    "  jmp cfunc\n"
    "lfun: ret\n"
    ".globl table\n"
    ".type table, @object\n"
    "table: .word gvar+3\n"
    "  .word helper\n"
    "  .word loop\n"
    "  .word .Lx\n"
    ".Lx: .long gvar\n"
    ".size table, .-table\n");
__asm__(
    ".weak wfn\n"
    "wfn:\n"
    "  call helper\n"
    ".globl plain\n"
    "plain:\n"
    "cfunc: ret\n");
__attribute__((naked)) void nk(void) { __asm__("push r28"); cfunc(); __asm__("pop r28\n ret"); }
static void snk(void) { __asm__("ret"); }
void (*fp[4])(void) = { snk, nk, snk, nk };
EOF

# The second unit: it calls the blocks' global labels by name, so their
# addresses -- a Thumb function's bit included -- are in the image.
cat > "$out/sup.c" <<'EOF'
extern void helper(void), nk(void), plain(void);
extern void wfn(void) __attribute__((weak));
int other(int x) { helper(); nk(); plain(); wfn(); return x; }
EOF

# ---- the comparison ---------------------------------------------------------
syms() {        # syms OBJ: every symbol but the assemblers' own DEFINED .L
                # labels (which they keep for a relocation to name) and
                # ARM's $t/$d -- an undefined one of any name is a symbol
    "$NM" "$1" | grep -v '^[0-9a-f][0-9a-f]* [a-zA-Z] [.$]' | sort
}
rels() {        # rels OBJ: each relocation's section, place and type
    "$LREADELF" -r "$1" |
        awk '/^Relocation section/ { sec = $3 }
             /^ *[0-9a-f]+ +[0-9a-f]+ +R_/ { print sec, $1, $3 }' |
        sed 's/\.rela\{0,1\}\./ ./' | sort
}
image() {       # image ELF: the loaded bytes and every symbol's address
    "$LOBJDUMP" -s -j .text -j .data "$1" | tail -n +4
    syms "$1"
}

for spec in "thumbv7em-none-eabi:thumb:thumbv7em-none-eabi::-Ttext 0x0 -Tdata 0x20000000" \
            "riscv32-unknown-elf:rv:riscv32:-mattr=+m,+c:-Ttext 0x80000000" \
            "riscv64-unknown-elf:rv:riscv64:-mattr=+m,+c:-Ttext 0x80000000" \
            "avr:avr:avr:-mcpu=atmega328p:-Ttext 0x0 -Tdata 0x100"; do
    t=${spec%%:*}; rest=${spec#*:}
    src=${rest%%:*}; rest=${rest#*:}
    mct=${rest%%:*}; rest=${rest#*:}
    mcf=${rest%%:*}; ldf=${rest#*:}
    d=$out/$t; mkdir -p "$d"

    "$EMBCC" --target=$t -O2 -c "$out/$src.c" -o "$d/ref.o" 2> "$d/c.err" ||
        { bad "$t: -c failed"; sed 's/^/     | /' "$d/c.err"; continue; }
    "$EMBCC" --target=$t -O2 -S "$out/$src.c" -o "$d/x.s" 2> "$d/s.err" ||
        { bad "$t: -S refused"; sed 's/^/     | /' "$d/s.err"; continue; }
    "$EMBCC" --target=$t -O2 -c "$out/sup.c" -o "$d/sup.o" ||
        { bad "$t: the second unit did not compile"; continue; }

    # the shape: the naked function and the block keep their names, and
    # what they name is relocated
    for want in '^nk:' '^helper:' '^table:' '^	\.weak	wfn' '^	\.size	nk, ' \
                'reloc.*, other' 'reloc.*, helper' '^snk:'; do
        grep -q "$want" "$d/x.s" || bad "$t: -S has no '$want'"
    done

    "$EMBLD" -e cfunc $ldf "$d/ref.o" "$d/sup.o" -o "$d/ref.elf" ||
        { bad "$t: the -c object does not link"; continue; }
    image "$d/ref.elf" > "$d/ref.img"
    syms "$d/ref.o" > "$d/ref.sym"
    rels "$d/ref.o" > "$d/ref.rel"
    "$LOBJCOPY" -O binary --only-section=.text "$d/ref.o" "$d/ref.text"

    asms=emb
    [ $have_mc = 1 ] && asms="emb mc"
    for a in $asms; do
        if [ $a = emb ]; then
            "$EMBCC" --target=$t -c "$d/x.s" -o "$d/$a.o" 2> "$d/$a.err"
        else
            # shellcheck disable=SC2086
            "$MC" -triple=$mct $mcf -filetype=obj "$d/x.s" -o "$d/$a.o" \
                2> "$d/$a.err"
        fi || { bad "$t: $a does not assemble the -S output"
                head -4 "$d/$a.err" | sed 's/^/     | /'; continue; }
        syms "$d/$a.o" > "$d/$a.sym"
        diff "$d/ref.sym" "$d/$a.sym" > "$d/$a.symdiff" ||
            { bad "$t ($a): the symbols differ from -c's (-c, then -S)"
              head -8 "$d/$a.symdiff" | sed 's/^/     | /'; }
        rels "$d/$a.o" > "$d/$a.rel"
        diff "$d/ref.rel" "$d/$a.rel" > "$d/$a.reldiff" ||
            { bad "$t ($a): the relocations differ from -c's (-c, then -S)"
              head -8 "$d/$a.reldiff" | sed 's/^/     | /'; }
        if [ $a = emb ] || [ $src != thumb ]; then
            "$LOBJCOPY" -O binary --only-section=.text "$d/$a.o" "$d/$a.text"
            cmp -s "$d/ref.text" "$d/$a.text" ||
                bad "$t ($a): .text is not -c's byte for byte"
        fi
        "$EMBLD" -e cfunc $ldf "$d/$a.o" "$d/sup.o" -o "$d/$a.elf" ||
            { bad "$t ($a): the reassembled object does not link"; continue; }
        image "$d/$a.elf" > "$d/$a.img"
        diff "$d/ref.img" "$d/$a.img" > "$d/$a.imgdiff" ||
            { bad "$t ($a): it links to a different program (-c, then -S)"
              head -8 "$d/$a.imgdiff" | sed 's/^/     | /'; }
    done
    [ $fail = 0 ] &&
        echo "  $t: -S reassembles to -c's symbols, relocations and linked image ($asms)"
done

echo "the Thumb unit's first block, as -S writes it:"
sed -n '/^\.Lasm0:/,/^\.Lasm0\.loop:/p' "$out/thumbv7em-none-eabi/x.s" | head -24

[ $fail = 0 ] || exit 1
echo "asm-S-blocks: blocks and naked functions keep their labels and relocations under -S"
