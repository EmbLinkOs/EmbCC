/* Small leftovers of glibc's: ulimit, the mount table (<mntent.h>),
 * Ethernet addresses (<netinet/ether.h>), the classful inet_lnaof,
 * inet_netof and inet_makeaddr, canonicalize_file_name, euidaccess,
 * lchmod, getpt, group_member and wcstoq. */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <netinet/ether.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ulimit.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "posix.h"

/* ---- ulimit: the file size limit, in 512-byte blocks ----------------------------- */

WEAK long ulimit(int cmd, ...)
{
    struct rlimit r;
    if (cmd == UL_GETFSIZE) {
        if (getrlimit(RLIMIT_FSIZE, &r))
            return -1;
        if (r.rlim_cur == RLIM_INFINITY || r.rlim_cur / 512 > (rlim_t)LONG_MAX)
            return LONG_MAX;
        return (long)(r.rlim_cur / 512);
    }
    if (cmd == UL_SETFSIZE) {
        va_list ap;
        va_start(ap, cmd);
        long v = va_arg(ap, long);
        va_end(ap);
        if (v < 0) {
            errno = EINVAL;
            return -1;
        }
        r.rlim_cur = r.rlim_max = (rlim_t)v * 512;
        return setrlimit(RLIMIT_FSIZE, &r) ? -1 : v;
    }
    errno = EINVAL;
    return -1;
}

/* ---- the mount table ------------------------------------------------------------- */

WEAK FILE *setmntent(const char *file, const char *mode)
{
    return fopen(file, mode);
}

WEAK int endmntent(FILE *f)
{
    if (f)
        fclose(f);
    return 1;
}

/* a field's \ooo escapes (\040 for a space, as the kernel writes it) */
static void unescape_oct(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' &&
            s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *d++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

WEAK struct mntent *getmntent_r(FILE *restrict f, struct mntent *restrict m,
                                char *restrict buf, int n)
{
    while (fgets(buf, n, f)) {
        char *p = buf;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || !*p)
            continue;
        char *field[6] = { 0 };
        int k = 0;
        for (char *t = strtok(p, " \t\n"); t && k < 6; t = strtok(0, " \t\n"))
            field[k++] = t;
        if (k < 4)
            continue;
        for (int i = 0; i < 4; i++)
            unescape_oct(field[i]);
        m->mnt_fsname = field[0];
        m->mnt_dir = field[1];
        m->mnt_type = field[2];
        m->mnt_opts = field[3];
        m->mnt_freq = k > 4 ? atoi(field[4]) : 0;
        m->mnt_passno = k > 5 ? atoi(field[5]) : 0;
        return m;
    }
    return 0;
}

WEAK struct mntent *getmntent(FILE *f)
{
    static struct mntent m;
    static char buf[4096];
    return getmntent_r(f, &m, buf, sizeof buf);
}

static void put_escaped(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\')
            fprintf(f, "\\%03o", (unsigned char)*s);
        else
            fputc(*s, f);
    }
}

WEAK int addmntent(FILE *restrict f, const struct mntent *restrict m)
{
    if (fseek(f, 0, SEEK_END))
        return 1;
    flockfile(f);
    put_escaped(f, m->mnt_fsname);
    fputc(' ', f);
    put_escaped(f, m->mnt_dir);
    fputc(' ', f);
    put_escaped(f, m->mnt_type);
    fputc(' ', f);
    put_escaped(f, m->mnt_opts);
    int r = fprintf(f, " %d %d\n", m->mnt_freq, m->mnt_passno) < 0;
    funlockfile(f);
    return r || fflush(f);
}

/* opt as a whole option of the comma-separated list: where it starts */
WEAK char *hasmntopt(const struct mntent *m, const char *opt)
{
    size_t n = strlen(opt);
    for (char *p = m->mnt_opts; p && *p;) {
        if (!strncmp(p, opt, n) && (p[n] == 0 || p[n] == ',' || p[n] == '='))
            return p;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return 0;
}

/* ---- Ethernet addresses ------------------------------------------------------------ */

WEAK char *ether_ntoa_r(const struct ether_addr *a, char *buf)
{
    const uint8_t *o = a->ether_addr_octet;
    sprintf(buf, "%x:%x:%x:%x:%x:%x", o[0], o[1], o[2], o[3], o[4], o[5]);
    return buf;
}

WEAK char *ether_ntoa(const struct ether_addr *a)
{
    static char buf[18];
    return ether_ntoa_r(a, buf);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* six bytes of one or two hex digits, colon-separated; where it stopped */
static const char *parse_ether(const char *s, struct ether_addr *a)
{
    for (int i = 0; i < 6; i++) {
        int h = hexval((unsigned char)*s);
        if (h < 0)
            return 0;
        s++;
        int l = hexval((unsigned char)*s);
        if (l >= 0) {
            h = h * 16 + l;
            s++;
        }
        a->ether_addr_octet[i] = (uint8_t)h;
        if (i < 5) {
            if (*s != ':')
                return 0;
            s++;
        }
    }
    return s;
}

WEAK struct ether_addr *ether_aton_r(const char *s, struct ether_addr *a)
{
    const char *end = parse_ether(s, a);
    return end && !*end ? a : 0;
}

WEAK struct ether_addr *ether_aton(const char *s)
{
    static struct ether_addr a;
    return ether_aton_r(s, &a);
}

/* "address name" (a /etc/ethers line): 0, or -1 */
WEAK int ether_line(const char *line, struct ether_addr *a, char *host)
{
    while (isspace((unsigned char)*line))
        line++;
    const char *p = parse_ether(line, a);
    if (!p || !isspace((unsigned char)*p))
        return -1;
    while (isspace((unsigned char)*p))
        p++;
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && p[n] != '#')
        n++;
    if (!n)
        return -1;
    memcpy(host, p, n);
    host[n] = 0;
    return 0;
}

static int ethers(const char *want_host, const struct ether_addr *want_addr,
                  char *host, struct ether_addr *addr)
{
    FILE *f = fopen("/etc/ethers", "re");
    if (!f)
        return -1;
    char line[256], name[256];
    struct ether_addr a;
    int r = -1;
    while (r && fgets(line, sizeof line, f)) {
        if (ether_line(line, &a, name))
            continue;
        if (want_host && !strcmp(name, want_host)) {
            *addr = a;
            r = 0;
        } else if (want_addr && !memcmp(&a, want_addr, sizeof a)) {
            strcpy(host, name);
            r = 0;
        }
    }
    fclose(f);
    return r;
}

WEAK int ether_ntohost(char *host, const struct ether_addr *a)
{
    return ethers(0, a, host, 0);
}

WEAK int ether_hostton(const char *host, struct ether_addr *a)
{
    return ethers(host, 0, 0, a);
}

/* ---- the classful address calls ------------------------------------------------------ */

WEAK in_addr_t inet_lnaof(struct in_addr in)
{
    uint32_t i = ntohl(in.s_addr);
    if (!(i & 0x80000000u))
        return i & 0x00ffffffu;
    if ((i & 0xc0000000u) == 0x80000000u)
        return i & 0x0000ffffu;
    return i & 0x000000ffu;
}

WEAK in_addr_t inet_netof(struct in_addr in)
{
    uint32_t i = ntohl(in.s_addr);
    if (!(i & 0x80000000u))
        return (i & 0xff000000u) >> 24;
    if ((i & 0xc0000000u) == 0x80000000u)
        return (i & 0xffff0000u) >> 16;
    return (i & 0xffffff00u) >> 8;
}

WEAK struct in_addr inet_makeaddr(in_addr_t net, in_addr_t host)
{
    uint32_t a;
    if (net < 128)
        a = net << 24 | (host & 0x00ffffffu);
    else if (net < 65536)
        a = net << 16 | (host & 0x0000ffffu);
    else if (net < 16777216)
        a = net << 8 | (host & 0x000000ffu);
    else
        a = net | host;
    struct in_addr r;
    r.s_addr = htonl(a);
    return r;
}

/* ---- the rest ------------------------------------------------------------------------- */

WEAK char *canonicalize_file_name(const char *path)
{
    return realpath(path, 0);
}

/* access by the effective ids, as open would check */
WEAK int euidaccess(const char *path, int mode)
{
    if (getuid() == geteuid() && getgid() == getegid())
        return access(path, mode);
    return faccessat(AT_FDCWD, path, mode, AT_EACCESS);
}

WEAK int eaccess(const char *path, int mode) { return euidaccess(path, mode); }

WEAK int lchmod(const char *path, mode_t mode)
{
    return fchmodat(AT_FDCWD, path, mode, AT_SYMLINK_NOFOLLOW);
}

WEAK int getpt(void) { return posix_openpt(O_RDWR | O_NOCTTY); }

/* in the supplementary groups -- only those, as glibc's asks */
WEAK int group_member(gid_t gid)
{
    gid_t g[64];
    int n = getgroups(64, g);
    for (int i = 0; i < n; i++)
        if (g[i] == gid)
            return 1;
    return 0;
}

WEAK long long wcstoq(const wchar_t *restrict s, wchar_t **restrict end, int base)
{
    return wcstoll(s, end, base);
}

WEAK unsigned long long wcstouq(const wchar_t *restrict s, wchar_t **restrict end,
                                int base)
{
    return wcstoull(s, end, base);
}
