#!/bin/sh
# The CONFIGURED default target: `embcc main.c` compiles for the board.
#
# A person whose work is one machine should not type --target= a hundred
# times a day, and a Makefile that inherited no environment should still
# build for the right machine. That is what `./configure --target=` buys
# a GCC cross build, and this is the same thing without a configure
# script: a compiled-in DEFAULT_TARGET, an EMBCC_DEFAULT_TARGET override
# for one shell, and --target= over both.
#
# Three things are worth a test rather than a reading of the code:
#
#  1. THE ORDER. Three sources of one setting is three chances to have
#     them backwards, and a compiler that quietly picks the wrong
#     machine produces objects that link and then do not run.
#  2. THE REFUSAL. A default that is not a target EmbCC knows must fail
#     by NAME and say WHICH source it came from -- "unknown target
#     'riscv32-elf-gnu'" with no mention of the environment sends a
#     person hunting through a Makefile for a --target= that is not
#     there (THE RULE: refuse loudly).
#  3. THE NAME. It is NOT `EMBCC_TARGET`. tests/lib.sh already uses that
#     name for which target the SUITE is exercising, and tests/run.sh
#     exports it for every golden test while passing --target=
#     explicitly. Had the driver honoured it, `make test-arm64` would
#     have retargeted every test that relies on the default, and the
#     failure would have looked like a miscompile. The last check here
#     holds that separation, because the collision is invisible until
#     someone "tidies up" the two names into one.
set -u
echo "TEST-MARKER default-target"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/default-target
rm -rf "$out"; mkdir -p "$out"
unset EMBCC_DEFAULT_TARGET        # whatever the person running has set

fail() { echo "$@"; exit 1; }

# `embcc -dumpmachine` prints the target it settled on, which is the
# whole of what is under test -- every other difference follows from it.
mach() { "$EMBCC" "$@" -dumpmachine 2>&1; }

# ---- 1. the order -----------------------------------------------------
got=$(mach)
[ "$got" = x86_64-elf ] ||
    fail "with nothing configured the default is '$got', not x86_64-elf"

got=$(EMBCC_DEFAULT_TARGET=riscv32-unknown-elf mach)
[ "$got" = riscv32-unknown-elf ] ||
    fail "EMBCC_DEFAULT_TARGET was ignored: got '$got'"

got=$(EMBCC_DEFAULT_TARGET=riscv32-unknown-elf mach --target=aarch64-elf)
[ "$got" = aarch64-elf ] ||
    fail "--target= did not override the environment: got '$got'"
echo "  --target= > EMBCC_DEFAULT_TARGET > x86_64-elf"

# ---- 2. the refusal ---------------------------------------------------
# In a subshell: `VAR=val func` leaves VAR set in the CALLING shell when
# the command is a function, so an unwrapped one here poisons every
# check below it (it did).
if ( EMBCC_DEFAULT_TARGET=riscv32-elf-gnu mach ) > "$out/bad.txt" 2>&1
then
    fail "a default of 'riscv32-elf-gnu' was accepted"
fi
grep -q "riscv32-elf-gnu" "$out/bad.txt" ||
    { echo "the refusal does not name the target:"; cat "$out/bad.txt"
      exit 1; }
grep -q "EMBCC_DEFAULT_TARGET" "$out/bad.txt" ||
    { echo "the refusal does not say where the name came from:"
      cat "$out/bad.txt"; exit 1; }
grep -q "riscv32-unknown-elf" "$out/bad.txt" ||
    { echo "the refusal does not list the targets that do exist:"
      cat "$out/bad.txt"; exit 1; }
echo "  a default that is not a target is refused by name, with its source"

# ---- 3. compiled in ---------------------------------------------------
# Built into its own object directory so this cannot disturb the ./embcc
# the rest of the suite is running (the same arrangement
# tests/golden/host-agnostic.sh uses).
b=$out/b
( cd "$EMBCC_ROOT" && make -s CC="${CC:-cc}" BUILD="$b" \
      DEFAULT_TARGET=riscv32-unknown-elf "$b/embcc" ) > "$out/build.log" 2>&1 ||
    { echo "a compiler with DEFAULT_TARGET= set did not build:"
      tail -5 "$out/build.log"; exit 1; }

got=$("$b/embcc" -dumpmachine 2>&1)
[ "$got" = riscv32-unknown-elf ] ||
    fail "DEFAULT_TARGET= did not reach the compiler: -dumpmachine says '$got'"

# And it must MEAN it: not a string in -dumpmachine but the machine the
# object is actually for, with no --target= anywhere on the line.
printf 'int add(int a, int b) { return a + b; }\n' > "$out/a.c"
"$b/embcc" -c "$out/a.c" -o "$out/a.o" ||
    fail "the compiled-in default did not compile a translation unit"
readelf -h "$out/a.o" > "$out/a.hdr" 2>&1 ||
    { echo "skipped the object check: no readelf"; : ; }
if [ -s "$out/a.hdr" ]; then
    grep -qi "RISC-V" "$out/a.hdr" ||
        { echo "the object is not RISC-V:"; grep -i machine "$out/a.hdr"
          exit 1; }
    grep -qi "ELF32" "$out/a.hdr" ||
        { echo "the object is not 32-bit:"; grep -i class "$out/a.hdr"
          exit 1; }
fi
echo "  DEFAULT_TARGET= produces a compiler that emits RV32 with no --target="

# The two overrides still reach a compiler that was built with a default.
got=$(EMBCC_DEFAULT_TARGET=thumbv7m-none-eabi "$b/embcc" -dumpmachine 2>&1)
[ "$got" = thumbv7m-none-eabi ] ||
    fail "the environment did not override the compiled-in default: '$got'"
got=$("$b/embcc" --target=x86_64-elf -dumpmachine 2>&1)
[ "$got" = x86_64-elf ] ||
    fail "--target= did not override the compiled-in default: '$got'"
echo "  and the environment and --target= still override it"

# A default reaches the same code as --target= naming it. The optimizer's
# view of which operations call a runtime helper was set only for a
# --target=, so a board compiler built with a default made different -O2
# code (tests/exec/va-copy.c at RV32) from itself told the board by name.
for t in riscv32-unknown-elf thumbv7em-none-eabi aarch64-elf; do
    a=$("$EMBCC" --target=$t -O2 -S -o - tests/exec/va-copy.c 2>&1)
    d=$(EMBCC_DEFAULT_TARGET=$t "$EMBCC" -O2 -S -o - tests/exec/va-copy.c 2>&1)
    [ "$a" = "$d" ] ||
        fail "-O2 code for $t differs between EMBCC_DEFAULT_TARGET and --target="
done
echo "  and a default makes the code --target= makes"

# ---- 4. the name that is already taken --------------------------------
# EMBCC_TARGET belongs to the test harness. tests/run.sh exports it right
# now, around this very script, so if the driver ever reads it the suite
# retargets itself.
got=$(EMBCC_TARGET=riscv64-unknown-elf mach)
[ "$got" = x86_64-elf ] ||
    fail "the driver read EMBCC_TARGET (got '$got') -- that name belongs to
       tests/lib.sh, and honouring it retargets the whole suite"
echo "  EMBCC_TARGET is the harness's, and the driver leaves it alone"
