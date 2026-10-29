#!/bin/sh
# GCC's __builtin_extract_return_addr and __builtin_frob_return_addr:
# the address __builtin_return_address gives, made the one the function
# returns to, and back. That is the address itself everywhere but SPARC,
# whose return is past its call -- 8 bytes, or 12 in a function that
# returns through the caller's buffer (a struct, a long double), where the
# caller put an `unimp` word after the delay slot: GCC's
# RETURN_ADDR_OFFSET, and src/arch/sparc/codegen.c's own `jmpl %i7 + 12`.
# tests/exec/builtin-powi.c runs frob(extract(r)) == r on the boards.
set -u
echo "TEST-MARKER builtin-return-addr"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/builtin-return-addr
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/r.c" <<'EOT'
struct S { int a[4]; };
void *volatile seen;
int scalar(void) { seen = __builtin_extract_return_addr(__builtin_return_address(0)); return 1; }
struct S agg(void) { struct S s = {{1}}; seen = __builtin_extract_return_addr(__builtin_return_address(0)); return s; }
long double ld(void) { seen = __builtin_extract_return_addr(__builtin_return_address(0)); return 1; }
void *frob(void *p) { return __builtin_frob_return_addr(p); }
EOT
"$EMBCC" --target=sparc-elf -O2 -c "$out/r.c" -o "$out/sparc.o" || fail "sparc: does not compile"
llvm-objdump -d "$out/sparc.o" > "$out/sparc.dis" || fail "llvm-objdump"
# fn PATTERN: the function's body holds the instruction (an extended
# regular expression: LLVM 18 writes the immediates in decimal, 23 in hex)
has() {
    sed -n "/<$1>:/,/^\$/p" "$out/sparc.dis" | grep -Eq "$2" || {
        cat "$out/sparc.dis"; fail "sparc: $1 does not $3"; }
}
has scalar 'add[[:space:]]*%o0, (0x8|8), %o0' "add 8 (a scalar return)"
has agg    'add[[:space:]]*%o0, (0xc|12), %o0' "add 12 (a struct return, past the unimp)"
has ld     'add[[:space:]]*%o0, (0xc|12), %o0' "add 12 (a long double return, past the unimp)"
has frob   'sub[[:space:]]*%o0, (0x8|8), %o0' "subtract 8"
echo "builtin-return-addr: SPARC adds 8, or 12 past a struct return's unimp, and frob takes it off"

"$EMBCC" --target=riscv32-unknown-elf -O2 -c "$out/r.c" -o "$out/rv.o" || fail "riscv32: does not compile"
llvm-objdump -d "$out/rv.o" | sed -n '/<frob>:/,/^$/p' > "$out/rv-frob.dis"
grep -q "ret" "$out/rv-frob.dis" && [ "$(grep -c '	[a-f0-9]\{4,8\} ' "$out/rv-frob.dis")" -le 1 ] || {
    cat "$out/rv-frob.dis"; fail "riscv32: frob is not the address itself (a bare ret)"; }
echo "builtin-return-addr: elsewhere both are the address itself"
