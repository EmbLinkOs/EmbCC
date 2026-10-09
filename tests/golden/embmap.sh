#!/bin/sh
# embmap: where an image's memory goes, refereed by the tools that already
# know -- EmbLD's own --print-memory-usage table for the regions, llvm-nm
# for every symbol's size, llvm-readelf for every section's -- on a Cortex-M
# firmware linked with a real STM32 linker script, an ELF64 RISC-V image,
# and a big-endian MIPS one when the compiler has that target. Then the
# parts only embmap does: the per-file breakdown from the map, --diff
# between two builds, the budgets' exit status, and --json that parses.
set -u
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
EMBMAP=${EMBMAP:-./embmap}
[ -x "$EMBMAP" ] || { echo "FAIL: $EMBMAP is not built (make embmap)"; exit 1; }
NM=${EMBCC_LLVM_NM:-llvm-nm}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
echo "TEST-MARKER embmap"
command -v "$NM" >/dev/null 2>&1 || { echo "SKIP: $NM not found"; exit 0; }
command -v "$RE" >/dev/null 2>&1 || { echo "SKIP: $RE not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
out=tests/golden/out/embmap
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- a Cortex-M4 firmware on the STM32 script ---------------------------
d=tests/golden/ldscript
T=thumbv7em-none-eabi
# -ffunction-sections, as firmware is built: one input section per
# function, whose long names wrap onto a second line in the map
for f in startup prog; do
    "$EMBCC" --target=$T -O2 -ffunction-sections -fdata-sections \
        -c "$d/$f.c" -o "$out/$f.o" || fail "$f.c"
done
"$EMBCC" --target=$T -O2 -c tests/harness/thumb/io.c -o "$out/io.o" || fail io.c
"$EMBLD" -T "$d/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    -o "$out/fw.elf" -Map "$out/fw.map" --print-memory-usage \
    > "$out/ld.txt" 2>&1 || { cat "$out/ld.txt"; fail "the STM32 image does not link"; }

# the regions, against EmbLD's table for the same script
"$EMBMAP" "$out/fw.elf" --region RAM:0x20000000:64K --region FLASH:0:256K \
    > "$out/regions.txt" || fail "embmap fails on the STM32 image"
for r in RAM FLASH; do
    want=$(awk -v r="$r:" '$1 == r { print $2 }' "$out/ld.txt")
    got=$(awk -v r="$r" '$1 == r { print $2 }' "$out/regions.txt")
    [ -n "$want" ] || { cat "$out/ld.txt"; fail "EmbLD printed no $r usage"; }
    [ "$want" = "$got" ] ||
        { cat "$out/ld.txt" "$out/regions.txt"; fail "$r: EmbLD counts $want bytes, embmap $got"; }
done

# Every symbol's size and every section's, against llvm's. check.py
# reads embmap's --json and the two llvm listings and compares them.
cat > "$out/check.py" <<'PY'
import json, re, subprocess, sys
img, nm, re_, js = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
d = json.load(open(js))
# llvm-readelf -S: allocated sections with a non-zero size
secs = {}
for l in subprocess.run([re_, '-SW', img], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*A\S*)\s', l)
    if m and int(m.group(4), 16):
        secs[m.group(1)] = (int(m.group(3), 16), int(m.group(4), 16))
got = {s['name']: (s['address'], s['size']) for s in d['sections']}
if got != secs:
    print('sections differ:', sorted(set(got.items()) ^ set(secs.items())))
    sys.exit(1)
# the top list (asked for every symbol): sizes as llvm-nm -S reports them
want = {}
for l in subprocess.run([nm, '-S', '--defined-only', img], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 4 and f[2] in 'TtDdBbRr' and int(f[1], 16):
        want.setdefault(f[3], set()).add(int(f[1], 16))
for s in d['top']:
    if s['name'] not in want or s['size'] not in want[s['name']]:
        print('symbol', s['name'], s['size'], 'not as llvm-nm says:', want.get(s['name']))
        sys.exit(1)
# every span of bytes once: two names for the same bytes (an alias) are
# one entry, and every span llvm-nm knows is there
arm0 = d['machine'] == 40
nmspans = set()
for l in subprocess.run([nm, '-S', '--defined-only', img], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 4 and f[2] in 'TtDdBbRr' and int(f[1], 16):
        a = int(f[0], 16)
        if arm0 and f[2] in 'Tt':
            a &= ~1
        nmspans.add((a, int(f[1], 16)))
tops = [(s['address'], s['size']) for s in d['top']]
if len(tops) != len(set(tops)):
    print('the same bytes listed twice:', sorted(t for t in tops if tops.count(t) > 1)); sys.exit(1)
if set(tops) != nmspans:
    print('spans differ from llvm-nm:', sorted(set(tops) ^ nmspans)); sys.exit(1)
# the bytes no symbol covers, per section: the section's size less the
# union of its functions' and objects' ranges as llvm-nm gives them (a
# Thumb function's address carries its low bit; aliases count once)
arm = d['machine'] == 40
spans = {}
for l in subprocess.run([nm, '-S', '--defined-only', img], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 4 and f[2] in 'TtDdBbRr' and int(f[1], 16):
        a, z = int(f[0], 16), int(f[1], 16)
        if arm and f[2] in 'Tt':
            a &= ~1
        spans.setdefault((a, z), 1)
for s in d['sections']:
    lo, hi = s['address'], s['address'] + s['size']
    iv = sorted((max(a, lo), min(a + z, hi)) for a, z in spans if a < hi and a + z > lo)
    cov, reach = 0, lo
    for x, y in iv:
        x = max(x, reach)
        if y > x:
            cov += y - x; reach = y
    if s['unattributed'] != s['size'] - cov:
        print('section', s['name'], 'unattributed', s['unattributed'], 'but llvm-nm leaves', s['size'] - cov)
        sys.exit(1)
# the totals add up
k = {'text': 0, 'rodata': 0, 'data': 0, 'bss': 0}
for s in d['sections']:
    k[s['kind']] += s['size']
for n in k:
    if d[n] != k[n]:
        print('total', n, d[n], 'is not the sum of its sections', k[n]); sys.exit(1)
if d['flash'] != k['text'] + k['rodata'] + k['data'] or d['ram'] != k['data'] + k['bss']:
    print('flash/ram are not text+rodata+data / data+bss'); sys.exit(1)
print('ok', len(d['sections']), 'sections', len(d['top']), 'symbols')
PY

check() {   # check IMAGE LABEL
    "$EMBMAP" "$1" --json --top 100000 > "$1.json" || fail "$2: embmap --json fails"
    python3 "$out/check.py" "$1" "$NM" "$RE" "$1.json" > "$1.chk" 2>&1 ||
        { cat "$1.chk"; fail "$2: embmap disagrees with llvm"; }
    echo "$2: $(cat "$1.chk")"
}
check "$out/fw.elf" "Cortex-M4"

# ---- the bytes by input file, from the map ------------------------------
"$EMBMAP" "$out/fw.elf" --map "$out/fw.map" --json > "$out/files.json" ||
    fail "embmap --map fails"
python3 - "$out/files.json" "$out/prog.o" "$NM" > "$out/files.txt" 2>&1 <<'PY' ||
import json, subprocess, sys
d = json.load(open(sys.argv[1]))
files = {f['file']: f for f in d['files']}
prog = [f for p, f in files.items() if p.endswith('prog.o')]
if len(prog) != 1:
    print('prog.o is not in the per-file listing:', list(files)); sys.exit(1)
# prog.o's text is at least the sum of its functions as llvm-nm sizes them
nm = subprocess.run([sys.argv[3], '-S', '--defined-only', sys.argv[2]],
                    capture_output=True, text=True).stdout.split('\n')
fn = sum(int(l.split()[1], 16) for l in nm if len(l.split()) == 4 and l.split()[2] in 'Tt')
if prog[0]['text'] < fn:
    print('prog.o text', prog[0]['text'], 'is less than its functions', fn); sys.exit(1)
tot = sum(f['text'] + f['rodata'] for f in d['files'])
if tot > d['text'] + d['rodata']:
    print('the files hold more text than the image', tot); sys.exit(1)
print('ok')
PY
{ cat "$out/files.txt"; fail "the per-file breakdown is wrong"; }

# ---- --diff: the same program built twice -------------------------------
"$EMBCC" --target=$T -O0 -c "$d/prog.c" -o "$out/prog0.o" || fail "prog.c -O0"
"$EMBLD" -T "$d/stm32.ld" "$out/startup.o" "$out/prog0.o" "$out/io.o" \
    -o "$out/fw0.elf" 2>/dev/null || fail "the -O0 image does not link"
"$EMBMAP" "$out/fw.elf" --diff "$out/fw0.elf" --json --top 100000 \
    > "$out/diff.json" || fail "embmap --diff fails"
python3 - "$out/diff.json" "$out/fw0.elf" "$out/fw.elf" "$NM" > "$out/diff.txt" 2>&1 <<'PY' ||
import json, subprocess, sys
d = json.load(open(sys.argv[1]))['diff']
def sizes(img):
    t = {}
    for l in subprocess.run([sys.argv[4], '-S', '--defined-only', img], capture_output=True, text=True).stdout.splitlines():
        f = l.split()
        if len(f) == 4 and f[2] in 'TtDdBbRr':
            t[f[3]] = t.get(f[3], 0) + int(f[1], 16)
    return t
old, new = sizes(sys.argv[2]), sizes(sys.argv[3])
got = {s['name']: (s['old'], s['new']) for s in d['symbols']}
for n in set(old) | set(new):
    o, w = old.get(n, 0), new.get(n, 0)
    if o != w and got.get(n) != (o, w):
        print('symbol', n, 'changed', o, '->', w, 'and embmap says', got.get(n)); sys.exit(1)
if 'main' not in got:
    print('main did not change between -O0 and -O2?'); sys.exit(1)
print('ok', len(got), 'symbols changed')
PY
{ cat "$out/diff.txt"; fail "--diff is wrong"; }

# ---- --diff sums two file-scope statics of one name -----------------------
printf 'static int helper(int x) { return x * 3 + 1; }\nint one(int x) { return helper(x); }\n' > "$out/tu1.c"
printf 'static int helper(int x) { return x * x %s; }\nint two(int x) { return helper(x); }\nint main(void) { extern int one(int); return one(2) + two(3); }\n' "" > "$out/tu2.c"
printf 'static int helper(int x) { return x * x / 7 + (x << 3) - (x >> 2); }\nint two(int x) { return helper(x); }\nint main(void) { extern int one(int); return one(2) + two(3); }\n' > "$out/tu2b.c"
for f in tu1 tu2 tu2b; do
    "$EMBCC" --target=riscv64-unknown-elf -O0 -c "$out/$f.c" -o "$out/$f.o" || fail "$f.c"
done
"$EMBLD" -e main -Ttext 0x80000000 "$out/tu1.o" "$out/tu2.o" -o "$out/h1.elf" 2>/dev/null &&
"$EMBLD" -e main -Ttext 0x80000000 "$out/tu1.o" "$out/tu2b.o" -o "$out/h2.elf" 2>/dev/null ||
    fail "the two-file images do not link"
"$EMBMAP" "$out/h2.elf" --diff "$out/h1.elf" --json --top 100000 > "$out/hdiff.json" ||
    fail "embmap --diff fails on the two-file images"
python3 - "$out/hdiff.json" "$out/h1.elf" "$out/h2.elf" "$NM" > "$out/hdiff.txt" 2>&1 <<'PY' ||
import json, subprocess, sys
d = json.load(open(sys.argv[1]))['diff']
def total(img, name):
    t = 0
    for l in subprocess.run([sys.argv[4], '-S', '--defined-only', img], capture_output=True, text=True).stdout.splitlines():
        f = l.split()
        if len(f) == 4 and f[3] == name:
            t += int(f[1], 16)
    return t
got = {s['name']: (s['old'], s['new']) for s in d['symbols']}
want = (total(sys.argv[2], 'helper'), total(sys.argv[3], 'helper'))
if got.get('helper') != want:
    print('helper (two statics) changed', want, 'and embmap says', got.get('helper')); sys.exit(1)
print('ok')
PY
{ cat "$out/hdiff.txt"; fail "--diff does not sum same-named symbols"; }

# ---- the budgets ---------------------------------------------------------
flash=$(python3 -c "import json; print(json.load(open('$out/fw.elf.json'))['flash'])")
ram=$(python3 -c "import json; print(json.load(open('$out/fw.elf.json'))['ram'])")
"$EMBMAP" "$out/fw.elf" --max-flash "$flash" --max-ram "$ram" >/dev/null 2>&1 ||
    fail "an image exactly at its budget is refused"
"$EMBMAP" "$out/fw.elf" --max-flash $((flash - 1)) >/dev/null 2>"$out/b.err" &&
    fail "an image one byte over --max-flash passes"
grep -q "needs $flash bytes of flash" "$out/b.err" || { cat "$out/b.err"; fail "the flash refusal does not say why"; }
"$EMBMAP" "$out/fw.elf" --max-ram $((ram - 1)) >/dev/null 2>&1 &&
    fail "an image one byte over --max-ram passes"
"$EMBMAP" "$out/fw.elf" --region FLASH:0:512 >/dev/null 2>&1 &&
    fail "a region the image overflows passes"

# ---- other images: ELF64, and big-endian when the target exists ----------
cat > "$out/p.c" <<'EOF'
int g = 3, big[100];
static const char msg[] = "hello";
int f(int x) { return x * g + msg[x & 3]; }
int f_alias(int x) __attribute__((alias("f")));
int main(void) { big[3] = f(1) + f_alias(2); return big[3]; }
EOF
"$EMBCC" --target=riscv64-unknown-elf -O2 -c "$out/p.c" -o "$out/rv.o" &&
"$EMBLD" -e main -Ttext 0x80000000 "$out/rv.o" -o "$out/rv.elf" 2>/dev/null ||
    fail "the RV64 image does not build"
check "$out/rv.elf" "RISC-V 64 (ELF64)"
check "$out/rv.o" "RISC-V 64 object"
if "$EMBCC" --target=mips-none-elf -c "$out/p.c" -o "$out/be.o" 2>/dev/null; then
    "$EMBLD" -e main -Ttext 0x80000000 "$out/be.o" -o "$out/be.elf" 2>/dev/null ||
        fail "the big-endian MIPS image does not link"
    check "$out/be.elf" "MIPS big-endian"
    grep -q '"byte_order": "big"' "$out/be.elf.json" || fail "the MIPS image is not reported big-endian"
else
    echo "(no mips-none-elf in this compiler: the big-endian image is skipped)"
fi

# ---- refusals ------------------------------------------------------------
"$EMBMAP" "$d/stm32.ld" >/dev/null 2>"$out/r.err" && fail "a text file is read as an image"
grep -q 'not an ELF file' "$out/r.err" || { cat "$out/r.err"; fail "a text file's refusal does not say why"; }
"$EMBMAP" "$out/fw.elf" --region FLASH >/dev/null 2>&1 && fail "a malformed --region is accepted"
echo "embmap: regions agree with EmbLD, every section and symbol with llvm, files, diff, budgets"
