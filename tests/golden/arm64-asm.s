// Every inline-asm template the EmbLinkOS aarch64 kernel uses, with its
// operands substituted as irgen would (x9, x10 for %0, %1). Collected by
// preprocessing each C file the ARM kernel build compiles; see
// src/asm/asm_arm64.h. The generated vocabulary lines are appended to these
// by tests/golden/arm64-asm.sh.
mrs x9, daif
msr daifset, #2
msr daifclr, #2
wfi
yield
msr daifclr, #2; wfi
msr daifset, #2; wfi
mrs x9, far_el1
mrs x9, tpidr_el1
dsb ishst
dsb sy
isb
mrs x9, mpidr_el1
dsb ish; isb
.inst 0xD500409F
.inst 0xD500419F
mrs x9, midr_el1
mrs x9, sctlr_el1
mrs x9, cntfrq_el0
mrs x9, ttbr0_el1
mrs x9, S3_0_C12_C12_5
msr S3_0_C12_C12_1, x9
msr S3_0_C12_C11_5, x9
msr cntv_cval_el0, x9
mrs x9, CurrentEL
brk #0
dsb sy; isb
msr S3_0_C12_C12_5, x9
msr S3_0_C4_C6_0, x9
msr S3_0_C12_C12_3, x9
msr S3_0_C12_C12_4, x9
msr S3_0_C12_C12_7, x9
mrs x9, S3_0_C12_C12_0
tlbi vmalle1
hvc #0
smc #0
dsb ish
isb; mrs x9, cntvct_el0
msr cntv_ctl_el0, x9
wfe
sev
msr tpidr_el0, x9
ldr q0, [x9]
str q0, [x9]
msr sctlr_el1, x9
mrs x9, pan
mrs x9, id_aa64mmfr1_el1
mrs x9, id_aa64pfr0_el1
mrs x9, id_aa64isar0_el1
mrs x9, S3_3_C2_C4_0
mrs x10, nzcv
mrs x9, cntvct_el0
mrs x9, cntpct_el0
msr tpidr_el1, x9
tlbi vaae1is, x9
tlbi vmalle1is
tlbi aside1is, x9
msr ttbr0_el1, x9
ldr q31, [sp, #496]
str x9, [x10, #8]
ldr w9, [x10, #4]
