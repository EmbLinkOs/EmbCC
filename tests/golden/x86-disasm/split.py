import os, re, subprocess, sys
# split.py EMBCC SRC... (tests/golden/x86-disasm.sh): does `embcc -S`'s instruction split agree with
# llvm-objdump's on the same object? Prints mismatches and a total.
embcc = sys.argv[1]
tot = bad = 0
for src in sys.argv[2:]:
    for opt in ('-O0', '-O2'):
        s = subprocess.run([embcc, '--target=x86_64-elf', opt, '-S', src, '-o', '-'],
                           capture_output=True, text=True).stdout
        o = os.environ.get('TMPDIR', '/tmp') + '/x86-disasm-%d.o' % os.getpid()
        if subprocess.run([embcc, '--target=x86_64-elf', opt, '-c', src, '-o', o],
                          capture_output=True).returncode:
            continue
        # embcc: per function, the byte count of each `.byte` line that has a comment
        ours, cur = {}, None
        for line in s.split('\n'):
            m = re.match(r'^([A-Za-z_$][\w.$]*):$', line)   # not a .L label
            if m:
                cur = m.group(1); ours[cur] = []; continue
            m = re.match(r'^\s*\.byte\s+([0-9a-fx, \t]+)#', line)
            if m and cur is not None:
                ours[cur].append(len([b for b in m.group(1).split(',') if b.strip()]))
        d = subprocess.run(['llvm-objdump', '-d', '--no-addresses', o],
                           capture_output=True, text=True).stdout
        theirs, cur, nops, pending_prefix = {}, None, {}, False
        for line in d.split('\n'):
            m = re.match(r'^<([^>]+)>:$', line)
            if m:
                cur = m.group(1); theirs[cur] = []; continue
            m = re.match(r'^\s+((?:[0-9a-f]{2} ?)+)', line)
            if m and cur is not None:
                n = len(m.group(1).split())
                if pending_prefix:         # objdump prints `lock` alone
                    theirs[cur][-1] += n
                    pending_prefix = False
                    continue
                if '\t' in line or not theirs[cur]:
                    theirs[cur].append(n)
                    nops.setdefault(cur, []).append('\tnop' in line)
                    pending_prefix = line.rstrip().endswith('\tlock')
                else:                      # a long instruction's continuation
                    theirs[cur][-1] += n
        for fn in theirs:                  # the padding after a function
            while theirs[fn] and nops[fn][-1]:
                theirs[fn].pop(); nops[fn].pop()
        for fn, lens in ours.items():
            if fn not in theirs:
                continue
            tot += 1
            if lens != theirs[fn]:
                bad += 1
                print(f'MISMATCH {src} {opt} {fn}: ours {len(lens)} insns, objdump {len(theirs[fn])}')
print(f'{tot} functions compared, {bad} differ')
sys.exit(1 if bad else 0)
