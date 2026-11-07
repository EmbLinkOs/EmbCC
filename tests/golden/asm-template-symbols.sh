#!/bin/sh
# Inline asm templates that name symbols (tests/exec/asm-symbols.c runs
# them on every board). Checked here, what a run does not show:
#
#   1. each target's object carries the template's relocations, against
#      the symbols it names, of the types its own assembler writes;
#   2. -S writes them as .reloc lines and EmbCC's assembler turns that
#      text back into the same relocations (x86-64 against GNU as is
#      tests/golden/asm-S.sh's);
#   3. a literal pool in a template is refused by name, not misplaced.
set -u
echo "TEST-MARKER asm-template-symbols"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/asm-template-symbols
rm -rf "${out:?}"; mkdir -p "$out"
command -v llvm-readelf >/dev/null 2>&1 || { echo "skipped: needs llvm-readelf"; exit 0; }
EMBCC=${EMBCC:-./embcc}
fails=0
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
src=tests/exec/asm-symbols.c

# target : relocation types the object must carry for g and answer
for spec in \
    "aarch64-elf:R_AARCH64_ADR_PREL_PG_HI21 R_AARCH64_ADD_ABS_LO12_NC R_AARCH64_CALL26" \
    "thumbv7m-none-eabi:R_ARM_THM_MOVW_ABS_NC R_ARM_THM_MOVT_ABS R_ARM_THM_CALL" \
    "armv7a-none-eabi:R_ARM_MOVW_ABS_NC R_ARM_MOVT_ABS R_ARM_CALL" \
    "riscv32-unknown-elf:R_RISCV_PCREL_HI20 R_RISCV_PCREL_LO12_I R_RISCV_CALL_PLT" \
    "riscv64-unknown-elf:R_RISCV_PCREL_HI20 R_RISCV_PCREL_LO12_I R_RISCV_CALL_PLT" \
    "loongarch64-none-elf:R_LARCH_PCALA_HI20 R_LARCH_PCALA_LO12 R_LARCH_B26" \
    "mipsel-none-elf:R_MIPS_HI16 R_MIPS_LO16 R_MIPS_26" \
    "x86_64-elf:R_X86_64_PC32"; do
    t=${spec%%:*}
    want=${spec#*:}
    "$EMBCC" --target=$t -O1 -c "$src" -o "$out/$t.o" ||
        { bad "$t: does not compile"; continue; }
    llvm-readelf -rW "$out/$t.o" > "$out/$t.rel"
    for r in $want; do
        grep -q "$r" "$out/$t.rel" || bad "$t: no $r"
    done
    grep -qE "$(echo "$want" | cut -d' ' -f1).* g( |\+|$)" "$out/$t.rel" ||
        bad "$t: the address relocation is not against g"
    [ "$t" = x86_64-elf ] && continue
    # -S, reassembled by EmbCC, relocates the same places the same way
    "$EMBCC" --target=$t -O1 -S "$src" -o "$out/$t.s" &&
    "$EMBCC" --target=$t -c "$out/$t.s" -o "$out/$t-s.o" ||
        { bad "$t: -S does not reassemble"; continue; }
    for o in "$t" "$t-s"; do
        llvm-readelf -rW "$out/$o.o" | awk '/R_/ { print $1, $3 }' | sort > "$out/$o.where"
    done
    cmp -s "$out/$t.where" "$out/$t-s.where" ||
        bad "$t: -S reassembles to relocations at other places or of other types"
done

printf 'void f(void) { __asm__ volatile("ldr r0, =g" ::: "r0"); }\n' > "$out/pool.c"
if "$EMBCC" --target=thumbv7m-none-eabi -c "$out/pool.c" -o "$out/pool.o" 2> "$out/pool.err"; then
    bad "a literal pool in a template was accepted"
else
    grep -q "literal pool" "$out/pool.err" || { cat "$out/pool.err"; bad "the pool refusal does not say why"; }
fi

[ $fails = 0 ] || { echo "asm-template-symbols: $fails failure(s)"; exit 1; }
echo "asm-template-symbols: relocations on eight targets, -S reassembles them, a pool is refused"
