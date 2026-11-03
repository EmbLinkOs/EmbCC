/* --sbom: a software bill of materials for what EmbCC builds.
 *
 * A compile with --sbom (or --sbom=FILE) records in its object, in a
 * debug section every linker keeps (.debug_embcc_sbom), what went into
 * it: the source and every header it read, each with its SHA-256 and the
 * SPDX-License-Identifier it declares (the first in its first 4 KiB), the
 * compiler and the target. The link with --sbom=FILE writes the image's
 * bill: the image (its SHA-256), the compiler that built it, and every
 * file -- sources, headers, the objects and archives linked (libc, the
 * runtime) -- with hashes and the image's GENERATED_FROM relationship to
 * each, as SPDX 2.3 JSON (--sbom-format=cyclonedx: CycloneDX 1.5).
 * `embcc --sbom-from IMAGE [-o FILE]` writes it from an image already
 * built. Paths are written as the prefix maps say (--prefix-map=), and
 * the time is SOURCE_DATE_EPOCH's when that is set, so the bill of a
 * reproducible build is reproducible too. */

#include "util.h"
#include "sbom.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../arch/target.h"
#include "../elf/write.h"
#include "../platform/platform.h"

void embdbg_sha256(const unsigned char *data, long n, unsigned char out[32]);

static const char *g_file;
static int g_cyclonedx, g_embed;
static const char **g_inputs;
static int g_ninputs;

void sbom_set(const char *file, int embed)
{
    if (file)
        g_file = file;
    if (embed)
        g_embed = 1;
}

int sbom_set_format(const char *f)
{
    if (!strcmp(f, "spdx")) g_cyclonedx = 0;
    else if (!strcmp(f, "cyclonedx")) g_cyclonedx = 1;
    else return -1;
    return 0;
}

const char *sbom_file(void) { return g_file; }
int sbom_embed(void) { return g_embed || g_file; }

void sbom_note_input(const char *path)
{
    /* not a compile-and-link's own temporary object: its source and
     * headers are in the bill already, from its records */
    if (!path || path[0] == '-' || strstr(path, ".embcc-tmp-"))
        return;
    for (int k = 0; k < g_ninputs; k++)
        if (!strcmp(g_inputs[k], path))
            return;
    g_inputs = xrealloc(g_inputs, (size_t)(g_ninputs + 1) * sizeof *g_inputs);
    g_inputs[g_ninputs++] = xstrndup(path, strlen(path));
}

static void hex(const unsigned char h[32], char out[65])
{
    for (int k = 0; k < 32; k++)
        snprintf(out + 2 * k, 3, "%02x", h[k]);
}

/* the file's SHA-256 and its declared license (or "") */
static int file_info(const char *path, char sha[65], char lic[96])
{
    long n;
    unsigned char *d = (unsigned char *)plat_read_file(path, &n);
    lic[0] = 0;
    if (!d) {
        strcpy(sha, "");
        return -1;
    }
    unsigned char h[32];
    embdbg_sha256(d, n, h);
    hex(h, sha);
    long lim = n < 4096 ? n : 4096;
    const char *key = "SPDX-License-Identifier:";
    size_t kl = strlen(key);
    for (long i = 0; i + (long)kl < lim; i++)
        if (!memcmp(d + i, key, kl)) {
            long j = i + (long)kl;
            while (j < lim && (d[j] == ' ' || d[j] == '\t'))
                j++;
            int k = 0;
            while (j < lim && k < 95 && d[j] != '\n' && d[j] != '\r' &&
                   !(d[j] == '*' && j + 1 < lim && d[j + 1] == '/'))
                lic[k++] = (char)d[j++];
            while (k && (lic[k - 1] == ' ' || lic[k - 1] == '\t'))
                k--;
            lic[k] = 0;
            break;
        }
    free(d);
    return 0;
}

void sbom_emit_unit(struct elfw *w, const char *src, int ndeps,
                    const char *(*dep)(int), int (*dep_sys)(int))
{
    if (!sbom_embed())
        return;
    struct outbuf b = { NULL, 0, 0 };
    ob_str(&b, "ESBOM 1\n");
    ob_fmt(&b, "C\tEmbCC\t%s\n", EMBCC_VERSION);
    ob_fmt(&b, "T\t%s\n", target_triple_now());
    char sha[65], lic[96];
    if (src) {
        file_info(src, sha, lic);
        ob_fmt(&b, "S\t%s\t%s\t%s\n", sha, lic, pmap_apply(src, PMAP_DEBUG));
    }
    for (int k = 0; k < ndeps; k++) {
        const char *p = dep(k);
        if (!p || (src && !strcmp(p, src)))
            continue;
        file_info(p, sha, lic);
        ob_fmt(&b, "H\t%s\t%s\t%s\t%d\n", sha, lic, pmap_apply(p, PMAP_DEBUG),
               dep_sys ? dep_sys(k) : 0);
    }
    elfw_add_section(w, ".debug_embcc_sbom", 1 /* SHT_PROGBITS */, 0, b.p,
                     (Elf64_Xword)b.n, 1);
    free(b.p);
}

/* ---- the bill ------------------------------------------------------- */

struct sfile { char *path, *sha, *lic; const char *type; };

static void json_str(FILE *o, const char *s)
{
    fputc('"', o);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(o, "\\%c", c);
        else if (c < 0x20) fprintf(o, "\\u%04x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}

static const char *timestamp(void)
{
    static char b[32];
    const char *e = getenv("SOURCE_DATE_EPOCH");
    time_t t = e && *e ? (time_t)strtoll(e, NULL, 10) : time(NULL);
    struct tm *tm = gmtime(&t);
    if (!tm || !strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", tm))
        strcpy(b, "1970-01-01T00:00:00Z");
    return b;
}

#include "stackinfo.h"

int sbom_write(const char *image, const char *out_path)
{
    long n;
    unsigned char *d = (unsigned char *)plat_read_file(image, &n);
    if (!d) {
        fprintf(stderr, "embcc: error: --sbom: cannot read '%s'\n", image);
        return 1;
    }
    unsigned char h[32];
    char isha[65];
    embdbg_sha256(d, n, h);
    hex(h, isha);
    struct sfile *fs = NULL;
    int nf = 0;
    const char *triple = target_triple_now(), *ver = EMBCC_VERSION;
    long sl = 0;
    unsigned char *s = stackinfo_elf_section(d, n, ".debug_embcc_sbom", &sl);
    /* the units' records: one ESBOM block each, concatenated */
    for (long at = 0; s && at < sl; ) {
        long e = at;
        while (e < sl && s[e] != '\n')
            e++;
        char line[4096];
        long ln = e - at < (long)sizeof line - 1 ? e - at : (long)sizeof line - 1;
        memcpy(line, s + at, (size_t)ln);
        line[ln] = 0;
        at = e + 1;
        if (line[0] == 'T' && line[1] == '\t' && !strcmp(triple,
                                                          target_triple_now()))
            triple = xstrndup(line + 2, strlen(line + 2));
        if ((line[0] != 'S' && line[0] != 'H') || line[1] != '\t')
            continue;
        char *f[5] = { 0 };
        int k = 0;
        for (char *p = line + 2; k < 5 && p; k++) {
            f[k] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (!f[2])
            continue;
        int dup = 0;
        for (int q = 0; q < nf && !dup; q++)
            dup = !strcmp(fs[q].path, f[2]) && !strcmp(fs[q].sha, f[0]);
        if (dup)
            continue;
        fs = xrealloc(fs, (size_t)(nf + 1) * sizeof *fs);
        fs[nf].sha = xstrndup(f[0], strlen(f[0]));
        fs[nf].lic = xstrndup(f[1] ? f[1] : "", f[1] ? strlen(f[1]) : 0);
        fs[nf].path = xstrndup(f[2], strlen(f[2]));
        fs[nf].type = line[0] == 'S' ? "SOURCE" : "SOURCE";
        nf++;
    }
    /* what the link took in */
    for (int k = 0; k < g_ninputs; k++) {
        char sha[65], lic[96];
        if (file_info(g_inputs[k], sha, lic) != 0)
            continue;
        size_t l = strlen(g_inputs[k]);
        fs = xrealloc(fs, (size_t)(nf + 1) * sizeof *fs);
        const char *p = pmap_apply(g_inputs[k], PMAP_DEBUG);
        fs[nf].path = xstrndup(p, strlen(p));
        fs[nf].sha = xstrndup(sha, 64);
        fs[nf].lic = xstrndup("", 0);
        fs[nf].type = l > 2 && !strcmp(g_inputs[k] + l - 2, ".a") ? "ARCHIVE"
                                                                  : "BINARY";
        nf++;
    }
    FILE *o = out_path && strcmp(out_path, "-") ? fopen(out_path, "w") : stdout;
    if (!o) {
        fprintf(stderr, "embcc: error: cannot write '%s'\n", out_path);
        free(d);
        return 1;
    }
    const char *base = strrchr(image, '/');
    base = base ? base + 1 : image;
    int firmware = !target_is_hosted();
    if (!g_cyclonedx) {
        fprintf(o, "{\n  \"spdxVersion\": \"SPDX-2.3\",\n"
                   "  \"dataLicense\": \"CC0-1.0\",\n"
                   "  \"SPDXID\": \"SPDXRef-DOCUMENT\",\n  \"name\": ");
        json_str(o, base);
        fprintf(o, ",\n  \"documentNamespace\": \"https://spdx.org/spdxdocs/"
                   "embcc-%.16s-%s\",\n", isha, isha + 16);
        fprintf(o, "  \"creationInfo\": {\n    \"created\": \"%s\",\n"
                   "    \"creators\": [\"Tool: EmbCC-%s\"]\n  },\n",
                timestamp(), ver);
        fprintf(o, "  \"packages\": [\n    {\n      \"name\": ");
        json_str(o, base);
        fprintf(o, ",\n      \"SPDXID\": \"SPDXRef-Package-Image\",\n"
                   "      \"downloadLocation\": \"NOASSERTION\",\n"
                   "      \"filesAnalyzed\": false,\n"
                   "      \"checksums\": [{\"algorithm\": \"SHA256\", "
                   "\"checksumValue\": \"%s\"}],\n"
                   "      \"licenseConcluded\": \"NOASSERTION\",\n"
                   "      \"licenseDeclared\": \"NOASSERTION\",\n"
                   "      \"copyrightText\": \"NOASSERTION\",\n"
                   "      \"primaryPackagePurpose\": \"%s\",\n"
                   "      \"comment\": \"built for %s\"\n    },\n",
                isha, firmware ? "FIRMWARE" : "APPLICATION", triple);
        fprintf(o, "    {\n      \"name\": \"EmbCC\",\n"
                   "      \"SPDXID\": \"SPDXRef-Package-EmbCC\",\n"
                   "      \"versionInfo\": \"%s\",\n"
                   "      \"downloadLocation\": \"NOASSERTION\",\n"
                   "      \"filesAnalyzed\": false,\n"
                   "      \"licenseConcluded\": \"NOASSERTION\",\n"
                   "      \"licenseDeclared\": \"NOASSERTION\",\n"
                   "      \"copyrightText\": \"NOASSERTION\",\n"
                   "      \"primaryPackagePurpose\": \"APPLICATION\"\n"
                   "    }\n  ],\n  \"files\": [", ver);
        for (int k = 0; k < nf; k++) {
            fprintf(o, "%s\n    {\n      \"fileName\": ", k ? "," : "");
            char nm[4096];
            snprintf(nm, sizeof nm, "%s%s", fs[k].path[0] == '/' ||
                     !strncmp(fs[k].path, "./", 2) ? "" : "./", fs[k].path);
            json_str(o, nm);
            fprintf(o, ",\n      \"SPDXID\": \"SPDXRef-File-%d\",\n"
                       "      \"fileTypes\": [\"%s\"],\n"
                       "      \"checksums\": [{\"algorithm\": \"SHA256\", "
                       "\"checksumValue\": \"%s\"}],\n"
                       "      \"licenseConcluded\": \"NOASSERTION\",\n"
                       "      \"licenseInfoInFiles\": [",
                    k + 1, fs[k].type, fs[k].sha);
            json_str(o, fs[k].lic[0] ? fs[k].lic : "NOASSERTION");
            fprintf(o, "],\n      \"copyrightText\": \"NOASSERTION\"\n    }");
        }
        fprintf(o, "\n  ],\n  \"relationships\": [\n"
                   "    {\"spdxElementId\": \"SPDXRef-DOCUMENT\", "
                   "\"relationshipType\": \"DESCRIBES\", "
                   "\"relatedSpdxElement\": \"SPDXRef-Package-Image\"},\n"
                   "    {\"spdxElementId\": \"SPDXRef-Package-EmbCC\", "
                   "\"relationshipType\": \"BUILD_TOOL_OF\", "
                   "\"relatedSpdxElement\": \"SPDXRef-Package-Image\"}");
        for (int k = 0; k < nf; k++)
            fprintf(o, ",\n    {\"spdxElementId\": \"SPDXRef-Package-Image\", "
                       "\"relationshipType\": \"GENERATED_FROM\", "
                       "\"relatedSpdxElement\": \"SPDXRef-File-%d\"}", k + 1);
        fprintf(o, "\n  ]\n}\n");
    } else {
        /* CycloneDX 1.5: the serial number from the image's hash, so the
         * same image has the same one */
        fprintf(o, "{\n  \"bomFormat\": \"CycloneDX\",\n"
                   "  \"specVersion\": \"1.5\",\n"
                   "  \"serialNumber\": \"urn:uuid:%.8s-%.4s-5%.3s-8%.3s-%.12s\",\n"
                   "  \"version\": 1,\n"
                   "  \"metadata\": {\n    \"timestamp\": \"%s\",\n"
                   "    \"tools\": {\"components\": [{\"type\": \"application\", "
                   "\"name\": \"EmbCC\", \"version\": \"%s\"}]},\n"
                   "    \"component\": {\"type\": \"%s\", \"bom-ref\": \"image\", "
                   "\"name\": ",
                isha, isha + 8, isha + 13, isha + 17, isha + 20, timestamp(),
                ver, firmware ? "firmware" : "application");
        json_str(o, base);
        fprintf(o, ", \"hashes\": [{\"alg\": \"SHA-256\", \"content\": \"%s\"}], "
                   "\"properties\": [{\"name\": \"embcc:target\", \"value\": ",
                isha);
        json_str(o, triple);
        fprintf(o, "}]}\n  },\n  \"components\": [");
        for (int k = 0; k < nf; k++) {
            fprintf(o, "%s\n    {\"type\": \"file\", \"bom-ref\": \"file-%d\", "
                       "\"name\": ", k ? "," : "", k + 1);
            json_str(o, fs[k].path);
            fprintf(o, ", \"hashes\": [{\"alg\": \"SHA-256\", \"content\": "
                       "\"%s\"}]", fs[k].sha);
            if (fs[k].lic[0]) {
                fprintf(o, ", \"licenses\": [{\"expression\": ");
                json_str(o, fs[k].lic);
                fprintf(o, "}]");
            }
            fprintf(o, "}");
        }
        fprintf(o, "\n  ],\n  \"dependencies\": [{\"ref\": \"image\", "
                   "\"dependsOn\": [");
        for (int k = 0; k < nf; k++)
            fprintf(o, "%s\"file-%d\"", k ? ", " : "", k + 1);
        fprintf(o, "]}]\n}\n");
    }
    if (o != stdout)
        fclose(o);
    for (int k = 0; k < nf; k++) {
        free(fs[k].path);
        free(fs[k].sha);
        free(fs[k].lic);
    }
    free(fs);
    free(d);
    return 0;
}

int sbom_from(int argc, char **argv)
{
    const char *image = NULL, *out = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strncmp(argv[i], "--sbom-format=", 14)) {
            if (sbom_set_format(argv[i] + 14) != 0) {
                fprintf(stderr, "embcc: error: --sbom-format=%s: spdx or "
                                "cyclonedx\n", argv[i] + 14);
                return 1;
            }
        } else if (argv[i][0] != '-')
            image = argv[i];
    }
    if (!image) {
        fprintf(stderr, "embcc: error: --sbom-from IMAGE: which image\n");
        return 1;
    }
    return sbom_write(image, out);
}
