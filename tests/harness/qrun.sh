#!/bin/sh
# Run a QEMU command with a hard timeout, portably.
#
# macOS ships no coreutils `timeout`, and `perl -e alarm; exec` does not
# work here: QEMU installs its own SIGALRM handler, so the alarm is
# swallowed and a hung guest hangs the whole suite. Backgrounding and
# killing is the only reliable form.
#
# The watchdog must not keep the CALLER's stdout open: a caller that captures
# the guest's output with $(...) waits for every holder of that pipe to close
# it, and an orphaned `sleep` would make every run take the full timeout. So
# the watchdog's own stdio is /dev/null, and stopping it stops its sleep.
#
# ---- --until, and why a timeout alone is not enough -----------------------
#
# A bare-metal image does not exit: a reset handler is the bottom of the call
# stack and none of these boards has a semihosting call to stop the emulator
# with. So every run costs its FULL timeout, however quickly the guest finished
# -- and with tests/run.sh exporting EMBCC_QEMU_TIMEOUT=20, a test that boots
# twenty-eight images pays nine minutes to watch twenty-eight programs that
# each took under a second. tests/golden/avr-float.sh is that test, and it
# became the slowest thing in the suite by a factor of ten.
#
# With `--until TEXT` the guest's output goes to a file, and the watchdog polls
# that file for TEXT five times a second: when it appears the guest is killed
# at once. The timeout still applies, so a guest that never prints it is
# bounded exactly as before.
#
# The output is not streamed in this mode -- it is written to a file and
# copied out at the end. That is deliberate. Streaming would mean `tee`, and
# the comment above is about precisely that: a caller capturing output with
# $(...) waits for every holder of that pipe to close it, so an extra process
# in the pipeline reintroduces the hang this script exists to avoid.
#
# usage: qrun.sh <seconds> [--until TEXT] <command> [args...]
#                                          -> the guest's exit status
timeout=$1; shift
until_text=
if [ "${1:-}" = "--until" ]; then
    until_text=$2
    shift 2
fi

if [ -n "$until_text" ]; then
    tmp=${TMPDIR:-/tmp}/qrun.$$
    # stdout to the file, stderr STRAIGHT THROUGH to the caller's stderr. Not
    # `2>&1`: callers separate the two -- tests/harness/avr/run.sh keeps QEMU's
    # own diagnostics on stderr on purpose, because it refuses an image whose
    # ELF entry point is not 0 and says so in one line there, and a test that
    # captures stdout with `> got 2>/dev/null` would otherwise find that
    # warning in its first field. It did.
    "$@" > "$tmp" &
    qpid=$!
    # Five polls a second, for `timeout` seconds. The kill -0 is what makes a
    # guest that exits on its own (a hosted binary, not a board) cost nothing.
    i=0
    lim=$((timeout * 5))
    while [ "$i" -lt "$lim" ]; do
        kill -0 "$qpid" 2>/dev/null || break
        grep -q -- "$until_text" "$tmp" 2>/dev/null && break
        sleep 0.2
        i=$((i + 1))
    done
    kill -9 "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null
    cat "$tmp"
    rm -f "$tmp"
    # Being killed is the expected end here, as it is below, so the status is
    # not the answer -- the sentinel the caller greps for is.
    exit 0
fi

"$@" & qpid=$!
(
    trap 'kill "$s" 2>/dev/null; exit 0' TERM
    sleep "$timeout" & s=$!
    wait "$s"
    kill -9 "$qpid" 2>/dev/null
) </dev/null >/dev/null 2>&1 &
wpid=$!
wait "$qpid" 2>/dev/null; status=$?
kill "$wpid" 2>/dev/null
wait "$wpid" 2>/dev/null
exit $status
