	.syntax unified
	.thumb
f:
	csel r1, r2, r3, eq
	csinc r1, r2, r3, ne
	csinv r4, r5, r6, gt
	csneg r7, r8, r9, lt
	cset r0, eq
	csetm r0, ne
	cinc r0, r1, mi
	cinv r2, r3, hs
	cneg r4, r5, mi
	csel r0, zr, r1, eq
	csel r10, r11, zr, le
.L1:
	nop
	le lr, .L1
	le .L1
	letp lr, .L1
	dls lr, r0
	dlstp.32 lr, r2
	dlstp.8 lr, r12
	wls lr, r1, .L2
	wlstp.16 lr, r3, .L2
	nop
	nop
.L2:
	lctp
	clrm {r0, r1, apsr}
	clrm {r1, r2, r12, lr}
	vscclrm {s0-s3, vpr}
	vscclrm {d0-d3, vpr}
	vscclrm {s17, vpr}
	vscclrm {d16-d17, vpr}
	vscclrm {vpr}
