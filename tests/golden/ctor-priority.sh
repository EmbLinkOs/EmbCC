#!/bin/sh
# __attribute__((constructor(N))) and destructor(N): the priority orders
# the array. EmbCC refused it ("cannot honour a priority"). Now, as with
# GCC, the address goes in .init_array.NNNNN (.fini_array.NNNNN), and the
# link lays the numbered sections out ascending ahead of the plain array
# -- embld's default layout, as GNU ld's default script does, and a
# script's SORT_BY_INIT_PRIORITY.
#
#   - the object has one section per priority, named as GCC names them,
#     and -S writes the same sections;
#   - a priority of 0-100 warns (-Wprio-ctor-dtor), and one past 65535
#     is refused;
#   - on the Cortex-M3 board, constructors from two objects -- one built
#     by EmbCC, one by clang, which spells the sections .init_array.101 --
#     run 101, 150, 200, then the plain ones in link order, at -O0 and
#     -O2, with the default layout and with a linker script.
set -u
echo "TEST-MARKER ctor-priority"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/ctor-priority
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
export EMBCC_VERIFY=1
T=thumbv7m-none-eabi

cat > "$out/a.c" <<'EOF'
extern int order[16], n;
__attribute__((constructor(200))) static void a200(void) { order[n++] = 200; }
__attribute__((constructor)) static void aplain(void) { order[n++] = 1; }
__attribute__((constructor(150))) static void a150(void) { order[n++] = 150; }
__attribute__((destructor(300))) static void d300(void) { order[n++] = 300; }
EOF
cat > "$out/b.c" <<'EOF'
extern int order[16], n;
__attribute__((constructor)) static void bplain(void) { order[n++] = 2; }
__attribute__((constructor(101))) static void b101(void) { order[n++] = 101; }
EOF
"$EMBCC" --target=$T -O2 -c "$out/a.c" -o "$out/a.o" || fail "a.c"
llvm-readelf -S "$out/a.o" | sed -n 's/.*\] \(\.[a-z_]*array[.0-9]*\) .*/\1/p' | sort > "$out/a.secs"
printf '%s\n' .fini_array.00300 .init_array .init_array.00150 .init_array.00200 > "$out/a.want"
cmp -s "$out/a.secs" "$out/a.want" ||
    fail "a.o's array sections: $(tr '\n' ' ' < "$out/a.secs")"
"$EMBCC" --target=$T -O2 -S "$out/a.c" -o - |
    sed -n 's/^[[:space:]]*\.section[[:space:]]*\([^,]*array[^,]*\),.*/\1/p' | sort > "$out/a.ssecs"
cmp -s "$out/a.ssecs" "$out/a.want" || fail "-S's array sections: $(tr '\n' ' ' < "$out/a.ssecs")"
echo "one section per priority, named as GCC names them, by -c and -S alike"

printf '__attribute__((constructor(50))) void lo(void) {}\n' > "$out/lo.c"
"$EMBCC" --target=$T -c "$out/lo.c" -o "$out/lo.o" 2> "$out/lo.err" || fail "priority 50 refused"
grep -q "reserved for the implementation" "$out/lo.err" || fail "priority 50 did not warn"
printf '__attribute__((constructor(70000))) void hi(void) {}\n' > "$out/hi.c"
"$EMBCC" --target=$T -c "$out/hi.c" -o "$out/hi.o" 2> "$out/hi.err" && fail "priority 70000 accepted"
grep -q "a priority is 0 to 65535" "$out/hi.err" || fail "priority 70000: $(cat "$out/hi.err")"
echo "a reserved priority warns and an impossible one is refused"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
CLANG=${EMBCC_REF_CLANG:-clang}
if command -v "$QEMU" >/dev/null 2>&1; then
    d=$out/m3
    mkdir -p "$d"
    cat > "$d/main.c" <<'EOF'
void putn(long v);
void puts_(const char *s);
int order[16], n;
int main(void)
{
    puts_("order");
    for (int i = 0; i < n; i++) { puts_(" "); putn(order[i]); }
    puts_("\n");
    return 42;
}
EOF
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    if command -v "$CLANG" >/dev/null 2>&1; then
        "$CLANG" --target=$T -O2 -c "$out/b.c" -o "$d/bc.o" || fail "clang b.c"
        llvm-readelf -S "$d/bc.o" | grep -q '\.init_array\.101 ' ||
            fail "clang's b.o has no .init_array.101: the mixed case is not tested"
        bobj=$d/bc.o
    else
        bobj=""
    fi
    cat > "$d/fw.ld" <<'EOF'
MEMORY { FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
ENTRY(reset)
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } > FLASH
  .data : { __data_start = .; *(.data*) __data_end = .; } > RAM AT > FLASH
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } > RAM
  /DISCARD/ : { *(.fini_array*) }
}
EOF
    for O in -O0 -O2; do
        "$EMBCC" --target=$T $O -c "$d/main.c" -o "$d/main.o" || fail "main.c at $O"
        for f in a b; do
            "$EMBCC" --target=$T $O -c "$out/$f.c" -o "$d/$f.o" || fail "$f.c at $O"
        done
        for B in "$d/b.o" $bobj; do
            for L in default script; do
                if [ $L = default ]; then
                    EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/p.elf" \
                        "$d/main.o" "$d/a.o" "$B" > "$d/link.log" 2>&1
                else
                    "$EMBLD" -T "$d/fw.ld" "$d/boot.o" "$d/io.o" "$d/main.o" "$d/a.o" \
                        "$B" -o "$d/p.elf" > "$d/link.log" 2>&1
                fi || fail "$O $L link with $(basename "$B"): $(head -3 "$d/link.log")"
                sh tests/harness/qrun.sh 10 "$QEMU" -M lm3s6965evb -cpu cortex-m3 \
                    -nographic -kernel "$d/p.elf" > "$d/console.log" 2>&1
                tr -d '\r' < "$d/console.log" | tr -s ' ' | grep -q "^order 101 150 200 1 2 *$" ||
                    fail "$O, $L layout, $(basename "$B"): $(grep order "$d/console.log")"
            done
        done
    done
    echo "on the Cortex-M3 the constructors run 101, 150, 200, then plain, with EmbCC's and clang's objects, either layout"
else
    echo "SKIP: the board run ($QEMU not found)"
fi
echo "ok ctor-priority"
