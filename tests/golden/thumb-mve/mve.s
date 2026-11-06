	.syntax unified
	.thumb
	.globl	f
f:
	vldrw.u32	q0, [r0]
	vldrw.u32	q1, [r1, #16]!
	vldrw.u32	q2, [r2], #-32
	vldrb.u8	q3, [r3, #-7]
	vldrb.u32	q4, [r4, #5]
	vldrh.s32	q5, [r5, #-6]
	vstrw.32	q1, [r0, #16]
	vstrb.8		q4, [r1], #32
	vstrh.16	q2, [r2, #-2]!
	vldrw.u32	q6, [q1, #8]
	vstrw.32	q7, [q2, #-8]!
	vldrw.u32	q0, [r0, q1, uxtw #2]
	vstrb.32	q2, [r3, q4]
	vld20.32	{q0, q1}, [r0]
	vld21.32	{q0, q1}, [r0]!
	vst40.8		{q0, q1, q2, q3}, [r1]
	vadd.i32	q0, q1, q2
	vadd.i32	q0, q0, r3
	vadd.f32	q3, q4, q5
	vmul.i32	q1, q2, q3
	vmla.u32	q0, q1, r2
	vfma.f32	q0, q1, q2
	vaddv.u32	r0, q1
	vaddva.s32	r2, q7
	vmlav.s16	r4, q0, q1
	vmlaldav.s32	r0, r1, q2, q3
	vdup.32		q0, r1
	vdup.8		q2, r3
	vmov		q0, q1
	vmov.i32	q0, #0x0
	vmov.i8		q1, #0xff
	vmov.f32	q2, #1.000000e+00
	vmov.32		q0[2], r0
	vmov.u8		r1, q3[15]
	vmov		r0, r1, q2[3], q2[1]
	vmvn		q4, q5
	vand		q0, q1, q2
	vorr.i32	q0, #0x10000
	vbic.i16	q1, #0xff
	vshr.u32	q0, q1, #3
	vshl.i16	q2, q3, #4
	vshl.s32	q0, q1, q2
	vrev16.8	q0, q1
	vrev64.32	q2, q3
	vcmp.i32	eq, q0, r1
	vcmp.f32	ge, q2, q3
	vcmp.s8		lt, q1, zr
	vpsel		q0, q1, q2
	vmaxv.u8	r0, q1
	vminnmv.f32	r1, q2
	vcvt.f32.s32	q0, q1
	vcvt.s32.f32	q2, q3, #16
	vqadd.s8	q0, q1, q2
	vqdmulh.s32	q0, q1, r2
	vmullb.s16	q0, q1, q2
	vmovlt.u8	q0, q1
	vabs.f32	q0, q1
	vneg.s32	q2, q3
	vrintn.f32	q0, q1
	vctp.32		r0
	vpst
	vaddt.i32	q0, q1, q2
	vpstte
	vldrwt.u32	q0, [r0]
	vstrwt.32	q1, [r1]
	vadde.i32	q0, q0, q0
	vpt.i32		eq, q0, q1
	vaddt.i32	q2, q2, q2
	vptte.f32	ge, q2, r3
	vmult.f32	q0, q0, q0
	vmult.f32	q1, q1, q1
	vmule.f32	q2, q2, q2
	vpsteee
	vmovt		q0, q1
	vmove		q2, q3
	vmove		q4, q5
	vmove		q6, q7
	vmsr		p0, r3
	vmrs		r2, p0
	lsll		r0, r1, #8
	dlstp.32	lr, r2
.Lloop:
	vldrw.u32	q0, [r0], #16
	vstrw.32	q0, [r1], #16
	letp		lr, .Lloop
	bx		lr
