/* One version string, used by --version, by the installed directory
 * layout, and by the Makefile's install rule.
 *
 * It lives in its own header because three things must agree about it:
 * what the compiler PRINTS, the directory it LOOKS IN
 * (<prefix>/lib/embcc/<version>), and the directory `make install`
 * CREATES. A copy in any two of them is a compiler that, after an
 * upgrade, quietly reads the old version's headers.
 */
#ifndef EMBCC_DRIVER_VERSION_H
#define EMBCC_DRIVER_VERSION_H

#define EMBCC_VERSION "1.0.0-m2.complete"

#endif
