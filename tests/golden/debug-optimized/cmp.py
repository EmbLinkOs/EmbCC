#!/usr/bin/env python3
"""cmp.py REF.jsonl GOT.jsonl -- every value GOT's debug info gives must be REF's at the same
stop. A stop is named by its invocation -- which call of each function on its stack -- its line,
and which visit of that line within the innermost call. A line visited a different number of
times within one call in the two builds (a rotated loop's test) is not compared in that call.

A stop inside a call GOT inlined (gdb's inline frame) cannot be told apart from the next call
of the same function when no stop comes between: there the value must be one REF shows for that
variable at that line in some call made from the same caller invocation."""
import json, sys
from collections import Counter

def load(p):
    rows = [json.loads(l) for l in open(p)]
    out = {}; calls = {}; prev = []; visits = {}; path = []; groups = {}
    for r in rows:
        st = r.get("stack") or [r["key"].split(":")[0]]
        n = 0
        while n < len(prev) and n < len(st) and prev[n] == st[n] and n < len(path):
            n += 1
        path = path[:n]
        for d in range(n, len(st)):
            parent = tuple(path)
            c = calls.get((parent, st[d]), 0)
            calls[(parent, st[d])] = c + 1
            path.append((st[d], c))
        prev = st
        inv = tuple(path)
        vk = (inv, r["line"])
        v = visits.get(vk, 0); visits[vk] = v + 1
        r["_inv"] = inv
        out[(inv, r["line"], v)] = r
        # every level's caller: (the calls above it, the functions from it down, the line)
        for d in range(len(st)):
            g = groups.setdefault((tuple(path[:d]), tuple(st[d:]), r["line"]), {})
            for name, val in r["vals"].items():
                g.setdefault(name, set()).add(val)
    return out, visits, groups

a, va, ga = load(sys.argv[1]); b, vb, gb = load(sys.argv[2])
total = avail = agree = skipped = 0; bad = []
for key, j in b.items():
    inv, line, v = key
    flags = j.get("inl") or []
    first = next((d for d, x in enumerate(flags) if x), -1)
    if first >= 0:
        st = j.get("stack")
        refset = ga.get((tuple(inv[:first]), tuple(st[first:]), line))
        ref = None
        if refset is None:
            skipped += 1; continue
    else:
        if va.get((inv, line)) != vb.get((inv, line)):
            skipped += 1; continue
        ref = a.get(key)
        if ref is None:
            skipped += 1; continue
    for name, s in j["vals"].items():
        decl = (ref or {}).get("decl", {}).get(name) or j.get("decl", {}).get(name) or 0
        if decl and line <= decl:
            continue
        # where the reference's own gdb cannot read it (an older gdb, at
        # clang's epilogue), there is nothing to compare with
        if ref is not None and str(ref["vals"].get(name, "")).startswith("ERR"):
            continue
        total += 1
        if s is None: continue
        avail += 1
        if ref is None:
            ok = s in refset.get(name, set())
            r = "one of " + ",".join(sorted(str(x) for x in refset.get(name, set())))[:60]
        else:
            r = ref["vals"].get(name); ok = r == s
        if ok: agree += 1
        else: bad.append((j["key"], name, s, r))
print("%d stops, %d compared, %d not (visited differently within a call); %d values, %d available (%.0f%%), %d agree, %d DISAGREE"
      % (len(b), len(b) - skipped, skipped, total, avail, 100.0 * avail / total if total else 0, agree, len(bad)))
c = Counter((x[0], x[1]) for x in bad)
for (key, v), n in sorted(c.items()):
    ex = [x for x in bad if x[0] == key and x[1] == v][0]
    print("  %-14s %-6s x%-3d e.g. got %s, reference %s" % (key, v, n, ex[2], ex[3]))
