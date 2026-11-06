#!/usr/bin/env python3
"""isa.json -> isa2.json: weights that are not powers of two dropped (their
bits are opcode bits), named SFR bits weighted by position, and each
form's base word and field mask (README.md)."""
import json,sys
t=json.load(open('isa.json'))
def p2(x): return x!=0 and (abs(x)&(abs(x)-1))==0
out=[]
for e in t:
    bad=False
    for f in e['fields']:
        keep=[]
        for b in f['bits']:
            if b[1] is None:
                keep.append(b); continue
            if p2(b[1]): keep.append(b)
            else: e['opbits'] |= 1<<b[0]
        f['bits']=keep
        # named SFR bits: weights from the run's position
        if f['kind']=='S' and f['bits']:
            lo=min(b[0] for b in f['bits'])
            for b in f['bits']:
                if b[1] is None: b[1]=1<<(b[0]-lo)
        # zero_is_ex: the value at all-bits-clear
        if e.get('zero_is_ex') and not isinstance(f['v0'],str):
            f['v0'] -= sum(w for bb,w,*r in f['bits'] if (e['ex']>>bb)&1 and w is not None)
    # the opcode value: the example's bits outside every field
    fm=0
    for f in e['fields']:
        for b in f['bits']: fm|=1<<b[0]
    e['fieldmask']=fm
    e['base']=e['ex'] & ~fm & ~e['reserved']
    out.append(e)
json.dump(out,open('isa2.json','w'),indent=0)
print(len(out))
