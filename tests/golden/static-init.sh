#!/bin/sh
# What must be initialized BEFORE any code runs, rather than by code.
#
# C++ divides namespace-scope initialization in two: a constant
# initializer becomes bytes in the object (with a relocation where it
# names an address), and everything else becomes a function the runtime
# calls through .init_array. The division is not cosmetic -- .init_array
# entries from different translation units run in an UNSPECIFIED order,
# so anything dynamically initialized has a window in which it is still
# zero, and anything statically initialized has no window at all.
#
# EmbCC put a reference bound to a constant address on the wrong side of
# that line. `ostream &cerr = *reinterpret_cast<ostream *>(__cerr_store);`
# is an address known at compile time, but because the initializer is an
# LVALUE cast, c_const answered "not constant" and the assignment went
# into .init_array. Every translation unit including <iostream> has its
# own __ios_init whose constructor writes to cerr, so a unit whose entry
# ran first found cerr null and stored through it.
#
# That crash was invisible for a second reason, fixed with it: the
# bare-metal test harness identity-mapped from address zero, so the
# store SUCCEEDED. tests/harness/x86_64/start.S now leaves the first
# page absent and the aarch64 one leaves the low 1 GiB unmapped, so a
# null dereference faults and says so.
set -eu
echo "TEST-MARKER static-init"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/static-init
rm -rf "$out"; mkdir -p "$out"

case "$ARCH" in
    x86_64)  NM=x86_64-elf-nm ;;
    aarch64) NM=aarch64-elf-nm ;;
    *) echo "skipped: no binutils for $ARCH"; exit 0 ;;
esac
command -v "$NM" > /dev/null 2>&1 || { echo "skipped: no $NM"; exit 0; }

# ---- 1. no .init_array at all for constant initializers -------------------
#
# The strongest statement of the property: a translation unit whose every
# initializer is a constant address should emit NO dynamic initializer.
cat > "$out/a.cc" << 'EOF'
struct T { int x; };
static unsigned char store[sizeof(T)];
static T obj;

T &ref_cast  = *reinterpret_cast<T *>(store);   /* the case that failed */
T &ref_plain = obj;
T *ptr       = reinterpret_cast<T *>(store);
T &ref_off   = *reinterpret_cast<T *>(store + 0);
int &member  = obj.x;
EOF
"$EMBCC" --target="$TARGET" -x c++ -c "$out/a.cc" -o "$out/a.o" \
    2> "$out/a.log" || { echo "FAIL: did not compile:"; cat "$out/a.log"
                         exit 1; }

# __cx_global_init is the function EmbCC emits to hold dynamic
# initializers; its absence from the symbol table is the property.
if "$NM" "$out/a.o" | grep -q "__cx_global_init"; then
    echo "FAIL: a unit whose initializers are all constant addresses"
    echo "      still emitted a dynamic initializer. Each of these has"
    echo "      a window in which it reads as null:"
    "$EMBCC" --target="$TARGET" -x c++ --emit-c "$out/a.cc" -o "$out/a.c" \
        2>/dev/null
    sed -n '/__cx_global_init/,/^}/p' "$out/a.c"
    exit 1
fi

# ... and the references really hold the addresses, in the object, as
# relocations -- not zero to be filled in later. A reference lowers to a
# pointer, so each is a pointer-sized slot with a relocation against the
# object it names.
for sym in ref_cast ref_plain ptr ref_off member; do
    "$NM" "$out/a.o" | grep -q " $sym\$" || {
        echo "FAIL: no symbol '$sym' in the object"; exit 1; }
done
# A dynamically-initialized pointer sits in .bss (lower-case b); a
# statically-initialized one sits in .data (d) with a relocation.
for sym in ref_cast ptr ref_off; do
    kind=$("$NM" "$out/a.o" | sed -n "s/^[0-9a-f]* \(.\) $sym\$/\1/p")
    case "$kind" in
        [dD]) ;;
        [bB]) echo "FAIL: '$sym' is in .bss, so it is zero until some"
              echo "      .init_array entry runs -- and the order those"
              echo "      run in, across translation units, is not"
              echo "      specified"; exit 1 ;;
        *)    echo "FAIL: '$sym' is in an unexpected section ('$kind')"
              exit 1 ;;
    esac
done
echo "constant-address references are initialized in the object, not by code"

# ---- 2. a static that is NAMED before it is defined -----------------------
#
# Making those initializers static exposed an ordering bug of its own:
# the definitions are emitted by a worklist, so a static object was
# written only once something asked for it, which is after the thing
# that asked. The C that came out named it before declaring it.
cat > "$out/b.cc" << 'EOF'
struct alignas(64) Big { char c; };
alignas(Big) static unsigned char store[sizeof(Big)];
alignas(128) static unsigned char more[4];
unsigned char *p1 = store;              /* names `store` before it appears */
unsigned char *p2 = more;
EOF
"$EMBCC" --target="$TARGET" -x c++ -c "$out/b.cc" -o "$out/b.o" \
    2> "$out/b.log" || {
    echo "FAIL: a static object named before its definition:"
    cat "$out/b.log"; exit 1; }

# And `alignas` survived: alignas(Big) is 64 because Big says so, and
# the explicit 128 is 128. Both are static locals of the object, so the
# check is where they landed.
a1=$("$NM" -S "$out/b.o" | sed -n 's/^\([0-9a-f]*\) [0-9a-f]* . store$/\1/p')
a2=$("$NM" -S "$out/b.o" | sed -n 's/^\([0-9a-f]*\) [0-9a-f]* . more$/\1/p')
[ -n "$a1" ] && [ -n "$a2" ] || {
    echo "FAIL: store/more missing from the object"; exit 1; }
[ $((0x$a1 % 64)) -eq 0 ] || {
    echo "FAIL: alignas(Big) asked for 64 and 'store' is at 0x$a1"; exit 1; }
[ $((0x$a2 % 128)) -eq 0 ] || {
    echo "FAIL: alignas(128) and 'more' is at 0x$a2"; exit 1; }
echo "a static object can be named before it is defined, keeping its alignas"

# ---- 3. and the harness can SEE a null dereference ------------------------
#
# The whole reason the cerr bug survived. If this exits 42 the harness
# has mapped page zero again, and every null dereference in tests/exec
# and tests/cxx is invisible.
H=$EMBCC_ROOT/tests/harness/$ARCH
[ -f "$H/link.sh" ] || { echo "skipped the fault half: no $ARCH harness"
                         exit 0; }
cat > "$out/null.c" << 'EOF'
volatile int *p = 0;
int main(void) { *p = 0x1234; return *p == 0x1234 ? 42 : 7; }
EOF
"$EMBCC" --target="$TARGET" -c "$out/null.c" -o "$out/null.o"
"$H/link.sh" -o "$out/null.elf" "$out/null.o" > "$out/link.log" 2>&1 || {
    echo "skipped the fault half: could not link ($(tail -1 "$out/link.log"))"
    exit 0; }
set +e
"$H/run.sh" "$out/null.elf" > "$out/null.txt" 2>&1
rc=$?
set -e
case "$rc" in
    125) echo "and the harness faults on a null dereference, so a test can"
         echo "see one" ;;
    42)  echo "FAIL: writing through a null pointer SUCCEEDED and read"
         echo "      back. The harness has mapped page zero, which makes"
         echo "      every null dereference in tests/exec and tests/cxx"
         echo "      invisible -- it is how the std::cerr bug above lived"
         exit 1 ;;
    124) echo "FAIL: the null dereference hung instead of reporting."
         echo "      A fault with no vector table spins; the handler is"
         echo "      what turns that into a legible failure"; exit 1 ;;
    *)   echo "FAIL: a null dereference exited $rc, wanted 125:"
         cat "$out/null.txt"; exit 1 ;;
esac
