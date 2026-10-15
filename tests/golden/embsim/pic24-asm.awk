# pic24-asm.awk -- the few PIC24 instructions tests/golden/embsim-pic24.sh's
# programs are written in, assembled to an Intel HEX image (a program word
# at twice its address, four bytes: its three and a phantom 0, as XC16's
# images have it). There is no PIC24 assembler on the build machine; the
# instructions a test examines are given as .word with their encoding, and
# these few carry the setup and the printing around them.
#
#   awk -f pic24-asm.awk PROG.s PROG.s > PROG.hex     (two passes: labels)
#   awk -v syms=FILE ...: and the symbols to FILE, a line each: "L NAME
#   ADDR" for a label, "E NAME VALUE" for an .equ (pic24-elf.py's input)
#
# Syntax: `label:`, `; comment`, `.equ NAME, VALUE`, `.org ADDR` (a
# program address), `.word V` (a 24-bit word or a label), `.fill N, V`;
# numbers in C syntax or 'c'. For the tests: `.dump ADDR` stores SR, W0
# ... W15 at ADDR (W0 first, then SR, so no flag changes before it is
# kept), then sets W15 to STACK and clears the stored SR's DC if the
# program .equs those names; `.show ADDR, ITEM...` prints those (sr,
# wN) or a data address's word in hex with the program's print routine,
# then a newline; `.case N` prints "NN:". Instructions, by DS70000157's
# encodings:
#   mov #lit16, wN     mov wN, f      mov f, wN      mov wS, wD
#   push wN            pop wN         lsr wB, #k, wD  sl wB, #k, wD
#   cp wB, #lit5       add #lit10, wN  dec wS, wD
#   bset f, #b         bclr f, #b      btss f, #b      btsc f, #b
#   bra L              bra cc, L       rcall L         call L
#   goto L             return          repeat #lit14   nop      reset
function num(s,    v, i, c, neg) {
    gsub(/^[ \t]+|[ \t]+$/, "", s)
    if (s in sym)
        return sym[s]
    if (s ~ /^'.'$/)
        return index(" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", substr(s, 2, 1)) + 31
    neg = 0
    if (s ~ /^-/) {
        neg = 1
        s = substr(s, 2)
    }
    v = 0
    if (s ~ /^0[xX][0-9a-fA-F]+$/) {
        for (i = 3; i <= length(s); i++) {
            c = index("0123456789abcdef", tolower(substr(s, i, 1))) - 1
            v = v * 16 + c
        }
    } else if (s ~ /^[0-9]+$/) {
        v = s + 0
    } else {
        if (pass == 2)
            die("unknown symbol " s)
        return 0
    }
    return neg ? -v : v
}
function reg(s) {
    gsub(/^[ \t]+|[ \t]+$/, "", s)
    if (s !~ /^[wW][0-9]+$/ || substr(s, 2) + 0 > 15)
        die("not a W register: " s)
    return substr(s, 2) + 0
}
function lit(s) {
    gsub(/^[ \t]+/, "", s)
    if (s !~ /^#/)
        die("not a literal: " s)
    return num(substr(s, 2))
}
function die(m) {
    printf "pic24-asm.awk: %s:%d: %s\n", FILENAME, FNR, m > "/dev/stderr"
    bad = 1
    exit 1
}
function and_(a, b,    r, k) {
    r = 0
    for (k = 1; a > 0 && b > 0; k *= 2) {
        if (a % 2 == 1 && b % 2 == 1)
            r += k
        a = int(a / 2)
        b = int(b / 2)
    }
    return r
}
function emit(w) {
    if (pass == 2)
        word[pc] = and_(w < 0 ? w + 16777216 : w, 16777215)
    pc += 2
}
function rel(target) {
    return and_((target - pc - 2) / 2 + 65536, 65535)
}
function asm(line,    op, args, n, a, x, y, c, b, base, t, k, it, m, item) {
    gsub(/^[ \t]+|[ \t]+$/, "", line)
    if (line == "")
        return
    op = tolower(line)
    sub(/[ \t].*/, "", op)
    args = line
    sub(/^[^ \t]+[ \t]*/, "", args)
    n = split(args, a, /,/)
    if (op == ".equ") { gsub(/[ \t]/, "", a[1]); sym[a[1]] = num(a[2]); return }
    if (op == ".org") { pc = num(a[1]); return }
    if (op == ".word") { emit(num(a[1])); return }
    if (op == ".fill") { for (k = 0; k < num(a[1]); k++) emit(num(a[2])); return }
    if (op == ".dump") {
        # W0, then SR (MOV f, Wnd changes no flag), then W1-W15, to the
        # words at ADDR: SR, W0, W1 ... W15
        m = num(a[1])
        asm("mov w0, " (m + 2))
        asm("mov 0x42, w0")
        asm("mov w0, " m)
        for (k = 1; k <= 15; k++)
            asm("mov w" k ", " (m + 2 * (k + 1)))
        if ("STACK" in sym)
            asm("mov #" sym["STACK"] ", w15")   # the program's stack again
        if ("NODC" in sym)
            asm("bclr " m ", #8")               # SR's DC, not compared
        return
    }
    if (op == ".show") {
        # each item of the last .dump (sr, wN) or a data address, printed
        for (k = 2; k <= n; k++) {
            item = tolower(a[k])
            gsub(/[ \t]/, "", item)
            if (item == "sr")
                asm("mov " num(a[1]) ", w0")
            else if (item ~ /^w[0-9]+$/)
                asm("mov " (num(a[1]) + 2 * (substr(item, 2) + 1)) ", w0")
            else
                asm("mov " a[k] ", w0")
            asm("rcall print")
        }
        asm("rcall newline")
        return
    }
    if (op == ".case") {
        # "NN:" on the UART
        it = sprintf("%02d:", num(a[1]))
        for (k = 1; k <= length(it); k++) {
            asm("mov #" (index(" !\"#$%&'()*+,-./0123456789:;<=>?@", substr(it, k, 1)) + 31) ", w1")
            asm("rcall putc")
        }
        return
    }
    if (op == "nop") { emit(0); return }
    if (op == "reset") { emit(16646144); return }                # 0xFE0000
    if (op == "return") { emit(393216); return }                 # 0x060000
    if (op == "repeat") { emit(589824 + and_(lit(a[1]), 16383)); return }
    if (op == "push") { emit(7872384 + reg(a[1])); return }      # 0x781F80
    if (op == "pop") { emit(7864399 + reg(a[1]) * 128); return } # 0x78004F
    if (op == "mov") {
        x = a[1]; y = a[2]
        gsub(/^[ \t]+|[ \t]+$/, "", x)
        gsub(/^[ \t]+|[ \t]+$/, "", y)
        if (x ~ /^#/) { emit(2097152 + and_(lit(x), 65535) * 16 + reg(y)); return }
        if (x ~ /^[wW][0-9]+$/ && y ~ /^[wW][0-9]+$/) { emit(7864320 + reg(y) * 128 + reg(x)); return }
        if (x ~ /^[wW][0-9]+$/) { emit(8912896 + int(num(y) / 2) * 16 + reg(x)); return }
        emit(8388608 + int(num(x) / 2) * 16 + reg(y)); return
    }
    if (op == "lsr" || op == "sl") {
        emit((op == "lsr" ? 14548992 : 14483456) + reg(a[1]) * 2048 + reg(a[3]) * 128 + 64 + and_(lit(a[2]), 15))
        return
    }
    if (op == "cp") { emit(14745600 + reg(a[1]) * 2048 + 96 + and_(lit(a[2]), 31)); return }
    if (op == "add") { emit(11534336 + and_(lit(a[1]), 1023) * 16 + reg(a[2])); return }
    if (op == "dec") { emit(15269888 + reg(a[2]) * 128 + reg(a[1])); return }
    if (op == "bset" || op == "bclr" || op == "btss" || op == "btsc") {
        b = and_(lit(a[2]), 15)
        base = op == "bset" ? 11010048 : op == "bclr" ? 11075584 : op == "btss" ? 11403264 : 11468800
        emit(base + and_(b, 7) * 8192 + and_(num(a[1]), 8190) + int(b / 8))
        return
    }
    if (op == "bra") {
        if (n == 1) { emit(3604480 + rel(num(a[1]))); return }  # 0x370000
        c = tolower(a[1])
        gsub(/[ \t]/, "", c)
        if (!(c in cc))
            die("no condition " c)
        emit(3145728 + cc[c] * 65536 + rel(num(a[2])))
        return
    }
    if (op == "rcall") { emit(458752 + rel(num(a[1]))); return }  # 0x070000
    if (op == "call" || op == "goto") {
        t = num(a[1])
        emit((op == "call" ? 131072 : 262144) + and_(t, 65534))
        emit(int(t / 65536))
        return
    }
    die("not an instruction this assembler has: " line)
}
BEGIN {
    cc["ov"] = 0; cc["c"] = 1; cc["geu"] = 1; cc["z"] = 2; cc["n"] = 3
    cc["le"] = 4; cc["lt"] = 5; cc["leu"] = 6; cc["nov"] = 8; cc["nc"] = 9
    cc["ltu"] = 9; cc["nz"] = 10; cc["nn"] = 11; cc["gt"] = 12; cc["ge"] = 13
    cc["gtu"] = 14
}
FNR == 1 { pass++; pc = 0 }
{
    line = $0
    sub(/;.*/, "", line)
    while (match(line, /^[ \t]*[A-Za-z_.][A-Za-z0-9_.]*:/)) {
        l = substr(line, RSTART, RLENGTH - 1)
        gsub(/[ \t]/, "", l)
        if (pass == 1) {
            sym[l] = pc
            islabel[l] = 1
        }
        line = substr(line, RSTART + RLENGTH)
    }
    asm(line)
}
END {
    if (bad)
        exit 1
    # Intel HEX: 16-byte records of four words, with the upper address
    # records when the byte address passes 64 KiB
    hi = -1
    for (p in word)
        pcs[++np] = p + 0
    for (i = 2; i <= np; i++) {                  # insertion sort
        v = pcs[i]
        for (j = i - 1; j >= 1 && pcs[j] > v; j--)
            pcs[j + 1] = pcs[j]
        pcs[j + 1] = v
    }
    i = 1
    while (i <= np) {
        start = pcs[i]
        k = 0
        while (i + k <= np && k < 4 && pcs[i + k] == start + 2 * k)
            k++
        ba = start * 2
        if (int(ba / 65536) != hi) {
            hi = int(ba / 65536)
            sum = 2 + 4 + int(hi / 256) + hi % 256
            printf ":02000004%04X%02X\n", hi, (256 - sum % 256) % 256
        }
        lo = ba % 65536
        sum = 4 * k + int(lo / 256) + lo % 256
        rec = sprintf(":%02X%04X00", 4 * k, lo)
        for (j = 0; j < k; j++) {
            w = word[pcs[i + j]]
            b0 = w % 256; b1 = int(w / 256) % 256; b2 = int(w / 65536) % 256
            rec = rec sprintf("%02X%02X%02X00", b0, b1, b2)
            sum += b0 + b1 + b2
        }
        print rec sprintf("%02X", (256 - sum % 256) % 256)
        i += k
    }
    print ":00000001FF"
    if (syms != "")
        for (n in sym)
            printf "%s %s %d\n", (n in islabel) ? "L" : "E", n, sym[n] > syms
}
