import re, sys
hdr, out = sys.argv[1], sys.argv[2]
src = open(hdr).read()
fns = re.findall(r'static __inline__ __attribute__\(\(__always_inline__, __unused__\)\) ([\w ]+?) (__riscv_\w+)\(([^)]*)\)', src)
VLEN = 128
LM = {'mf8': 0.125, 'mf4': 0.25, 'mf2': 0.5, 'm1': 1, 'm2': 2, 'm4': 4, 'm8': 8}
MASKLMUL = {1: 'm8', 2: 'm4', 4: 'm2', 8: 'm1', 16: 'mf2', 32: 'mf4', 64: 'mf8'}

def vinfo(ty):
    m = re.match(r'v(int|uint|float)(\d+)(m\w+)_t$', ty)
    if not m:
        return None
    k = {'int': 'i', 'uint': 'u', 'float': 'f'}[m.group(1)]
    sew = int(m.group(2)); ln = m.group(3)
    return k, sew, ln, int(VLEN * LM[ln] // sew)

def ety(k, sew):
    return {'i': 'int%d_t' % sew, 'u': 'uint%d_t' % sew}.get(k) or ('float' if sew == 32 else 'double')

skip = set(sys.argv[3:])
L = ['#include <riscv_vector.h>', '#include <stdint.h>', '#include <string.h>',
     'void puts_(const char *s); void putn(long v);',
     'static unsigned long long H;',
     'static void mix(const void *p, long n) { const unsigned char *c = p; for (long i = 0; i < n; i++) { H ^= c[i]; H *= 1099511628211ULL; } }',
     'static unsigned seed = 12345;',
     'static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }',
     'static unsigned char ib[1024] __attribute__((aligned(16))), ob[1024] __attribute__((aligned(16)));',
     'static void fillb(void) { for (int i = 0; i < 1024; i++) ib[i] = (unsigned char)rnd(); }',
     'static void fillf(int es) { for (int i = 0; i < 1024; i += es) { if (es == 4) { float f = (float)((int)(rnd() % 2000) - 1000) / 8.0f; memcpy(ib + i, &f, 4); } else { double d = (double)((int)(rnd() % 2000) - 1000) / 8.0; memcpy(ib + i, &d, 8); } } }',
     'int main(void) {']
ncalls = 0
for ret, name, params in fns:
    ret = ret.strip()
    if name in skip or 'vsetvl' in name:
        continue
    ps = [p.strip() for p in params.split(',')] if params.strip() and params.strip() != 'void' else []
    pre, args = [], []
    vl = None
    ok = True
    for idx, p in enumerate(ps):
        m = re.match(r'(const )?([\w ]+?) (\*)?(\w+)$', p)
        if not m:
            ok = False; break
        const, ty, star, pn = m.groups()
        var = 'a%d' % idx
        vi = vinfo(ty)
        if star:
            k = 'f' if ty in ('float', 'double') else 'i'
            pre.append('fillf(%d);' % (4 if ty == 'float' else 8) if ty in ('float', 'double') else 'fillb();')
            args.append('(%s%s *)%s' % ('const ' if const else '', ty, 'ib' if const else 'ob'))
            if not const:
                pre.append('memset(ob, 0, 1024);')
        elif vi:
            k, sew, ln, n = vi
            if vl is None:
                vl = n
            e = ety(k, sew)
            pre.append('%s; %s %s = __riscv_vle%d_v_%s%d%s((const %s *)ib, %d);'
                       % ('fillf(%d)' % (sew // 8) if k == 'f' else 'fillb()', ty, var, sew, k, sew, ln, e, n))
            if (name.startswith('__riscv_vsll') or name.startswith('__riscv_vsr')) and idx == 1:
                pass
            args.append(var)
        elif ty.startswith('vbool'):
            nb = int(ty[5:-2])
            ln = MASKLMUL[nb]
            n = int(VLEN * LM[ln] // 8)
            pre.append('fillb(); vint8%s_t %sv = __riscv_vle8_v_i8%s((const int8_t *)ib, %d); %s %s = __riscv_vmslt_vx_i8%s_b%d(%sv, 0, %d);'
                       % (ln, var, ln, n, ty, var, ln, nb, var, n))
            if vl is None:
                vl = n
            args.append(var)
        elif ty == 'size_t' and pn == '__vl':
            args.append('VL')
        elif ty == 'size_t':
            args.append('%d' % (rnd_amt := 3))
        elif ty == 'ptrdiff_t':
            args.append('%d' % 0)          # stride filled below by the element size
        elif ty in ('float', 'double'):
            pre.append('%s %s = (%s)((int)(rnd() %% 200) - 100) / 4;' % (ty, var, ty))
            args.append(var)
        else:
            pre.append('%s %s; { unsigned long long __x = ((unsigned long long)rnd() << 24) ^ rnd(); memcpy(&%s, &__x, sizeof %s); }' % (ty, var, var, var))
            args.append(var)
    if not ok:
        continue
    if vl is None:
        rv = vinfo(ret)
        if rv:
            vl = rv[3]
        elif ret.startswith('vbool'):
            nb = int(ret[5:-2]); vl = int(VLEN * LM[MASKLMUL[nb]] // 8)
        else:
            vl = 1
    # strided ops: a stride of one element
    if 'vlse' in name or 'vsse' in name:
        sew = int(re.search(r'vl?s?se(\d+)', name).group(1))
        args = [a if a != '0' else str(sew // 8) for a in args]
    call = '%s(%s)' % (name, ', '.join(args))
    call = call.replace('VL', str(vl))
    body = ' '.join(pre)
    rv = vinfo(ret)
    if ret == 'void':
        body += ' %s; mix(ob, 1024);' % call
    elif rv:
        k, sew, ln, n = rv
        keep = 1 if 'red' in name or name.startswith('__riscv_vmv_s_') or name.startswith('__riscv_vfmv_s_') else min(n, vl)
        zap = ''
        if name.endswith('_m'):
            # a masked form's inactive elements are agnostic: zero them by
            # the same mask in both builds before looking
            zap = ' __r = __riscv_vmerge_vvm_%s%d%s(__riscv_v%smv_v_%s_%s%d%s(0, %d), __r, a0, %d);' % (
                k, sew, ln, 'f' if k == 'f' else '', 'f' if k == 'f' else 'x', k, sew, ln, n, n)
        body += ' { %s __r = %s;%s __riscv_vse%d_v_%s%d%s((%s *)ob, __r, %d); mix(ob, %d); }' % (ret, call, zap, sew, k, sew, ln, ety(k, sew), n, keep * sew // 8)
    elif ret.startswith('vbool'):
        nb = int(ret[5:-2]); ln = MASKLMUL[nb]; n = int(VLEN * LM[ln] // 8)
        body += ' { %s __m = %s; vint8%s_t __z = __riscv_vmv_v_x_i8%s(0, %d); vint8%s_t __o = __riscv_vmerge_vxm_i8%s(__z, 1, __m, %d); __riscv_vse8_v_i8%s((int8_t *)ob, __o, %d); mix(ob, %d); }' % (ret, call, ln, ln, n, ln, ln, n, ln, n, min(n, vl))
    else:
        body += ' { %s __r = %s; mix(&__r, sizeof __r); }' % (ret, call)
    L.append('  /* %s */ { H = 1469598103934665603ULL; %s } puts_("%s "); putn((long)(H & 0xffffffff)); puts_("\\n");' % (name, body, name[8:]))
    ncalls += 1
L += ['  puts_("==END==\\n");', '  return 0;', '}']
open(out, 'w').write('\n'.join(L) + '\n')
print(ncalls, 'calls')
