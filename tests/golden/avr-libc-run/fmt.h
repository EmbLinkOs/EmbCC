/* The formats tests/golden/avr-libc-run.sh checks against the host's C
 * library: each F(format, arguments) line, with values that are the same
 * on a 16-bit and a 32-bit int. */
F("[%d|%i|%u]", -1234, 77, 40000u)
F("[%5d|%-5d|%05d|%+d|% d]", 42, 42, 42, 42, 42)
F("[%x|%X|%#x|%o|%#o]", 0xbeefu, 0xbeefu, 0x1fu, 8u, 8u)
F("[%.3d|%.0d|%8.3x]", 7, 0, 0xau)
F("[%ld|%lu|%lx|%li]", -2000000000L, 4000000000UL, 0xdeadbeefUL, 123456789L)
F("[%lld|%llu|%llx]", -9000000000000000000LL, 18000000000000000000ULL, 0x123456789abcdefULL)
F("[%hd|%hu|%hhd|%hhu]", -3, 65535u, -1, 255u)
F("[%c|%3c|%-3c]", 'a', 'b', 'c')
F("[%s|%.2s|%6s|%-6s]", "flash", "flash", "ab", "ab")
F("[%*d|%-*d|%.*s]", 6, 9, 4, 9, 3, "abcdef")
F("[%%|%c%%]", 'x')
