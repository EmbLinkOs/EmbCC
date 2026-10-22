#!/bin/sh
# verify/torture/one.sh MODE TARGET OPT FILE -- one torture file, for run.sh.
#
#   compile: EmbCC compiles FILE for TARGET at OPT. Prints
#     ok        it compiled
#     refused   an error that names what EmbCC does not do
#     crash     a signal, an internal error, or no answer in 60 s: never
#               acceptable -- EmbCC compiles or refuses by name
#   execute: FILE is built for TARGET, this host, linked by the host's cc
#     and run. Prints pass, or why not: refused, crash, link, fail N,
#     timeout.
#
# One line, tab-separated: STATUS FILE DETAIL, with FILE relative to the
# suite's gcc.c-torture directory (TORTURE).
set -u
mode=$1 target=$2 opt=$3 f=$4
rel=${f#"$TORTURE"/}
work=$WORK/$(printf '%s' "$rel-$opt" | tr '/' '_')
limit() { perl -e '$t = shift; alarm $t; exec @ARGV or exit 127' "$@"; }
say() {   # the detail on one line: an error can span several
    printf '%s\t%s\t%s\n' "$1" "$rel" "$(printf '%s' "$2" | tr '\t\n' '  ')"
    rm -f "$work.o" "$work"; exit 0
}

# the file's own directory: some tests include their neighbours
dir=$(dirname "$f")
msg=$(cd "$dir" && limit 60 "$EMBCC" --target="$target" -w $opt -c "$f" -o "$work.o" 2>&1)
st=$?
first=$(printf '%s\n' "$msg" | grep -m1 -E 'error|internal|Assertion|fatal' |
        sed 's/^[^ ]*: [^ ]*:[0-9:]* //' | cut -c1-200)
if [ $st -eq 142 ]; then say crash "no answer in 60 s"; fi
if [ $st -ge 128 ] || printf '%s\n' "$msg" | grep -q -E 'internal (compiler )?error|Assertion'; then
    say crash "exit $st: ${first:-$(printf '%s' "$msg" | head -c 200)}"
fi
[ $st -ne 0 ] && say refused "$first"
[ "$mode" = compile ] && say ok ""

# execute: link with the host's cc, as tests/run.sh links its corpus
case $target in
    x86_64-elf) lflags="-no-pie -Wl,-z,noexecstack" ;;
    *) lflags= ;;
esac
cc $lflags -o "$work" "$work.o" -lm > "$work.lerr" 2>&1 ||
    { d=$(head -c 200 "$work.lerr"); rm -f "$work.lerr"; say link "$d"; }
rm -f "$work.lerr"
limit 20 "$work" > /dev/null 2>&1
st=$?
[ $st -eq 0 ] && say pass ""
[ $st -eq 142 ] && say timeout ""
say fail "exit $st"
