#!/bin/sh
# -g on the embedded targets: DWARF that is true, not DWARF that exists.
#
# `-g` was refused on thumbv7m, riscv32 and riscv64 with "the DWARF
# frame description would be a guess". The guess it meant was the frame
# BASE: x86-64 and aarch64 set a frame pointer in the prologue and name
# it, and these two keep none -- every slot is sp-relative.
#
# But sp does not move either. The prologue subtracts the frame once and
# the outgoing argument area is part of it, so `sp + 0` is an exact
# frame base for the life of the body, and each variable's slot IS its
# offset from it. That is what this emits, and what this checks: not
# that a DW_AT_location is present, but that it names the address the
# generated code actually stores to.
#
# Enabling it also turned up a latent bug for ANY 32-bit target: the
# writer emitted eight-byte addresses everywhere and said address_size
# 8 in the CU header. A debugger on ARMv7-M would have read the next
# field out of the second half of an address.
set -u
echo "TEST-MARKER debug-embedded"
. "$(dirname "$0")/../lib.sh"

DWDUMP=${EMBCC_LLVM_DWARFDUMP:-llvm-dwarfdump}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=$EMBCC_ROOT/tests/golden/out/debug-embedded
rm -rf "$out"; mkdir -p "$out"
command -v "$DWDUMP" >/dev/null 2>&1 || {
    echo "skipped: llvm-dwarfdump not found"; exit 0; }

cat > "$out/p.c" <<'CEOF'
struct point { int x, y; };
int scale(struct point *p, int k)
{
    int wide = p->x * k;
    int tall = p->y * k;
    return wide + tall;
}
CEOF

fail=0
for spec in "x86_64-elf:8:DW_OP_reg6" \
            "aarch64-elf:8:DW_OP_reg29" \
            "thumbv7m-none-eabi:4:DW_OP_breg13" \
            "riscv32-unknown-elf:4:DW_OP_breg2" \
            "riscv64-unknown-elf:8:DW_OP_breg2" \
            "mipsel-none-elf:4:DW_OP_breg29" \
            "loongarch64-unknown-elf:8:DW_OP_breg3" \
            "tricore-none-elf:4:DW_OP_breg26" \
            "xtensa-none-elf:4:DW_OP_breg1" \
            "avr:4:DW_OP_breg28"; do
    t=${spec%%:*}; rest=${spec#*:}; want_as=${rest%%:*}; want_fb=${rest#*:}
    o="$out/$t.o"
    "$EMBCC" --target="$t" -g -O0 -c "$out/p.c" -o "$o" 2> "$out/$t.err" || {
        echo "FAIL $t: -g does not compile"
        head -2 "$out/$t.err" | sed 's/^/     | /'; fail=1; continue; }

    if ! "$DWDUMP" --verify "$o" > "$out/$t.verify" 2>&1; then
        echo "FAIL $t: llvm-dwarfdump --verify rejects it"
        grep -iE 'error' "$out/$t.verify" | head -3 | sed 's/^/     | /'
        fail=1; continue
    fi

    "$DWDUMP" --debug-info "$o" > "$out/$t.di" 2>/dev/null
    got_as=$("$DWDUMP" --debug-info "$o" 2>/dev/null |
             grep -oE 'addr_size = 0x0*([0-9a-f]+)' | head -1 |
             grep -oE '[0-9a-f]+$' | sed 's/^0*//')
    [ -n "$got_as" ] || got_as=$(od -An -tu1 -j 10 -N 1 \
                                   "$out/$t.di" 2>/dev/null | tr -d ' ')
    if [ -n "$got_as" ] && [ "$got_as" != "$want_as" ]; then
        echo "FAIL $t: address_size is $got_as, this target's is $want_as"
        fail=1
    fi

    grep -q "$want_fb" "$out/$t.di" || {
        echo "FAIL $t: frame base is not $want_fb"
        grep -i frame_base "$out/$t.di" | head -1 | sed 's/^/     | /'
        fail=1; continue; }

    # Every named variable must have a location, and a line table must
    # exist -- an object with neither verifies clean and debugs nothing.
    nloc=$(grep -c 'DW_AT_location' "$out/$t.di") || nloc=0
    [ "$nloc" -ge 4 ] || {
        echo "FAIL $t: only $nloc variables have a location (want 4)"
        fail=1; }
    "$DWDUMP" --debug-line "$o" 2>/dev/null | grep -qE '^0x[0-9a-f]+ +[0-9]+' || {
        echo "FAIL $t: the line table has no rows"; fail=1; }
    echo "  $t: verifies, frame base $want_fb, address_size $want_as, $nloc locations"
done

# The claim that matters, on the two targets whose frame base is sp:
# a variable's DW_OP_fbreg offset must be the offset the CODE stores
# to. The frame base is sp+0 there, so the two are directly comparable
# -- and a wrong one would be a debugger printing a neighbouring
# variable, which verifies perfectly well.
if command -v "$OBJDUMP" >/dev/null 2>&1; then
    for spec in "riscv32-unknown-elf:riscv32:sp" "riscv64-unknown-elf:riscv64:sp"; do
        t=${spec%%:*}; rest=${spec#*:}; tri=${rest%%:*}
        o="$out/$t.o"
        [ -f "$o" ] || continue
        # p is the first parameter: fbreg +0, and the prologue stores
        # the first argument register there.
        off=$("$DWDUMP" --debug-info "$o" 2>/dev/null |
              grep -A2 'DW_AT_name	("p")' | grep -oE 'fbreg [+-][0-9]+' |
              grep -oE '[+-][0-9]+' | head -1)
        [ -n "$off" ] || { echo "FAIL $t: no location for 'p'"; fail=1; continue; }
        n=$(printf '%d' "$off")
        # [[:space:]] and not \t: POSIX ERE reads \t as a literal 't',
        # and objdump separates the mnemonic from its operands with a tab.
        if "$OBJDUMP" -d --mattr=+m,+c "$o" 2>/dev/null |
             grep -qE "s[dw][[:space:]]+a0,[[:space:]]*(0x)?$(printf '%x' "$n")\(sp\)"; then
            echo "  $t: 'p' at fbreg $off is the slot the prologue writes"
        else
            echo "FAIL $t: 'p' is at fbreg $off but nothing stores a0 there"
            "$OBJDUMP" -d --mattr=+m,+c "$o" 2>/dev/null |
                grep -E 's[dw].*\(sp\)' | head -4 | sed 's/^/     | /'
            fail=1
        fi
    done
    # ...and MIPS, whose frame base is $sp too: p arrives in $4 (a0)
    o="$out/mipsel-none-elf.o"
    off=$("$DWDUMP" --debug-info "$o" 2>/dev/null |
          grep -A2 'DW_AT_name	("p")' | grep -oE 'fbreg [+-][0-9]+' |
          grep -oE '[+-][0-9]+' | head -1)
    if [ -n "$off" ] && "$OBJDUMP" -d "$o" 2>/dev/null |
         grep -qE "sw[[:space:]]+\\\$4,[[:space:]]*0x$(printf '%x' "$(printf '%d' "$off")")\(\\\$sp\)"; then
        echo "  mipsel-none-elf: 'p' at fbreg $off is the slot the prologue writes"
    else
        echo "FAIL mipsel-none-elf: 'p' is at fbreg '$off' but nothing stores \$4 there"
        fail=1
    fi
    # ...and LoongArch64, whose frame base is $sp (r3): p arrives in a0
    o="$out/loongarch64-unknown-elf.o"
    off=$("$DWDUMP" --debug-info "$o" 2>/dev/null |
          grep -A2 'DW_AT_name	("p")' | grep -oE 'fbreg [+-][0-9]+' |
          grep -oE '[+-][0-9]+' | head -1)
    if [ -n "$off" ] && "$OBJDUMP" -d "$o" 2>/dev/null |
         grep -qE "st\.d[[:space:]]+\\\$a0, \\\$sp, $(printf '%d' "$off")\$"; then
        echo "  loongarch64-unknown-elf: 'p' at fbreg $off is the slot the prologue writes"
    else
        echo "FAIL loongarch64-unknown-elf: 'p' is at fbreg '$off' but nothing stores \$a0 there"
        fail=1
    fi
fi
# ...and Xtensa, whose frame base is a1 (sp): p arrives in a2. There is no
# Xtensa llvm-objdump; GNU's comes with the reference GCC (hostpaths.sh).
XOBJDUMP=$XTENSA_REF_DIR/bin/xtensa-esp-elf-objdump
if [ -x "$XOBJDUMP" ]; then
    o="$out/xtensa-none-elf.o"
    off=$("$DWDUMP" --debug-info "$o" 2>/dev/null |
          grep -A2 'DW_AT_name	("p")' | grep -oE 'fbreg [+-][0-9]+' |
          grep -oE '[+-][0-9]+' | head -1)
    if [ -n "$off" ] && "$XOBJDUMP" -d "$o" 2>/dev/null |
         grep -qE "s32i[[:space:]]+a2, a1, $(printf '%d' "$off")\$"; then
        echo "  xtensa-none-elf: 'p' at fbreg $off is the slot the prologue writes"
    else
        echo "FAIL xtensa-none-elf: 'p' is at fbreg '$off' but nothing stores a2 there"
        fail=1
    fi
fi

# A pointer is the target's width in the type DIEs too. The pointer
# DIE's byte_size was 8 on every target, so a debugger read a 32-bit
# target's pointer variable together with the four bytes after it.
for t in thumbv7m-none-eabi riscv32-unknown-elf riscv64-unknown-elf \
         mipsel-none-elf loongarch64-unknown-elf tricore-none-elf xtensa-none-elf \
         avr; do
    want=4; [ $t = riscv64-unknown-elf ] && want=8
    [ $t = loongarch64-unknown-elf ] && want=8
    [ $t = avr ] && want=2
    "$EMBCC" --target=$t -g -c "$out/p.c" -o "$out/ptr-$t.o" 2>/dev/null || {
        echo "FAIL $t: p.c with -g"; fail=1; continue; }
    got=$("$DWDUMP" --debug-info "$out/ptr-$t.o" 2>/dev/null |
          awk '/DW_TAG_pointer_type/ { p = 1; next }
               p && /DW_AT_byte_size/ { gsub(/[()]/, "", $2); print $2 + 0; p = 0 }' |
          sort -u | tr '\n' ' ')
    [ "$got" = "$want " ] || {
        echo "FAIL $t: pointer DIEs have byte_size '$got', the target's is $want"
        fail=1; }
done
[ "$fail" -eq 0 ] && echo "  pointer DIEs carry the target's pointer width"

# AVR. Its DWARF was EMPTY: the CU header said address_size 2 and every
# address field was written eight bytes wide, so a reader lost the thread
# at the first one; and the backend recorded no line rows at all. Now its
# addresses are four bytes, as avr-gcc writes them (a code address is a
# byte address in flash, past 64K on the larger parts; a data address is
# 0x800000 up), and the frame base is Y, r28:r29, which every slot is
# addressed from -- so a variable's DW_OP_fbreg IS the displacement in
# the code's own `std Y+q` / `ldd Y+q`. Checked at -O0 and -O2:
#   * p arrives in r24:r25 and the prologue stores it: `std Y+fbreg, r24`;
#   * at -O0, `wide` is stored to its slot by code the line table puts on
#     line 4, where `int wide = ...` is -- a location and a line that
#     agree with each other and with the instruction;
#   * at -O2 a local mem2reg took out of memory has an EMPTY location
#     (optimized out), not a slot nothing stores to: every location a
#     variable does have is a slot some instruction writes;
#   * the first row is the function's own line at its entry, so a
#     debugger takes the second as the end of the prologue.
if command -v "$OBJDUMP" >/dev/null 2>&1; then
    for O in 0 2; do
        o="$out/avr-O$O.o"
        "$EMBCC" --target=avr -g -O$O -c "$out/p.c" -o "$o" 2>/dev/null || {
            echo "FAIL avr -O$O: -g does not compile"; fail=1; continue; }
        "$DWDUMP" --verify "$o" > "$out/avr-O$O.verify" 2>&1 || {
            echo "FAIL avr -O$O: llvm-dwarfdump --verify rejects it"
            grep -iE 'error' "$out/avr-O$O.verify" | head -3 | sed 's/^/     | /'
            fail=1; continue; }
        "$DWDUMP" --debug-info "$o" > "$out/avr-O$O.di" 2>/dev/null
        "$DWDUMP" --debug-line "$o" > "$out/avr-O$O.dl" 2>/dev/null
        "$OBJDUMP" -d "$o" > "$out/avr-O$O.dis" 2>/dev/null
        # the address of each row of line N, and of the row after it
        rows=$(grep -E '^0x[0-9a-f]+ +[0-9]+ ' "$out/avr-O$O.dl" |
               while read -r a l rest; do echo "$(printf '%d' "$a") $l"; done)
        first=$(echo "$rows" | head -1)
        [ "$first" = "0 2" ] || {
            echo "FAIL avr -O$O: the first row is '$first', not line 2 at the entry"
            fail=1; }
        loc() {     # loc NAME -> its fbreg offset, or "empty", or nothing
            grep -A2 "DW_AT_name	(\"$1\")" "$out/avr-O$O.di" |
                sed -n -e 's/.*DW_OP_fbreg +\([0-9]*\).*/\1/p' \
                       -e 's/.*DW_AT_location	(<empty>).*/empty/p' | head -1
        }
        stores_to() {   # stores_to OFF -> the addresses of `std Y+OFF, ...`
            grep -E "std[[:space:]]+Y\+$1, r[0-9]+\$" "$out/avr-O$O.dis" |
                sed 's/^ *\([0-9a-f]*\):.*/\1/'
        }
        off=$(loc p)
        if [ -n "$off" ] && [ "$off" != empty ] &&
           grep -qE "std[[:space:]]+Y\+$off, r24\$" "$out/avr-O$O.dis"; then
            echo "  avr -O$O: 'p' at fbreg +$off is the slot the prologue writes (std Y+$off, r24)"
        else
            echo "FAIL avr -O$O: 'p' is at fbreg '$off' but nothing stores r24 there"
            grep -E 'std' "$out/avr-O$O.dis" | head -4 | sed 's/^/     | /'
            fail=1
        fi
        for v in wide tall k; do
            off=$(loc $v)
            [ -n "$off" ] || { echo "FAIL avr -O$O: '$v' has no DW_AT_location"; fail=1; continue; }
            [ "$off" = empty ] && continue
            [ -n "$(stores_to "$off")" ] || {
                echo "FAIL avr -O$O: '$v' is at fbreg +$off and nothing stores to Y+$off"
                fail=1; }
        done
        [ $O = 2 ] && continue
        # -O0: wide's store is on line 4
        off=$(loc wide)
        at=$(stores_to "$off" | head -1)
        line=$(echo "$rows" | awk -v a=$((0x${at:-0})) '$1 <= a { l = $2 } END { print l }')
        [ -n "$at" ] && [ "$line" = 4 ] || {
            echo "FAIL avr -O0: 'wide' (fbreg +$off) is stored at 0x$at, which the line table puts on line '$line', not 4"
            fail=1; }
    done
    # A PARAMETER the body assigns, at -O2: mem2reg turns `n` into
    # temporaries, and the prologue's store of the incoming value is the
    # only write its slot ever gets -- so the slot is right on entry and
    # wrong from the assignment on. Its location must be empty, or a slot
    # something writes after the prologue. `m`, never assigned, keeps its
    # slot: there the prologue's store is the value for good.
    cat > "$out/as.c" <<'CEOF'
int tick(void);
int bump(int n, int m)
{
    n = n * 3 + tick();
    return n + m + tick();
}
CEOF
    if "$EMBCC" --target=avr -g -O2 -c "$out/as.c" -o "$out/avr-as.o" 2>/dev/null; then
        "$DWDUMP" --debug-info "$out/avr-as.o" > "$out/avr-as.di" 2>/dev/null
        "$OBJDUMP" -d "$out/avr-as.o" > "$out/avr-as.dis" 2>/dev/null
        for v in n m; do
            off=$(grep -A2 "DW_AT_name	(\"$v\")" "$out/avr-as.di" |
                  sed -n -e 's/.*DW_OP_fbreg +\([0-9]*\).*/\1/p' \
                         -e 's/.*DW_AT_location	(<empty>).*/empty/p' | head -1)
            [ "$off" = empty ] && [ $v = n ] && continue
            nst=$(grep -cE "std[[:space:]]+Y\+$off, r[0-9]+\$" "$out/avr-as.dis") || nst=0
            if [ $v = n ] && [ "$nst" -lt 2 ]; then
                echo "FAIL avr -O2: the assigned parameter 'n' is at fbreg +$off, which only the prologue writes"
                fail=1
            elif [ $v = m ] && [ "$nst" -lt 1 ]; then
                echo "FAIL avr -O2: the parameter 'm' is at fbreg '$off', which nothing writes"
                fail=1
            fi
        done
    else
        echo "FAIL avr -O2: as.c does not compile with -g"; fail=1
    fi
    o0=$(grep -c 'DW_AT_location	(<empty>)' "$out/avr-O0.di" 2>/dev/null) || o0=0
    o2=$(grep -c 'DW_AT_location	(<empty>)' "$out/avr-O2.di" 2>/dev/null) || o2=0
    [ "$o0" -eq 0 ] || { echo "FAIL avr -O0: $o0 variables have no location"; fail=1; }
    [ "$fail" -eq 0 ] &&
        echo "  avr: -O0 and -O2 verify; locations are the slots the code writes, and" &&
        echo "  the line table puts wide's store on line 4 ($o2 optimized out at -O2)"
fi

# A function whose sp moves (alloca, a VLA) addresses its frame from the
# copy of sp the prologue leaves in r7 (Thumb) or s0 (RISC-V), so that is
# its frame base. It was sp, and every location was off once the block
# was allocated. The store of `n` is checked against its location.
cat > "$out/al.c" <<'CEOF'
int g(int n)
{
    char *buf = __builtin_alloca(n);
    buf[0] = 1;
    return n + buf[0];
}
CEOF
for t in thumbv7m-none-eabi riscv32-unknown-elf mipsel-none-elf \
         loongarch64-unknown-elf; do
    reg=r7; dw=breg7; [ $t = riscv32-unknown-elf ] && { reg=s0; dw=breg8; }
    [ $t = mipsel-none-elf ] && { reg='\$fp'; dw=breg30; }
    [ $t = loongarch64-unknown-elf ] && { reg='\$fp'; dw=breg22; }
    "$EMBCC" --target=$t -g -O0 -c "$out/al.c" -o "$out/al-$t.o" 2>/dev/null || {
        echo "FAIL $t: al.c with -g"; fail=1; continue; }
    "$DWDUMP" --debug-info "$out/al-$t.o" > "$out/al-$t.dw" 2>/dev/null
    grep -q "DW_AT_frame_base.*DW_OP_$dw" "$out/al-$t.dw" || {
        echo "FAIL $t: an alloca function's frame base is not $reg:"
        grep frame_base "$out/al-$t.dw"; fail=1; continue; }
    off=$(grep -A3 'DW_AT_name.*"n"' "$out/al-$t.dw" |
          sed -n 's/.*DW_OP_fbreg +\([0-9]*\).*/\1/p' | head -1)
    hex=$(printf '0x%x' "$off")
    # (a zero offset disassembles as `[r7]` and `0(s0)`; LoongArch's is
    # `st.d $a0, $fp, 16`, the offset in decimal)
    zero=; [ "$off" = 0 ] && zero="|\\[$reg\\]|[( ]0\\($reg\\)"
    lad=; [ $t = loongarch64-unknown-elf ] && lad="|st\\.[dw][[:space:]]+[^,]*, $reg, $off\$"
    "$OBJDUMP" -d "$out/al-$t.o" 2>/dev/null |
        grep -Eq "(str|sw).*(\[$reg, #$hex\]|$hex\($reg\)$zero)$lad" || {
        echo "FAIL $t: 'n' at fbreg +$off is not where the prologue stores it"
        fail=1; }
done
# Xtensa keeps sp at entry in a7 once the parameters are placed (which
# store from a1, the same address then): a7 is the frame base.
"$EMBCC" --target=xtensa-none-elf -g -O0 -c "$out/al.c" -o "$out/al-xt.o" 2>/dev/null &&
"$DWDUMP" --debug-info "$out/al-xt.o" 2>/dev/null |
    grep -q "DW_AT_frame_base.*DW_OP_breg7" || {
    echo "FAIL xtensa-none-elf: an alloca function's frame base is not a7"
    fail=1; }
[ "$fail" -eq 0 ] && echo "  an alloca function's locations are relative to r7 / s0 / fp / a7"

# What is NOT claimed. embld drops non-SHF_ALLOC sections and writes
# its own .embdbg sidecar, so the LINKED image carries no DWARF and a
# stock gdb cannot open it. The objects are right; carrying DWARF
# through a link means merging .debug_info across objects and rebasing
# every CU's abbrev and line-program offsets, which is a linker feature
# and not this one. Asserted so the day it changes, this says so.
printf 'int f(void){return 0;}\n' > "$out/one.c"
"$EMBCC" --target=riscv32-unknown-elf -g -c "$out/one.c" -o "$out/one.o" 2>/dev/null
if command -v llvm-readelf >/dev/null 2>&1; then
    llvm-readelf -S "$out/one.o" 2>/dev/null | grep -q '.debug_info' && \
        echo "  the object carries .debug_info (the linked image still does not)"
fi

[ "$fail" -eq 0 ] || exit 1
