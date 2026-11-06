#!/usr/bin/env python3
"""Between two forms of one mnemonic, shape and length that both hold a
statement, the one GNU as takes: rank.json (README.md)."""
import json,subprocess,os,re,itertools,sys
sys.path.insert(0,'.')
from formsel import fit
from learn import disasm, norm_shape
T=json.load(open('isa3.json'))
AS=os.path.expanduser('~/cross/tricore/bin/tricore-elf-as')
by={}
for i,e in enumerate(T): by.setdefault((e['mnem'],e['shape'],e['len']),[]).append(i)
def fixed(e):
    # a value per field: the fixed one where it has no bits, else the smallest
    return [f['v0'] if not f['bits'] else None for f in e['fields']]
probes=[]
for k,ids in by.items():
    if len(ids)<2: continue
    for a,b in itertools.combinations(ids,2):
        ea,eb=T[a],T[b]
        fa,fb=fixed(ea),fixed(eb)
        vals=[]
        ok=True
        for x,y,f in zip(fa,fb,ea['fields']):
            if x is not None and y is not None and x!=y: ok=False
            v = x if x is not None else y
            if v is None:
                v = f['v0'] + (min(abs(bb[1]) for bb in f['bits']) if f['bits'] and not isinstance(f['v0'],str) else 0)
            vals.append(v)
        if not ok: continue
        wa,wb=fit(ea,vals),fit(eb,vals)
        if wa is None or wb is None: continue
        probes.append((a,b,wa,ea['len']))
words=[(T[a]['len'],w) for a,b,w,l in probes]
dis,pcs=disasm(words)
rank={}
for (a,b,w,ln),d in zip(probes,dis):
    if not d: continue
    open('/tmp/rk.s','w').write(' .text\n %s %s\n'%d)
    r=subprocess.run([AS,'-mtc162','/tmp/rk.s','-o','/tmp/rk.o'],capture_output=True,text=True)
    if r.returncode: continue
    subprocess.run([os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy'),'-O','binary','-j','.text','/tmp/rk.o','/tmp/rk.bin'])
    got=int.from_bytes(open('/tmp/rk.bin','rb').read()[:ln],'little')
    wa=fit(T[a],[None]*0) if False else None
    for c in (a,b):
        fm=T[c]['fieldmask']
        if (got & ~fm & ~T[c]['reserved'])==T[c]['base'] & ~T[c]['reserved']:
            rank.setdefault(c,0); rank[c]+=1
            other=b if c==a else a
            rank.setdefault(other,0); rank[other]-=1
            break
json.dump(rank,open('rank.json','w'))
print(len(probes),'probes;',sum(1 for v in rank.values() if v>0),'preferred entries')
