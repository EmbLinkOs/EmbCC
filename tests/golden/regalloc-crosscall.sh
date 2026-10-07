#!/bin/sh
# Which values crossing a call get the callee-saved registers.
#
# A value live across a call may only take a callee-saved register, and
# there are few: five on x86-64, eleven on RISC-V, five the Thumb backend
# can give. The allocator's simplify phase decides who goes without by
# spill cost -- but only among nodes it cannot already prove colourable,
# and it proved that against the WHOLE pool, twenty registers on RISC-V.
# Every call-crossing value passed, the cost never ran, and the colouring
# order chose: in a loop of calls it kept the constants and addresses
# LICM had hoisted, each read once a trip, and sent the loop's own counter
# to a stack slot -- `i++; i < 700` as seven instructions through two
# slots on RV32.
#
# The program is that shape (a string hash table's insert loop). What is
# checked is the emitted code: no load or store to a stack slot inside an
# innermost loop, a loop being the span of a backward branch. Where a
# value must go to memory it is one read once a trip in an outer loop,
# not the inner loop's counter. Thumb gives five registers (r4-r8) to
# twelve values that cross a call, so there the constants read once a
# trip are still reloaded: the LCG's multiplier, and since `% 900` became
# a multiply (pass_divmagic, umull) the divisor's magic number -- two
# accesses are allowed there and no more, neither of them the counter.
#
# EMBCC_RA_POOL_K=1 restores the old count, and this test fails under it
# on all five targets.
set -u
echo "TEST-MARKER regalloc-crosscall"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }

out=tests/golden/out/regalloc-crosscall
rm -rf "$out"; mkdir -p "$out"

cat > "$out/hot.c" <<'EOF'
void mk(char *k, unsigned v);
unsigned *put(const char *k);
unsigned seed;
unsigned char used[1024];
unsigned hot(long n)
{
    unsigned acc = 0;
    char k[12];
    for (long r = 0; r < n; r++) {
        for (int i = 0; i < 1024; i++)
            used[i] = 0;
        for (int i = 0; i < 700; i++) {
            seed = seed * 1103515245u + 12345u;
            mk(k, (seed >> 8) % 900);
            *put(k) += (unsigned)i;
        }
        for (unsigned v = 0; v < 900; v += 7) {
            mk(k, v);
            acc = acc * 17u + *put(k);
        }
    }
    return acc;
}
EOF

# Stack-slot accesses inside innermost loops of an objdump listing. Calls
# are not branches; a backward branch's span [target, branch] is a loop.
cat > "$out/inner.awk" <<'EOF'
function hex(s,   i, v) {
    v = 0; s = tolower(s); sub(/^0x/, "", s)
    for (i = 1; i <= length(s); i++)
        v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
    return v
}
/^ +[0-9a-f]+:/ {
    a = $1; sub(/:$/, "", a); addr[n] = hex(a); text[n] = $0
    stk[n] = ($0 ~ /\((sp|s0)\)|\(%r[bs]p\)|\[(sp|x29)[],]/) ? 1 : 0
    tgt[n] = -1
    if ($2 !~ /^(bl|blx|jal|jalr|call|callq|ret|retq|bx)$/ && $2 ~ /^(b|j|cb)/ &&
        match($0, /0x[0-9a-f]+ </))
        tgt[n] = hex(substr($0, RSTART, RLENGTH - 2))
    n++
}
END {
    for (i = 0; i < n; i++)
        if (tgt[i] >= 0 && tgt[i] <= addr[i]) { lo[nl] = tgt[i]; hi[nl] = addr[i]; nl++ }
    for (L = 0; L < nl; L++) {
        inner = 1
        for (M = 0; M < nl; M++)
            if (M != L && lo[M] >= lo[L] && hi[M] <= hi[L] &&
                (lo[M] != lo[L] || hi[M] != hi[L]))
                inner = 0
        if (!inner) continue
        for (i = 0; i < n; i++)
            if (addr[i] >= lo[L] && addr[i] <= hi[L] && stk[i]) {
                total++; print text[i] > "/dev/stderr"
            }
    }
    print total + 0
}
EOF

fail=0
for t in x86_64-elf:0 aarch64-elf:0 riscv32-unknown-elf:0 \
         riscv64-unknown-elf:0 thumbv7em-none-eabi:2; do
    tg=${t%:*}; max=${t#*:}
    "$EMBCC" --target=$tg -O2 -c "$out/hot.c" -o "$out/$tg.o" || {
        echo "$tg: does not compile"; exit 1; }
    "$OD" -d --no-show-raw-insn "$out/$tg.o" > "$out/$tg.s" 2>&1
    got=$(awk -f "$out/inner.awk" "$out/$tg.s" 2> "$out/$tg.inner")
    if [ "$got" -gt "$max" ]; then
        echo "$tg: $got stack-slot access(es) inside an inner loop (at most $max):"
        cat "$out/$tg.inner"
        fail=1
    fi
done
[ $fail = 0 ] || exit 1
echo "a loop of calls keeps its counter in a callee-saved register and
sends what is read once a trip to memory instead, on all five targets"
