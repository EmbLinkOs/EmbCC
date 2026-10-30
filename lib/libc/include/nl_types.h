/* <nl_types.h> -- message catalogs. There are none to open: catopen
 * fails with ENOENT (no catalog), and catgets hands back the message the
 * program gave as its default, which is what a program with no
 * translation shows. */
#ifndef _NL_TYPES_H
#define _NL_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_item;
typedef void *nl_catd;

#define NL_SETD       1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *name, int flag);
char   *catgets(nl_catd cat, int set, int number, const char *message);
int     catclose(nl_catd cat);

#ifdef __cplusplus
}
#endif

#endif
