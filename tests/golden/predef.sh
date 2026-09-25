#!/bin/sh
# The predefined-macro table (ARCHITECTURE.md §5). Golden test: when the
# reference compiler is available, EmbCC's table must match it exactly
# (minus the excluded __GNUC*/__STDC* families — see tools/gen-predef.sh).
# Without it, fall back to the macros whose absence made newlib's headers
# hard-#error under TCC.
set -u
echo "TEST-MARKER predef"
. "$(dirname "$0")/../lib.sh"

# Each target's table must equal its reference compiler's, filtered by the
# ONE exclusion list in tools/gen-predef.sh (asked for directly, not restated
# here, so the test and the generator cannot drift apart).
checked=0
for arch in x86_64 aarch64 thumb; do
    # The triple is not always "<arch>-elf": ARMv7-M is spelled the way
    # every other toolchain spells it, and gen-predef.sh keys on the short
    # name, so the two are named apart here rather than assumed equal.
    case $arch in thumb) triple=thumbv7m-none-eabi ;; *) triple=$arch-elf ;; esac
    out=$("$EMBCC" --target=$triple --dump-predef) || {
        echo "--target=$triple --dump-predef exited nonzero"; exit 1; }
    gcc=$(sh tools/gen-predef.sh --reference "$arch" 2>/dev/null >/dev/null && echo yes)
    if [ "$gcc" = yes ]; then
        ref=$(sh tools/gen-predef.sh --reference "$arch")
        if [ "$out" != "$ref" ]; then
            echo "$arch table disagrees with its reference gcc:"
            printf '%s\n' "$out" > "${TMPDIR:-/tmp}/predef.embcc.$$"
            printf '%s\n' "$ref" | diff -u - "${TMPDIR:-/tmp}/predef.embcc.$$"
            rm -f "${TMPDIR:-/tmp}/predef.embcc.$$"
            exit 1
        fi
        echo "$arch matches its reference gcc -dM -E ($(printf '%s\n' "$out" | wc -l | tr -d ' ') macros)"
        checked=$((checked + 1))
    else
        # No reference compiler: check the macros whose absence made newlib's
        # headers hard-#error under TCC (patch 0002), plus the arch's own.
        # __LP64__ is NOT in the shared list: ARMv7-M is ILP32 and must not
        # define it, so asking every target for it would demand the wrong
        # answer from one of them.
        case $arch in
            x86_64)  own="__x86_64__ __LP64__" ;;
            aarch64) own="__aarch64__ __LP64__" ;;
            thumb)   own="__arm__ __thumb__ __ARM_EABI__ __CHAR_UNSIGNED__" ;;
        esac
        for m in __INT64_TYPE__ __INTPTR_TYPE__ __SIZE_TYPE__ __PTRDIFF_TYPE__ \
                 __CHAR_BIT__ __SIZEOF_POINTER__ __SIZEOF_LONG__ \
                 __ELF__ $own; do
            echo "$out" | grep -q "^#define $m " || {
                echo "$arch: missing $m (the TCC-patch-0002 class of break)"
                exit 1
            }
        done
        n=$(printf '%s\n' "$out" | wc -l)
        [ "$n" -ge 300 ] || { echo "$arch: only $n macros — table looks truncated"; exit 1; }
        echo "$arch: no reference gcc; the known-fatal macros are present"
    fi
done
