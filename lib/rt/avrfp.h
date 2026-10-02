/* IEEE-754 binary32 in software, for AVR: the shared vocabulary.
 *
 * lib/rt/avrfp.c is the core -- unpack, round_pack, add and subtract -- and
 * every other avrfp*.c is one group of routines in its own object so a program
 * links only what it calls. This header is what they share.
 *
 * The rule from rt.h holds in all of them: a routine may not use the
 * operation it implements. Values cross in and out as `float` because the ABI
 * requires it, and are converted to their BITS through `union fb` at once. A
 * single `a * b` anywhere here would be a call to __mulsf3.
 */
#ifndef EMBCC_RT_AVRFP_H
#define EMBCC_RT_AVRFP_H

typedef unsigned long      u32;
typedef long               s32;

union fb { float f; u32 u; };

static u32 f2u(float f) { union fb b; b.f = f; return b.u; }
static float u2f(u32 u) { union fb b; b.u = u; return b.f; }

#define EXPBITS  8
#define MANTBITS 23
#define BIAS     127
#define EXPMAX   255

/* The quiet NaN this file produces. Not "a" NaN: one value, so a result is
 * reproducible and comparable. */
#define QNAN 0x7fc00000ul

static int   fsign(u32 u) { return (int)(u >> 31); }
static int   fexp(u32 u)  { return (int)((u >> MANTBITS) & EXPMAX); }
static u32   fmant(u32 u) { return u & 0x7ffffful; }
static u32   pack(int s, int e, u32 m)
{
    return ((u32)(s & 1) << 31) | ((u32)(e & EXPMAX) << MANTBITS) |
           (m & 0x7ffffful);
}
static int is_nan(u32 u) { return fexp(u) == EXPMAX && fmant(u) != 0; }
static int is_inf(u32 u) { return fexp(u) == EXPMAX && fmant(u) == 0; }

/* These two are the only things here that are NOT static: they are real
 * functions in lib/rt/avrfp.c, called from the other objects. A static copy
 * per object would defeat the split -- round_pack is the largest routine in
 * the whole implementation after addsub, and five objects want it. */
void  __avrfp_unpack(u32 u, u32 *m, int *e);
float __avrfp_round(int s, u32 m, int e, int sticky);

#endif /* EMBCC_RT_AVRFP_H */
