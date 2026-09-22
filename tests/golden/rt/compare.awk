# Compare two runs of rtcomplex.c.
#
# Multiply lines (dm, fm, sm) and the Annex G division specials (sd) must
# match BIT FOR BIT: those are specified, and lib/rt agrees with libgcc
# on every one.
#
# Ordinary division (dd, fd) is allowed one unit in the last place. C
# does not fix the rounding of a complex divide for finite operands, and
# three implementations round differently -- Smith's method here,
# something more elaborate in modern libgcc, a third algorithm in
# clang's compiler-rt. Demanding identity would assert which toolchain
# built the reference.
#
# One ulp is a cheap test on the BITS: two finite floats of the same
# sign are adjacent exactly when their bit patterns differ by one, since
# IEEE-754 lays out the exponent above the mantissa on purpose. Across
# zero, or where either side is an infinity or a NaN, only an exact
# match will do -- those are the cases where "close" means nothing.
#
#   usage: awk -f compare.awk ref.txt ours.txt

function hex(s,   i, c, n, d) {          # hex string -> number
    n = 0
    for (i = 1; i <= length(s); i++) {
        c = tolower(substr(s, i, 1))
        d = index("0123456789abcdef", c) - 1
        if (d < 0) return -1
        n = n * 16 + d
    }
    return n
}

function signbit(s) {                     # top bit set?
    return index("89abcdef", tolower(substr(s, 1, 1))) > 0
}

function iszero(s,   i) {
    for (i = 2; i <= length(s); i++)
        if (substr(s, i, 1) != "0") return 0
    return (substr(s, 1, 1) == "0" || substr(s, 1, 1) == "8")
}

function isfinite(s) {
    if (s == "nan") return 0
    if (length(s) == 16) return substr(tolower(s), 1, 3) != "7ff" &&
                                substr(tolower(s), 1, 3) != "fff"
    return substr(tolower(s), 1, 2) != "7f" && substr(tolower(s), 1, 2) != "ff"
}

# Returns 3 where the reference gave up and we did not, 2 for "equal" or
# within one ulp, 1 for "differs only in the sign of a zero", 0 for a
# real difference.
function within_one_ulp(a, b,   x, y) {
    if (a == b) return 2
    # The reference produced a NaN where we produced a finite number.
    # That is Smith's method doing its job: an intermediate that
    # overflows in the squared-denominator formula does not overflow
    # here. Checked by hand for (1 - 1e300i)/(1e-160 i), where the true
    # imaginary part is -1e160 -- a perfectly representable double, and
    # what lib/rt returns to within 5e-18 relative error, while libgcc
    # returns NaN. An INFINITY counts here too: where the true quotient
    # is about 1e468 an overflow is the correct answer and a NaN is not.
    # Counted separately and reported, never as a pass in disguise.
    if (a == "nan" && b != "nan") return 3
    if (a == "nan" || b == "nan") return 0
    # Both zero, opposite signs. The exact quotient underflowed or
    # cancelled to nothing, and which zero comes out of that depends on
    # the order the algorithm adds its terms -- libgcc's scaled method
    # and Smith's method disagree here on values whose true answer is
    # about 1e-460. Counted and reported rather than ignored, because a
    # sign that flips on ORDINARY values would be a real bug.
    if (iszero(a) && iszero(b)) return 1
    if (signbit(a) != signbit(b)) return 0    # across zero: exact or nothing
    x = hex(a); y = hex(b)
    if (x < 0 || y < 0) return 0
    return (x - y == 1 || y - x == 1) ? 2 : 0
}

NR == FNR { ref[FNR] = $0; nref = FNR; next }

{
    line = FNR
    if (ref[line] == $0) next
    split(ref[line], r, " ")
    split($0, o, " ")
    tag = o[1]
    exact = (tag == "dm" || tag == "fm" || tag == "sm" || tag == "sd")
    if (exact) {
        bad++
        if (bad <= 5)
            printf("  exact mismatch, line %d:\n    ref  %s\n    ours %s\n",
                   line, ref[line], $0)
        next
    }
    # ordinary division: the last two fields are the two parts
    nr = split(ref[line], r, " ")
    no = split($0, o, " ")
    if (nr != no) { bad++; next }
    c1 = within_one_ulp(r[nr - 1], o[no - 1])
    c2 = within_one_ulp(r[nr], o[no])
    if (c1 && c2) {
        if (c1 == 3 || c2 == 3) better++
        else if (c1 == 1 || c2 == 1) zsign++
        else ulp++
        next
    }
    bad++
    if (bad <= 5)
        printf("  division off by more than one ulp, line %d:\n" \
               "    ref  %s\n    ours %s\n", line, ref[line], $0)
}

END {
    if (nref != FNR) {
        printf("  the two runs printed different numbers of lines: " \
               "%d and %d\n", nref, FNR)
        bad++
    }
    printf("RESULT %d %d %d %d %d\n", nref, bad + 0, ulp + 0, zsign + 0,
           better + 0)
}
