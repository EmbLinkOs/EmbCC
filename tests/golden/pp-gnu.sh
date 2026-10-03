#!/bin/sh
# Preprocessor features real code uses that EmbCC refused or left as
# written: GNU comma elision and named variadic parameters, __COUNTER__,
# __DATE__, __TIME__, __FILE_NAME__, __BASE_FILE__, __INCLUDE_LEVEL__,
# `#include MACRO`, and more than 16 macro parameters (C requires 127).
set -u
echo "TEST-MARKER pp-gnu"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/pp-gnu
rm -rf "${out:?}"; mkdir -p "$out/inc"
printf '#define FROM_CFG 7\n' > "$out/inc/cfg.h"
cat > "$out/pp.c" <<'SRC'
#define CFG "inc/cfg.h"
#include CFG
#define SYS <stddef.h>
#include SYS
#define LOG(fmt, ...) printf(fmt, ## __VA_ARGS__)
#define LOG2(fmt, args...) printf(fmt , ## args)
#define NAMED(args...) f(args)
a: LOG("x");
b: LOG("x", 1, 2);
c: LOG2("y");
d: LOG2("y", 3);
e: NAMED(1, 2);
f: __COUNTER__ __COUNTER__ __COUNTER__
g: __FILE_NAME__ __INCLUDE_LEVEL__
#if defined(__COUNTER__) && defined(__DATE__) && defined(__TIME__) && defined(__BASE_FILE__)
h: yes
#endif
i: FROM_CFG
#define P18(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r) r
j: P18(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18)
k: __DATE__ __TIME__
SRC
SOURCE_DATE_EPOCH=86400 "$EMBCC" -E "$out/pp.c" > "$out/got.txt" 2> "$out/err.txt" || {
    echo "FAIL: -E failed:"; cat "$out/err.txt"; exit 1; }
# Compared without spaces: only the tokens matter.
grep -E '^[a-k]:' "$out/got.txt" | tr -d ' \t' > "$out/got.cmp"
cat > "$out/want.cmp" <<'WANT'
a:printf("x");
b:printf("x",1,2);
c:printf("y");
d:printf("y",3);
e:f(1,2);
f:012
g:"pp.c"0
h:yes
i:7
j:18
k:"Jan21970""00:00:00"
WANT
cmp -s "$out/want.cmp" "$out/got.cmp" || {
    echo "FAIL: the preprocessed text differs:"; diff "$out/want.cmp" "$out/got.cmp"; exit 1; }
echo "pp-gnu: comma elision, named variadics, the dynamic macros, #include MACRO and 18 parameters"
