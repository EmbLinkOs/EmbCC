# Where this host keeps the trees EmbCC's tools and tests need. Sourced by
# tools/*.sh and tests/lib.sh — the ONE place the layout is known.
#
# Each path tries this machine's layout ($HOME/EmbLinkOs, $HOME/cross/...)
# first and the original Linux box's (/home/motsou/...) second, and each can
# be set outright from the environment. Before this file every script
# hard-coded the Linux paths, so on the Mac the tests skipped and the tools
# failed — and a skipped test is how a stale build manifest went unnoticed.

_hp_first() { for c in "$@"; do [ -e "$c" ] && { echo "$c"; return; }; done; echo "$1"; }

MYOS=${EMBCC_MYOS:-${MYOS:-$(_hp_first "$HOME/EmbLinkOs" /home/motsou/myos)}}
MYOS_BUILD=${EMBCC_MYOS_BUILD:-$MYOS/build}
X86_NEWLIB=${EMBCC_X86_NEWLIB:-$(_hp_first "$HOME/cross/newlib-c99/x86_64-elf" \
                                           /home/motsou/cross/newlib-c99/x86_64-elf)}
AARCH64_NEWLIB=${EMBCC_AARCH64_NEWLIB:-$(_hp_first "$HOME/cross/newlib-aarch64-c99/aarch64-elf" \
                                                   /home/motsou/cross/newlib-aarch64-c99/aarch64-elf)}

# The reference C++ toolchains (tools/build-ref-gxx.sh): g++ and a libstdc++
# built for each target over its newlib. EmbCC-compiled C++ links their
# libstdc++/libsupc++ until EmbCC compiles those itself (D-013), and their g++
# referees EmbCC's C++ the way the cross gcc referees its C.
X86_REF_GXX=${EMBCC_X86_REF_GXX:-$HOME/cross/gcc-cxx-x86_64-elf}
AARCH64_REF_GXX=${EMBCC_AARCH64_REF_GXX:-$HOME/cross/gcc-cxx-aarch64-elf}

# GNU binutils that understand x86-64 ELF: the host's own on Linux, the cross
# ones on a Mac (whose `objdump` is LLVM's and prints differently).
_hp_tool() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { command -v "$c"; return; }; done; echo "$1"; }
READELF=${READELF:-$(_hp_tool readelf x86_64-elf-readelf)}
OBJDUMP=${OBJDUMP:-$(_hp_tool x86_64-elf-objdump objdump)}
OBJCOPY=${OBJCOPY:-$(_hp_tool x86_64-elf-objcopy objcopy)}
