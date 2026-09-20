#!/bin/sh
# Stable symbol identity and interface hashes (vision §8.2, §21).
#
#     §21 Level 2: "a header edit that changes no interface hash used by a
#     TU does not rebuild that TU."
#
# `-MD` cannot say that. It knows a unit read a header, so a comment added
# to that header rebuilds it, and so does a declaration the unit never uses.
# On a tree the size of an operating system that is most of the rebuilds.
#
# The test is therefore two-sided, and both sides matter equally: an edit
# that changes nothing observable must change NO hash (or the feature buys
# nothing), and an edit that changes something observable must change one
# (or the feature is unsound and the build is wrong).
set -eu
echo "TEST-MARKER interfaces"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/interfaces
rm -rf "$out"; mkdir -p "$out"

write_header() { cat > "$out/h.h"; }

write_header << 'EOF'
struct Point { int x; int y; };
extern int compute(struct Point p, int scale);
extern int shared;
EOF
cat > "$out/a.c" << 'EOF'
#include "h.h"
int use(void) { struct Point p = {1, 2}; return compute(p, shared); }
EOF

iface() { "$EMBCC" --emit-interfaces -I"$out" "$out/a.c" | grep -v '^;'; }

iface > "$out/base.txt"
cat "$out/base.txt"

# What it says about itself, and about what it compiled against.
grep -q "^provides c:@F@use " "$out/base.txt" ||
    { echo "FAIL: does not report what it defines"; exit 1; }
grep -q "^uses     c:@F@compute " "$out/base.txt" ||
    { echo "FAIL: does not report the function it calls"; exit 1; }
grep -q "^uses     c:@V@shared " "$out/base.txt" ||
    { echo "FAIL: does not report the global it reads"; exit 1; }
grep -q "^uses     c:@S@Point " "$out/base.txt" ||
    { echo "FAIL: does not report the struct whose layout it compiled in"; exit 1; }
echo "a USR and a hash for what it provides, and for each interface it used"

# ---- the edits that must change NOTHING ------------------------------------
write_header << 'EOF'
/* A comment at the top. */
struct Point {
    int x;   /* the horizontal one */
    int y;
};
extern int shared;                     /* moved above compute */
extern int compute(struct Point p, int scale);
extern int unrelated_new_thing(long);  /* a.c never uses this */
EOF
iface > "$out/same.txt"
diff "$out/base.txt" "$out/same.txt" > "$out/d1.txt" ||
    { cat "$out/d1.txt"
      echo "FAIL: an edit that changes nothing observable moved a hash"; exit 1; }
echo "unchanged by: a comment, reformatting, reordering declarations, and
adding a declaration this unit does not use"

# A function's BODY is not its interface. A caller cannot observe it, and
# invalidating callers when a body changes is the rebuild storm this ends.
cat > "$out/b.c" << 'EOF'
#include "h.h"
int compute(struct Point p, int scale) { return p.x * scale; }
EOF
"$EMBCC" --emit-interfaces -I"$out" "$out/b.c" | grep "provides c:@F@compute" \
    > "$out/body1.txt"
cat > "$out/b.c" << 'EOF'
#include "h.h"
int compute(struct Point p, int scale)
{
    int t = p.x + p.y;          /* a completely different body */
    for (int i = 0; i < scale; i++) t += i;
    return t;
}
EOF
"$EMBCC" --emit-interfaces -I"$out" "$out/b.c" | grep "provides c:@F@compute" \
    > "$out/body2.txt"
cmp "$out/body1.txt" "$out/body2.txt" ||
    { echo "FAIL: rewriting a body changed its interface hash"; exit 1; }
echo "and unchanged by rewriting a function's body: a caller cannot see it"

# ---- the edits that MUST change something ----------------------------------
check_changes() {
    what=$1
    iface > "$out/now.txt"
    if diff -q "$out/base.txt" "$out/now.txt" > /dev/null; then
        echo "FAIL: $what did not change any hash — a build using these
              would not rebuild, and would be wrong"; exit 1
    fi
    echo "  changed by $what: $(diff "$out/base.txt" "$out/now.txt" |
        grep '^>' | sed 's/^> *uses  *//;s/  */ /' | head -1)"
}

write_header << 'EOF'
struct Point { int x; int y; int z; };
extern int compute(struct Point p, int scale);
extern int shared;
EOF
check_changes "adding a struct member"
# and ONLY that one: the function and the global were untouched.
grep -q "^uses     c:@F@compute .*$(sed -n 's/^uses     c:@F@compute  *//p' \
    "$out/base.txt")" "$out/now.txt" ||
    { echo "FAIL: an unrelated interface moved with it"; exit 1; }

write_header << 'EOF'
struct Point { int x; int y; };
extern int compute(struct Point p, long scale);
extern int shared;
EOF
check_changes "changing a parameter type"

write_header << 'EOF'
struct Point { int x; int y; };
extern int compute(struct Point p, int scale);
extern long shared;
EOF
check_changes "changing a global's type"

write_header << 'EOF'
struct Point { int y; int x; };
extern int compute(struct Point p, int scale);
extern int shared;
EOF
check_changes "reordering struct members (the layout differs)"

# ---- determinism (R4) ------------------------------------------------------
# A build cache is only safe if the hash is a pure function of the input.
write_header << 'EOF'
struct Point { int x; int y; };
extern int compute(struct Point p, int scale);
extern int shared;
EOF
iface > "$out/again.txt"
cmp "$out/base.txt" "$out/again.txt" ||
    { echo "FAIL: the same input gave a different hash"; exit 1; }
cp "$out/a.c" "$out/moved.c"
"$EMBCC" --emit-interfaces -I"$out" "$out/moved.c" | grep -v '^;' \
    | grep '^uses' > "$out/moved.txt"
grep '^uses' "$out/base.txt" > "$out/base-uses.txt"
cmp "$out/base-uses.txt" "$out/moved.txt" ||
    { echo "FAIL: the hashes depend on the source file's name"; exit 1; }
echo "deterministic: same input, same hashes, and independent of the
compiling file's name"
