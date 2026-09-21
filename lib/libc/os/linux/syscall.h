/* The bottom of the Linux backend: how a syscall is made, and what the
 * numbers are.
 *
 * This library does not link against glibc, musl or anything else on
 * Linux. It talks to the kernel directly, for the same reason the
 * EmbLinkOS backend talks to EmbLinkOS directly: the seam in
 * ../backend.h is meant to sit on a kernel, not on another C library.
 * Layering our stdio over glibc's stdio would mean two buffer pools,
 * two errno variables and two ideas about what a FILE is.
 *
 * ---- the ABI ------------------------------------------------------------
 *
 * x86-64: number in rax, arguments in rdi, rsi, rdx, r10, r8, r9, the
 *         `syscall` instruction, result in rax; rcx and r11 destroyed.
 * aarch64: number in x8, arguments in x0..x5, `svc #0`, result in x0.
 *
 * Both are what the compiler generates for the register-pinned operands
 * below, and tests/golden/linux.sh disassembles these functions and
 * checks it -- the register assignment is an ABI, so it is checked
 * against the architecture rather than assumed.
 *
 * ---- the numbers --------------------------------------------------------
 *
 * Unlike EmbLinkOS, whose numbers this tree reads out of the OS's own
 * header, Linux's numbers have to be written down here: there is no
 * Linux header on the machine this file is usually built on. They are
 * ABI and have not moved since the ports were merged -- that is the
 * whole point of a syscall number -- but "has not moved" is not
 * "checked", so tests/golden/linux.sh compares every one of them
 * against <asm/unistd.h> WHEN IT RUNS ON LINUX and fails on any
 * disagreement. On a machine with no such header it says it skipped
 * that check rather than implying it passed.
 *
 * x86-64 has both the old calls (open, stat, mkdir) and the newer
 * directory-relative ones (openat, newfstatat, mkdirat); aarch64, added
 * to the kernel after the *at calls existed, has ONLY the latter. So
 * this backend uses the *at forms with AT_FDCWD throughout and gets one
 * code path for both architectures instead of two.
 */
#ifndef EMBLIBC_OS_LINUX_SYSCALL_H
#define EMBLIBC_OS_LINUX_SYSCALL_H

#if defined(__x86_64__)

#define LSYS_read          0
#define LSYS_write         1
#define LSYS_close         3
#define LSYS_lseek         8
#define LSYS_brk          12
#define LSYS_ioctl        16
#define LSYS_writev       20
#define LSYS_sched_yield  24
#define LSYS_nanosleep    35
#define LSYS_exit         60
#define LSYS_ftruncate    77
#define LSYS_getcwd       79
#define LSYS_chdir        80
#define LSYS_truncate     76
#define LSYS_statfs      137
#define LSYS_futex       202
#define LSYS_getdents64  217
#define LSYS_clock_gettime 228
#define LSYS_exit_group  231
#define LSYS_openat      257
#define LSYS_mkdirat     258
#define LSYS_newfstatat  262
#define LSYS_unlinkat    263
#define LSYS_renameat    264
#define LSYS_linkat      265
#define LSYS_symlinkat   266
#define LSYS_readlinkat  267
#define LSYS_fchmodat    268
#define LSYS_utimensat   280
#define LSYS_getrandom   318
#define LSYS_statx       332

/* O_DIRECTORY. Almost every open flag has the same value everywhere --
 * the seam's own __OS_O_* are passed straight through for exactly that
 * reason -- but this is one of the handful x86 numbered before the
 * generic table existed, and x86-64 and aarch64 SWAP it with O_DIRECT.
 * Using the wrong one does not fail cleanly: opening a directory with
 * O_DIRECT returns EINVAL on tmpfs, which reads like a bad argument
 * rather than like the wrong constant. */
#define LO_DIRECTORY   0200000

#elif defined(__aarch64__)

/* asm-generic/unistd.h -- the table every architecture added since 2012
 * shares. The gaps against x86-64 above are not omissions: they are the
 * same calls at the numbers the generic table gave them. */
#define LSYS_getcwd       17
#define LSYS_ioctl        29
#define LSYS_mkdirat      34
#define LSYS_unlinkat     35
#define LSYS_symlinkat    36
#define LSYS_linkat       37
#define LSYS_renameat     38
#define LSYS_statfs       43
#define LSYS_truncate     45
#define LSYS_ftruncate    46
#define LSYS_chdir        49
#define LSYS_fchmodat     53
#define LSYS_openat       56
#define LSYS_close        57
#define LSYS_getdents64   61
#define LSYS_lseek        62
#define LSYS_read         63
#define LSYS_write        64
#define LSYS_writev       66
#define LSYS_readlinkat   78
#define LSYS_newfstatat   79
#define LSYS_utimensat    88
#define LSYS_exit         93
#define LSYS_exit_group   94
#define LSYS_futex        98
#define LSYS_nanosleep   101
#define LSYS_clock_gettime 113
#define LSYS_sched_yield 124
#define LSYS_brk         214
#define LSYS_getrandom   278
#define LSYS_statx       291

/* The other half of the swap described above: here 0200000 is O_DIRECT
 * and this is O_DIRECTORY. */
#define LO_DIRECTORY    040000

#else
#error "the Linux backend has been built for an architecture it has no syscall table for"
#endif

/* ---- making the call ------------------------------------------------------
 *
 * The kernel returns the result, or a small negative errno, in the same
 * register. Nothing here interprets that: __os_* functions above do,
 * because -1 with errno set is the seam's convention and not the
 * kernel's.
 *
 * `memory` on every one of these is not laziness. A syscall reads and
 * writes buffers the compiler cannot see through a pointer argument --
 * read(2) fills one -- so the optimiser must not keep anything in a
 * register across it.
 */

#if defined(__x86_64__)

static inline long lsys1(long n, long a)
{
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}
static inline long lsys2(long n, long a, long b)
{
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static inline long lsys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}
static inline long lsys4(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "rcx", "r11", "memory");
    return r;
}
static inline long lsys5(long n, long a, long b, long c, long d, long e)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return r;
}

#elif defined(__aarch64__)

/* x0 is both an input and the result, so it is a "+r" operand: writing
 * it as separate "=r" and "r" would let the compiler choose two
 * different registers and the argument would never reach the kernel. */
static inline long lsys1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}
static inline long lsys2(long n, long a, long b)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile ("svc #0" : "+r"(x0)
                      : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}
static inline long lsys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile ("svc #0" : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static inline long lsys4(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile ("svc #0" : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory", "cc");
    return x0;
}
static inline long lsys5(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile ("svc #0" : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                      : "memory", "cc");
    return x0;
}

#endif

static inline long lsys0(long n) { return lsys1(n, 0); }

/* ---- the constants the calls above take -----------------------------------
 *
 * These are uapi values, shared by both architectures: the *at calls,
 * statx and getdents64 were all added after the kernel stopped letting
 * each port pick its own numbers.
 */
#define LAT_FDCWD              (-100)
#define LAT_SYMLINK_NOFOLLOW   0x100
#define LAT_REMOVEDIR          0x200
#define LAT_EMPTY_PATH         0x1000
#define LAT_STATX_SYNC_AS_STAT 0x0000

#define LSTATX_BASIC_STATS     0x000007ffU

/* The mode bits, which C's own <sys/stat.h> numbers the same way
 * because POSIX fixed them long before Linux existed. */
#define LS_IFMT   0170000
#define LS_IFSOCK 0140000
#define LS_IFLNK  0120000
#define LS_IFREG  0100000
#define LS_IFBLK  0060000
#define LS_IFDIR  0040000
#define LS_IFCHR  0020000
#define LS_IFIFO  0010000

/* getdents64's d_type, which is not the mode bits and does not
 * correspond to them numerically. */
#define LDT_UNKNOWN  0
#define LDT_FIFO     1
#define LDT_CHR      2
#define LDT_DIR      4
#define LDT_BLK      6
#define LDT_REG      8
#define LDT_LNK     10
#define LDT_SOCK    12

#define LCLOCK_REALTIME          0
#define LCLOCK_PROCESS_CPUTIME   2

/* FUTEX_WAIT and FUTEX_WAKE, with the private flag: private means the
 * kernel may key the wait on the address alone rather than on the page
 * it belongs to, which is both faster and correct for every use this
 * library has -- a futex word shared between processes would have to
 * live in shared memory, and nothing here puts one there. */
#define LFUTEX_WAIT         0
#define LFUTEX_WAKE         1
#define LFUTEX_PRIVATE    128

/* TCGETS, which isatty asks for. asm-generic/ioctls.h gives it the same
 * number x86 chose. */
#define LTCGETS 0x5401

/* The layout statx fills. This is the reason the backend calls statx
 * rather than fstatat: `struct stat` has a DIFFERENT field order on
 * x86-64 and on aarch64, so a shared backend would need two structures
 * and would silently read the wrong offsets if either were wrong.
 * statx has one layout on every architecture, fixed when it was added
 * in Linux 4.11 (2017), and the kernel is told how big the buffer is,
 * so it is the version of this structure that cannot rot.
 *
 * Padding is written out rather than left to the compiler: this is
 * someone else's structure and its offsets are not ours to choose.
 */
struct lstatx_timestamp {
    long long tv_sec;
    unsigned tv_nsec;
    int __reserved;
};

struct lstatx {
    unsigned stx_mask;                  /*   0 */
    unsigned stx_blksize;               /*   4 */
    unsigned long long stx_attributes;  /*   8 */
    unsigned stx_nlink;                 /*  16 */
    unsigned stx_uid;                   /*  20 */
    unsigned stx_gid;                   /*  24 */
    unsigned short stx_mode;            /*  28 */
    unsigned short __spare0;            /*  30 */
    unsigned long long stx_ino;         /*  32 */
    unsigned long long stx_size;        /*  40 */
    unsigned long long stx_blocks;      /*  48 */
    unsigned long long stx_attributes_mask; /* 56 */
    struct lstatx_timestamp stx_atime;  /*  64 */
    struct lstatx_timestamp stx_btime;  /*  80 */
    struct lstatx_timestamp stx_ctime;  /*  96 */
    struct lstatx_timestamp stx_mtime;  /* 112 */
    unsigned stx_rdev_major;            /* 128 */
    unsigned stx_rdev_minor;            /* 132 */
    unsigned stx_dev_major;             /* 136 */
    unsigned stx_dev_minor;             /* 140 */
    unsigned long long __spare2[14];    /* 144, to 256 */
};

/* struct statfs. Both architectures reach the same layout here -- every
 * field is eight bytes on a 64-bit kernel, in the same order -- so
 * unlike `struct stat` this one CAN be shared, and is. */
struct lstatfs {
    unsigned long long f_type, f_bsize;
    unsigned long long f_blocks, f_bfree, f_bavail;
    unsigned long long f_files, f_ffree;
    unsigned long long f_fsid;
    unsigned long long f_namelen, f_frsize, f_flags;
    unsigned long long f_spare[4];
};

struct ltimespec { long tv_sec, tv_nsec; };

/* What getdents64 returns, back to back, in the buffer it is given.
 * d_name is not an array: the kernel writes a NUL-terminated name
 * immediately after d_type and pads to d_reclen, so the name's LENGTH
 * is not recorded anywhere and the record's is. */
struct ldirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

#endif
