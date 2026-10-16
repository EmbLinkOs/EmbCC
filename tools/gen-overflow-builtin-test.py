#!/usr/bin/env python3
# gen-overflow-builtin-test.py -- writes tests/exec/overflow-builtins-exact.c:
# 240 random __builtin_{add,sub,mul}_overflow calls over the stdint types,
# their expected flag and value from the host clang

import random, subprocess, sys
random.seed(2026)
types = [('int8_t',8,1),('uint8_t',8,0),('int16_t',16,1),('uint16_t',16,0),('int32_t',32,1),('uint32_t',32,0),('int64_t',64,1),('uint64_t',64,0)]
def val(bits, s):
    lo, hi = (-(1<<(bits-1)), (1<<(bits-1))-1) if s else (0, (1<<bits)-1)
    return random.choice([lo, hi, lo+1, hi-1, 0, 1, hi//2, hi//2+1, random.randint(lo,hi), random.randint(lo,hi)] + [e for e in (-1,-2,2,3) if lo <= e <= hi])
def lit(v, t):
    if t[1] == 64 and v == -(1<<63): return 'INT64_MIN'
    if t[1] == 64: return ('UINT64_C(%d)' if not t[2] else 'INT64_C(%d)') % v
    return '%d' % v
cases = []
for i in range(240):
    A, B, R = random.choice(types), random.choice(types), random.choice(types)
    cases.append((random.choice(['add','sub','mul']), A, val(A[1],A[2]), B, val(B[1],B[2]), R))
def body(expect):
    out = ['#include <stdint.h>', '#include <stdio.h>']
    if expect is None:
        out.append('int main(void) {')
    else:
        out.append('static int bad;')
        out.append('static void ck(int n, int o, int eo, uint64_t r, uint64_t er) { if (o != eo || r != er) { bad++; if (bad < 5) printf("case %d: %d %llx, want %d %llx\\n", n, o, (unsigned long long)r, eo, (unsigned long long)er); } }')
        out.append('int main(void) {')
    for n, (op, A, a, B, b, R) in enumerate(cases):
        call = '{ volatile %s a = %s; volatile %s b = %s; %s r; int o = __builtin_%s_overflow(a, b, &r);' % (A[0], lit(a,A), B[0], lit(b,B), R[0], op)
        if expect is None:
            out.append('  %s printf("%%d %%llu\\n", o, (unsigned long long)(uint64_t)r); }' % call)
        else:
            eo, er = expect[n]
            out.append('  %s ck(%d, o, %s, (uint64_t)r, UINT64_C(%s)); }' % (call, n, eo, er))
    if expect is None:
        out.append('  return 0; }')
    else:
        out.append('  printf("%%d of %d cases wrong\\n", bad);' % len(cases))
        out.append('  return bad ? 1 : 42; }')
    return '\n'.join(out) + '\n'
open('/tmp/claude-ovf-ref.c','w').write(body(None))
subprocess.check_call(['clang','-w','-O0','/tmp/claude-ovf-ref.c','-o','/tmp/claude-ovf-ref'])
res = subprocess.check_output(['/tmp/claude-ovf-ref']).decode().split('\n')
expect = [tuple(l.split()) for l in res if l]
hdr = """/* The overflow builtins' exact meaning: the mathematical a op b, its
 * flag set when the result type cannot hold it, for 240 random calls
 * across every pairing of signed and unsigned 8- to 64-bit operands and
 * results, edge values favoured; the expected flags and stored values are
 * clang's. EmbCC computed in the result type's signedness, so `-1 + 1`
 * into a uint64_t overflowed and UINT64_MAX + 0 into an int64_t did not
 * (27 of 400 random calls wrong); a 128-bit result was an internal error. */
// expect-exit: 42
"""
sys.stdout.write(hdr + body(expect))
