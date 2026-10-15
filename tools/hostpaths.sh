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

# Microchip's PIC32MZ EF device pack (Microchip.PIC32MZ-EF_DFP, unpacked):
# its ATDF files describe every register of the parts, which EmbSim's
# PIC32 boards are built from (tests/golden/embsim-pic32.sh).
PIC32MZ_EF_DFP=${EMBCC_PIC32MZ_EF_DFP:-${PIC32MZ_EF_DFP:-$HOME/EmbRef/dl/pic32mz-ef-dfp/x}}
# and its PIC24F GA/GB pack (Microchip.PIC24F-GA-GB_DFP, unpacked): the EDC
# files of the PIC24FJ1024GB610 and its family (tests/golden/embsim-pic24.sh).
PIC24F_GA_GB_DFP=${EMBCC_PIC24F_GA_GB_DFP:-${PIC24F_GA_GB_DFP:-$HOME/EmbRef/dl/pic24f-ga-gb-dfp/x}}
# and its PIC32MX pack (Microchip.PIC32MX_DFP, unpacked): the EDC files the
# PIC32MX boards are built from (the ATDFs place many registers wrongly),
# with the ATDFs for their interrupt vectors (tests/golden/embsim-pic32mx.sh).
PIC32MX_DFP=${EMBCC_PIC32MX_DFP:-${PIC32MX_DFP:-$HOME/EmbRef/dl/pic32mx-dfp/x}}
# and its dsPIC33E pack (Microchip.dsPIC33E-GM-GP-MC-GU-MU_DFP, unpacked):
# the EDC files of the dsPIC33EP parts (tests/golden/embsim-dspic33.sh).
DSPIC33E_DFP=${EMBCC_DSPIC33E_DFP:-${DSPIC33E_DFP:-$HOME/EmbRef/dl/dspic33e-dfp/x}}

# The Xtensa reference: Espressif's GCC told to generate for the de212 core
# QEMU emulates, through a configuration plugin built from that core's
# overlay (tools/xtensa-ref-gcc.sh makes both). XTENSA_REF_GCC is the
# compiler and XTENSA_REF_FLAGS the -mdynconfig that selects the core.
XTENSA_REF_DIR=${EMBCC_XTENSA_REF_DIR:-$HOME/EmbRef/xtensa-de212-gcc-16.1.0/xtensa-esp-elf}
XTENSA_REF_GCC=${EMBCC_REF_GCC_XTENSA:-$XTENSA_REF_DIR/bin/xtensa-esp-elf-gcc}
XTENSA_REF_FLAGS=${EMBCC_REF_GCC_XTENSA_FLAGS:--mdynconfig=$XTENSA_REF_DIR/lib/xtensa_de212.so}
