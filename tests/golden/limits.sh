#!/bin/sh
# What each target compiles: every construct below, on every target, and
# whether EmbCC compiles it, accepts it with a warning, or refuses it by
# name. The answers are tests/golden/limits/expected.txt, and the manual's
# page of them, docs/manual/limits.md, is made from that file.
#
# The refusal tables in the manual drifted from the compiler before this:
# a feature's pull request updated its own page and not the others that
# listed the old refusal, so C++ read as refused on MIPS, Xtensa and
# TriCore for weeks after it compiled there, and nothing failed. Now a
# lifted refusal, a new one, or a reworded message fails this test until
# the matrix and the page say so too:
#
#   sh tests/golden/limits.sh --update     # rewrite both from the compiler
#
# then read the diff: every changed line is a claim the manual makes.
set -u
echo "TEST-MARKER limits"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
UPDATE=0
[ "${1:-}" = --update ] && UPDATE=1
WANT=tests/golden/limits/expected.txt
PAGE=docs/manual/limits.md
out=tests/golden/out/limits
rm -rf "${out:?}"; mkdir -p "$out/src"

# One triple per family and variant, bare metal first, then the hosted ones.
TARGETS="thumbv6m-none-eabi thumbv7m-none-eabi thumbv7em-none-eabihf
thumbv8m.base-none-eabi thumbv8m.main-none-eabi armv7a-none-eabi
riscv32-unknown-elf riscv64-unknown-elf avr mipsel-none-elf mips-none-elf
mips64el-none-elf mips64-none-elf loongarch64-unknown-elf xtensa-none-elf
tricore-none-elf rx-none-elf powerpc-none-eabi sparc-none-elf m68k-none-elf
x86_64-elf aarch64-elf x86_64-linux-gnu aarch64-apple-darwin
x86_64-windows-gnu"

# construct NAME FILE "OPTIONS" "WHAT THE PAGE CALLS IT" -- the source is
# read from standard input. Each is the smallest unit that needs the
# construct; `plain.c` is there for the options that apply to any code.
# A FILE an earlier construct wrote is shared, and its caller gives no
# source: reading standard input again once emptied plain.c, so the -S,
# -g and --unwind-tables cells compiled an empty file on every target.
CONSTRUCTS=
construct() {
    [ -f "$out/src/$2" ] || cat > "$out/src/$2"
    cat > "$out/src/$2"
    again "$@"
}
# again NAME FILE "OPTIONS" "WHAT": another construct over a file an earlier
# one wrote (plain.c), which must not read standard input -- it once did,
# and emptied plain.c, or waited for input that never came
again() {
    CONSTRUCTS="$CONSTRUCTS $1"
    eval "file_$1=\$2; opts_$1=\$3; what_$1=\$4"
}
construct int128 int128.c "" '`__int128`' <<'EOF'
__int128 g;
__int128 f(__int128 a) { return a * g / 3; }
EOF
construct ldouble ldouble.c "" '`long double` arithmetic' <<'EOF'
long double f(long double a, long double b) { return a * b + 1.0L; }
EOF
construct float16 float16.c "" '`_Float16`' <<'EOF'
_Float16 f(_Float16 a) { return a + a; }
EOF
construct fp16 fp16.c "" '`__fp16`' <<'EOF'
__fp16 x;
float f(void) { return x; }
EOF
construct bf16 bf16.c "" '`__bf16`' <<'EOF'
__bf16 f(__bf16 a, long long b) { return a + b; }
EOF
construct float16struct float16struct.c "" 'a struct holding a `_Float16`, by value' <<'EOF'
struct h2 { _Float16 a, b; };
struct h2 f(struct h2 p) { p.a += p.b; return p; }
EOF
construct float16va float16va.c "" 'a `_Float16` through `...`' <<'EOF'
void g(int, ...);
void f(_Float16 h) { g(1, h); }
EOF
construct bitint bitint.c "" '`_BitInt(N)`' <<'EOF'
_BitInt(37) f(_BitInt(37) a) { return a * 3; }
EOF
construct complexint complexint.c "" '`_Complex int`' <<'EOF'
_Complex int f(_Complex int a, _Complex int b) { return a * b / a; }
EOF
construct vector vector.c "" 'GNU vector types (`vector_size`)' <<'EOF'
typedef int v4 __attribute__((vector_size(16)));
v4 f(v4 a, v4 b) { return a * b + (a == b); }
EOF
construct atomic1 atomic1.c "" 'one-byte atomic read-modify-write' <<'EOF'
unsigned char v;
unsigned char f(void) { return __atomic_fetch_add(&v, 1, __ATOMIC_SEQ_CST); }
EOF
construct atomic8 atomic8.c "" 'eight-byte atomic read-modify-write' <<'EOF'
long long v;
long long f(void) { return __atomic_fetch_add(&v, 1, __ATOMIC_SEQ_CST); }
EOF
construct atomic16 atomic16.c "" 'sixteen-byte compare-and-swap' <<'EOF'
_Alignas(16) __int128 v;
int f(__int128 a, __int128 b)
{ return __atomic_compare_exchange_n(&v, &a, b, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
EOF
construct tls tls.c "" '`_Thread_local`' <<'EOF'
_Thread_local int t;
int f(void) { return ++t; }
EOF
construct vla vla.c "" 'variable-length arrays' <<'EOF'
int f(int n) { volatile int a[n]; a[0] = n; return a[0]; }
EOF
construct align32 align32.c "" 'a local aligned to 32 bytes' <<'EOF'
int f(void) { _Alignas(32) int x = 1; return *(volatile int *)&x; }
EOF
construct frameaddr frameaddr.c "" '`__builtin_frame_address(0)`' <<'EOF'
void *f(void) { return __builtin_frame_address(0); }
EOF
construct retaddr1 retaddr1.c "" '`__builtin_return_address(1)`' <<'EOF'
void *f(void) { return __builtin_return_address(1); }
EOF
construct setjmp setjmp.c "" '`__builtin_setjmp`' <<'EOF'
void *b[5];
int f(void) { return __builtin_setjmp(b); }
EOF
construct cgoto cgoto.c "" 'computed `goto`' <<'EOF'
int f(int i)
{
    static void *t[] = { &&a, &&b };
    goto *t[i & 1];
a:  return 1;
b:  return 2;
}
EOF
construct asmgoto asmgoto.c "" '`asm goto`' <<'EOF'
int f(void) { asm goto("" :::: out); return 0; out: return 1; }
EOF
construct nested nested.c "" 'nested functions' <<'EOF'
int f(int x) { int g(int y) { return x + y; } return g(1); }
EOF
construct interrupt interrupt.c "" '`__attribute__((interrupt))`' <<'EOF'
__attribute__((interrupt)) void h(void) { }
EOF
construct naked naked.c "" '`__attribute__((naked))`' <<'EOF'
__attribute__((naked)) void h(void) { }
EOF
construct asmfile asm.s "" 'an assembly source (`.s`) with an instruction' <<'EOF'
	.text
	.globl f
f:
	nop
EOF
construct dashS plain.c "-S" '`-S` (assembly output)' <<'EOF'
int f(int a) { return a + 1; }
EOF
again debug plain.c "-g" '`-g` (DWARF)'
again unwind plain.c "--unwind-tables" '`--unwind-tables`'
construct cxx cxx.cc "--no-exceptions" 'C++ (without exceptions)' <<'EOF'
struct A { int x; A(int v) : x(v) {} virtual int get() const { return x; } };
int f(int v) { A a(v); return a.get() + 1; }
EOF
construct cxxeh cxxeh.cc "" 'C++ exceptions' <<'EOF'
void h();
int f() { try { h(); } catch (...) { return 1; } return 0; }
EOF

# the outcome: yes, warns: MESSAGE or no: MESSAGE, with the message as the
# compiler words it after `error:` or `warning:` (paths and lines dropped)
result() {
    t=$1; c=$2
    eval "f=\$file_$c; o=\$opts_$c"
    case $o in -S) obj=$out/x.s ;; *) obj=$out/x.o ;; esac
    rm -f "$obj"
    case $o in -S) mode= ;; *) mode=-c ;; esac
    # shellcheck disable=SC2086
    if msg=$("$EMBCC" --target="$t" -O2 $o $mode "$out/src/$f" -o "$obj" 2>&1) &&
       [ -s "$obj" ]; then
        # the Windows target warns on every compile that it is not yet
        # Microsoft's ABI; the page says that once, not in every row
        w=$(printf '%s\n' "$msg" | grep -v 'is not yet the Microsoft x64 ABI' |
            sed -n 's/.*warning: //p' | head -1)
        if [ -n "$w" ]; then echo "warns: $w"; else echo yes; fi
    else
        e=$(printf '%s\n' "$msg" | sed -n 's/.*error: //p' | head -1)
        echo "no: ${e:-$(printf '%s\n' "$msg" | head -1)}"
    fi
}

: > "$out/got.txt"
for t in $TARGETS; do
    for c in $CONSTRUCTS; do
        printf '%s %s %s\n' "$t" "$c" "$(result "$t" "$c")" >> "$out/got.txt"
    done
done

# The page: per construct, where it is refused and in what words, grouped
# by message; the matrix of yes and no for the rest is the table at the end.
render() {
    m=$1
    cat <<'EOF'
# What each target compiles

<!-- Generated from tests/golden/limits/expected.txt by
     `sh tests/golden/limits.sh --update`: do not edit by hand. -->

Each construct below was compiled for every target, and this page records
what EmbCC did: compiled it, accepted it with a warning, or refused it with
an error that names it. EmbCC never compiles a construct it refuses into
code that does something else (see
[Status and known limitations](../internals/status.md#how-embcc-refuses)).
A construct that compiles may still have limits on a target; each target's
section of [Targets](targets.md) gives them. `tests/golden/limits.sh` checks
every cell of this page against the compiler.

Where a message names its own target, it is written `TARGET` here.
Every compile for `x86_64-windows-gnu` also warns that the target is not
yet the Microsoft x64 ABI ([Windows](targets.md#windows-coff)).

EOF
    for c in $CONSTRUCTS; do
        eval "w=\$what_$c; o=\$opts_$c"
        n_no=$(awk -v c="$c" '$2 == c && $3 == "no:"' "$m" | wc -l | tr -d ' ')
        n_warn=$(awk -v c="$c" '$2 == c && $3 == "warns:"' "$m" | wc -l | tr -d ' ')
        echo "## $w"
        echo
        if [ "$n_no" = 0 ] && [ "$n_warn" = 0 ]; then
            echo "Compiles on every target."
            echo
            continue
        fi
        if [ "$n_no" = 0 ]; then
            echo "Compiles on every target; some warn."
        else
            echo "Refused on $n_no of the $(echo $TARGETS | wc -w | tr -d ' ') targets."
        fi
        echo
        for kind in no: warns:; do
            awk -v c="$c" -v k="$kind" '$2 == c && $3 == k {
                    msg = $0; sub(/^[^ ]+ [^ ]+ [^ ]+ /, "", msg)
                    gsub($1, "TARGET", msg)   # one entry for the same words
                    if (!(msg in seen)) { seen[msg] = ++n; order[n] = msg }
                    who[msg] = who[msg] (who[msg] == "" ? "" : ", ") "`" $1 "`" }
                END { for (i = 1; i <= n; i++) {
                        m = order[i]; gsub(/`/, "\x27", m)
                        printf "- %s on %s:\n\n  ```text\n  %s\n  ```\n\n",
                               (k == "no:" ? "Refused" : "A warning"), who[order[i]], m } }' "$m"
        done
    done
    echo "## The whole matrix"
    echo
    printf '| Target |'
    for c in $CONSTRUCTS; do printf ' %s |' "$c"; done
    echo
    printf '|---|'
    for c in $CONSTRUCTS; do printf -- '---|'; done
    echo
    for t in $TARGETS; do
        printf '| `%s` |' "$t"
        for c in $CONSTRUCTS; do
            r=$(awk -v t="$t" -v c="$c" '$1 == t && $2 == c { print $3 }' "$m")
            case $r in yes) printf ' yes |' ;; warns:) printf ' warns |' ;; *) printf ' no |' ;; esac
        done
        echo
    done
    echo
    echo "The columns are the sections above, in order:"
    for c in $CONSTRUCTS; do
        eval "w=\$what_$c"
        printf -- '- `%s`: %s\n' "$c" "$w"
    done
}

if [ $UPDATE = 1 ]; then
    cp "$out/got.txt" "$WANT"
    render "$WANT" > "$PAGE"
    echo "limits: wrote $WANT and $PAGE -- review the diff before committing"
    exit 0
fi

fail=0
if ! cmp -s "$WANT" "$out/got.txt"; then
    echo "FAIL: the compiler no longer does what $WANT says:"
    diff "$WANT" "$out/got.txt" | head -40
    fail=1
fi
render "$WANT" > "$out/page.md"
if ! cmp -s "$PAGE" "$out/page.md"; then
    echo "FAIL: $PAGE is not what $WANT renders:"
    diff "$PAGE" "$out/page.md" | head -20
    fail=1
fi
if [ $fail = 1 ]; then
    echo "If the change is meant: sh tests/golden/limits.sh --update, and update"
    echo "every other page of the manual that states the old behavior."
    exit 1
fi
echo "limits: $(wc -l < "$WANT" | tr -d ' ') cells ($(echo $CONSTRUCTS | wc -w | tr -d ' ') constructs on $(echo $TARGETS | wc -w | tr -d ' ') targets) as $WANT and $PAGE say"
