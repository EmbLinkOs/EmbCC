/* --sbom (sbom.c): what went into a build, and the image's bill */
#ifndef EMBCC_SBOM_H
#define EMBCC_SBOM_H
struct elfw;
void sbom_set(const char *file, int embed);
int sbom_set_format(const char *f);
const char *sbom_file(void);
int sbom_embed(void);
void sbom_note_input(const char *path);
void sbom_emit_unit(struct elfw *w, const char *src, int ndeps,
                    const char *(*dep)(int), int (*dep_sys)(int));
int sbom_write(const char *image, const char *out_path);
int sbom_from(int argc, char **argv);
#endif
