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
/* the same record for a Mach-O or COFF object, whose writer gives the
 * calls to functions not defined in the unit (offsets into its code);
 * NULL when --stack-info is off */
struct si_call { long off; const char *name; };
unsigned char *stackinfo_record(struct unit *u, struct ir_unit *iu,
                                const struct code *text,
                                const struct si_call *calls, int ncalls,
                                const char *src, long *len);
int stackinfo_report(int argc, char **argv);
/* the record section `which` ("stack", "sbom") of an ELF, Mach-O or
 * COFF/PE file: .debug_embcc_WHICH, or __EMBCC,__WHICH in Mach-O */
unsigned char *stackinfo_section(const unsigned char *d, long n,
                                 const char *which, long *len);
/* an ELF file's section by name (any class and byte order), or NULL */
unsigned char *stackinfo_elf_section(const unsigned char *d, long n,
                                     const char *want, long *len);
#endif
