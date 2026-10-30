/* Network interfaces: <net/if.h>'s names and indexes, and getifaddrs.
 *
 * Linux's interface ioctls on a datagram socket answer for one interface
 * at a time; /proc/net/dev names them all, and /proc/net/if_inet6 has
 * their IPv6 addresses. getifaddrs lists, as glibc's does, a link-level
 * (AF_PACKET) entry for every interface, then each IPv4 address, then
 * each IPv6 one. Where those files and requests do not exist there are
 * no interfaces to report: an empty list, and if_nametoindex 0 (ENODEV).
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "posix.h"

static int ctl_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        s = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    return s;
}

/* one interface request, on a fresh socket; 0 or -1 */
static int ifreq(unsigned long req, struct ifreq *r)
{
    int s = ctl_socket();
    if (s < 0)
        return -1;
    int e = ioctl(s, req, r);
    int saved = errno;
    close(s);
    errno = saved;
    return e;
}

WEAK unsigned if_nametoindex(const char *name)
{
    struct ifreq r;
    memset(&r, 0, sizeof r);
    if (strlen(name) >= IFNAMSIZ) {
        errno = ENODEV;
        return 0;
    }
    strcpy(r.ifr_name, name);
    if (ifreq(SIOCGIFINDEX, &r) < 0) {
        if (errno != ENODEV)
            errno = ENODEV;
        return 0;
    }
    return (unsigned)r.ifr_ifindex;
}

WEAK char *if_indextoname(unsigned index, char *name)
{
    struct ifreq r;
    memset(&r, 0, sizeof r);
    r.ifr_ifindex = (int)index;
    if (ifreq(SIOCGIFNAME, &r) < 0) {
        errno = ENXIO;
        return 0;
    }
    memcpy(name, r.ifr_name, IFNAMSIZ);
    name[IFNAMSIZ - 1] = 0;
    return name;
}

/* every interface's name, from /proc/net/dev: n of them into names */
static int names(char (*names)[IFNAMSIZ], int max)
{
    FILE *f = fopen("/proc/net/dev", "re");
    if (!f)
        return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;          /* the two heading lines */
        *colon = 0;
        char *p = line;
        while (*p == ' ')
            p++;
        if (!*p || strlen(p) >= IFNAMSIZ)
            continue;
        strcpy(names[n++], p);
    }
    fclose(f);
    return n;
}

#define MAXIF 64

/* without /proc: the names the kernel gives indexes 1 up, as far as a
 * run of 16 indexes with no interface (interfaces come and go and leave
 * gaps, but not that many) */
static int names_by_index(char (*nm)[IFNAMSIZ], int max)
{
    int n = 0, misses = 0;
    for (unsigned i = 1; n < max && misses < 16; i++) {
        if (if_indextoname(i, nm[n])) {
            n++;
            misses = 0;
        } else {
            misses++;
        }
    }
    return n;
}

WEAK struct if_nameindex *if_nameindex(void)
{
    char nm[MAXIF][IFNAMSIZ];
    int n = names(nm, MAXIF);
    if (!n)
        n = names_by_index(nm, MAXIF);
    struct if_nameindex *v = calloc((size_t)n + 1, sizeof *v + IFNAMSIZ);
    if (!v)
        return 0;
    char *store = (char *)(v + n + 1);
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned idx = if_nametoindex(nm[i]);
        if (!idx)
            continue;
        v[k].if_index = idx;
        v[k].if_name = strcpy(store + k * IFNAMSIZ, nm[i]);
        k++;
    }
    return v;                  /* ends with {0, NULL} */
}

WEAK void if_freenameindex(struct if_nameindex *list) { free(list); }

/* ---- getifaddrs ------------------------------------------------------------------ */

/* one entry, its name and addresses in the same allocation */
struct entry {
    struct ifaddrs ifa;
    char name[IFNAMSIZ];
    struct sockaddr_storage addr, mask, extra;
};

static struct entry *add(struct ifaddrs **head, struct ifaddrs **tail,
                         const char *name, unsigned flags)
{
    struct entry *e = calloc(1, sizeof *e);
    if (!e)
        return 0;
    strncpy(e->name, name, IFNAMSIZ - 1);
    e->ifa.ifa_name = e->name;
    e->ifa.ifa_flags = flags;
    if (*tail)
        (*tail)->ifa_next = &e->ifa;
    else
        *head = &e->ifa;
    *tail = &e->ifa;
    return e;
}

static int hexbyte(const char *p)
{
    unsigned v;
    return sscanf(p, "%2x", &v) == 1 ? (int)v : -1;
}

WEAK int getifaddrs(struct ifaddrs **list)
{
    char nm[MAXIF][IFNAMSIZ];
    struct ifaddrs *head = 0, *tail = 0;
    int n = names(nm, MAXIF);
    if (!n)
        n = names_by_index(nm, MAXIF);
    unsigned flags[MAXIF];

    /* link level: one each, with the hardware address */
    for (int i = 0; i < n; i++) {
        struct ifreq r;
        memset(&r, 0, sizeof r);
        strcpy(r.ifr_name, nm[i]);
        flags[i] = ifreq(SIOCGIFFLAGS, &r) == 0 ? (unsigned short)r.ifr_flags : 0;
        struct entry *e = add(&head, &tail, nm[i], flags[i]);
        if (!e)
            goto nomem;
        struct sockaddr_ll *ll = (struct sockaddr_ll *)&e->addr;
        ll->sll_family = AF_PACKET;
        ll->sll_ifindex = (int)if_nametoindex(nm[i]);
        memset(&r, 0, sizeof r);
        strcpy(r.ifr_name, nm[i]);
        if (ifreq(SIOCGIFHWADDR, &r) == 0) {
            ll->sll_hatype = (unsigned short)r.ifr_hwaddr.sa_family;
            ll->sll_halen = 6;
            memcpy(ll->sll_addr, r.ifr_hwaddr.sa_data, 6);
        }
        e->ifa.ifa_addr = (struct sockaddr *)ll;
    }

    /* IPv4: the address each interface has, with its mask and broadcast
     * or point-to-point peer */
    for (int i = 0; i < n; i++) {
        struct ifreq r;
        memset(&r, 0, sizeof r);
        strcpy(r.ifr_name, nm[i]);
        if (ifreq(SIOCGIFADDR, &r) < 0 || r.ifr_addr.sa_family != AF_INET)
            continue;
        struct entry *e = add(&head, &tail, nm[i], flags[i]);
        if (!e)
            goto nomem;
        memcpy(&e->addr, &r.ifr_addr, sizeof(struct sockaddr_in));
        e->ifa.ifa_addr = (struct sockaddr *)&e->addr;
        memset(&r, 0, sizeof r);
        strcpy(r.ifr_name, nm[i]);
        if (ifreq(SIOCGIFNETMASK, &r) == 0) {
            memcpy(&e->mask, &r.ifr_netmask, sizeof(struct sockaddr_in));
            e->mask.ss_family = AF_INET;
            e->ifa.ifa_netmask = (struct sockaddr *)&e->mask;
        }
        unsigned long req = flags[i] & IFF_BROADCAST ? SIOCGIFBRDADDR
                            : flags[i] & IFF_POINTOPOINT ? SIOCGIFDSTADDR : 0;
        memset(&r, 0, sizeof r);
        strcpy(r.ifr_name, nm[i]);
        if (req && ifreq(req, &r) == 0) {
            memcpy(&e->extra, &r.ifr_broadaddr, sizeof(struct sockaddr_in));
            e->ifa.ifa_broadaddr = (struct sockaddr *)&e->extra;
        }
    }

    /* IPv6, from the kernel's table: address, index, prefix, scope,
     * flags, name */
    FILE *f = fopen("/proc/net/if_inet6", "re");
    if (f) {
        char a[33], name[IFNAMSIZ + 1];
        unsigned idx, plen, scope, fl;
        while (fscanf(f, "%32s %x %x %x %x %16s", a, &idx, &plen, &scope, &fl,
                      name) == 6) {
            unsigned ifl = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(nm[i], name))
                    ifl = flags[i];
            struct entry *e = add(&head, &tail, name, ifl);
            if (!e) {
                fclose(f);
                goto nomem;
            }
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&e->addr;
            struct sockaddr_in6 *m6 = (struct sockaddr_in6 *)&e->mask;
            s6->sin6_family = m6->sin6_family = AF_INET6;
            for (int b = 0; b < 16; b++)
                s6->sin6_addr.s6_addr[b] = (unsigned char)hexbyte(a + 2 * b);
            if ((s6->sin6_addr.s6_addr[0] == 0xfe &&
                 (s6->sin6_addr.s6_addr[1] & 0xc0) == 0x80))
                s6->sin6_scope_id = idx;           /* link-local */
            for (unsigned b = 0; b < plen && b < 128; b++)
                m6->sin6_addr.s6_addr[b / 8] |= (unsigned char)(0x80 >> (b % 8));
            e->ifa.ifa_addr = (struct sockaddr *)s6;
            e->ifa.ifa_netmask = (struct sockaddr *)m6;
        }
        fclose(f);
    }
    *list = head;
    return 0;

nomem:
    freeifaddrs(head);
    errno = ENOMEM;
    return -1;
}

WEAK void freeifaddrs(struct ifaddrs *list)
{
    while (list) {
        struct ifaddrs *next = list->ifa_next;
        free(list);            /* the entry that holds it */
        list = next;
    }
}
