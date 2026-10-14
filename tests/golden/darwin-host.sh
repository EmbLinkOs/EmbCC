#!/bin/sh
# EmbCC as a macOS host compiler, for a project that builds with Apple's
# clang: EmbLinkRTOS's native port and its tests, configured by CMake. The
# build stopped on each of these, in turn:
#
#   - the driver flags CMake and Apple's toolchain pass: -arch arm64,
#     -isysroot SDK, -mmacosx-version-min=V, -pthread;
#   - no <pthread.h>, <unistd.h> or <sys/wait.h>: a Darwin target now reads
#     the SDK's usr/include (found by -isysroot, $SDKROOT, or the Command
#     Line Tools / Xcode SDK), then EmbCC's freestanding headers;
#   - the SDK's headers themselves: `#if` on a macro that EXPANDS to
#     `defined X` (<pthread.h>), asm labels (`__asm("_" "x" "$UNIX2003")`),
#     __arm64__, va_list -- <stdarg.h> said `char *`, <stdio.h> then said
#     `void *` (the SDK's choice without __GNUC__), a conflicting typedef --
#     and <math.h>'s _Float16 prototypes (float-types.sh has those).
#
# Each is compiled here as Apple's clang compiles it: the preprocessor's
# answers, the symbol names asm labels give (Mach-O and ELF), and a
# program using threads, a child process, varargs into the SDK's vsnprintf
# and <math.h>, run natively, printing what clang's build prints.
set -u
echo "TEST-MARKER darwin-host"
. "$(dirname "$0")/../lib.sh"

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || {
    echo "SKIP: needs macOS on arm64"; exit 0; }
command -v cc > /dev/null 2>&1 && command -v clang > /dev/null 2>&1 || {
    echo "SKIP: no system cc"; exit 0; }
SDK=$(xcrun --show-sdk-path 2>/dev/null)
[ -n "$SDK" ] && [ -f "$SDK/usr/include/pthread.h" ] || { echo "SKIP: no macOS SDK"; exit 0; }
NM=${EMBCC_LLVM_NM:-llvm-nm}
command -v "$NM" > /dev/null 2>&1 || { echo "SKIP: no $NM"; exit 0; }

EMBCC=${EMBCC:-./embcc}
out=$PWD/tests/golden/out/darwin-host
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- 1. the preprocessor ---------------------------------------------------
cat > "$out/pp.c" << 'EOF'
#define FOO 1
#define COMPAT() defined(FOO) && (!defined(BAR) || (BAR < 1))
#if COMPAT()
int has_ok = 1;
#endif
#define NOT_COMPAT() defined(BAR)
#if NOT_COMPAT()
int has_bad = 1;
#endif
#if defined(__arm64__) && defined(__aarch64__) && defined(__APPLE__)
int arm64_ok = 1;
#endif
EOF
"$EMBCC" -arch arm64 -isysroot "$SDK" -mmacosx-version-min=11.0 -pthread \
    -E "$out/pp.c" > "$out/pp.i" 2> "$out/pp.err" || { cat "$out/pp.err"; fail "pp.c"; }
grep -q 'has_ok' "$out/pp.i" || fail "#if on a macro that expands to defined() was false"
grep -q 'has_bad' "$out/pp.i" && fail "#if on a macro that expands to defined(UNDEFINED) was true"
clang -E "$out/pp.c" 2> /dev/null | grep -c '_ok\|has_bad' | grep -qx 2 ||
    fail "clang reads pp.c differently from this test's expectations"
grep -q 'arm64_ok' "$out/pp.i" || fail "-arch arm64 does not define __arm64__"
echo "darwin-host: -arch/-isysroot/-mmacosx-version-min/-pthread taken; defined() from a macro; __arm64__"

# ---- 2. asm labels: the names clang gives -----------------------------------
cat > "$out/lab.c" << 'EOF'
extern int ext_v __asm__("_real_ext_v");
extern int ext_f(void) __asm__("_real_ext_f");
extern int raw_f(void) __asm__("raw_symbol");
int def_v __asm__("_real_def_v") = 3;
int def_f(void) __asm__("_real_def_f");
int def_f(void) { return ext_f() + raw_f() + ext_v + def_v; }
int same(void) __asm__("_same");
int same(void) { return def_f(); }
EOF
# the global and undefined names (clang's assembler adds local ltmpN labels)
syms() { "$NM" "$1" | awk '$(NF-1) ~ /^[A-Z]$/ { print $(NF-1), $NF }' | sort; }
"$EMBCC" -arch arm64 -c "$out/lab.c" -o "$out/lab.o" 2> "$out/lab.err" ||
    { cat "$out/lab.err"; fail "asm labels on Mach-O"; }
clang -arch arm64 -c "$out/lab.c" -o "$out/lab-clang.o" || fail "clang lab.c"
syms "$out/lab.o" > "$out/lab.syms"; syms "$out/lab-clang.o" > "$out/lab-clang.syms"
diff "$out/lab-clang.syms" "$out/lab.syms" || fail "Mach-O symbols differ from clang's"
"$EMBCC" --target=thumbv7em-none-eabi -c "$out/lab.c" -o "$out/lab-elf.o" 2> "$out/lab.err" ||
    { cat "$out/lab.err"; fail "asm labels on ELF"; }
clang --target=thumbv7em-none-eabi -c "$out/lab.c" -o "$out/lab-elf-clang.o" || fail "clang ELF lab.c"
syms "$out/lab-elf.o" > "$out/lab-elf.syms"; syms "$out/lab-elf-clang.o" > "$out/lab-elf-clang.syms"
diff "$out/lab-elf-clang.syms" "$out/lab-elf.syms" || fail "ELF symbols differ from clang's"
echo "darwin-host: asm labels name definitions and references as clang does, Mach-O and ELF"

# ---- 3. a POSIX program, run -------------------------------------------------
cat > "$out/prog.c" << 'EOF'
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdalign.h>
#include <stdnoreturn.h>
#include <iso646.h>

static alignas(16) int counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *work(void *arg)
{
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&lock);
        counter += *(int *)arg;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static int fmt(char *b, size_t n, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}

int main(void)
{
    pthread_t t[4];
    int one = 1;
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, work, &one);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    pid_t p = fork();
    if (p == 0)
        _exit(7);
    int st = 0;
    waitpid(p, &st, 0);
    char b[64];
    fmt(b, sizeof b, "%d %s %.3f", counter, "x", sqrt(2.0));
    printf("%s child=%d %d\n", b, WEXITSTATUS(st), (int)(alignof(int) >= 4 and 1));
    return 0;
}
EOF
clang -O2 "$out/prog.c" -o "$out/prog-clang" || fail "clang prog.c"
"$out/prog-clang" > "$out/want.txt"
for O in -O0 -O2; do
    "$EMBCC" -arch arm64 -isysroot "$SDK" -pthread $O -c "$out/prog.c" -o "$out/prog.o" \
        2> "$out/prog.err" || { cat "$out/prog.err"; fail "prog.c $O"; }
    cc -pthread "$out/prog.o" -o "$out/prog" || fail "linking prog.o $O"
    "$out/prog" > "$out/got.txt" || fail "prog $O exited non-zero"
    diff "$out/want.txt" "$out/got.txt" || fail "prog $O printed differently from clang's build"
done
echo "darwin-host: threads, fork/waitpid, varargs into the SDK's vsnprintf and <math.h> run as clang's build at -O0 and -O2: $(cat "$out/got.txt")"

# ---- 4. EmbCC links it -------------------------------------------------------
# Through Apple's linker, as clang does: one command from sources to a
# program, two sources and a static library, -l, -framework, and GNU's
# --gc-sections and -Map= in ld64's spelling.
printf 'int helper(int x) { return x * 2; }\n' > "$out/h.c"
printf '#include <stdio.h>\nint helper(int);\nint lib_fn(void);\nint main(void) { printf("%%d\\n", helper(20) + lib_fn()); return 0; }\n' > "$out/m.c"
printf 'int lib_fn(void) { return 2; }\n' > "$out/l.c"
"$EMBCC" -arch arm64 -c "$out/l.c" -o "$out/l.o" || fail "l.c"
ar rcs "$out/libl.a" "$out/l.o" 2> /dev/null || fail "ar"
"$EMBCC" -arch arm64 -O2 "$out/m.c" "$out/h.c" -L"$out" -ll -lm \
    -framework CoreFoundation -Wl,--gc-sections "-Wl,-Map=$out/m.map" \
    -o "$out/m" 2> "$out/m.err" || { cat "$out/m.err"; fail "embcc linking a Darwin program"; }
[ "$("$out/m")" = 42 ] || fail "the program EmbCC linked printed $("$out/m")"
[ -s "$out/m.map" ] || fail "-Wl,-Map= wrote no map"
otool -L "$out/m" 2> /dev/null | grep -q CoreFoundation ||
    fail "-framework CoreFoundation is not among the program's libraries"
"$EMBCC" -arch arm64 -pthread -O2 "$out/prog.c" -o "$out/prog2" 2> "$out/p2.err" ||
    { cat "$out/p2.err"; fail "embcc linking prog.c"; }
"$out/prog2" > "$out/got2.txt" && diff "$out/want.txt" "$out/got2.txt" ||
    fail "prog.c linked by EmbCC printed differently"
# -g: a warning, no debug information, and the same code
"$EMBCC" -arch arm64 -O2 -g -c "$out/h.c" -o "$out/hg.o" 2> "$out/g.err" ||
    { cat "$out/g.err"; fail "-g stopped a Darwin compile"; }
grep -q 'warning: -g: no debug information for aarch64-apple-darwin' "$out/g.err" ||
    { cat "$out/g.err"; fail "-g on Darwin gave no warning"; }
"$EMBCC" -arch arm64 -O2 -c "$out/h.c" -o "$out/h0.o"
cmp -s "$out/hg.o" "$out/h0.o" || fail "-g changed a Darwin object"
echo "darwin-host: EmbCC links Darwin programs (sources, -L/-l, -framework, --gc-sections, -Map); -g warns and changes nothing"
