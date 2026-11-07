#!/usr/bin/env python3
"""Learn every group of groups.txt (learn.learn): a group's first example is
learned, the examples its opcode covers are removed, and what is left is
learned again, until nothing is: isa.json (README.md)."""
import json,sys,collections
sys.path.insert(0,'.')
from learn import learn
groups=collections.defaultdict(list)
for l in open('groups.txt'):
    mn,sh,ln,hx,ops=l.rstrip('\n').split('\t')
    groups[(mn,sh,int(ln))].append((hx.split(),ops))
table=[]
for rnd in range(12):
    batch=[]
    for k,v in groups.items():
        if v:
            batch.append({'mnem0':k[0],'shape':k[1],'hex':v[0][0],'exops':v[0][1],'grp':k})
    if not batch: break
    got=learn(batch)
    learned={id(s) for s in got}
    for s in batch:
        k=s['grp']
        v=groups[k]
        if id(s) not in learned or 'opbits' not in s:
            v.pop(0); continue
        mask=s['opbits']; want=s['ex']&mask
        keep=[]
        for hx,ops in v:
            w=int.from_bytes(bytes(int(b,16) for b in hx),'little')
            if (w&mask)!=want: keep.append((hx,ops))
        groups[k]=keep
        table.append(s)
    print('round',rnd,'learned',len(got),'left',sum(len(v) for v in groups.values()))
for s in table: s.pop('grp',None)
json.dump(table,open('isa.json','w'),indent=0)
print('entries',len(table))
