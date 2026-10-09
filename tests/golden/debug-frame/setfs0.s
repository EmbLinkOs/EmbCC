# set_fs0(const float *p): fs0 = *p. Assembled by llvm-mc for the
# RISC-V run of tests/golden/debug-frame.sh: EmbCC's own inline
# assembler has no F instructions yet.
    .text
    .globl set_fs0
set_fs0:
    flw fs0, 0(a0)
    ret
