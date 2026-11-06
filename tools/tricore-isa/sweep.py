#!/usr/bin/env python3
"""The sweeps: what GNU objdump decodes, as the learner's first examples.

    h16.dis   every 16-bit word (bit 0 clear: a 16-bit instruction)
    w32.dis   512,000 random 32-bit words (bit 0 set), seed 1
    w32b.dis  6,400,000 more, seed 2
    w32c.dis  8,388,608 more, seed 3

Each is a .s of .short/.word directives, assembled and disassembled.
collect.py reads them (with discover.py's disc.dis and disc2.dis). They
are large -- the last two are hundreds of megabytes -- and only examples:
the learner needs one decode of each form to start from."""
import os, random, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import AS, OBJDUMP

def sweep(name, words, short):
    with open(name + '.s', 'w') as f:
        f.write(' .text\n')
        for w in words:
            f.write((' .short 0x%04x\n' if short else ' .word 0x%08x\n') % w)
    subprocess.run([AS, '-mtc162', name + '.s', '-o', name + '.o'], check=True)
    with open(name + '.dis', 'w') as f:
        subprocess.run([OBJDUMP, '-d', name + '.o'], stdout=f, check=True)
    print(name, len(words))

only = sys.argv[1:]
if not only or 'h16' in only:
    sweep('h16', range(0, 0x10000, 2), True)
for name, n, seed in (('w32', 512000, 1), ('w32b', 6400000, 2), ('w32c', 8388608, 3)):
    if only and name not in only:
        continue
    r = random.Random(seed)
    sweep(name, (r.getrandbits(32) | 1 for _ in range(n)), False)
