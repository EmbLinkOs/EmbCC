#!/usr/bin/env python3
"""cmp.py REF.jsonl GOT.jsonl [-v] -- every value GOT's debug info gives must
be the unoptimized build's at the same visit of the same line. Prints stops, how many values
the optimized build makes available, and every disagreement."""
import json, sys
def load(p):
    r = {}
    for l in open(p):
        j = json.loads(l); r[(j["key"], j["k"])] = j
    return r
a, b = load(sys.argv[1]), load(sys.argv[2])
verbose = "-v" in sys.argv
cnt0 = {}; cnt2 = {}
for (k, n) in a: cnt0[k] = cnt0.get(k, 0) + 1
for (k, n) in b: cnt2[k] = cnt2.get(k, 0) + 1
avail = total = agree = 0; bad = []; unaligned = set()
for key, j in sorted(b.items(), key=lambda kv: kv[1]["pc"]):
    line = key[0]
    if cnt0.get(line) != cnt2.get(line):
        unaligned.add(line); continue
    ref = a.get(key)
    if not ref: continue
    for v, s in j["vals"].items():
        # up to its declaration's line a local holds nothing yet: its block
        # is its scope in the DWARF, but C's starts at the declarator
        d = ref.get("decl", {}).get(v) or j.get("decl", {}).get(v) or 0
        if d and j.get("line", 0) <= d:
            continue
        total += 1
        if s is None: continue
        avail += 1
        r = ref["vals"].get(v)
        if r == s: agree += 1
        else: bad.append((key[0], key[1], v, s, r))
print("%d stops at -O2, %d aligned with -O0; %d values, %d available (%.0f%%), %d agree, %d DISAGREE; %d lines visited a different number of times"
      % (len(b), sum(1 for k in b if k[0] not in unaligned), total, avail, 100.0 * avail / total if total else 0, agree, len(bad), len(unaligned)))
from collections import Counter
c = Counter((x[0], x[2]) for x in bad)
for (line, v), n in sorted(c.items()):
    ex = [x for x in bad if x[0] == line and x[2] == v][0]
    print("  %-14s %-6s x%-3d e.g. visit %d: -O2 says %s, -O0 has %s" % (line, v, n, ex[1], ex[3], ex[4]))
if verbose: print("unaligned:", sorted(unaligned))
