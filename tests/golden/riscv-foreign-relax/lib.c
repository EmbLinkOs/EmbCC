/* Built by clang as it builds by default -- relaxation on, DWARF 5 --
 * for tests/golden/riscv-foreign-relax.sh. Everything here asks embld for
 * a relocation EmbCC's own objects never carry: compressed branches and
 * jumps between labels the linker may move, a switch's jump table of
 * label differences, DWARF lengths, and NOP padding for an alignment that
 * the linker must cut to the right length. (The assembler pads exactly
 * where nothing before the alignment can shrink, so a call -- relaxable
 * -- comes first.) */
extern int lib_ext(int x);

int lib_classify(int x)
{
    switch (x) {
    case 0: return 11;
    case 1: return 23;
    case 2: return 35;
    case 3: return 47;
    case 4: return 59;
    case 5: return 61;
    case 6: return 73;
    default: return -1;
    }
}

int lib_pre(int x) { return lib_ext(x) * 3 + 1; }

/* a trap vector's alignment, and a smaller one after it */
__attribute__((aligned(64), noinline)) int lib_vec(void) { return 5; }
__attribute__((aligned(8), noinline)) int lib_after(int x)
{
    return x ? x + 1 : 9;
}

int lib_sum(const int *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i] > 0 ? p[i] : -p[i];
    return s;
}
