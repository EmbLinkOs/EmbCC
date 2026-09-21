#!/bin/sh
# EmbCC's exception regions (the C statement C++'s lowering targets,
# __builtin_eh_region ... __builtin_eh_landing) against exceptions g++'s
# code throws: the landing pad gets the exception and the selector the
# personality routine chose from EmbCC's LSDA, catches by type through
# __cxa_begin_catch, lets a type it does not catch pass to g++'s catcher
# above, and a nested cleanup region runs and hands on to the enclosing
# handler (by goto: resuming inside the frame that handles it would find
# the same landing pad again). -O0 and -O2, both targets.
set -u
echo "TEST-MARKER eh-regions"
. "$(dirname "$0")/../lib.sh"

if [ "$ARCH" = aarch64 ]; then
    REF=$AARCH64_REF_GXX NL=$AARCH64_NEWLIB
else
    REF=$X86_REF_GXX NL=$X86_NEWLIB
fi
GXX=$REF/bin/$TARGET-g++
if [ ! -x "$GXX" ]; then
    echo "skipped: no reference g++ at $REF (tools/build-ref-gxx.sh $TARGET)"
    exit 0
fi
out=$EMBCC_ROOT/tests/golden/out/eh-regions-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/lp.c" << 'EOF'
/* EmbCC: exception regions, landing pads, selectors */
#include <string.h>
extern void *_ZTIi[];                   /* typeid(int) */
void *__cxa_begin_catch(void *);
void __cxa_end_catch(void);
void _Unwind_Resume(void *) __attribute__((noreturn));
extern void may_throw(int k);           /* g++: throws int k > 0, long k < 0 */
char trail[128];

int catch_int(int k)
{
    void *exc;
    long sel;
    int r = -1;
    __builtin_eh_region {
        may_throw(k);
        r = 0;
    } __builtin_eh_landing (exc, sel, _ZTIi) {
        if (sel == __builtin_eh_typeid(_ZTIi)) {
            int *p = __cxa_begin_catch(exc);
            r = *p;
            __cxa_end_catch();
        } else {
            _Unwind_Resume(exc);
        }
    }
    return r;
}

int nested(int k)
{
    void *exc;
    long sel;
    int r = -1;
    __builtin_eh_region {
        strcat(trail, "[");
        __builtin_eh_region {
            may_throw(k);
            r = 0;
        } __builtin_eh_landing (exc, sel, __eh_cleanup) {
            strcat(trail, "cleanup");
            goto outer;                 /* the enclosing region's pad */
        }
        strcat(trail, "]");
    } __builtin_eh_landing (exc, sel, _ZTIi) {
    outer:
        if (sel == __builtin_eh_typeid(_ZTIi)) {
            int *p = __cxa_begin_catch(exc);
            r = 100 + *p;
            __cxa_end_catch();
        } else {
            strcat(trail, "-resume");
            _Unwind_Resume(exc);
        }
    }
    return r;
}
EOF
cat > "$out/main.cc" << 'EOF'
#include <stdio.h>
extern "C" int catch_int(int), nested(int);
extern "C" char trail[];
extern "C" void may_throw(int k)
{
    if (k > 0) throw k;
    if (k < 0) throw (long)k;
}
int main()
{
    int ok = 1;
    ok &= catch_int(0) == 0 && catch_int(5) == 5;
    try { catch_int(-3); ok = 0; } catch (long v) { ok &= v == -3; }
    ok &= nested(0) == 0 && nested(7) == 107;
    try { nested(-2); ok = 0; } catch (long v) { ok &= v == -2; }
    printf("%s %d\n", trail, ok);
    return ok ? 42 : 1;
}
EOF
"$GXX" -std=c++20 -O2 -c "$out/main.cc" -o "$out/main.o" || {
    echo "g++ failed"; exit 1; }
for opt in -O0 -O2; do
    "$EMBCC" --target="$TARGET" -I"$NL/include" -funwind-tables $opt \
        -c "$out/lp.c" -o "$out/lp.o" || { echo "embcc failed ($opt)"; exit 1; }
    EMBCC_REF_GXX=$REF "$EMBCC_ROOT/tests/harness/$ARCH/link.sh" --cxx \
        -o "$out/prog" "$out/main.o" "$out/lp.o" || {
        echo "link failed ($opt)"; exit 1; }
    res=$(t_run "$out/prog"); st=$?
    [ "$st" -eq 42 ] && [ "$res" = "[][cleanup[cleanup-resume 1" ] || {
        echo "$opt: exit $st, output '$res'"; exit 1; }
done
echo "EmbCC's landing pads catch, clean up and pass on g++'s exceptions ($ARCH)"
