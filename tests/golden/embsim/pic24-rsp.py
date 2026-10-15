# pic24-rsp.py PORT ADDR -- a GDB remote protocol client for
# tests/golden/embsim-pic24.sh: no gdb has a PIC24 target, so this reads
# EmbSim's PIC24 registers the way a client of its target description
# would. It connects (retrying while the server starts), reads
# target.xml, puts a breakpoint at program address ADDR (hex), continues
# to it, and prints each register the description names with the value
# the g packet holds for it ("w8 0x1111"), then pc again by its number
# (the p packet), and kills the target.
import re, socket, sys, time

def main(port, addr):
    s = None
    for _ in range(100):
        try:
            s = socket.create_connection(('127.0.0.1', int(port)))
            break
        except OSError:
            time.sleep(0.1)
    if s is None:
        sys.exit('pic24-rsp: no server on port ' + port)
    s.settimeout(20)
    buf = b''

    def packet():
        nonlocal buf
        while True:
            m = re.search(rb'\$([^#]*)#..', buf)
            if m:
                buf = buf[m.end():]
                s.sendall(b'+')
                return m.group(1).decode()
            data = s.recv(4096)
            if not data:
                sys.exit('pic24-rsp: the server closed the connection')
            buf += data

    def cmd(c):
        sum_ = sum(c.encode()) & 0xff
        s.sendall(('$%s#%02x' % (c, sum_)).encode())
        return packet()

    sup = cmd('qSupported:xmlRegisters=i386')
    if 'qXfer:features:read+' not in sup:
        sys.exit('pic24-rsp: no target description offered: ' + sup)
    xml, off = '', 0
    while True:
        r = cmd('qXfer:features:read:target.xml:%x,400' % off)
        xml += r[1:]
        off += len(r) - 1
        if r[0] == 'l':
            break
    regs = re.findall(r'<reg name="([^"]+)" bitsize="(\d+)"', xml)
    print('feature', re.search(r'<feature name="([^"]+)"', xml).group(1))
    print('registers', ' '.join(n for n, _ in regs))
    if cmd('Z0,%s,2' % addr) != 'OK':
        sys.exit('pic24-rsp: the breakpoint was refused')
    stop = cmd('c')
    print('stop', stop[:3])
    g = cmd('g')
    k = 0
    for name, bits in regs:
        n = int(bits) // 4
        v = bytes.fromhex(g[k:k + n])
        k += n
        print(name, '0x%0*x' % (n, int.from_bytes(v, 'little')))
    if k != len(g):
        sys.exit('pic24-rsp: the g packet is %d digits, the description %d' % (len(g), k))
    pc = bytes.fromhex(cmd('p%x' % [n for n, _ in regs].index('pc')))
    print('p pc', '0x%08x' % int.from_bytes(pc, 'little'))
    s.sendall(b'$k#6b')
    s.close()

main(*sys.argv[1:3])
