#!/bin/sh
# Memory in loops: the loads LICM hoists, the locations it promotes to a
# register, and -- the weight of this test -- the ones it must leave
# where they are (src/opt/opt.c, "memory in loops").
#
# Checked on the IR, because the question is WHERE an access is. A load
# hoisted past a store that can reach it computes a different answer
# only on the inputs where the pointers meet, and a load hoisted past a
# volatile read or an atomic computes a different answer only when an
# interrupt or another core runs at the wrong moment; neither is
# something a run reliably shows. tests/exec/licm-mem.c runs the cases
# that a run CAN show, on every harness and board.
#
# Every function below is one loop. `region F in PAT` counts the IR
# lines matching PAT between the label the loop's back edge goes to and
# that back edge, `out` the lines outside that span, `all` every line of
# F. (An exit's store can sit inside the span, after the branch that
# leaves, which is why p_exit is judged by its load and its store count.)
set -eu
echo "TEST-MARKER licm-mem"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/licm-mem
rm -rf "$out"; mkdir -p "$out"

cat > "$out/m.c" <<'EOF'
#define NOINL __attribute__((noinline))
struct node { struct node *next; int k; };
struct st { int len; int cap; int data[16]; };
int g_arr[64], g_scale, g_sum, g2, gseq, gdata;
int *gpa[4];
volatile int vreg;
extern void ext_fn(void);
NOINL static int square(int x) { return x * x; }   /* touches no memory */
NOINL static int peek(void) { return g2; }         /* reads, never writes */

/* ---- these move ---- */
/* a global nothing in the loop writes; the store is to another object */
void h_global(int n) { for (int i = 0; i < n; i++) g_arr[i] = g_scale * i; }
/* the bound, through the pointer, beside a load of another field */
int h_field(struct st *s)
{ int t = 0; for (int i = 0; i < s->len; i++) t += s->data[i]; return t; }
/* read under a condition, but a global in bounds cannot fault */
int h_cond_global(int n)
{ int t = 0; for (int i = 0; i < n; i++) if (i & 1) t += g_scale; return t; }
/* a call to a function proven to touch no memory */
int h_constcall(int n)
{ int t = 0; for (int i = 0; i < n; i++) t += square(i) + g_scale; return t; }
/* x->k is read by the rotated loop's guard before the loop is entered */
int h_guard(struct node *q, struct node *x)
{ int c = 0; while (q && q->k < x->k) { q = q->next; c++; } return c; }
/* promoted: a global sum */
void p_global(int n) { for (int i = 0; i < n; i++) g_sum += g_arr[i]; }
/* promoted, with an exit by return as well as the loop's own */
int p_exit(struct st *s, int n)
{ for (int i = 0; i < n; i++) { s->len += i; if (s->len > 100) return i; } return -1; }
/* promoted, stored and never read: only the last value goes out */
void p_last(int *p, int n) { for (int i = 0; i < n; i++) *p = i * 3; }

/* ---- these do not ---- */
/* *q can be *p */
int n_alias(int *p, int *q, int n)
{ int t = 0; for (int i = 0; i < n; i++) { t += *p; *q = i; } return t; }
/* a call that could write g_scale */
int n_call(int n)
{ int t = 0; for (int i = 0; i < n; i++) { t += g_scale; ext_fn(); } return t; }
/* volatile: read every time */
int n_vol(int n) { int t = 0; for (int i = 0; i < n; i++) t += vreg; return t; }
/* ...and nothing else moves across one either */
int n_volbar(int n)
{ int t = 0; for (int i = 0; i < n; i++) { t += g_scale; t ^= vreg; } return t; }
/* a seqlock's data read across an acquire load (no fence on x86-64) */
int n_seq(int n)
{ int t = 0;
  for (int i = 0; i < n; i++) { t += gdata; t ^= __atomic_load_n(&gseq, __ATOMIC_ACQUIRE); }
  return t; }
/* an atomic read-modify-write in the loop */
int n_atomic(int n)
{ int t = 0;
  for (int i = 0; i < n; i++) { t += g_scale; __atomic_fetch_add(&g2, 1, __ATOMIC_RELAXED); }
  return t; }
/* *p only when a flag says so: p may be null otherwise */
int n_fault(const int *p, const char *f, int n)
{ int t = 0; for (int i = 0; i < n; i++) if (f[i]) t += *p; return t; }
/* a callee that READS the global the loop increments */
int n_peek(int n) { int t = 0; for (int i = 0; i < n; i++) { g2++; t += peek(); } return t; }
/* the two locations are one */
void n_alias_store(int *c, int *o, int n)
{ for (int i = 0; i < n; i++) { *c += 1; *o = i; } }
/* the location's address escapes in the loop */
void n_escape(int n) { for (int i = 0; i < n; i++) { g2++; gpa[i & 3] = &g2; } }
/* the store does not happen on every way out */
void n_condstore(int *p, int n) { for (int i = 0; i < n; i++) if (i == 50) *p += 1; }
/* ...nor before the first way out: leaving at once must not store */
void n_storelate(int *p, int n) { for (int i = 0; i < n; i++) { if (i == 50) break; *p = i; } }
EOF

ir() {                          # ir TARGET [flags] -> $out/ir.txt
    t=$1; shift
    "$EMBCC" inspect ir --target=$t -O2 -fno-unroll "$@" -c "$out/m.c" \
        -o /dev/null > "$out/ir.txt" 2> "$out/err.txt" || {
        echo "FAIL: could not compile for $t:"; cat "$out/err.txt"; exit 1; }
}

# region F WHICH PAT: count PAT's lines in F's loop (in) or outside it (out)
region() {
    awk -v f="func @$1 " -v which="$2" -v pat="$3" '
    index($0, f) == 1 { on = 1; next }
    on && /^}/ { on = 0 }
    on { line[++n] = $0 }
    END {
        for (i = 1; i <= n; i++)
            if (line[i] ~ /^L[0-9]+:/) { l = line[i]; sub(/:.*/, "", l); pos[l] = i }
        lo = 0; hi = 0
        for (i = 1; i <= n; i++) {
            if (!match(line[i], /(-> |jmp )L[0-9]+/)) continue
            m = substr(line[i], RSTART, RLENGTH); sub(/.* /, "", m)
            if ((m in pos) && pos[m] < i) {
                if (!lo || pos[m] < lo) lo = pos[m]
                if (i > hi) hi = i
            }
        }
        if (!lo) { print "noloop"; exit }
        c = 0
        for (i = 1; i <= n; i++) {
            inl = i >= lo && i <= hi
            if (which != "all" && (which == "in") != inl) continue
            if (line[i] ~ pat) c++
        }
        print c
    }' "$out/ir.txt"
}

LOAD=' = load\.'
VLOAD=' = load\.[0-9]+:[0-9]+s?v '
PLOAD=' = load\.[0-9]+:[0-9]+s? '
STORE='^  store:'
fails=0
expect() {                      # expect F in|out PAT N WHY
    got=$(region "$1" "$2" "$3")
    if [ "$got" != "$4" ]; then
        echo "FAIL: $1: $got lines /$3/ ($2, the loop), want $4 -- $5"
        fails=1
    fi
}

for T in x86_64-linux-gnu thumbv7em-none-eabi riscv32-unknown-elf; do
    ir $T
    # what must move
    expect h_global      in "$LOAD"  0 "g_scale is invariant"
    expect h_field       in "$LOAD"  1 "s->len is invariant, s->data[i] is not"
    expect h_cond_global in "$LOAD"  0 "a global in bounds cannot fault"
    expect h_constcall   in "$LOAD"  0 "square() touches no memory"
    expect h_guard       in "$LOAD"  2 "x->k was loaded by the guard"
    expect p_global      in "$STORE" 0 "g_sum lives in a register"
    expect p_global      out "$STORE" 1 "...and is stored once after the loop"
    expect p_exit        in "$LOAD"  0 "s->len lives in a register"
    expect p_exit        all "$STORE" 2 "...stored on both ways out"
    expect p_last        in "$STORE" 0 "only the last store matters"
    # what must not
    expect n_alias       in "$LOAD"  1 "*q can be *p"
    expect n_call        in "$LOAD"  1 "ext_fn() can write g_scale"
    expect n_vol         in "$VLOAD" 1 "a volatile read happens every iteration"
    expect n_volbar      in "$PLOAD" 1 "nothing moves across a volatile access"
    expect n_seq         in "$PLOAD" 1 "nothing moves across an atomic load"
    expect n_atomic      in "$PLOAD" 1 "nothing moves across an atomic update"
    expect n_fault       in "$LOAD"  2 "*p may be null where no flag is set"
    expect n_peek        in "$STORE" 1 "peek() reads g2 every iteration"
    expect n_alias_store in "$STORE" 2 "*o is *c"
    expect n_escape      in "$STORE" 2 "g2's address escapes"
    expect n_condstore   in "$STORE" 1 "no store on most ways out"
    expect n_storelate   in "$STORE" 1 "no store on the way out by break"
    [ "$fails" = 0 ] || { echo "(IR for $T in $out/ir.txt)"; exit 1; }
    echo "$T: 8 loops lose a load or a store; 12 that must not, do not"
done

# and the flag turns it off
ir x86_64-linux-gnu -fno-licm-mem
expect h_global in "$LOAD" 1 "-fno-licm-mem leaves the load"
expect p_global in "$STORE" 1 "-fno-licm-mem leaves the store"
[ "$fails" = 0 ] || exit 1
echo "-fno-licm-mem: the loads and stores stay in the loop"

# the remarks name what happened
"$EMBCC" --target=x86_64-linux-gnu -O2 -fremarks -c "$out/m.c" -o /dev/null \
    2> "$out/remarks.txt" || { echo "FAIL: -fremarks"; exit 1; }
grep -q 'licm/load' "$out/remarks.txt" && grep -q 'licm/promote' "$out/remarks.txt" || {
    echo "FAIL: no licm/load or licm/promote remark:"; cat "$out/remarks.txt"; exit 1; }
echo "remarks: licm/load and licm/promote"
