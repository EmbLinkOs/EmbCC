/* The shapes tests/golden/embedded-abi-{caller,callee}.c pass between
 * them, so that one may be compiled by EmbCC and the other by clang and
 * the two must still agree.
 *
 * Shared by the ARMv7-M and RISC-V suites, and the sharper for it: the
 * declarations below name no machine, but the three ABIs they are run
 * against DISAGREE about several of them. A two-word composite splits
 * across the last register and the stack on AAPCS32 and on RISC-V; an
 * eight-byte SCALAR rounds the register number up to even on AAPCS32 and
 * does NOT on RISC-V -- unless it is variadic, where RISC-V does. Each
 * pairing has to agree with itself, so a backend that read one rule as
 * the other fails against the reference compiler for its own target. */
struct s1 { char a; };
struct s3 { char a, b, c; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };
struct s20 { int v[5]; };
struct mix { char c; int i; short s; };

struct s1  m1(char a);
struct s3  m3(int k);
struct s8  m8(int a, int b);
struct s20 m20(int k);
struct mix mm(int k);
int u1(struct s1 s);
int u3(struct s3 s);
int u8(struct s8 s);
int u20(struct s20 s);
int umix(struct mix s);
/* Seven words in registers, then two eight-byte scalars: on RISC-V the
 * first splits across a7 and the stack and the second lands on the stack
 * aligned to EIGHT (psABI) -- sp+8, not sp+4 straight after the split
 * half, which is where EmbCC put it and clang never looked. */
long long stk64(int a, int b, int c, int d, int e, int f, int g,
                long long x, long long y);
/* Three words placed, then a two-word composite -- which splits across
 * the last argument register and the stack on AAPCS32, and travels in
 * registers on RISC-V, where there are eight of them. s12 is twelve
 * bytes, so it goes by REFERENCE at RV32 and in two registers at RV64. */
int split(int a, int b, int c, struct s8 s, struct s12 t);
/* An eight-byte SCALAR. AAPCS32 rounds the register number up to even
 * for it where a four-aligned composite does not; the RISC-V psABI does
 * NOT, for a fixed argument, and DOES for a variadic one. */
long long mix64(int a, long long b, int c, struct s8 s);

/* A composite is placed by its NATURAL alignment -- its members' --
 * not by an aligned attribute on the struct itself. AAPCS32 rounds to
 * an even register for nat_ll8 and nat_m8 and NOT for nat_ov8, which
 * EmbCC did, reading r2 where clang had put the struct in r1. The
 * nine-argument forms reach the stack on RISC-V too. */
struct __attribute__((aligned(8))) ov8 { int x; };
struct ll8 { long long x; };
struct m8 { int x __attribute__((aligned(8))); };
int nat_ov8(int a, struct ov8 s, int z);
int nat_ll8(int a, struct ll8 s, int z);
int nat_m8(int a, struct m8 s, int z);
int nat_stk(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            struct ov8 s, struct m8 t, int z);

/* Variadic, where the ABI question is whether the callee's register
 * save area lines up with where the caller left the arguments. */
int vsum(int n, ...);
long long vmix(int n, ...);
int vafter4(int a, int b, int c, int d, ...);
