#!/bin/sh
# verify/torture/fetch.sh [DIR] -- GCC's torture suite, at the commit the
# baselines here were made against.
#
# gcc/testsuite/gcc.c-torture of GCC's sources, from the GitHub mirror:
# sparse, and without the contents of anything else, so 54 MB rather than
# GCC's few gigabytes. DIR defaults to $EMBCC_TORTURE_DIR, else
# build/gcc-torture; verify/torture/run.sh reads the same place. Run again,
# it moves an existing clone to the pinned commit.
set -eu
COMMIT=70e0da089fe77227c7b9e257030e0d9674e7551a    # 2026-10-15
URL=https://github.com/gcc-mirror/gcc.git
DIR=${1:-${EMBCC_TORTURE_DIR:-build/gcc-torture}}

if [ ! -d "$DIR/.git" ]; then
    mkdir -p "$DIR"
    git -C "$DIR" init -q
    git -C "$DIR" remote add origin "$URL"
    git -C "$DIR" config remote.origin.promisor true
    git -C "$DIR" config remote.origin.partialclonefilter blob:none
    git -C "$DIR" sparse-checkout set gcc/testsuite/gcc.c-torture
fi
if [ "$(git -C "$DIR" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$COMMIT" ]; then
    git -C "$DIR" fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
    git -C "$DIR" checkout -q --detach "$COMMIT"
fi
n=$(find "$DIR/gcc/testsuite/gcc.c-torture" -name '*.c' | wc -l | tr -d ' ')
echo "fetch: GCC's torture suite at ${COMMIT%${COMMIT#????????????}} in $DIR ($n C files)"
