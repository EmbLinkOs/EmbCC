#!/usr/bin/env python3
"""std-check.py -- the language standards, case by case, against a referee.

    tools/std-check.py record CORPUS EXPECT     (needs a GCC: $STD_REFEREE)
    tools/std-check.py check  CORPUS EXPECT     (EmbCC: $EMBCC, default ./embcc)

CORPUS is tests/std/c-corpus.txt or cxx-corpus.txt: `== NAME` lines, each
followed by the case's source. Every case is compiled with -fsyntax-only
under every standard of its language, once as it is and once with
-pedantic-errors, and the outcome -- accepted or refused -- is the line
`NAME MODE plain|pedantic ok|err` in EXPECT.

`record` writes EXPECT from the referee (GCC 16 here: x86_64-elf-gcc,
whose freestanding headers are all a case may include). `check` compiles
the same cases with EmbCC and prints every line where EmbCC and the record
disagree; it exits 1 when one does. A case EmbCC is known to differ on
for a reason of its own is listed in EXPECT's companion,
EXPECT.differs, with that reason, and is not counted.
"""
import os, subprocess, sys, tempfile

C_MODES = ['c89', 'gnu89', 'c99', 'gnu99', 'c11', 'gnu11', 'c17', 'gnu17',
           'c23', 'gnu23', 'c2y', 'gnu2y']
CXX_MODES = ['c++98', 'gnu++98', 'c++11', 'gnu++11', 'c++14', 'gnu++14',
             'c++17', 'gnu++17', 'c++20', 'gnu++20', 'c++23', 'gnu++23',
             'c++26', 'gnu++26']


def read_corpus(path):
    cases, name, lines = [], None, []
    for line in open(path):
        if line.startswith('== '):
            if name:
                cases.append((name, ''.join(lines)))
            name, lines = line[3:].strip(), []
        elif name is not None:
            lines.append(line)
        # lines before the first case are the file's comment
    if name:
        cases.append((name, ''.join(lines)))
    return cases


def compile_ok(cc, extra, mode, src, cxx):
    suffix = '.cc' if cxx else '.c'
    with tempfile.NamedTemporaryFile('w', suffix=suffix, delete=False) as f:
        f.write(src)
        p = f.name
    try:
        cmd = cc + ['-fsyntax-only', '-std=' + mode] + extra
        if cxx:
            cmd += ['-x', 'c++']
        cmd.append(p)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        return r.returncode == 0
    finally:
        os.unlink(p)


def run(cc, corpus, cxx):
    modes = CXX_MODES if cxx else C_MODES
    out = {}
    for name, src in read_corpus(corpus):
        for mode in modes:
            for variant, extra in (('plain', []), ('pedantic', ['-pedantic-errors'])):
                ok = compile_ok(cc, extra, mode, src, cxx)
                out[(name, mode, variant)] = 'ok' if ok else 'err'
    return out


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('record', 'check'):
        print(__doc__.strip().split('\n\n')[0], file=sys.stderr)
        return 2
    what, corpus, expect = sys.argv[1:]
    cxx = 'cxx' in os.path.basename(corpus)
    if what == 'record':
        cc = os.environ.get('STD_REFEREE', 'x86_64-elf-gcc').split()
        res = run(cc, corpus, cxx)
        with open(expect, 'w') as f:
            f.write('# %s under each standard, as %s decides: '
                    'tools/std-check.py record\n' % (os.path.basename(corpus), cc[0]))
            for (n, m, v), o in res.items():
                f.write('%s %s %s %s\n' % (n, m, v, o))
        return 0
    cc = os.environ.get('EMBCC', './embcc').split()
    cc += os.environ.get('STD_EMBCC_FLAGS', '--target=x86_64-elf').split()
    want = {}
    for line in open(expect):
        if line.startswith('#') or not line.strip():
            continue
        n, m, v, o = line.split()
        want[(n, m, v)] = o
    differs = set()
    if os.path.exists(expect + '.differs'):
        for line in open(expect + '.differs'):
            if line.strip() and not line.startswith('#'):
                differs.add(tuple(line.split()[:3]))
    got = run(cc, corpus, cxx)
    bad = 0
    for k in sorted(got):
        if k in want and got[k] != want[k] and k not in differs:
            print('%s %s %s: referee %s, EmbCC %s' % (k[0], k[1], k[2], want[k], got[k]))
            bad += 1
    total = len([k for k in got if k in want])
    print('%d of %d outcomes agree with the referee (%d known differences)'
          % (total - bad, total, len(differs)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
