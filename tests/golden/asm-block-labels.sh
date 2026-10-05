#!/bin/sh
# A block's own label is the one its references reach.
#
# Each file-scope asm block and naked function on Cortex-M, RISC-V and AVR
# is assembled on its own (src/as/gas.c, gas_assemble_block). A reference
# the assembler leaves to a relocation -- a literal pool's word, `la`,
# `call`, and on AVR every branch to a label -- went to the driver BY NAME,
# and the driver looked the name up in C first and then in the blocks in
# order. So two naked functions that each had a `tbl:` both loaded the
# first one's, two AVR loops on `loop:` both branched into the first, and a
# local label named like a C function reached the C function. GNU as would
# refuse such a file, which defines one name twice; EmbCC took it and
# resolved to the wrong place, silently.
#
# Here two naked functions, a and b, each define the same local labels --
# one of them named like a C function -- and refer to them in every way
# that leaves a relocation. Each relocation in a and b must land inside
# the function that holds it; and on Cortex-M a local label typed as a
# function keeps its Thumb bit in a data word.
set -u
echo "TEST-MARKER asm-block-labels"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
LREADELF=${EMBCC_LLVM_READELF:-llvm-readelf}
out=$EMBCC_ROOT/tests/golden/out/asm-block-labels
rm -rf "$out"; mkdir -p "$out"
command -v "$LREADELF" > /dev/null 2>&1 ||
    { echo "skipped: $LREADELF not found"; exit 0; }

cat > "$out/thumb.c" <<'EOF'
int cfunc(void) { return 7; }
#define BODY(n) \
    __asm__("  b 1f\n" \
            ".type lf, %function\n" \
            "lf: bx lr\n" \
            "1: ldr r0, =tbl\n" \
            "  ldr r1, =cfunc\n" \
            "  ldr r2, =lf\n" \
            "  movw r3, #:lower16:tbl\n" \
            "  movt r3, #:upper16:tbl\n" \
            "  bx lr\n" \
            "cfunc: bx lr\n" \
            "  .p2align 2\n" \
            "tbl: .word " #n "\n" \
            "  .ltorg\n")
__attribute__((naked)) void a(void) { BODY(1); }
__attribute__((naked)) void b(void) { BODY(2); }
EOF
cat > "$out/rv.c" <<'EOF'
int cfunc(void) { return 7; }
#define BODY(n) \
    __asm__("  la a0, tbl\n" \
            "  lw a1, tbl\n" \
            "  call cfunc\n" \
            "  ret\n" \
            "cfunc: ret\n" \
            "tbl: .word " #n "\n")
__attribute__((naked)) void a(void) { BODY(1); }
__attribute__((naked)) void b(void) { BODY(2); }
EOF
cat > "$out/avr.c" <<'EOF'
void cfunc(void) { }
#define BODY(n) \
    __asm__("loop: dec r24\n" \
            "  brne loop\n" \
            "  rjmp 1f\n" \
            "1: rcall cfunc\n" \
            "  call cfunc\n" \
            "  ldi r30, lo8(tbl)\n" \
            "  ldi r31, hi8(tbl)\n" \
            "  ret\n" \
            "cfunc: ret\n" \
            "tbl: .word " #n "\n")
__attribute__((naked)) void a(void) { BODY(1); }
__attribute__((naked)) void b(void) { BODY(2); }
EOF

fail=0
for spec in thumbv7em-none-eabi:thumb riscv32-unknown-elf:rv \
            riscv64-unknown-elf:rv avr:avr; do
    t=${spec%%:*}; src=${spec#*:}
    o=$out/$t.o
    "$EMBCC" --target=$t -O2 -c "$out/$src.c" -o "$o" 2> "$out/$t.err" ||
        { echo "FAIL $t: did not compile"; sed 's/^/     | /' "$out/$t.err"
          fail=1; continue; }
    "$LREADELF" -sW "$o" > "$out/$t.sym"
    "$LREADELF" -rW "$o" > "$out/$t.rel"
    # every relocation in a or b, and where it lands; the Thumb bit is
    # not part of an address
    awk -v thumb=$([ $src = thumb ] && echo 1 || echo 0) '
        function hex(s,   v, i, c) {
            v = 0; s = tolower(s)
            for (i = 1; i <= length(s); i++) {
                c = index("0123456789abcdef", substr(s, i, 1)) - 1
                v = v * 16 + c
            }
            return v
        }
        FNR == NR {
            if ($4 == "FUNC" && ($8 == "a" || $8 == "b")) {
                lo[$8] = hex($2); if (thumb) lo[$8] -= lo[$8] % 2
                hi[$8] = lo[$8] + $3
            }
            next
        }
        /R_(ARM|RISCV|AVR)_/ {
            off = hex($1); at = hex($4)
            if (thumb && $3 != "R_ARM_ABS32") at -= at % 2
            add = $6 == "-" ? -hex($7) : hex($7)
            to = at + add
            for (f in lo)
                if (off >= lo[f] && off < hi[f]) {
                    n[f]++
                    if (to < lo[f] || to > hi[f]) {
                        printf "  %s+%d: %s %s%s%s lands at %d, outside %s\n",
                               f, off - lo[f], $3, $5, $6, $7, to, f
                        bad = 1
                    }
                    if (thumb && $3 == "R_ARM_ABS32" && to == lo[f] + 3)
                        bit[f] = 1
                }
        }
        END {
            for (f in lo) {
                if (!n[f]) { printf "  %s has no relocations\n", f; bad = 1 }
                if (thumb && !bit[f]) {
                    printf "  %s: lf is not loaded with its Thumb bit\n", f
                    bad = 1
                }
            }
            if (!("a" in lo) || !("b" in lo)) { print "  no a or b"; bad = 1 }
            exit bad
        }' "$out/$t.sym" "$out/$t.rel" > "$out/$t.why" ||
        { echo "FAIL $t: a block's references miss its own labels"
          cat "$out/$t.why"; fail=1; continue; }
    echo "  $t: every reference in a and in b reaches its own labels"
done
[ $fail = 0 ] || exit 1
echo "asm-block-labels: two blocks' labels of one name stay their own"
