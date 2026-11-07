/* The TriCore instruction set's tables and the code that reads a field
 * (src/arch/tricore/isa.h). */
#include "isa.h"

#include <stdlib.h>
#include <string.h>

#include "isa.inc"

const int tc_ninsn = (int)(sizeof tc_insns / sizeof tc_insns[0]);
const int tc_nsymrules = (int)(sizeof tc_symrules / sizeof tc_symrules[0]);
const int tc_nnarrows = (int)(sizeof tc_narrows / sizeof tc_narrows[0]);

int tc_fit_field(const struct tc_field *f, long long v, unsigned *w)
{
    long long x = v - f->v0;
    unsigned bits = 0;
    const struct tc_fbit *b = &tc_fbits[f->fb];
    if (f->rule == 1 && (v & 1))
        return 0;
    if (f->rule == 2) {
        int ok = 0;
        for (int k = 0; k < f->nok && !ok; k++)
            ok = tc_oks[f->ok + k] == v;
        if (!ok)
            return 0;
    }
    /* the sign bit first, then the others: two's complement, whatever
     * order the bits lie in */
    for (int k = 0; k < f->nb; k++)
        if (b[k].neg && x < 0) {
            x += 1LL << b[k].shift;
            bits |= 1u << b[k].pos;
        }
    for (int k = 0; k < f->nb; k++)
        if (!b[k].neg && x > 0 && ((x >> b[k].shift) & 1)) {
            x -= 1LL << b[k].shift;
            bits |= 1u << b[k].pos;
        }
    if (x != 0)
        return 0;
    *w |= bits;
    return 1;
}

void tc_field_range(const struct tc_field *f, long long *lo, long long *hi,
                    long long *step)
{
    const struct tc_fbit *b = &tc_fbits[f->fb];
    *lo = *hi = f->v0;
    *step = 0;
    for (int j = 0; j < f->nb; j++) {
        long long wt = 1LL << b[j].shift;
        if (b[j].neg)
            *lo -= wt;
        else
            *hi += wt;
        if (!*step || wt < *step)
            *step = wt;
    }
    if (!*step)
        *step = 1;
}

unsigned tc_field_mask(const struct tc_field *f)
{
    unsigned m = 0;
    for (int j = 0; j < f->nb; j++)
        m |= 1u << tc_fbits[f->fb + j].pos;
    return m;
}

int tc_first_of(const char *mn)
{
    int lo = 0, hi = tc_ninsn;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(tc_insns[mid].mnem, mn) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < tc_ninsn && !strcmp(tc_insns[lo].mnem, mn) ? lo : -1;
}

/* Each form's opcode bits (every bit no field holds), computed once. */
static unsigned *opmask;

static void opmasks(void)
{
    opmask = malloc((size_t)tc_ninsn * sizeof *opmask);
    if (!opmask)
        return;
    for (int i = 0; i < tc_ninsn; i++) {
        unsigned m = 0;
        for (int k = 0; k < tc_insns[i].nf; k++)
            if (tc_fields[tc_insns[i].f + k].kind != 'M')
                m |= tc_field_mask(&tc_fields[tc_insns[i].f + k]);
            else
                m |= 0xfu << tc_fields[tc_insns[i].f + k].fb;
        opmask[i] = tc_insns[i].len == 2 ? ~m & 0xffffu : ~m;
    }
}

int tc_reloc_site(unsigned long w, int len, int type, int *entry, int *field)
{
    if (!opmask)
        opmasks();
    if (!opmask)
        return -1;
    for (int r = 0; r < tc_nsymrules; r++) {
        const struct tc_symrule *sr = &tc_symrules[r];
        int e = sr->entry, k = sr->field;
        if (sr->rel != type || e < 0 || tc_insns[e].len != len)
            continue;
        if (((unsigned)w & opmask[e]) != (tc_insns[e].base & opmask[e]))
            continue;
        if (sr->map[0])
            for (k = 0; sr->map[k] && sr->map[k] - '0' != sr->field; k++)
                ;
        if (k >= tc_insns[e].nf)
            continue;
        *entry = e;
        *field = k;
        return 0;
    }
    return -1;
}
