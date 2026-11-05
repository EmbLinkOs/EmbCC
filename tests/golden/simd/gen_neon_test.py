import re, sys, subprocess, os
hdr, out = sys.argv[1], sys.argv[2]
src = open(hdr).read()
fns = re.findall(r'static __inline__ __attribute__\(\(__always_inline__, __unused__\)\) (\w+) (\w+)\(([^)]*)\)', src)
# vector types and their lanes
vt = {}
for m in re.finditer(r'typedef (\w+) (\w+) __attribute__\(\(__vector_size__\((\d+)\)\)\);', src):
    vt[m.group(2)] = (m.group(1), int(m.group(3)))
st = set(re.findall(r'typedef struct \{ (\w+) val\[(\d)\]; \} (\w+);', src) and
         [m[2] for m in re.findall(r'typedef struct \{ (\w+) val\[(\d)\]; \} (\w+);', src)])
skip = set(sys.argv[3:])
lines = ['#include <arm_neon.h>', '#include <stdio.h>', '#include <string.h>', '#include <stdint.h>',
         'static unsigned long long H = 1469598103934665603ULL;',
         'static void mix(const void *p, int n) { const unsigned char *c = p; for (int i = 0; i < n; i++) { H ^= c[i]; H *= 1099511628211ULL; } }',
         'static unsigned seed = 12345;',
         'static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }',
         'static void fill(void *p, int n, int fl, int es) {',
         '  unsigned char *c = p;',
         '  if (fl) { for (int i = 0; i < n; i += es) { if (es == 4) { float f = (float)((int)(rnd() % 2000) - 1000) / 8.0f; memcpy(c + i, &f, 4); } else { double d = (double)((int)(rnd() % 2000) - 1000) / 8.0; memcpy(c + i, &d, 8); } } }',
         '  else for (int i = 0; i < n; i++) c[i] = (unsigned char)rnd(); }',
         'static unsigned char buf[4096] __attribute__((aligned(16)));',
         '#pragma GCC diagnostic ignored "-Wall"',
         'int main(void) {']
calls = []
for ret, name, params in fns:
    if name.startswith('__') or name in skip:
        continue
    ps = [p.strip() for p in params.split(',')] if params.strip() else []
    args = []
    pre = []
    ok = True
    for idx, p in enumerate(ps):
        m = re.match(r'(const )?(\w+) (\*)?(\w+)', p)
        if not m:
            ok = False; break
        const, ty, star, pn = m.groups()
        var = 'a%d' % idx
        if star:
            pre.append('fill(buf, 512, 0, 1);')
            if ty in ('float32_t', 'float64_t'):
                pre.append('fill(buf, 512, 1, %d);' % (4 if ty == 'float32_t' else 8))
            args.append('(%s *)buf' % ty)
        elif ty in vt:
            et, nb = vt[ty]
            fl = et.startswith('float')
            es = int(re.search(r'(\d+)', et).group(1)) // 8
            pre.append('%s %s; fill(&%s, %d, %d, %d);' % (ty, var, var, nb, 1 if fl else 0, es))
            if name.startswith('vshl') and not name.startswith('vshl_n') and not name.startswith('vshlq_n') and idx == 1:
                pre.append('for (int i = 0; i < %d; i++) ((unsigned char *)&%s)[i] &= 7;' % (nb, var))
            if name.startswith('vbsl') and idx == 0:
                pass
            args.append(var)
        elif ty == 'int' and const and pn == '__l':
            args.append('0')
        elif ty == 'int' and const:
            # a lane or a shift count: 1 always fits (and n for vext/shift)
            args.append('1')
        elif ty.endswith('x2_t') or ty.endswith('x3_t') or ty.endswith('x4_t'):
            pre.append('%s %s; fill(&%s, sizeof %s, 0, 1);' % (ty, var, var, var))
            args.append(var)
        elif ty in ('float32_t', 'float64_t'):
            pre.append('%s %s = (%s)((int)(rnd() %% 200) - 100) / 4;' % (ty, var, ty))
            args.append(var)
        else:
            pre.append('%s %s; fill(&%s, sizeof %s, 0, 1);' % (ty, var, var, var))
            args.append(var)
    if not ok:
        continue
    call = '%s(%s)' % (name, ', '.join(args))
    body = ' '.join(pre)
    if ret == 'void':
        if name.startswith('vst'):
            body += ' memset(buf + 1024, 0, 512); '
            call = call.replace('(%s *)buf' % re.match(r'(const )?(\w+) ', ps[0]).group(2), '(%s *)(buf + 1024)' % re.match(r'(const )?(\w+) ', ps[0]).group(2), 1)
            body += call + '; mix(buf + 1024, 512);'
        else:
            body += call + ';'
    else:
        body += ' { %s __r = %s; mix(&__r, sizeof __r); }' % (ret, call)
    calls.append('  /* %s */ { %s }' % (name, body))
lines += calls
lines += ['  printf("%016llx %d\\n", H, ' + str(len(calls)) + ');', '  return 0;', '}']
open(out, 'w').write('\n'.join(lines) + '\n')
print(len(calls), 'calls')
