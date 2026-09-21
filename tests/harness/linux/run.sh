#!/bin/sh
# Boot one static Linux program as PID 1 on a REAL Linux kernel.
#
#   usage: run.sh ARCH IMAGE     -> the guest's stdout; its exit status
#          run.sh ARCH --check   -> 0 if this harness could run something
#
# The other harnesses in this directory boot our code on bare metal and
# supply the handful of services it needs themselves. This one does the
# opposite and boots LINUX, because what needs checking is precisely the
# thing a hand-written shim cannot check: whether the syscall numbers,
# argument registers and structure layouts in lib/libc/os/linux are the
# ones the kernel actually uses. A harness that implemented those
# numbers would agree with us about them by construction, and would
# still agree if every one of them were wrong.
#
# So IMAGE is booted as /init, the first and only user process. Nothing
# else is in the initramfs -- no shell, no busybox, no libraries --
# which is only possible because the image is static and complete. If it
# needed anything from a distribution, there would be nothing there to
# give it.
#
# ---- how the exit status gets out ----------------------------------------
#
# PID 1 is not allowed to exit, so when it does the kernel panics -- and
# the panic message carries the status: "Attempted to kill init!
# exitcode=0x00002a00", which is 42 << 8. That is a better source than
# anything the program could print, because the KERNEL is reporting what
# it saw the program return, not the program reporting about itself.
#
# ---- getting a kernel -----------------------------------------------------
#
# None is committed here: it is ten megabytes of somebody else's build.
# Any Linux kernel for the architecture works, and one way to get one is
#
#   curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/\
# x86_64/netboot/vmlinuz-virt
#
# saved as tests/harness/linux/vmlinuz-x86_64 (or -aarch64), or named by
# EMBCC_LINUX_KERNEL_X86_64 / EMBCC_LINUX_KERNEL_AARCH64. With no kernel
# this exits 126 and the caller skips rather than failing: an absent
# kernel is a missing tool, not a broken compiler.
set -u

[ $# -eq 2 ] || { echo "usage: run.sh ARCH IMAGE" >&2; exit 125; }
arch=$1; image=$2
here=$(cd "$(dirname "$0")" && pwd)

case "$arch" in
    x86_64)
        qemu=${EMBCC_QEMU_X86:-qemu-system-x86_64}
        kernel=${EMBCC_LINUX_KERNEL_X86_64:-$here/vmlinuz-x86_64}
        machine="-M q35"; console=ttyS0; native=x86_64 ;;
    aarch64)
        qemu=${EMBCC_QEMU_AARCH64:-qemu-system-aarch64}
        kernel=${EMBCC_LINUX_KERNEL_AARCH64:-$here/vmlinuz-aarch64}
        machine="-M virt"; console=ttyAMA0; native=arm64 ;;
    *)  echo "run.sh: no Linux harness for $arch" >&2; exit 125 ;;
esac

[ -f "$kernel" ] || exit 126
command -v "$qemu" > /dev/null 2>&1 || exit 126

# A caller deciding between running and skipping needs to ask THAT
# question, and asking it by booting something is both slow and a lie
# about what failed. 0 means the kernel and the emulator are both here.
[ "$image" = --check ] && exit 0

[ -f "$image" ] || { echo "run.sh: no such image: $image" >&2; exit 125; }

# Hardware virtualisation where the guest architecture IS the host's --
# which on an Apple Silicon machine makes an aarch64 boot take about a
# twentieth of a second. Everywhere else the emulator does the work,
# which for a program that prints a line and exits is still under two
# seconds.
accel="-accel tcg -cpu max"
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = "$native" ]; then
    accel="-accel hvf -cpu host"
fi

# The initramfs: one file, mode 755, named /init.
work=${TMPDIR:-/tmp}/embcc-linux-$$
rm -rf "$work"; mkdir -p "$work/root"
cp "$image" "$work/root/init"
chmod 755 "$work/root/init"
(cd "$work/root" && find . | cpio -o -H newc) > "$work/initramfs.cpio" 2>/dev/null

# panic=-1 reboots the instant init dies, and -no-reboot turns that
# reboot into QEMU exiting -- so the guest ending is what ends the run,
# not the timeout.
out=$("$here/../qrun.sh" "${EMBCC_QEMU_TIMEOUT:-60}" "$qemu" \
        $machine $accel -m 256 -nographic -no-reboot \
        -kernel "$kernel" -initrd "$work/initramfs.cpio" \
        -append "console=$console panic=-1 rdinit=/init" 2>/dev/null)
qs=$?
rm -rf "$work"

# The guest's own output is what appears after the kernel says it is
# starting init, minus the kernel's own lines -- which all carry a
# "[    1.234567] " timestamp, and nothing from a C program does.
#
# Both halves are needed. Before init there is firmware output (SeaBIOS,
# iPXE) that carries no timestamp and would otherwise look like the
# program's; after it the panic and its backtrace all carry one.
#
# The carriage returns go too. This came out of a SERIAL CONSOLE, and a
# terminal line ends CR LF there -- so every line the guest printed
# arrives with a trailing \r that nothing downstream expects and that
# makes an exact-line comparison fail against text that looks identical.
printf '%s\n' "$out" \
    | sed -n '/Run \/init as init process/,$p' \
    | grep -v '^\[ *[0-9][0-9]*\.[0-9]*\]' \
    | tr -d '\r'

[ "$qs" -eq 137 ] && exit 124                      # killed by the timeout

code=$(printf '%s\n' "$out" \
         | sed -n 's/.*Attempted to kill init.*exitcode=0x\([0-9a-f]*\).*/\1/p' \
         | tail -1)
[ -n "$code" ] || exit 125                         # never reached init
exit $(( (0x$code >> 8) & 0xff ))
