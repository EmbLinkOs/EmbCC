#!/usr/bin/env python3
"""Print an ELF32's allocated sections as `name vma size`, one per line (the test's check)."""
import struct
import sys

elf = open(sys.argv[1], "rb").read()
shoff, = struct.unpack_from("<I", elf, 32)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 46)
sh = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
strtab = sh[shstrndx][4]
for name, _type, flags, addr, _off, size, *_ in sh:
    if flags & 2 and size:  # SHF_ALLOC
        n = elf[strtab + name:elf.index(b"\0", strtab + name)].decode()
        print(n, hex(addr), size)
