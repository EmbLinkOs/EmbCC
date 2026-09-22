/* The platform layer — every host interaction EmbCC performs.
 *
 * Vision §16: "Porting EmbCC to a new host means implementing this layer and
 * nothing else." Above this header, no stage knows which filesystem backs a
 * file, whether the host has an environment, or where diagnostics go. There
 * are no `#ifdef __APPLE__` above it and must never be (§5.1).
 *
 * The layer is deliberately tiny, because EmbCC's host needs are: read a
 * file, write a file, ask the environment a question, write to the console.
 * EmbLinkOS has no fork/exec (ARCHITECTURE §1), so there is no process API
 * here and there must never be one — a design that spawned `as` or `ld`
 * could not be hosted on the target at all.
 *
 * ---- Sources are different ----
 *
 * Reading a SOURCE file is not the same as reading a data file, because an
 * editor holds sources that are not on disk yet (§7). Source bytes therefore
 * come through `src_read`, which a language server can redirect to its own
 * buffers by installing a provider. Everything else — objects, archives,
 * dependency files, the output — is an ordinary file and goes through
 * `plat_read_file` / `plat_write_file`.
 */
#ifndef EMBCC_PLATFORM_H
#define EMBCC_PLATFORM_H

#include <stddef.h>

/* ---- files ----
 *
 * plat_read_file returns a NUL-terminated buffer the caller frees, and sets
 * *len to the byte count (not counting the NUL, which is there so text can
 * be scanned as a string). NULL means it could not be read; the caller
 * decides whether that is fatal, because a missing include is not a missing
 * object file.
 */
char *plat_read_file(const char *path, long *len);

/* 0 on success, -1 on failure. `len` bytes, exactly; no text translation. */
int plat_write_file(const char *path, const void *data, size_t len);

/* Whether a path can be opened for reading. Used to choose between
 * candidates (an include directory search), never as a guarantee — the
 * answer can be stale by the time the file is opened. */
int plat_file_exists(const char *path);

/* ---- environment ----
 *
 * Returns NULL where the host has no environment at all, which is the
 * EmbLinkOS case; callers must treat "not set" and "no environment" the
 * same way.
 */
const char *plat_getenv(const char *name);

/* ---- where this program is ----
 *
 * The absolute path of the running executable, or NULL where the host
 * cannot say. An installed compiler needs this to find the files that
 * were installed beside it -- its headers, its per-target libraries,
 * its linker script -- without a prefix baked in at build time, which
 * is what makes an unpacked tarball work from any directory and what
 * lets the same binary run from the build tree.
 *
 * This is a question about the file system, not a process API: it asks
 * where a file IS, not to run one. The rule above -- that EmbLinkOS has
 * no fork/exec and this layer must never grow a process API -- is
 * untouched, and on a host with no answer this returns NULL and the
 * caller falls back to the paths it was given.
 *
 * The returned string is owned by the platform layer and lives until
 * the process exits; callers must not free it.
 */
const char *plat_self_path(void);

/* ---- the source provider (§7) ----
 *
 * `src_read` is how the frontend — and only the frontend — obtains source
 * bytes. By default it is plat_read_file. A language server installs its own
 * so that an unsaved buffer is compiled exactly as the user sees it, with no
 * temporary file and no window in which the two disagree.
 *
 * The provider returns a buffer the CALLER frees, so an in-memory provider
 * hands back a copy. `len` may be NULL when the caller does not need it.
 */
typedef char *(*src_provider_fn)(const char *path, long *len, void *ctx);

void src_set_provider(src_provider_fn fn, void *ctx);
char *src_read(const char *path, long *len);

/* Whether a source path is available, asked through the provider — an
 * editor buffer exists even when the file on disk does not. */
int src_exists(const char *path);

#endif
