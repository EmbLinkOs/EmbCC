#!/bin/sh
# ROADMAP M0: "a --version that prints something honest": what the compiler
# is, which machines it targets, and what is still missing — it must never
# claim more than the tree holds (docs/COMPATIBILITY.md is the long form).
set -u
echo "TEST-MARKER version"
. "$(dirname "$0")/../lib.sh"

out=$("$EMBCC" --version) || { echo "--version exited nonzero"; exit 1; }
echo "$out"

echo "$out" | grep -q "EmbCC"       || { echo "missing project name"; exit 1; }
echo "$out" | grep -q "target x86_64-elf" || { echo "missing the selected target"; exit 1; }
echo "$out" | grep -q "aarch64-elf" || { echo "does not list the aarch64 target"; exit 1; }
echo "$out" | grep -q "C11"         || { echo "does not name the language"; exit 1; }
echo "$out" | grep -qi "not yet"    || { echo "does not admit what is missing"; exit 1; }
a64=$("$EMBCC" --target=aarch64-elf --version) || { echo "--version exited nonzero"; exit 1; }
echo "$a64" | grep -q "target aarch64-elf" || {
    echo "--target=aarch64-elf --version does not name aarch64"; exit 1; }
