#!/usr/bin/env python3
"""The sweeps' decodes grouped by (mnemonic, operand shape, length), up to
400 examples each: groups.txt, the learner's input (README.md)."""
import re,sys,collections
sys.path.insert(0,'.')
from learn import norm_shape
groups=collections.defaultdict(list)
for f in ['h16.dis','w32.dis','w32b.dis','w32c.dis','disc.dis','disc2.dis']:
    for l in open(f):
        m=re.match(r'\s*[0-9a-f]+:\t([0-9a-f ]+)\t(\S+)\s*(.*)$',l.rstrip())
        if not m or m.group(2).startswith('.'): continue
        ops=m.group(3).strip()
        # (a probe file's labels: objdump names them, <L123+N>)
        if '<L' in ops: continue
        if re.search(r'%e(1[0-5]|[13579])\b',ops) or '(unknown SFR)' in ops and False: pass
        if re.search(r'%e(1[1-5]|[13579])\b',ops): continue
        hx=m.group(1).split()
        k=(m.group(2),norm_shape(ops),len(hx))
        if len(groups[k])<400: groups[k].append((m.group(1).strip(),ops))
with open('groups.txt','w') as o:
    for (mn,sh,ln),v in sorted(groups.items()):
        for h,ops in v: o.write('%s\t%s\t%d\t%s\t%s\n'%(mn,sh,ln,h,ops))
print(len(groups),'groups', sum(len(v) for v in groups.values()),'examples')
