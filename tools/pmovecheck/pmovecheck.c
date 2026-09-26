/* ra_parallel_move, checked by SIMULATION over every small case.
 *
 * The routine's whole job is an ordering, and an ordering is exactly the
 * kind of thing that looks right and is wrong for one shape. So this
 * does not inspect the sequence it produces: it EXECUTES it against a
 * model register file and requires every destination to end up holding
 * the value its source held BEFORE the move began. That is the property
 * a parallel move has, stated directly.
 *
 * Exhaustive over every mapping of up to 5 destinations drawn from 5
 * registers -- 5^5 = 3125 source assignments per destination count, so
 * every chain, every cycle, every swap, and every combination of the
 * two is covered by construction rather than by a list someone thought
 * of. Three sites in the backends depend on this being right, and the
 * first attempt at them got three separate bugs out of open-coding it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/regalloc.h"

#define NREG 6          /* r0..r4 usable, r5 the scratch */
#define SCRATCH 5

static int checked, failed;

static void one(const int *dst, const int *src, int n)
{
    int before[NREG], reg[NREG];
    int od[32], os[32];
    int nout, i;

    /* Distinct, recognisable starting values. */
    for (i = 0; i < NREG; i++) before[i] = reg[i] = 100 + i;

    nout = ra_parallel_move(dst, src, n, SCRATCH, od, os, 32);
    if (nout < 0) {
        /* Only legitimate for a repeated destination, which the caller
         * below never generates. */
        printf("FAIL n=%d: refused a well-formed move\n", n);
        failed++;
        return;
    }
    for (i = 0; i < nout; i++) {
        if (od[i] < 0 || od[i] >= NREG || os[i] < 0 || os[i] >= NREG) {
            printf("FAIL n=%d: move %d touches a register outside the file\n",
                   n, i);
            failed++;
            return;
        }
        reg[od[i]] = reg[os[i]];
    }
    for (i = 0; i < n; i++) {
        if (reg[dst[i]] != before[src[i]]) {
            printf("FAIL n=%d: r%d holds %d, wanted r%d's old %d  (in %d moves)\n",
                   n, dst[i], reg[dst[i]], src[i], before[src[i]], nout);
            for (int k = 0; k < n; k++)
                printf("      want r%d <- r%d\n", dst[k], src[k]);
            for (int k = 0; k < nout; k++)
                printf("      emit r%d <- r%d\n", od[k], os[k]);
            failed++;
            return;
        }
    }
    /* A move must not disturb a register nobody asked it to write --
     * except the scratch, which is the caller's to lose. */
    for (i = 0; i < NREG; i++) {
        int is_dst = (i == SCRATCH);
        for (int k = 0; k < n; k++) if (dst[k] == i) is_dst = 1;
        if (!is_dst && reg[i] != before[i]) {
            printf("FAIL n=%d: clobbered r%d, which is not a destination\n",
                   n, i);
            failed++;
            return;
        }
    }
    checked++;
}

int main(void)
{
    int dst[5], src[5];
    /* Destinations are distinct (the routine rejects a repeat, and a
     * caller that produces one has a different bug); sources range over
     * everything, so chains, cycles and swaps all appear. */
    for (int n = 1; n <= 5; n++) {
        for (int i = 0; i < n; i++) dst[i] = i;
        int total = 1;
        for (int i = 0; i < n; i++) total *= 5;
        for (int code = 0; code < total; code++) {
            int c = code;
            for (int i = 0; i < n; i++) { src[i] = c % 5; c /= 5; }
            one(dst, src, n);
        }
    }
    /* And a few with scattered, non-contiguous destinations, which is
     * what a real argument setup looks like when some arguments are
     * already in place. */
    {
        int d2[3] = { 4, 1, 3 }, s2[3] = { 1, 3, 4 };      /* a 3-cycle */
        one(d2, s2, 3);
        int d3[2] = { 2, 0 }, s3[2] = { 0, 2 };            /* a swap */
        one(d3, s3, 2);
        int d4[3] = { 0, 1, 2 }, s4[3] = { 3, 3, 3 };      /* one fan-out */
        one(d4, s4, 3);
    }
    /* A repeated destination must be REFUSED, not ordered. */
    {
        int d[2] = { 1, 1 }, s[2] = { 2, 3 }, od[8], os[8];
        if (ra_parallel_move(d, s, 2, SCRATCH, od, os, 8) >= 0) {
            printf("FAIL: accepted two values into one register\n");
            failed++;
        } else {
            checked++;
        }
    }
    /* So must a result that would not fit the caller's buffer. */
    {
        int d[2] = { 0, 1 }, s[2] = { 1, 0 }, od[8], os[8];
        if (ra_parallel_move(d, s, 2, SCRATCH, od, os, 1) >= 0) {
            printf("FAIL: wrote a swap into a one-entry buffer\n");
            failed++;
        } else {
            checked++;
        }
    }

    if (failed) {
        printf("%d of %d parallel moves are wrong\n", failed, checked + failed);
        return 1;
    }
    printf("%d parallel moves each leave every destination holding its "
           "source's old value\n", checked);
    return 0;
}
