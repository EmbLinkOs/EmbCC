# pic24-elf.py HEX SYMS OUT -- an XC16-like ELF of a pic24-asm.awk image,
# for tests/golden/embsim-pic24.sh's analyses, which read symbols: its
# program memory (four bytes a word, as the HEX file has it) in one
# loadable segment per run of words, at the program address (half the
# HEX's), e_machine 118 (EM_PIC30); each label a function (STT_FUNC, sized
# to the next label) but the local ones (.L...), each .equ an absolute
# symbol (__SP_init and
# __SPLIM_init tell the stack report where the stack is).
import struct, sys

def main(hexpath, sympath, out):
    mem = {}
    hi = 0
    for line in open(hexpath):
        line = line.strip()
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ = b[0], b[1] << 8 | b[2], b[3]
        if typ == 4:
            hi = b[4] << 8 | b[5]
        elif typ == 0:
            for k in range(n):
                mem[(hi << 16) + addr + k] = b[4 + k]
    runs = []                       # (bus address, bytes)
    for a in sorted(mem):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].append(mem[a])
        else:
            runs.append((a, bytearray([mem[a]])))
    labels, equs = [], []
    for line in open(sympath):
        kind, name, value = line.split()
        if not name.startswith('.L'):           # a local label: no symbol
            (labels if kind == 'L' else equs).append((name, int(value)))
    labels.sort(key=lambda x: x[1])

    strtab = bytearray(b'\0')
    def s(name):
        off = len(strtab)
        strtab.extend(name.encode() + b'\0')
        return off
    syms = [struct.pack('<IIIBBH', 0, 0, 0, 0, 0, 0)]
    for i, (name, addr) in enumerate(labels):
        end = labels[i + 1][1] if i + 1 < len(labels) else addr + 2
        syms.append(struct.pack('<IIIBBH', s(name), addr, end - addr,
                                0x12, 0, 1))            # GLOBAL FUNC, .text
    for name, value in equs:
        syms.append(struct.pack('<IIIBBH', s(name), value, 0, 0x10, 0, 0xfff1))
    symtab = b''.join(syms)
    shstr = bytearray(b'\0')
    def sh(name):
        off = len(shstr)
        shstr.extend(name.encode() + b'\0')
        return off
    n_text, n_sym, n_str, n_shs = sh('.text'), sh('.symtab'), sh('.strtab'), sh('.shstrtab')

    ehsize, phsize, shsize = 52, 32, 40
    off = ehsize + phsize * len(runs)
    body = bytearray()
    phdrs = []
    for a, data in runs:
        phdrs.append(struct.pack('<8I', 1, off + len(body), a // 2, a // 2,
                                 len(data), len(data), 5, 2))
        body.extend(data)
    text_off, text_size, text_addr = ehsize + phsize * len(runs), len(body), runs[0][0] // 2
    sym_off = off + len(body)
    body.extend(symtab)
    str_off = off + len(body)
    body.extend(strtab)
    shs_off = off + len(body)
    body.extend(shstr)
    while (off + len(body)) % 4:
        body.append(0)
    shoff = off + len(body)
    shdrs = [bytes(shsize),
             struct.pack('<10I', n_text, 1, 6, text_addr, text_off, text_size, 0, 0, 2, 0),
             struct.pack('<10I', n_sym, 2, 0, 0, sym_off, len(symtab), 3, 1, 4, 16),
             struct.pack('<10I', n_str, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0),
             struct.pack('<10I', n_shs, 3, 0, 0, shs_off, len(shstr), 0, 0, 1, 0)]
    eh = b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
    eh += struct.pack('<HHIIIIIHHHHHH', 2, 118, 1, 0, ehsize, shoff, 0,
                      ehsize, phsize, len(runs), shsize, len(shdrs), 4)
    open(out, 'wb').write(eh + b''.join(phdrs) + body + b''.join(shdrs))

main(*sys.argv[1:4])
