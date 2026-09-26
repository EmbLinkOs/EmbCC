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
/* Three words placed, then a two-word composite -- which splits across
 * the last argument register and the stack on AAPCS32, and travels in
 * registers on RISC-V, where there are eight of them. s12 is twelve
 * bytes, so it goes by REFERENCE at RV32 and in two registers at RV64. */
int split(int a, int b, int c, struct s8 s, struct s12 t);
/* An eight-byte SCALAR. AAPCS32 rounds the register number up to even
 * for it where a four-aligned composite does not; the RISC-V psABI does
 * NOT, for a fixed argument, and DOES for a variadic one. */
long long mix64(int a, long long b, int c, struct s8 s);

/* Variadic, where the ABI question is whether the callee's register
 * save area lines up with where the caller left the arguments. */
int vsum(int n, ...);
long long vmix(int n, ...);
int vafter4(int a, int b, int c, int d, ...);
