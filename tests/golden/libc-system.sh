#!/bin/sh
# --libc=system: a Linux program compiled against glibc's own headers and
# linked against its shared objects by the system's linker, run on a real
# Linux kernel. POSIX threads are glibc's libpthread: four threads, a
# mutex, a condition variable, a semaphore, thread-local storage, atexit,
# libm, and a variadic function of EmbCC's handing its va_list to glibc's
# vprintf. And glibc's types laid out as GCC lays them out, byte for byte.
#
# Where: on a Linux host of the target's architecture, the system itself
# (/, its ld, run directly); elsewhere a Debian root
# (tools/glibc-sysroot.sh; tools/hostpaths.sh says where), a GNU ld for
# the architecture (TRIPLE-ld, ld.lld, or the bare-metal ARCH-elf-ld),
# and tests/harness/linux. What is missing is skipped, and said.
set -u
echo "TEST-MARKER libc-system"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
out=$PWD/tests/golden/out/libc-system
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- what is refused, and how ----------------------------------------------
"$EMBCC" --target=thumbv7m-none-eabi --libc=system -c -x c /dev/null \
    -o "$out/x.o" 2> "$out/r1.err" && fail "--libc=system on a bare-metal target"
grep -q "is for the Linux targets, x86_64-linux-gnu and aarch64-linux-gnu" "$out/r1.err" ||
    { cat "$out/r1.err"; fail "the bare-metal refusal does not say why"; }
"$EMBCC" --target=x86_64-linux-gnu --libc=glibc -c -x c /dev/null \
    -o "$out/x.o" 2> "$out/r2.err" && fail "--libc=glibc was accepted"
grep -q "embcc (EmbCC's own) or system" "$out/r2.err" ||
    { cat "$out/r2.err"; fail "an unknown --libc= does not list the two"; }
echo "libc-system: refused off Linux, and an unknown library named"

cat > "$out/a.c" <<'EOF'
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int helper(int);
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static sem_t sem;
static int counter, ready;
static __thread int mine;

static int say(const char *fmt, ...)
{
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);          /* EmbCC's va_list, glibc's reader */
    va_end(ap);
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return n == (int)strlen(b) ? n : -1;
}

static void *worker(void *arg)
{
    mine = (int)(intptr_t)arg;         /* each thread's own */
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    ready++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    sem_post(&sem);
    return (void *)(intptr_t)mine;
}

static void bye(void) { printf("atexit\n"); }

int main(void)
{
    pthread_t t[4];
    long sum = 0;
    atexit(bye);
    sem_init(&sem, 0, 0);
    mine = 100;
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, worker, (void *)(intptr_t)(i + 1));
    pthread_mutex_lock(&mu);
    while (ready < 4)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 4; i++) {
        void *r;
        sem_wait(&sem);
        pthread_join(t[i], &r);
        sum += (long)(intptr_t)r;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (say("v %d %s %.1f %ld|", 5, "x", 2.5, 123456789012L) != 23)
        return 3;
    printf("counter %d sum %ld mine %d sqrt %d pid %d\n", counter, sum, mine,
           (int)(sqrt(2.0) * 1000), getpid() > 0);
    return counter == 4000 && sum == 10 && mine == 100 ? helper(7) : 1;
}
EOF
printf 'int helper(int x) { return x * 6; }\n' > "$out/b.c"
want="v 5 x 2.5 123456789012|counter 4000 sum 10 mine 100 sqrt 1414 pid 1"

any=0
for arch in x86_64 aarch64; do
    triple=$arch-linux-gnu
    if [ "$(uname -s)-$(uname -m)" = "Linux-$arch" ] ||
       { [ "$(uname -s)" = Linux ] && [ "$arch" = aarch64 ] &&
         [ "$(uname -m)" = arm64 ]; }; then
        native=1; sr=; sysopt=; link=
    else
        native=0
        if [ "$arch" = x86_64 ]; then sr=$GLIBC_SYSROOT_X86_64
        else sr=$GLIBC_SYSROOT_AARCH64; fi
        [ -e "$sr/usr/include/pthread.h" ] || {
            echo "libc-system: $triple skipped: no glibc root at $sr" \
                 "(tools/glibc-sysroot.sh makes one)"; continue; }
        sysopt=--sysroot=$sr
        link=
        for ld in $triple-ld ld.lld $arch-elf-ld; do
            command -v "$ld" >/dev/null 2>&1 && { link=--linker=$ld; break; }
        done
        [ -n "$link" ] || {
            echo "libc-system: $triple skipped: no linker for it"; continue; }
    fi
    any=1
    d=$out/$arch; mkdir -p "$d"

    # ---- glibc's types, as GCC lays them out ----------------------------
    gcc=$arch-elf-gcc
    if [ "$native" = 0 ] && command -v "$gcc" >/dev/null 2>&1 &&
       command -v llvm-objcopy >/dev/null 2>&1; then
        {
            printf '#define _GNU_SOURCE\n'
            for h in pthread.h semaphore.h signal.h time.h sys/time.h \
                     sys/stat.h setjmp.h ucontext.h sys/socket.h netinet/in.h \
                     sys/select.h poll.h dirent.h termios.h fcntl.h wchar.h \
                     stddef.h sched.h sys/resource.h sys/uio.h stdlib.h \
                     mqueue.h spawn.h stdio.h; do
                printf '#include <%s>\n' "$h"
            done
            printf 'long layout[] = {\n'
            # (not va_list: EmbCC's is a pointer to the ABI's tag, which is
            # what a call passes either way, and 8 bytes where GCC's is
            # the tag itself)
            for t in pthread_t pthread_attr_t pthread_mutex_t \
                     pthread_mutexattr_t pthread_cond_t pthread_condattr_t \
                     pthread_rwlock_t pthread_key_t pthread_once_t \
                     pthread_barrier_t pthread_spinlock_t sem_t sigset_t \
                     "struct sigaction" siginfo_t stack_t "struct timespec" \
                     "struct timeval" "struct stat" "struct tm" jmp_buf \
                     sigjmp_buf ucontext_t mcontext_t "struct sockaddr_in6" \
                     "struct sockaddr_storage" fd_set "struct pollfd" \
                     "struct dirent" "struct termios" "struct flock" \
                     mbstate_t max_align_t off_t time_t "struct msghdr" \
                     "struct mq_attr" posix_spawn_file_actions_t fpos_t \
                     "long double"; do
                printf '  sizeof(%s), _Alignof(%s),\n' "$t" "$t"
            done
            printf '  offsetof(struct stat, st_mtim), offsetof(siginfo_t, si_value),\n'
            printf '  offsetof(pthread_mutex_t, __data.__kind),\n'
            printf '  offsetof(struct tm, tm_gmtoff),\n};\n'
        } > "$d/layout.c"
        "$EMBCC" --target=$triple --libc=system $sysopt -c "$d/layout.c" \
            -o "$d/l-embcc.o" || fail "$triple: glibc's headers do not compile"
        "$gcc" -nostdinc -isystem "$sr/usr/include/$triple" \
            -isystem "$sr/usr/include" -isystem "$("$gcc" -print-file-name=include)" \
            -c "$d/layout.c" -o "$d/l-gcc.o" ||
            fail "$triple: $gcc does not compile the layout probe"
        llvm-objcopy -O binary -j .data "$d/l-embcc.o" "$d/l-embcc.bin"
        llvm-objcopy -O binary -j .data "$d/l-gcc.o" "$d/l-gcc.bin"
        cmp -s "$d/l-embcc.bin" "$d/l-gcc.bin" ||
            fail "$triple: glibc's types are laid out otherwise than GCC lays them"
        echo "libc-system: $triple: 40 of glibc's types laid out as $gcc lays them"
    fi

    # ---- the program ----------------------------------------------------
    "$EMBCC" --target=$triple --libc=system $sysopt $link -pthread -O1 \
        -c "$d/../a.c" -o "$d/a.o" || fail "$triple: a.c does not compile"
    "$EMBCC" --target=$triple --libc=system $sysopt $link -pthread -O2 \
        "$d/a.o" "$out/b.c" -lm -o "$d/prog" 2> "$d/link.err" ||
        { cat "$d/link.err"; fail "$triple: the program does not link"; }
    if grep -q 'GNU-stack' "$d/link.err"; then
        cat "$d/link.err"; fail "$triple: an object without .note.GNU-stack"
    fi
    if [ "$native" = 1 ]; then
        got=$("$d/prog" | tr '\n' ' '); st=$?
    else
        root=$d/root
        if [ "$arch" = x86_64 ]; then
            mkdir -p "$root/lib64" "$root/lib/$triple"
            cp -L "$sr/lib64/ld-linux-x86-64.so.2" "$root/lib64/"
        else
            mkdir -p "$root/lib/$triple"
            cp -L "$sr/lib/ld-linux-aarch64.so.1" "$root/lib/"
        fi
        cp -L "$sr/lib/$triple/libc.so.6" "$sr/lib/$triple/libm.so.6" \
            "$root/lib/$triple/"
        sh tests/harness/linux/run.sh "$arch" --check >/dev/null 2>&1 || {
            echo "libc-system: $triple linked; not run: no Linux kernel" \
                 "for tests/harness/linux"; continue; }
        EMBCC_LINUX_ROOT=$root sh tests/harness/linux/run.sh "$arch" \
            "$d/prog" > "$d/run.out"
        st=$?
        got=$(tr '\n' ' ' < "$d/run.out")
    fi
    [ "$st" = 42 ] || fail "$triple: the program exited $st: $got"
    case "$got" in
        "$want atexit "*) ;;
        *) fail "$triple: it printed '$got'" ;;
    esac
    echo "libc-system: $triple: glibc's threads, semaphores, TLS, atexit, libm and vprintf, run"
done
[ "$any" = 1 ] || echo "libc-system: no Linux target could be tried here"
