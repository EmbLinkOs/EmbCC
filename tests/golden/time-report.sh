#!/bin/sh
# -ftime-report: GCC's and clang's flag for where a compile's time went.
# It must be accepted, report each phase and a total on stderr, and change
# nothing else -- the object is the same with or without it.
set -u
echo "TEST-MARKER time-report"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/time-report
rm -rf "${out:?}"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
printf 'int sq(int x) { return x * x; }\nint main(void) { return sq(6) - 36; }\n' > "$out/t.c"
for t in x86_64-elf thumbv7em-none-eabi riscv32-unknown-elf avr; do
    "$EMBCC" --target=$t -O2 -c "$out/t.c" -o "$out/plain.o" ||
        { echo "FAIL: $t: does not compile"; exit 1; }
    "$EMBCC" --target=$t -O2 -ftime-report -c "$out/t.c" -o "$out/timed.o" \
        2> "$out/report.txt" || { echo "FAIL: $t: -ftime-report refused:"
        head -3 "$out/report.txt"; exit 1; }
    for phase in preprocess parse "semantic analysis" "IR generation" \
                 optimization "code generation" TOTAL; do
        grep -q "^ $phase  *:" "$out/report.txt" || {
            echo "FAIL: $t: no '$phase' line in the report:"
            cat "$out/report.txt"; exit 1; }
    done
    cmp -s "$out/plain.o" "$out/timed.o" || {
        echo "FAIL: $t: -ftime-report changed the object"; exit 1; }
done
echo "-ftime-report reports every phase and a total, and changes no object"
