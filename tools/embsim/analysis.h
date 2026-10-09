/* analysis.h -- what EmbSim measures of a run beyond its counts: the
 * code it covered (coverage.c), where its time went (profile.c), how
 * deep its stacks went (stack.c), the faults it took (fault.c), and the
 * record of its inputs that replays it (record.c).
 *
 * All of them watch the run one step at a time through an_step, which
 * the run loop calls in place of the core's step when any is on, so a
 * run with none of them is the loop it always was. an_step notes the pc
 * and the counts before the step and, after it, gives each analysis the
 * instruction that ran and what it cost; and it follows the program's
 * calls and returns, as the core executes them, on a shadow stack of
 * frames (the profile's call paths, a function's frame size, the
 * backtrace when there is no CFI). The cores tell it of exception entry
 * and return (an_exc_entry, an_exc_return), which no instruction shows. */
#ifndef EMBSIM_ANALYSIS_H
#define EMBSIM_ANALYSIS_H

#include "image.h"
#include "sim.h"

enum { AN_ARM, AN_RV, AN_AVR };

/* a frame of the shadow stack: a call or an exception's entry */
struct an_frame {
    u32 ret;                        /* where it returns to */
    u32 sp;                         /* sp at its first instruction */
    int node;                       /* its call path (struct an_node) */
    int exc;                        /* an exception's (its number + 1) */
    u32 vec;                        /* an exception's: the vector's jump */
};

/* a call path: a function, called along its parent's path. The counts
 * are the instructions and cycles run in the function itself along this
 * path; each step's go to exactly one path. */
struct an_node {
    int parent, fn;                 /* fn: m->fn's index, or -1: no symbol */
    int child, sib;                 /* the paths it calls, as a list */
    u64 insns, cycles, calls;
};

/* what a step did besides running its instruction */
enum { EV_ENTRY = 1, EV_RETURN };

struct analysis {
    struct sim *s;
    struct image *img;
    int arch;

    /* ---- the step ---- */
    u32 pc0;                        /* the instruction's address */
    int kind, size;                 /* its kind (a call, a return), its bytes */
    struct { int what; u32 n, ret; } ev[4];   /* the step's exceptions */
    int nev;

    /* ---- the shadow stack (calls on) ---- */
    int calls;
    struct an_frame *fr;
    int nfr, capfr;
    struct an_node *node;
    int nnode, capnode;
    int cur_base, cur_fn, cur_node;   /* the path the pc is on, cached */
    u32 fn_lo, fn_hi;               /* the cached function's range */
    int fn_idx;

    /* ---- coverage (coverage.c) ---- */
    const char *cov_path;
    int cov_lcov;
    u64 *cov[IMG_CODE];             /* a count per halfword of code */

    /* ---- the profile (profile.c) ---- */
    int prof;
    const char *prof_path;          /* 0: stderr */
    int prof_fmt;                   /* 0 the report; collapsed by 1 cycles, 2 insns */
};

/* the analyses on for this run (from main.c's options), after sim_load;
 * 0 when none is on */
struct analysis *an_create(struct sim *s, int calls);
/* the run is over: each analysis writes its report (an_step and the
 * cores' an_exc_entry and an_exc_return are sim.h's) */
void an_finish(struct sim *s);

/* the function holding pc (an index into img->fn, or -1), cached */
int an_fn(struct analysis *a, u32 pc);
/* the stack pointer, as an address on the bus */
u32 an_sp(struct analysis *a);

/* coverage.c */
void cov_init(struct analysis *a);
void cov_report(struct analysis *a);

/* profile.c */
void prof_report(struct analysis *a);

#endif
