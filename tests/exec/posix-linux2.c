/* What this library adds on Linux's kernel: waitid, posix_fallocate,
 * the in-kernel copies, priorities and scheduling, the saved ids,
 * sigqueue, the three kinds of POSIX timer, epoll, eventfd, timerfd,
 * signalfd, inotify, memfd, close_range, POSIX message queues, XSI
 * messages, semaphores and shared memory, asynchronous I/O, network
 * interfaces, the hosts database and its reentrant lookup, error() and
 * on_exit. Every answer is glibc 2.36's for the same calls: the program
 * ran there as a transcript, identical line for line. Elsewhere there
 * is nothing of this to test, and it says so with 42. */
// expect-exit: 42
// needs-libc: Linux's interfaces, from lib/libc
#define _GNU_SOURCE 1
#include <aio.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <mqueue.h>
#include <net/if.h>
#include <netdb.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sem.h>
#include <sys/sendfile.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/wait.h>

#if defined(__linux__) && defined(EMBCC_TEST_HAS_FS)

static int g_line;
#define CHECK(c) do { if (!(c)) { g_line = __LINE__; return 1; } } while (0)

static volatile int g_val, g_code, g_hits, g_fired;

static void on_sig(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)ctx;
    g_val = si->si_value.sival_int;
    g_code = si->si_code;
    g_hits++;
}

static void fired(union sigval v) { g_fired = v.sival_int; }

static void nap(long ms)
{
    struct timespec t = { ms / 1000, ms % 1000 * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

static int processes(void)
{
    pid_t c = fork();
    if (c == 0)
        _exit(7);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    CHECK(waitid(P_PID, (id_t)c, &si, WEXITED) == 0 && si.si_code == CLD_EXITED &&
          si.si_status == 7 && si.si_pid == c);
    CHECK(waitid(P_ALL, 0, &si, WEXITED | WNOHANG) == -1 && errno == ECHILD);
    errno = 0;
    CHECK(getpriority(PRIO_PROCESS, 0) == 0 && nice(2) == 2 &&
          getpriority(PRIO_PROCESS, 0) == 2);
    struct sched_param sp;
    CHECK(sched_getscheduler(0) == SCHED_OTHER && sched_getparam(0, &sp) == 0 &&
          sp.sched_priority == 0);
    uid_t r, e, s;
    CHECK(getresuid(&r, &e, &s) == 0 && r == getuid() && e == geteuid());
    cpu_set_t cs;
    CHECK(sched_getaffinity(0, sizeof cs, &cs) == 0 && CPU_COUNT(&cs) > 0 &&
          CPU_ISSET(0, &cs));
    clockid_t cid;
    CHECK(clock_getcpuclockid(0, &cid) == 0);
    CHECK(syscall(SYS_getpid) == getpid() && gettid() == getpid());
    CHECK(getauxval(AT_PAGESZ) == 4096);
    struct sysinfo inf;
    CHECK(sysinfo(&inf) == 0 && inf.procs > 0);
    char name[17] = { 0 };
    prctl(PR_SET_NAME, "tname", 0, 0, 0);
    prctl(PR_GET_NAME, name, 0, 0, 0);
    CHECK(!strcmp(name, "tname"));
    return 0;
}

static int files(void)
{
    int fd = open("/tmp/fa", O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0 && posix_fallocate(fd, 0, 8192) == 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 8192);
    CHECK(write(fd, "hello world", 11) == 11);
    int out = open("/tmp/fb", O_CREAT | O_TRUNC | O_RDWR, 0644);
    off_t off = 0;
    CHECK(sendfile(out, fd, &off, 5) == 5 && off == 5);
    off_t a = 6, b = 0;
    CHECK(copy_file_range(fd, &a, out, &b, 5, 0) == 5);
    char buf[64] = { 0 };
    CHECK(pread(out, buf, sizeof buf - 1, 0) == 5 && !strcmp(buf, "world"));
    struct statfs sfs;
    CHECK(fstatfs(fd, &sfs) == 0 && sfs.f_bsize > 0);
    close(fd);
    close(out);
    return 0;
}

static int signals_timers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    union sigval v = { .sival_int = 99 };
    sigqueue(getpid(), SIGUSR1, v);
    CHECK(g_val == 99 && g_code == SI_QUEUE);

    timer_t t;
    struct sigevent ev;
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    ev.sigev_value.sival_int = 5;
    g_hits = 0;
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
    struct itimerspec its = { { 0, 0 }, { 0, 20000000 } };
    CHECK(timer_settime(t, 0, &its, 0) == 0);
    nap(100);
    CHECK(g_hits == 1 && g_val == 5 && g_code == SI_TIMER);
    timer_delete(t);

    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_THREAD;
    ev.sigev_notify_function = fired;
    ev.sigev_value.sival_int = 77;
    CHECK(timer_create(CLOCK_REALTIME, &ev, &t) == 0);
    timer_settime(t, 0, &its, 0);
    for (int i = 0; i < 100 && !g_fired; i++)
        nap(10);
    CHECK(g_fired == 77);
    timer_delete(t);

    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_NONE;
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
    its.it_value.tv_sec = 5;
    timer_settime(t, 0, &its, 0);
    struct itimerspec now;
    CHECK(timer_gettime(t, &now) == 0 && now.it_value.tv_sec >= 4);
    timer_delete(t);
    return 0;
}

static int descriptors(void)
{
    int ep = epoll_create1(EPOLL_CLOEXEC), efd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event e = { EPOLLIN, { .fd = efd } }, got;
    CHECK(ep >= 0 && efd >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, efd, &e) == 0);
    eventfd_write(efd, 3);
    CHECK(epoll_wait(ep, &got, 1, 1000) == 1 && got.data.fd == efd);
    eventfd_t val;
    CHECK(eventfd_read(efd, &val) == 0 && val == 3);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec ti = { { 0, 0 }, { 0, 1000000 } };
    timerfd_settime(tfd, 0, &ti, 0);
    unsigned long long exp = 0;
    CHECK(read(tfd, &exp, sizeof exp) == 8 && exp == 1);

    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, 0);
    int sfd = signalfd(-1, &m, 0);
    raise(SIGUSR2);
    struct signalfd_siginfo ssi;
    CHECK(read(sfd, &ssi, sizeof ssi) == 128 && ssi.ssi_signo == SIGUSR2);

    int ino = inotify_init1(0), wd = inotify_add_watch(ino, "/tmp", IN_CREATE);
    close(open("/tmp/newfile", O_CREAT | O_WRONLY, 0644));
    char ib[512];
    struct inotify_event *ie = (struct inotify_event *)ib;
    CHECK(read(ino, ib, sizeof ib) > 0 && ie->wd == wd && ie->mask == IN_CREATE &&
          !strcmp(ie->name, "newfile"));

    int mfd = memfd_create("x", MFD_CLOEXEC);
    CHECK(mfd >= 0 && write(mfd, "abc", 3) == 3);
    int d1 = dup(0), d2 = dup(0);
    CHECK(close_range((unsigned)d1, (unsigned)d2, 0) == 0 && fcntl(d1, F_GETFD) < 0);
    return 0;
}

static int ipc(void)
{
    struct mq_attr ma = { 0, 4, 32, 0, { 0 } };
    mqd_t q = mq_open("/emq", O_CREAT | O_RDWR, 0600, &ma);
    CHECK(q >= 0 && mq_send(q, "ping", 4, 3) == 0);
    unsigned prio;
    char qm[64];
    CHECK(mq_receive(q, qm, sizeof qm, &prio) == 4 && prio == 3 && !memcmp(qm, "ping", 4));
    mq_close(q);
    CHECK(mq_unlink("/emq") == 0);

    int msq = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    struct { long t; char d[8]; } mb = { 2, "hey" }, mr;
    CHECK(msgsnd(msq, &mb, 4, 0) == 0 && msgrcv(msq, &mr, sizeof mr.d, 2, 0) == 4 &&
          !strcmp(mr.d, "hey"));
    struct msqid_ds mds;
    CHECK(msgctl(msq, IPC_STAT, &mds) == 0 && mds.msg_qnum == 0);
    msgctl(msq, IPC_RMID, 0);

    int sem = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    semctl(sem, 0, SETVAL, 3);
    struct sembuf so = { 0, -1, 0 };
    CHECK(semop(sem, &so, 1) == 0 && semctl(sem, 0, GETVAL) == 2);
    struct semid_ds sds;
    CHECK(semctl(sem, 0, IPC_STAT, &sds) == 0 && sds.sem_nsems == 1);
    semctl(sem, 0, IPC_RMID);

    int shm = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    char *sh = shmat(shm, 0, 0);
    CHECK(sh != (char *)-1);
    strcpy(sh, "shared");
    struct shmid_ds shds;
    CHECK(shmctl(shm, IPC_STAT, &shds) == 0 && shds.shm_segsz == 4096 &&
          shds.shm_nattch == 1);
    shmdt(sh);
    shmctl(shm, IPC_RMID, 0);
    CHECK(ftok("/tmp", 'x') != -1);

    int af = open("/tmp/aio", O_CREAT | O_TRUNC | O_RDWR, 0644);
    struct aiocb cb;
    memset(&cb, 0, sizeof cb);
    cb.aio_fildes = af;
    cb.aio_buf = "asynchronous";
    cb.aio_nbytes = 12;
    CHECK(aio_write(&cb) == 0);
    const struct aiocb *lst[] = { &cb };
    aio_suspend(lst, 1, 0);
    CHECK(aio_error(&cb) == 0 && aio_return(&cb) == 12);
    char ab[16] = { 0 };
    memset(&cb, 0, sizeof cb);
    cb.aio_fildes = af;
    cb.aio_buf = ab;
    cb.aio_nbytes = 5;
    cb.aio_offset = 1;
    CHECK(aio_read(&cb) == 0);
    while (aio_error(&cb) == EINPROGRESS)
        aio_suspend(lst, 1, 0);
    CHECK(aio_return(&cb) == 5 && !strcmp(ab, "synch"));
    close(af);
    return 0;
}

static int net_and_messages(void)
{
    CHECK(if_nametoindex("lo") == 1 && if_nametoindex("nope0") == 0);
    char ifn[IF_NAMESIZE];
    CHECK(if_indextoname(1, ifn) && !strcmp(ifn, "lo"));
    struct if_nameindex *ni = if_nameindex();
    CHECK(ni && ni[0].if_index == 1);
    if_freenameindex(ni);

    mkdir("/etc", 0755);
    FILE *h = fopen("/etc/hosts", "w");
    fputs("127.0.0.1 localhost lh\n::1 ip6-localhost\n", h);
    fclose(h);
    sethostent(0);
    struct hostent *he = gethostent();
    CHECK(he && !strcmp(he->h_name, "localhost") && he->h_addrtype == AF_INET &&
          !strcmp(he->h_aliases[0], "lh"));
    he = gethostent();      /* ::1 is IPv4's loopback here, as in glibc */
    CHECK(he && !strcmp(he->h_name, "ip6-localhost") && he->h_addrtype == AF_INET);
    endhostent();
    struct hostent hb, *hr;
    char hbuf[512];
    int herr;
    CHECK(gethostbyname_r("localhost", &hb, hbuf, sizeof hbuf, &hr, &herr) == 0 && hr &&
          !strcmp(hr->h_name, "localhost"));
    CHECK(gethostbyname_r("localhost", &hb, hbuf, 8, &hr, &herr) == ERANGE);

    /* error(): the program's whole name, the message, the errno's words */
    int p[2];
    CHECK(pipe(p) == 0);
    int saved = dup(2);
    dup2(p[1], 2);
    error(0, ENOENT, "file %s", "x.txt");
    error_at_line(0, 0, "f.c", 12, "line msg");
    fflush(stderr);
    dup2(saved, 2);
    close(p[1]);
    char out[256] = { 0 };
    CHECK(read(p[0], out, sizeof out - 1) > 0);
    char want[256];
    snprintf(want, sizeof want, "%s: file x.txt: No such file or directory\n%s:f.c:12: line msg\n",
             program_invocation_name, program_invocation_name);
    CHECK(!strcmp(out, want) && error_message_count == 2);
    return 0;
}

static int g_pipe;
static void bye(int status, void *arg)
{
    char m[32];
    int n = snprintf(m, sizeof m, "%d %s", status, (char *)arg);
    write(g_pipe, m, (size_t)n);
}

static int exits(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    pid_t c = fork();
    if (c == 0) {
        g_pipe = p[1];
        on_exit(bye, "arg1");
        exit(3);
    }
    close(p[1]);
    char m[32] = { 0 };
    int st;
    CHECK(read(p[0], m, sizeof m - 1) > 0 && !strcmp(m, "3 arg1"));
    CHECK(waitpid(c, &st, 0) == c && WEXITSTATUS(st) == 3);
    return 0;
}

int main(void)
{
    mkdir("/tmp", 0777);
    if (processes() || files() || signals_timers() || descriptors() || ipc() ||
        net_and_messages() || exits()) {
        printf("failed at line %d\n", g_line);
        return 1;
    }
    return 42;
}

#else
int main(void) { return 42; }
#endif
