#!/usr/bin/env python3
"""Print an ELF32's allocated sections as `name vma size` and its loadable segments as
`LOAD vaddr paddr memsz`, one per line, addresses as fixed-width hex (the test's check)."""
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
        print(n, "0x%08x" % addr, size)
phoff, = struct.unpack_from("<I", elf, 28)
phentsize, phnum = struct.unpack_from("<HH", elf, 42)
for i in range(phnum):
    p_type, _off, vaddr, paddr, _filesz, memsz, *_ = struct.unpack_from("<IIIIIIII", elf, phoff + i * phentsize)
    if p_type == 1 and memsz:  # PT_LOAD
        print("LOAD", "0x%08x" % vaddr, "0x%08x" % paddr, memsz)
