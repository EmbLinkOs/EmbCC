# The CMSE shape of each function in an llvm-objdump listing: for an entry
# function (one that ends in bxns lr) the registers it overwrites with lr,
# and whether it writes APSR from lr; for a call (blxns rT) the registers
# holding the target at the branch -- rT and every one moved from it --
# and whether APSR is written from it. One line per register, so that a
# sort makes two listings comparable whichever register holds the target.
function flush() {
    if (fn == "") return
    if (bx) { for (r in from_lr) print fn " entry " r
              if (msr_lr) print fn " entry apsr" }
    if (blx != "") { print fn " call " blx
              for (r in from_t) if (from_t[r] == blx) print fn " call " r
              if (msr_t == blx) print fn " call apsr" }
}
/^[0-9a-f]+ <[^>]+>:$/ {
    flush(); fn = $2; gsub(/[<>:]/, "", fn)
    bx = 0; blx = ""; msr_lr = 0; msr_t = ""
    delete from_lr; delete from_t; delete mv
    next
}
/^ *[0-9a-f]+:/ {
    line = $0; sub(/^ *[0-9a-f]+: +/, "", line)
    t = index(line, "\t"); if (!t) next
    rest = substr(line, t + 1); m = rest; sub(/\t.*/, "", m)
    ops = rest; sub(/^[^\t]*\t?/, "", ops); gsub(/ /, "", ops)
    n = split(ops, o, ",")
    if (m == "mov" && n == 2) {
        if (o[2] == "lr") from_lr[o[1]] = 1
        mv[o[1]] = o[2]
    }
    if (m == "msr" && o[1] ~ /^apsr/) { if (o[2] == "lr") msr_lr = 1; msr_reg = o[2] }
    if (m == "bxns" && ops == "lr") bx = 1
    if (m == "blxns") {
        blx = ops; msr_t = msr_reg
        for (r in mv) if (mv[r] == ops) from_t[r] = ops
    }
}
END { flush() }
