/* Stable symbol identity (USRs) and interface hashes — the project graph's
 * first piece (vision §8.2) and what Level-2 incremental builds need (§21).
 * See iface.c. */
#ifndef EMBCC_IFACE_H
#define EMBCC_IFACE_H

struct outbuf;
struct unit;

/* Write what this unit provides and what it observes of elsewhere, each
 * with its USR and its interface hash. */
void iface_emit(struct outbuf *b, struct unit *u);

#endif
