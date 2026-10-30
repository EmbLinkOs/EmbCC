/* The third round: XSI's single-signal calls and the BSD masks, ulimit,
 * open_wmemstream, floating-point traps and modes, <mntent.h>,
 * <netinet/ether.h>, the classful inet calls, canonicalize_file_name,
 * euidaccess, the qecvt family, twalk_r, the pthread _np calls, the
 * errno numbers past the first 71, and Linux's preadv2, renameat2 and
 * statx. Every answer is glibc 2.36's (the program ran there as a
 * transcript), except two the comments name. */
// expect-exit: 42
// needs-libc: XSI signals, <mntent.h> and the rest, from lib/libc
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <float.h>
#include <mntent.h>
#include <netinet/ether.h>
#include <pthread.h>
#include <search.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ulimit.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/uio.h>

static int g_line;
#define CHECK(c) do { if (!(c)) { g_line = __LINE__; return 1; } } while (0)

static volatile int g_hits;
static void h(int s) { (void)s; g_hits++; }

static int signals(void)
{
    /* sigaction, not signal: without a kernel, signal is C's, and resets
     * the handler before it runs */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigaction(SIGUSR1, &sa, 0);
    sighold(SIGUSR1);
    raise(SIGUSR1);
    CHECK(g_hits == 0);
    sigrelse(SIGUSR1);
    CHECK(g_hits == 1);
    CHECK(sigset(SIGUSR1, SIG_HOLD) == h);
    CHECK(sigset(SIGUSR1, SIG_DFL) == SIG_HOLD);
    sigignore(SIGUSR2);
    raise(SIGUSR2);
    int old = sigblock(sigmask(SIGUSR1));
    CHECK(old == 0 && (siggetmask() & sigmask(SIGUSR1)));
    sigsetmask(old);
    CHECK(siggetmask() == 0);
    return 0;
}

static int streams(void)
{
    wchar_t *wb;
    size_t ws;
    FILE *f = open_wmemstream(&wb, &ws);
    CHECK(f);
    fwprintf(f, L"h\xe9llo %d", 42);
    fflush(f);
    CHECK(ws == 8 && !wcscmp(wb, L"h\xe9llo 42"));
    fseek(f, 1, SEEK_SET);
    fputwc(L'E', f);
    fclose(f);
    /* the size is the position; what lies past it stays, as our
     * open_memstream keeps it (glibc ends the buffer there) */
    CHECK(ws == 2 && wb[0] == L'h' && wb[1] == L'E');
    free(wb);
    return 0;
}

static int fenv(void)
{
#if defined(__x86_64__) || defined(__aarch64__)
    feclearexcept(FE_ALL_EXCEPT);
    fesetexcept(FE_INEXACT);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == FE_INEXACT);
    fexcept_t fl;
    fegetexceptflag(&fl, FE_ALL_EXCEPT);
    CHECK(fetestexceptflag(&fl, FE_INEXACT | FE_OVERFLOW) == FE_INEXACT);
    femode_t m;
    fegetmode(&m);
    fesetround(FE_UPWARD);
    fesetmode(&m);
    CHECK(fegetround() == FE_TONEAREST);
#endif
#if defined(__x86_64__)
    /* aarch64's traps are optional in the architecture: QEMU's have none */
    CHECK(fegetexcept() == 0);
    CHECK(feenableexcept(FE_DIVBYZERO) == 0 && fegetexcept() == FE_DIVBYZERO);
    fedisableexcept(FE_ALL_EXCEPT);
    CHECK(fegetexcept() == 0);
#endif
    return 0;
}

static int names(void)
{
    struct ether_addr *ea = ether_aton("0:1a:2b:c:d:ff");
    CHECK(ea && !strcmp(ether_ntoa(ea), "0:1a:2b:c:d:ff"));
    CHECK(!ether_aton("0:1:2:3:4"));
    char host[64];
    struct ether_addr e2;
    CHECK(ether_line("aa:bb:cc:dd:ee:ff  myhost # c", &e2, host) == 0 &&
          !strcmp(host, "myhost") && !strcmp(ether_ntoa(&e2), "aa:bb:cc:dd:ee:ff"));
    struct in_addr in;
    inet_aton("10.20.30.40", &in);
    CHECK(inet_lnaof(in) == 0x141e28 && inet_netof(in) == 0xa);
    inet_aton("172.16.5.9", &in);
    CHECK(inet_lnaof(in) == 0x509 && inet_netof(in) == 0xac10);
    in = inet_makeaddr(192 << 16 | 168 << 8 | 1, 7);
    CHECK(!strcmp(inet_ntoa(in), "192.168.1.7"));
    CHECK(wcstoq(L"-12345", 0, 10) == -12345);
    if (LDBL_MANT_DIG >= 53) {
        int dp, sg;
        char qb[32];
        CHECK(!strcmp(qecvt(3.25L, 3, &dp, &sg), "325") && dp == 1 && !sg);
        CHECK(!strcmp(qgcvt(1234.5L, 6, qb), "1234.5"));
    }
    static const struct { int e; const char *name, *msg; } errs[] = {
        { ENOMEDIUM, "ENOMEDIUM", "No medium found" },
        { EKEYEXPIRED, "EKEYEXPIRED", "Key has expired" },
        { EHWPOISON, "EHWPOISON", "Memory page has hardware error" },
        { ERFKILL, "ERFKILL", "Operation not possible due to RF-kill" },
        { ECHRNG, "ECHRNG", "Channel number out of range" },
        { EDQUOT, "EDQUOT", "Disk quota exceeded" },
        { ESTALE, "ESTALE", "Stale file handle" },
        { EREMOTEIO, "EREMOTEIO", "Remote I/O error" },
    };
    for (unsigned i = 0; i < sizeof errs / sizeof errs[0]; i++)
        CHECK(!strcmp(strerrorname_np(errs[i].e), errs[i].name) &&
              !strcmp(strerror(errs[i].e), errs[i].msg));
    return 0;
}

static char g_walk[16];
static void act(const void *n, VISIT v, void *c)
{
    if (v == postorder || v == leaf) {
        size_t k = strlen(g_walk);
        g_walk[k] = (char)('0' + **(const int **)n);
        g_walk[k + 1] = *(char *)c;
    }
}

static int icmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void *thr(void *a) { return a; }

static int threads(void)
{
    void *root = 0;
    static int keys[] = { 5, 3, 8 };
    for (int i = 0; i < 3; i++)
        tsearch(&keys[i], &root, icmp);
    twalk_r(root, act, ",");
    CHECK(!strcmp(g_walk, "3,5,8,"));

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    CHECK(pthread_mutexattr_setprioceiling(&ma, 10) == 0 &&
          pthread_mutexattr_setprioceiling(&ma, 1000) == EINVAL);
    int c;
    CHECK(pthread_mutexattr_getprioceiling(&ma, &c) == 0 && c == 10);
    pthread_rwlockattr_t ra;
    pthread_rwlockattr_init(&ra);
    int k;
    CHECK(pthread_rwlockattr_setkind_np(&ra, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) == 0 &&
          pthread_rwlockattr_getkind_np(&ra, &k) == 0 && k == 2);

    pthread_attr_t a;
    pthread_attr_init(&a);
    cpu_set_t cs, back;
    CPU_ZERO(&cs);
    CPU_SET(0, &cs);
    CHECK(pthread_attr_setaffinity_np(&a, sizeof cs, &cs) == 0);
    CHECK(pthread_attr_getaffinity_np(&a, sizeof back, &back) == 0 &&
          CPU_ISSET(0, &back) && CPU_COUNT(&back) == 1);
    pthread_t th;
    if (pthread_create(&th, &a, thr, (void *)7) == 0) {
        void *res;
        struct timespec dl;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += 5;
        CHECK(pthread_clockjoin_np(th, &res, CLOCK_MONOTONIC, &dl) == 0 && res == (void *)7);
    }
    pthread_attr_destroy(&a);
    CHECK(pthread_getaffinity_np(pthread_self(), sizeof cs, &cs) == 0);
    return 0;
}

#ifdef EMBCC_TEST_HAS_FS
static int files(void)
{
    CHECK(ulimit(UL_GETFSIZE) > 0);
    mkdir("/tmp", 0777);
    FILE *t = setmntent("/tmp/fstab", "w+");
    struct mntent me = { "/dev/sda1", "/mnt/my disk", "ext4", "rw,noatime,data=ordered", 0, 2 };
    CHECK(t && addmntent(t, &me) == 0);
    endmntent(t);
    t = setmntent("/tmp/fstab", "r");
    struct mntent *mr = getmntent(t);
    CHECK(mr && !strcmp(mr->mnt_dir, "/mnt/my disk") && !strcmp(mr->mnt_type, "ext4") &&
          mr->mnt_passno == 2);
    CHECK(!strcmp(hasmntopt(mr, "noatime"), "noatime,data=ordered") &&
          hasmntopt(mr, "data") && !hasmntopt(mr, "ro"));
    endmntent(t);
    int fd = open("/tmp/fstab", O_RDONLY);
    char raw[128] = { 0 };
    CHECK(read(fd, raw, sizeof raw - 1) > 0);
    close(fd);
    CHECK(!strcmp(raw, "/dev/sda1 /mnt/my\\040disk ext4 rw,noatime,data=ordered 0 2\n"));
    char *cf = canonicalize_file_name("/tmp/../tmp/./fstab");
    CHECK(cf && !strcmp(cf, "/tmp/fstab"));
    free(cf);
    CHECK(euidaccess("/tmp/fstab", R_OK) == 0 && euidaccess("/nonexist", F_OK) == -1);
#if defined(__linux__)
    int pfd = open("/tmp/rw", O_CREAT | O_TRUNC | O_RDWR, 0644);
    struct iovec iv = { "abcdef", 6 };
    CHECK(pwritev2(pfd, &iv, 1, 0, 0) == 6);
    char rb[8] = { 0 };
    struct iovec ir = { rb, 3 };
    CHECK(preadv2(pfd, &ir, 1, 2, 0) == 3 && !strcmp(rb, "cde"));
    close(pfd);
    CHECK(renameat2(AT_FDCWD, "/tmp/rw", AT_FDCWD, "/tmp/fstab", RENAME_NOREPLACE) == -1 &&
          errno == EEXIST);
    struct statx sx;
    CHECK(statx(AT_FDCWD, "/tmp/rw", 0, STATX_SIZE, &sx) == 0 && sx.stx_size == 6);
#ifndef __GLIBC__
    /* the main thread's stack, where glibc (with no /proc) says ENOENT */
    pthread_attr_t ga;
    CHECK(pthread_getattr_np(pthread_self(), &ga) == 0);
    void *sa;
    size_t ss;
    pthread_attr_getstack(&ga, &sa, &ss);
    int local;
    CHECK((char *)&local > (char *)sa && (char *)&local < (char *)sa + ss);
#endif
#endif
    return 0;
}
#endif

int main(void)
{
    if (signals() || streams() || fenv() || names() || threads()) {
        printf("failed at line %d\n", g_line);
        return 1;
    }
#ifdef EMBCC_TEST_HAS_FS
    if (files()) {
        printf("failed at line %d\n", g_line);
        return 2;
    }
#endif
    return 42;
}
