#!/bin/sh
# 64-bit integers on Xtensa, in register pairs (#184: an RTOS's 64-bit
# tick arithmetic, embk_deadline_from_timeout, was 177 bytes to GCC's 62).
# A 64-bit add, subtract and logic op are done in the pairs themselves
# where the result's pair is each operand's or apart from it -- not
# through two scratch pairs and back -- and an (in)equality with its
# branch is one test per half, against an immediate where bnei/beqi name
# it (`a == ~0ull`: bnei a2, -1; bnei a3, -1) -- not a 0/1 built and
# tested. Sizes in bytes, from the object (before: 26, 28, 15, 36, 27);
# that the code is right is xtensa-exec's (its corpus computes in 64
# bits).
set -u
echo "TEST-MARKER xtensa-int64"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/xtensa-int64
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_NM:-llvm-nm}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-nm"; exit 0; }
cat > "$out/i.c" <<'EOC'
typedef unsigned long long u64;
u64 add64(u64 a, u64 b) { return a + b; }
u64 sub64(u64 a, u64 b) { return a - b; }
u64 xor64(u64 a, u64 b) { return a ^ b; }
int ismax(u64 a) { return a == ~0ull ? 3 : 5; }
int iseq(u64 a, u64 b) { if (a == b) return 3; return 5; }
EOC
"$EMBCC" --target=xtensa-esp32-elf -Os -c "$out/i.c" -o "$out/i.o" || exit 1
"$OD" -S --radix=d "$out/i.o" | awk '{print $4, $2 + 0}' > "$out/sizes.txt"
fail=0
for want in add64:16 sub64:18 xor64:12 ismax:20 iseq:19; do
    f=${want%%:*}; max=${want#*:}
    got=$(awk -v f="$f" '$1 == f {print $2}' "$out/sizes.txt")
    [ -n "$got" ] && [ "$got" -le "$max" ] || {
        echo "FAIL: $f is ${got:-missing} bytes, at most $max wanted"; fail=1; }
done
[ "$fail" = 0 ] || { cat "$out/sizes.txt"; exit 1; }
echo "xtensa-int64: $(tr '\n' ' ' < "$out/sizes.txt")bytes -- in place, and a branch per half"
