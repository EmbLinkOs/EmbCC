/* `embld --doctor`: why the link failed, per undefined symbol, in terms of
 * what the inputs hold (tools/embld/doctor.c, docs/tools/diagnostics.md T6). */
#ifndef EMBCC_EMBLD_DOCTOR_H
#define EMBCC_EMBLD_DOCTOR_H

/* Reports every undefined symbol across the inputs (objects and archives),
 * each with what can be said about it. 0 if nothing is undefined. */
int doctor_run(char **inputs, int n);

#endif
