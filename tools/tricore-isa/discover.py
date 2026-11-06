#!/usr/bin/env python3
"""Find the mnemonics the sweeps never decoded, and examples of each.

The 16-bit sweep decodes every halfword, but the 32-bit space is only
sampled, and an instruction whose opcode is a long run of fixed bits (clz,
dsync, calli) can be missed. GNU as knows them all: every word in its
binary that it does not answer `Unknown instruction` to is a mnemonic.
Each new one is then offered every operand shape the table knows, with
random operands; what GNU assembles is disassembled into disc.dis, which
collect.py reads with the sweeps. Output: disc.dis, disc.mnemonics."""
import json, os, random, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import AS, OBJDUMP

T = json.load(open('isa3.json'))
have = {e['mnem'] for e in T}
words = subprocess.run(['strings', '-n', '2', AS], capture_output=True, text=True).stdout.split('\n')
cand = sorted({w for w in words if re.match(r'^[a-z][a-z0-9]*(\.[a-z0-9]+){0,3}$', w)} - have)

def assemble(lines):
    """the lines GNU refuses, by index, until the rest assemble"""
    keep = list(range(len(lines)))
    refused = {}
    for rnd in range(30):
        src = [' .text']
        for i in keep:
            src.append('L%d: %s' % (i, lines[i]))
        open('/tmp/disc.s', 'w').write('\n'.join(src) + '\n')
        r = subprocess.run([AS, '-mtc162', '/tmp/disc.s', '-o', '/tmp/disc.o'],
                           capture_output=True, text=True)
        bad = []
        for l in r.stderr.splitlines():
            m = re.match(r'/tmp/disc\.s:(\d+): (?:Error|Fatal error): (.*)', l)
            if m:
                bad.append(keep[int(m.group(1)) - 2])
                refused[keep[int(m.group(1)) - 2]] = m.group(2)
        if r.returncode == 0 and not bad:
            return keep, refused
        if not bad:
            # a fatal error with no line: drop half and retry
            keep = keep[:len(keep) // 2]
            continue
        keep = [i for i in keep if i not in set(bad)]
    return keep, refused

# 1. which are mnemonics
keep, refused = assemble(['%s %%d1' % c for c in cand])
mn = [c for i, c in enumerate(cand) if 'Unknown instruction' not in refused.get(i, '')]
open('disc.mnemonics', 'w').write('\n'.join(mn) + '\n')
# DISC_ALL: every mnemonic, for the forms of known ones the sweeps missed
OUTD = 'disc.dis'
if os.environ.get('DISC_ALL'):
    mn = sorted(have | set(mn))
    OUTD = 'disc2.dis'
print(len(cand), 'candidates,', len(mn), 'mnemonics new to the table:', ' '.join(mn))

# 2. every known shape, random operands
shapes = sorted({e['shape'] for e in T if '<' not in e['shape']})
random.seed(11)
NUMS = [0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 32, 63, 100, 255, 256, 1000, 4095,
        32767, 65535, -1, -2, -4, -8, -100, -512, -32768]
def inst(m, sh, i):
    out, k = [], 0
    branch = (m.startswith('j') or m.startswith('loop') or m in ('call', 'fcall')) and \
        m not in ('ja', 'jla', 'ji', 'jli')
    nN = len(re.findall(r'N', sh.replace('$S', '')))
    seenN = 0
    j = 0
    while j < len(sh):
        c = sh[j]
        if c == 'D':
            r = random.choice([random.randrange(16), 15])
            suf = ''
            while j + 1 < len(sh) and sh[j + 1] in 'lu':
                suf += sh[j + 1]; j += 1
            out.append('%%d%d%s' % (r, suf))
        elif c == 'A':
            out.append('%%a%d' % random.choice([random.randrange(16), 15, 10, 2 * random.randrange(8)]))
        elif c == 'E':
            out.append('%%e%d' % (2 * random.randrange(8)))
        elif c == 'P':
            out.append('%%p%d' % (2 * random.randrange(8)))
        elif c == '$' and sh[j:j + 2] == '$S':
            out.append(random.choice(['$psw', '$pcxi', '$0xfe2c', '$icr']))
            j += 1
        elif c == 'N':
            seenN += 1
            if branch and seenN == nN:
                out.append('L%d%+d' % (i, [4, 8, 30, -2, 200, 4000, 60000, 2][T_ % 8]))
            elif os.environ.get('DISC_ALL') and seenN == 1:
                out.append(str([0, 1, 2, 4, 15, 100, 300, 40000][T_ % 8]))
            else:
                out.append(str(random.choice(NUMS)))
        else:
            out.append(c)
        j += 1
    return '%s %s' % (m, ''.join(out))

T_ = 0
lines = []
for m in mn:
    for sh in shapes + ['']:
        for t in range(8 if os.environ.get('DISC_ALL') else 6):
            T_ = t
            lines.append(inst(m, sh, len(lines)))
# in chunks, in parallel: one fatal error ends GNU's whole file
import concurrent.futures, tempfile
def chunk(c):
    d = tempfile.mkdtemp(prefix='disc')
    ls = lines[c:c + 3000]
    keep = list(range(len(ls)))
    for rnd in range(60):
        src = [' .text'] + ['L%d: %s' % (c + i, ls[i]) for i in keep]
        open(d + '/a.s', 'w').write('\n'.join(src) + '\n')
        r = subprocess.run([AS, '-mtc162', d + '/a.s', '-o', d + '/a.o'], capture_output=True, text=True)
        bad = {keep[int(m.group(1)) - 2] for m in re.finditer(r'a\.s:(\d+): (?:Error|Fatal error)', r.stderr)
               if 2 <= int(m.group(1)) < len(keep) + 2}
        if r.returncode == 0 and not bad:
            out = subprocess.run([OBJDUMP, '-d', d + '/a.o'], capture_output=True, text=True).stdout
            return len(keep), out
        if not bad:
            keep = keep[:len(keep) // 2]
            continue
        keep = [i for i in keep if i not in bad]
    return 0, ''
total, d = 0, ''
with concurrent.futures.ThreadPoolExecutor(8) as ex:
    for n, out in ex.map(chunk, range(0, len(lines), 3000)):
        total += n
        d += out
print(len(lines), 'statements,', total, 'assembled')
open(OUTD, 'w').write(d)
got = set()
for l in d.splitlines():
    m = re.match(r'\s*[0-9a-f]+:\t([0-9a-f ]+)\t(\S+)', l)
    if m:
        got.add(m.group(2))
print('decoded mnemonics:', len(got & set(mn)), 'missing:', ' '.join(sorted(set(mn) - got)))
