/* POSIX's networking: addresses as text both ways (inet_pton strict,
 * inet_aton lenient, inet_ntop as RFC 5952 and glibc write IPv6), and,
 * where there is a kernel with sockets, a socketpair, poll and select,
 * TCP and a local datagram socket over loopback, and getaddrinfo and
 * getnameinfo from the hosts and services files.
 *
 * glibc 2.36 gave the same answers on the same kernel. The program writes
 * its own /etc/hosts and /etc/services, and -- booted straight to /init,
 * with no init system to do it -- brings the loopback interface up. */
// expect-exit: 42
// needs-libc: POSIX's networking, on lib/libc's backends
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int text(void)
{
    static const char *const in[] = {
        "::", "::1", "1::", "2001:db8::1", "fe80::1:2:3:4", "::ffff:1.2.3.4",
        "::1.2.3.4", "1:0:0:2:0:0:0:3", "2001:DB8:0:0:1:0:0:1",
        "1:2:3:4:5:6:7:8" };
    static const char *const out[] = {
        "::", "::1", "1::", "2001:db8::1", "fe80::1:2:3:4", "::ffff:1.2.3.4",
        "::1.2.3.4", "1:0:0:2::3", "2001:db8::1:0:0:1", "1:2:3:4:5:6:7:8" };
    unsigned char a[16];
    char t[INET6_ADDRSTRLEN];
    for (size_t i = 0; i < sizeof in / sizeof *in; i++)
        if (inet_pton(AF_INET6, in[i], a) != 1 ||
            !inet_ntop(AF_INET6, a, t, sizeof t) || strcmp(t, out[i]))
            return 1 + (int)i;
    static const char *const bad6[] = { "1:::2", "1:2:3:4:5:6:7:8:9",
                                        "12345::", ":1", "1:", "" };
    for (size_t i = 0; i < sizeof bad6 / sizeof *bad6; i++)
        if (inet_pton(AF_INET6, bad6[i], a) != 0)
            return 20 + (int)i;
    static const char *const four[] = { "1.2.3.4", "01.2.3.4", "256.1.1.1",
                                        "1.2.3", "1.2.3.4.5", " 1.2.3.4" };
    static const int ok4[] = { 1, 0, 0, 0, 0, 0 };
    for (size_t i = 0; i < sizeof four / sizeof *four; i++)
        if (inet_pton(AF_INET, four[i], a) != ok4[i])
            return 30 + (int)i;
    errno = 0;
    if (inet_pton(99, "1.2.3.4", a) != -1 || errno != EAFNOSUPPORT)
        return 36;
    if (inet_ntop(AF_INET6, a, t, 3) || errno != ENOSPC)
        return 37;
    struct in_addr ia;
    if (!inet_aton("127.1", &ia) || strcmp(inet_ntoa(ia), "127.0.0.1") ||
        !inet_aton("0x7f.0.0.010", &ia) || strcmp(inet_ntoa(ia), "127.0.0.8") ||
        inet_aton("1.2.3.4x", &ia) || ntohl(inet_addr("10.0.0.1")) != 0x0a000001 ||
        inet_addr("nope") != INADDR_NONE)
        return 38;
    if (htons(0x1234) != (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ ? 0x1234
                                                                 : 0x3412))
        return 39;
    return 0;
}

#ifdef EMBCC_TEST_HAS_FS
static void lo_up(void)
{
    struct { char name[16]; short flags; char pad[22]; } ifr = { "lo", 0, { 0 } };
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    ioctl(s, 0x8913, &ifr);                 /* SIOCGIFFLAGS */
    ifr.flags |= 1;                         /* IFF_UP */
    ioctl(s, 0x8914, &ifr);                 /* SIOCSIFFLAGS */
    close(s);
}

static int sockets(void)
{
    int sv[2];
    char b[32] = { 0 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
        return 50;
    struct pollfd pf = { sv[0], POLLIN, 0 };
    if (poll(&pf, 1, 0) != 0 || send(sv[1], "ping", 4, 0) != 4 ||
        poll(&pf, 1, 1000) != 1 || !(pf.revents & POLLIN))
        return 51;
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(sv[0], &rs);
    struct timeval tmo = { 1, 0 };
    if (select(sv[0] + 1, &rs, NULL, NULL, &tmo) != 1 || !FD_ISSET(sv[0], &rs) ||
        recv(sv[0], b, sizeof b, 0) != 4 || strcmp(b, "ping"))
        return 52;
    close(sv[0]);
    close(sv[1]);

    lo_up();
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sin;
    if (bind(ls, (struct sockaddr *)&sin, sizeof sin) || listen(ls, 4) ||
        getsockname(ls, (struct sockaddr *)&sin, &sl) || !sin.sin_port)
        return 53;
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(cs, (struct sockaddr *)&sin, sizeof sin))
        return 54;
    int as = accept(ls, NULL, NULL);
    memset(b, 0, sizeof b);
    if (as < 0 || send(cs, "over tcp", 8, 0) != 8 ||
        recv(as, b, sizeof b, 0) != 8 || strcmp(b, "over tcp"))
        return 55;
    int ty;
    socklen_t tl = sizeof ty;
    if (getsockopt(cs, SOL_SOCKET, SO_TYPE, &ty, &tl) || ty != SOCK_STREAM)
        return 56;
    close(cs);
    close(as);
    close(ls);

    int us = socket(AF_UNIX, SOCK_DGRAM, 0), uc = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sun = { AF_UNIX, "/tmp/sock" };
    memset(b, 0, sizeof b);
    if (bind(us, (struct sockaddr *)&sun, sizeof sun) ||
        sendto(uc, "dgram", 5, 0, (struct sockaddr *)&sun, sizeof sun) != 5 ||
        recvfrom(us, b, sizeof b, 0, NULL, NULL) != 5 || strcmp(b, "dgram"))
        return 57;
    close(us);
    close(uc);
    return 0;
}

static int lookups(void)
{
    mkdir("/etc", 0755);
    FILE *f = fopen("/etc/hosts", "w");
    if (!f)
        return 58;
    fputs("127.0.0.1 localhost\n10.1.2.3 alpha.example alpha a1\n"
          "2001:db8::5 six.example\n", f);
    fclose(f);
    if (!(f = fopen("/etc/services", "w")))
        return 59;
    fputs("http 80/tcp www\nhttp 80/udp\nmyservice 4321/tcp\n", f);
    fclose(f);

    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    if (getaddrinfo("a1", "http", &hints, &res))
        return 60;
    struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
    if (res->ai_family != AF_INET || ntohs(s4->sin_port) != 80 ||
        ntohl(s4->sin_addr.s_addr) != 0x0a010203 || res->ai_next ||
        strcmp(res->ai_canonname, "alpha.example"))
        return 61;
    freeaddrinfo(res);
    memset(&hints, 0, sizeof hints);
    int n = 0;
    if (getaddrinfo("six.example", "4321", &hints, &res))
        return 62;
    for (ai = res; ai; ai = ai->ai_next)
        n += ai->ai_family == AF_INET6;
    freeaddrinfo(res);
    if (n != 3)                     /* stream, datagram and raw */
        return 63;
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, "myservice", &hints, &res))
        return 64;
    s4 = (struct sockaddr_in *)res->ai_addr;
    if (s4->sin_addr.s_addr != 0 || ntohs(s4->sin_port) != 4321)
        return 65;
    freeaddrinfo(res);
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo("alpha", NULL, &hints, &res) != EAI_NONAME)
        return 66;
    hints.ai_flags = 0;
    if (getaddrinfo("alpha", "nosuchsvc", &hints, &res) != EAI_SERVICE)
        return 67;
    hints.ai_family = 99;
    if (getaddrinfo("alpha", NULL, &hints, &res) != EAI_FAMILY ||
        strcmp(gai_strerror(EAI_NONAME), "Name or service not known"))
        return 68;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo("localhost", NULL, &hints, &res))
        return 69;
    freeaddrinfo(res);

    struct sockaddr_in back = { AF_INET, htons(80), { htonl(0x0a010203) }, { 0 } };
    char host[64], serv[16];
    if (getnameinfo((struct sockaddr *)&back, sizeof back, host, sizeof host,
                    serv, sizeof serv, 0) ||
        strcmp(host, "alpha.example") || strcmp(serv, "http"))
        return 70;
    if (getnameinfo((struct sockaddr *)&back, sizeof back, host, sizeof host,
                    serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) ||
        strcmp(host, "10.1.2.3") || strcmp(serv, "80"))
        return 71;
    struct hostent *he = gethostbyname("alpha");
    if (!he || strcmp(he->h_name, "alpha.example") ||
        ntohl(*(in_addr_t *)he->h_addr) != 0x0a010203)
        return 72;
    struct servent *se = getservbyname("http", "tcp");
    if (!se || ntohs((uint16_t)se->s_port) != 80)
        return 73;
    return 0;
}
#endif

int main(void)
{
    int r;
    if ((r = text()))
        return r;
#ifdef EMBCC_TEST_HAS_FS
    if ((r = sockets()) || (r = lookups()))
        return r;
#else
    errno = 0;
    if (socket(AF_INET, SOCK_STREAM, 0) != -1 || errno != ENOSYS)
        return 80;
#endif
    return 42;
}
