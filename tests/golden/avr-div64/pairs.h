/* tests/golden/avr-div64.sh: the operands, each P(n, d): small and large,
 * divisors above the dividend, powers of two, both top bits set (the
 * remainder's carry out of 64 bits), and the extremes. */
P(0ULL, 1ULL)
P(3999999ULL, 1000000ULL)
P(0xffffffffffffffffULL, 1000000ULL)
P(0xffffffffffffffffULL, 1ULL)
P(0xffffffffffffffffULL, 0xffffffffffffffffULL)
P(0x8000000000000000ULL, 0x8000000000000001ULL)
P(0xfedcba9876543210ULL, 0x8000000000000000ULL)
P(0xfedcba9876543210ULL, 0xc000000000000001ULL)
P(0x123456789abcdef0ULL, 0x1234ULL)
P(0x123456789abcdef0ULL, 0x100000000ULL)
P(0x00000000ffffffffULL, 0x0000000100000000ULL)
P(0x7fffffffffffffffULL, 0x3ULL)
P(1000000000000000000ULL, 999999999999ULL)
P(0xdeadbeefcafebabeULL, 0xfeedfaceULL)
P(12345ULL, 0x100000000000ULL)
