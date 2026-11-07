#!/usr/bin/env python3
"""isa2.json against objdump: random fillings of every form encoded with
the learned fields and decoded; each operand must read back (README.md)."""
import json,random,re,sys
sys.path.insert(0,'.')
from learn import disasm, atoms, atom_value
t=json.load(open('isa2.json'))
random.seed(5)
batch=[]; exp=[]
for e in t:
    for trial in range(6):
        w=e['base']; vals=[]
        for f in e['fields']:
            v=f['v0'] if not isinstance(f['v0'],str) else None
            for b,wt,*r in f['bits']:
                if random.random()<0.5:
                    w|=1<<b
                    if v is not None: v+=wt
            vals.append((f['kind'],v,f['target']))
        batch.append((e['len'],w)); exp.append((e,vals))
dis,pcs=disasm(batch)
bad=0; badent=set()
for (e,vals),d,pc in zip(exp,dis,pcs):
    ok = d is not None and d[0]==e['mnem']
    if ok:
        a=atoms(d[1])
        if len(a)!=len(vals): ok=False
        else:
            for (k,v,tg),aa in zip(vals,a):
                kk,vv=atom_value(aa,pc,tg)
                if v is None or isinstance(vv,str): continue
                if vv!=v: ok=False
    if not ok:
        bad+=1; badent.add((e['mnem'],e['shape']))
print("checked",len(exp),"bad",bad,"entries",len(badent))
for x in sorted(badent)[:20]: print("  ",x)
