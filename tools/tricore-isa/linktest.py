#!/usr/bin/env python3
"""EmbLD's TriCore relocations against GNU ld.

Every symbol statement of relocs.json GNU as takes is assembled (by GNU
as, so only the linkers differ) with its symbol given a value its field
holds -- 1 for a constant, an address for hi:/lo:/up: and an absolute
field, the small-data base plus 4 for sm:, a label of another section
for a branch -- and the object is linked by tricore-elf-ld and by embld
with one script. The two images' .text must be the same bytes.

    linktest.py EMBLD"""
import json, os, re, subprocess, sys
AS = os.path.expanduser('~/cross/tricore/bin/tricore-elf-as')
LD = os.path.expanduser('~/cross/tricore/bin/tricore-elf-ld')
OC = os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy')
OD = os.path.expanduser('~/cross/tricore/bin/tricore-elf-objdump')
EMBLD = sys.argv[1]
R = json.load(open('relocs.json'))
SDA = 0xd0008000
stmts, seen = [], set()
for o in R:
    if 'refused' in o or o['text'] in seen:
        continue
    seen.add(o['text'])
    types = [r[1] for r in o['relocs'] if r[1] != 'R_TRICORE_RELAX']
    if not types:
        continue
    t = types[0]
    i = len(stmts)
    # (a bare number as a branch target: GNU relocates it, EmbCC asks for
    # a label -- not a form the two share)
    if os.environ.get('LT_AS') and re.match(r'^(j|loop)', o['mnem']) and \
            re.search(r',(0x[0-9a-f]+|[0-9]+)$', o['text']):
        continue
    if t in ('R_TRICORE_24REL', 'R_TRICORE_15REL'):
        txt = o['text'].replace('sym', 'oth')
        val = None
    elif o['op'] == 'oth':
        continue
    elif o['op'] in ('hi:sym', 'lo:sym', 'up:sym'):
        txt, val = o['text'].replace('sym', 's%d' % i), 0xd0001234
    elif o['op'] == 'sm:sym':
        # only from a0: the base GNU ld's default script gives
        if not re.search(r'\[%a0[\]+]', o['text']):
            continue
        txt, val = o['text'].replace('sym', 's%d' % i), 'sdata'
    elif t == 'R_TRICORE_18ABS_14':
        txt, val = o['text'].replace('sym', 's%d' % i), 0xd0004000
    elif t in ('R_TRICORE_18ABS', 'R_TRICORE_24ABS'):
        txt, val = o['text'].replace('sym', 's%d' % i), 0xd0000010
    else:
        txt, val = o['text'].replace('sym', 's%d' % i), 1
    stmts.append((txt, val, t))

sd, absy = [], []
src = [' .section .text.oth,"ax",@progbits', ' .globl oth', 'oth: nop', ' .text']
for i, (txt, val, t) in enumerate(stmts):
    src.append('S%d: %s' % (i, txt))
    if val == 'sdata':
        sd.append(' .globl s%d' % i)
        sd.append('s%d: .word 0' % i)
    elif val is not None:
        absy.append(' .globl s%d' % i)
        absy.append(' .set s%d, 0x%x' % (i, val))
src.append('Send: nop')
src += [' .section .sdata,"aw",@progbits'] + sd
open('/tmp/lt.s', 'w').write('\n'.join(src) + '\n')
# (the absolute symbols in an object of their own: in the same file a
# .set constant is a number to an assembler, not a symbol to relocate)
open('/tmp/lt_abs.s', 'w').write('\n'.join(absy) + '\n')
subprocess.run([AS, '-mtc162', '/tmp/lt_abs.s', '-o', '/tmp/lt_abs.o'], check=True)
# LT_AS=EMBCC: EmbCC assembles the object instead (both linkers still
# link the same one; the bytes are then also compared with GNU's image)
r = subprocess.run([AS, '-mtc162', '/tmp/lt.s', '-o', '/tmp/lt.o'], capture_output=True, text=True)
if os.environ.get('LT_AS') and r.returncode == 0:
    subprocess.run(['cp', '/tmp/lt.o', '/tmp/lt_gas.o'])
    r = subprocess.run([os.environ['LT_AS'], '--target=tricore-none-elf', '-c', '/tmp/lt.s', '-o', '/tmp/lt.o'],
                       capture_output=True, text=True)
if r.returncode:
    print(r.stderr[:2000]); sys.exit(1)
open('/tmp/lt.ld', 'w').write('''ENTRY(oth)
SECTIONS {
  . = 0x80000000;
  .text : { *(.text) }
  .text.oth : { *(.text.oth) }
  . = 0xd0000000;
  .sdata : { *(.sdata) }
  _SMALL_DATA_ = ADDR(.sdata) + 0x100;
  .data : { *(.data) }
}
''')
g = subprocess.run([LD, '--mcpu=tc162', '-T', '/tmp/lt.ld', '/tmp/lt.o', '/tmp/lt_abs.o', '-o', '/tmp/lt_g.elf'], capture_output=True, text=True)
if g.returncode:
    print('GNU ld:', g.stderr[:1500]); sys.exit(1)
e = subprocess.run([EMBLD, '-T', '/tmp/lt.ld', '/tmp/lt.o', '/tmp/lt_abs.o', '-o', '/tmp/lt_e.elf'], capture_output=True, text=True)
if e.returncode:
    print('embld:', e.stderr[:1500]); sys.exit(1)
def text(f):
    subprocess.run([OC, '-O', 'binary', '-j', '.text', f, f + '.bin'], check=True)
    return open(f + '.bin', 'rb').read()
gb, eb = text('/tmp/lt_g.elf'), text('/tmp/lt_e.elf')
if os.environ.get('LT_AS'):
    subprocess.run([LD, '--mcpu=tc162', '-T', '/tmp/lt.ld', '/tmp/lt_gas.o', '/tmp/lt_abs.o', '-o', '/tmp/lt_gg.elf'],
                   capture_output=True)
    print('EmbCC as + GNU ld %s GNU as + GNU ld' % ('==' if text('/tmp/lt_gg.elf') == gb else '!='))
if gb == eb:
    print('%d statements, %d bytes of .text: the same' % (len(stmts), len(gb)))
    sys.exit(0)
gd = subprocess.run([OD, '-d', '/tmp/lt_g.elf'], capture_output=True, text=True).stdout.splitlines()
ed = subprocess.run([OD, '-d', '/tmp/lt_e.elf'], capture_output=True, text=True).stdout.splitlines()
n = 0
for a, b in zip(gd, ed):
    if a != b:
        n += 1
        if n <= 30:
            print('GNU  ', a.strip()); print('embld', b.strip())
print('%d statements; %d lines differ' % (len(stmts), n))
