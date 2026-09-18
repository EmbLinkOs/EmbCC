/* __attribute__((section("name"))) on file-scope variables: the linker
 * gathers a named section's entries contiguously and the program walks
 * them between the section's bracket symbols — GNU ld's __start_NAME /
 * __stop_NAME here; the EmbLinkOS kernel's .embk_exports table is the same
 * shape under its linker script's names (embld's side of that is
 * tests/golden/embld-sections.sh). The relocations inside an entry (a
 * string, a function, a global) must land in the named section, not .data.
 * Order-independent on purpose: gcc may reorder top-level definitions.
 */
// expect-exit: 42
struct entry {
    const char *name;
    int (*fn)(int);
    int *counter;
    long tag;
};

static int twice(int x) { return 2 * x; }
static int neg(int x) { return -x; }
int hits;
static int local_hits;

static const struct entry e_twice __attribute__((used, section("embcc_tab"))) =
    { "twice", twice, &hits, 1 };
int unrelated = 7;   /* plain .data between the entries */
static const struct entry e_neg __attribute__((section("embcc_tab"), used)) =
    { "neg", neg, &local_hits, 2 };
const struct entry e_third __attribute__((section("embcc_tab"))) =
    { "third", twice, 0, 4 };

/* no initializer, but a named PROGBITS section: zero, and writable */
static long zeroed[4] __attribute__((section("embcc_zero")));
/* a scalar in a .rodata.* named section */
static const int magic __attribute__((section(".rodata.embcc"))) = 0x1234;

extern const struct entry __start_embcc_tab[], __stop_embcc_tab[];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(void)
{
    if (__stop_embcc_tab - __start_embcc_tab != 3)
        return 1;
    long tags = 0;
    int sum = 0;
    for (const struct entry *e = __start_embcc_tab; e < __stop_embcc_tab; e++) {
        tags |= e->tag;
        sum += e->fn(5);
        if (e->counter)
            (*e->counter)++;
        if (!streq(e->name, e->tag == 1 ? "twice" : e->tag == 2 ? "neg"
                                                               : "third"))
            return 2;
    }
    if (tags != 7 || sum != 10 - 5 + 10)
        return 3;
    if (hits != 1 || local_hits != 1 || unrelated != 7)
        return 4;
    for (int i = 0; i < 4; i++)
        if (zeroed[i] != 0)
            return 5;
    zeroed[2] = 5;
    if (zeroed[2] + (magic == 0x1234 ? 37 : 0) != 42)
        return 6;
    return 42;
}
