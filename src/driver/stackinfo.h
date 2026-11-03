/* --stack-info and --stack-report (stackinfo.c): the whole program's
 * worst-case stack and cycles */
#ifndef EMBCC_STACKINFO_H
#define EMBCC_STACKINFO_H
struct elfw;
struct unit;
struct ir_unit;
struct code;
void stackinfo_set(int on);
int stackinfo_on(void);
void stackinfo_emit(struct elfw *w, struct unit *u, struct ir_unit *iu,
                    const struct code *text, int text_ndx,
                    int (*text_at)(long off, long *o),
                    int (*sec_of)(int gi, int text_ndx), const char *src);
int stackinfo_report(int argc, char **argv);
#endif
