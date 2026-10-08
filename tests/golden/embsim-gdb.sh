#!/bin/sh
# embsim-gdb: EmbSim's GDB server (embsim --gdb), refereed by QEMU's stub.
#
# The same image, the same debugger, the same script, on QEMU's gdbstub
# and on EmbSim's server: the transcripts must be the same. An image built
# with -g -O0 from tests/golden/embsim/gdb-fw.c, for the Cortex-M3
# (lm3s6965evb, flash at 0) and the Cortex-M4 with its FPU (mps2-an386).
#
#  1. gdb, tests/golden/embsim/gdb-session.gdb: connect at reset, break in
#     a function, continue, read the registers, the arguments and the
#     locals, finish, next, step, stepi and nexti, read memory, change a
#     variable (the exit status shows the program saw it), a write
#     watchpoint twice, a read and an access watchpoint, a breakpoint on
#     a source line, every register (the FPU's on the M4), and on to the
#     program's exit. Lines that may differ are made the same first: the
#     port, and the note gdb prints because EmbSim gives it a memory map
#     with flash in it (QEMU's stub has none).
#  2. gdb interrupting a running target (0x03): in a loop, and in a WFI
#     nothing can wake, where EmbSim waits for the debugger rather than
#     ending the run. Where in the loop it stops is QEMU's timing, so the
#     address is left out.
#  3. lldb (gdb-remote): breakpoint, registers, memory, a step.
#  4. EmbDBG's own client (embdbg FILE remote PORT).
#  5. gdb's `load` replacing the image in flash (EmbSim through its memory
#     map, vFlashErase and vFlashWrite; QEMU through plain writes).
#  6. EmbSim only: `monitor` (the counts at a breakpoint must be those of
#     a run without a debugger stopped after as many instructions:
#     --max-insns, --count and --trace say), `monitor reset` keeping what
#     `load` wrote and `monitor reload` not, the memory map and the
#     target description gdb received, a lockup reported as SIGSEGV,
#     detach (the target runs on to its end) and kill.
#
# Every QEMU runs under tests/harness/qrun.sh; every QEMU, EmbSim and
# debugger is bounded by a timeout and killed when the test ends, however
# it ends.
#
# Each of these server bugs, put in on purpose, made it fail, and for its
# own reason: r1 and r2 swapped in the g packet; z0/z1 not removing the
# breakpoint (a stale stop after `delete`); the watchpoint's address
# reported 4 bytes off (gdb no longer recognises the stop); a step that
# ran two instructions; memory read one byte off; the FPU missing from
# the M4's target description; the interrupt byte ignored while running;
# `monitor insns` one high; flash given to gdb as RAM; and a watchpoint
# that stops after the access instead of before it, as QEMU's does.
set -u
echo "TEST-MARKER embsim-gdb"
. "$(dirname "$0")/../lib.sh"

QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
GDB=${EMBCC_GDB:-gdb}
LLDB=${EMBCC_LLDB:-lldb}
EMBCC=$(cd "$(dirname "${EMBCC:-./embcc}")" && pwd)/$(basename "${EMBCC:-./embcc}")
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
EMBDBG=${EMBDBG:-$(dirname "$EMBCC")/embdbg}
out=tests/golden/out/embsim-gdb
rm -rf "$out"; mkdir -p "$out"
src=tests/golden/embsim

# ---- every process started is stopped when the test ends ----------------
pids=
ports=
cleanup() {
    for p in $pids; do
        pkill -P "$p" 2>/dev/null
        kill "$p" 2>/dev/null
    done
    for p in $ports; do
        pkill -f "tcp::$p\$" 2>/dev/null
        pkill -f "gdb $p( |\$)" 2>/dev/null
    done
}
trap cleanup EXIT
trap 'cleanup; exit 1' INT TERM
fail() { echo "FAIL: $*"; exit 1; }

command -v "$GDB" >/dev/null 2>&1 || { echo "skipped: no gdb"; exit 0; }
"$GDB" -nx -batch -ex 'set architecture arm' 2>&1 | grep -q 'architecture is set to "arm"' ||
    { echo "skipped: $GDB has no ARM support"; exit 0; }
command -v "$QA" >/dev/null 2>&1 || { echo "skipped: no $QA to referee with"; exit 0; }
[ -x "$EMBSIM" ] || fail "$EMBSIM is not built (make embsim)"

port=$(( 31000 + ($$ % 1000) * 40 ))     # 36 used
next_port() { port=$((port + 1)); ports="$ports $port"; }

# bounded SECONDS OUT CMD...: CMD with its output in OUT (its input from
# $IN), killed after SECONDS; its status
bounded() {
    secs=$1; o=$2; shift 2
    "$@" < "${IN:-/dev/null}" > "$o" 2>&1 &
    bp=$!
    (
        trap 'kill "$s" 2>/dev/null; exit 0' TERM
        sleep "$secs" & s=$!
        wait "$s"
        kill -9 "$bp" 2>/dev/null
    ) </dev/null >/dev/null 2>&1 &
    wd=$!
    wait "$bp"; st=$?
    kill "$wd" 2>/dev/null
    wait "$wd" 2>/dev/null
    return $st
}

# qemu_at PORT ELF MACHINE-ARGS...: QEMU halted at reset with its stub
qemu_at() {
    p=$1; e=$2; shift 2
    sh tests/harness/qrun.sh 120 "$QA" "$@" -nographic -semihosting \
        -kernel "$e" -S -gdb "tcp::$p" > /dev/null 2>&1 &
    pids="$pids $!"
}
# sim_at PORT ELF EMBSIM-ARGS...: EmbSim halted at reset with its server;
# its status is written to sim.PORT.st when it ends. RUN=1: running, not
# waiting for the debugger.
sim_at() {
    p=$1; e=$2; shift 2
    w=--gdb-wait
    [ "${RUN:-0}" = 1 ] && w=
    ( "$EMBSIM" "$e" "$@" --gdb $p $w > "$out/sim.$p.out" 2> "$out/sim.$p.err"
      echo $? > "$out/sim.$p.st" ) &
    pids="$pids $!"
}
# sim_status PORT: EmbSim's exit status once it has ended (or "running")
sim_status() {
    i=0
    while [ ! -s "$out/sim.$1.st" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    cat "$out/sim.$1.st" 2>/dev/null || echo running
}
# gdb_run PORT ELF SCRIPT OUT: a batch gdb session; gdb retries the
# connection until the stub listens (set tcp auto-retry, the default)
gdb_run() {
    bounded 120 "$4" "$GDB" -nx -batch -ex "file $2" \
        -ex "target remote localhost:$1" -x "$3"
}
# the lines that legitimately differ
norm() {
    sed -e "s/localhost:$2/localhost:PORT/g" \
        -e '/^Note: automatically using hardware breakpoints for read-only addresses\.$/d' \
        -e 's/^Transfer rate: .*/Transfer rate: .../' "$1"
}
same() {        # same TAG QFILE QPORT EFILE EPORT
    norm "$2" "$3" > "$out/$1.qemu"
    norm "$4" "$5" > "$out/$1.embsim"
    cmp -s "$out/$1.qemu" "$out/$1.embsim" || {
        diff "$out/$1.qemu" "$out/$1.embsim" | head -40
        fail "$1: EmbSim's transcript is not QEMU's"; }
}
want() {        # want FILE PATTERN WHAT: the transcript says it
    grep -q -- "$2" "$1" || { tail -20 "$1"; fail "$3"; }
}

# ---- the images --------------------------------------------------------------
"$EMBCC" --target=thumbv7m-none-eabi -g -O0 -c $src/gdb-fw.c -o "$out/m3.o" &&
"$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/m3.o" -o "$out/m3.elf" &&
"$EMBCC" --target=thumbv7m-none-eabi -g -O0 -DVARIANT=5 -c $src/gdb-fw.c -o "$out/m3v.o" &&
"$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/m3v.o" -o "$out/m3v.elf" &&
"$EMBCC" --target=thumbv7m-none-eabi -g -O0 -DSPIN=1 -c $src/gdb-fw.c -o "$out/m3s.o" &&
"$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/m3s.o" -o "$out/m3s.elf" &&
"$EMBCC" --target=thumbv7m-none-eabi -g -O0 -DIDLE=1 -c $src/gdb-fw.c -o "$out/m3i.o" &&
"$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/m3i.o" -o "$out/m3i.elf" &&
"$EMBCC" --target=thumbv7em-none-eabihf -g -O0 -c $src/gdb-fw.c -o "$out/m4.o" &&
"$EMBLD" -e reset -Ttext 0 -Tdata 0x20000000 "$out/m4.o" -o "$out/m4.elf" ||
    fail "the images do not build"
"$EMBSIM" "$out/m3.elf" > /dev/null 2>&1; st=$?
[ $st = 71 ] || fail "m3.elf exits $st without a debugger, not 71"
"$EMBSIM" "$out/m4.elf" --board mps2-an386 > /dev/null 2>&1; st=$?
[ $st = 65 ] || fail "m4.elf exits $st without a debugger, not 65"
sum=$(grep -n 'SUM-LINE' $src/gdb-fw.c | cut -d: -f1)
sed "s/@SUM@/$sum/" $src/gdb-session.gdb > "$out/session.gdb"

M3Q="-M lm3s6965evb -cpu cortex-m3"; M3E="--board lm3s6965evb"
M4Q="-M mps2-an386 -cpu cortex-m4"; M4E="--board mps2-an386"

# ---- 1. the gdb session --------------------------------------------------------
for cfg in "m3|$M3Q|$M3E|0134" "m4|$M4Q|$M4E|0114"; do
    IFS='|' read tag qa ea code <<EOF
$cfg
EOF
    next_port; qp=$port; next_port; ep=$port
    # shellcheck disable=SC2086
    qemu_at $qp "$out/$tag.elf" $qa
    # shellcheck disable=SC2086
    sim_at $ep "$out/$tag.elf" $ea
    gdb_run $qp "$out/$tag.elf" "$out/session.gdb" "$out/$tag.gdb-qemu"
    gdb_run $ep "$out/$tag.elf" "$out/session.gdb" "$out/$tag.gdb-embsim"
    same "$tag.gdb" "$out/$tag.gdb-qemu" $qp "$out/$tag.gdb-embsim" $ep
    # both can be wrong the same way, so the transcript must also say
    # what the session did
    t=$out/$tag.gdb-embsim
    want "$t" '^Breakpoint 1, compute (a=0, b=2)' "$tag: no stop at compute(0, 2)"
    want "$t" '^Value returned is \$1 = 1' "$tag: finish did not return 1"
    want "$t" '^\$3 = {x = 10, y = -4}' "$tag: the variable was not changed"
    want "$t" '^New value = 11' "$tag: the write watchpoint did not stop twice"
    want "$t" '^Hardware read watchpoint 3: table\[3\]' "$tag: no read watchpoint stop"
    want "$t" '^Value = 4' "$tag: the read watchpoint's value"
    want "$t" '^Hardware access (read/write) watchpoint 5: scale' "$tag: no access watchpoint stop"
    want "$t" "exited with code $code\]" "$tag: the program did not run on to exit $code"
    [ "$(sim_status $ep)" = $((0$code)) ] ||
        fail "$tag: EmbSim's exit status is $(sim_status $ep), not $((0$code))"
    if [ $tag = m4 ]; then
        for r in d0 d15 fpscr s0 s31; do
            want "$t" "^$r " "m4: the FPU's $r is not among the registers"
        done
    fi
    echo "embsim-gdb: $tag: gdb's session on EmbSim is its session on QEMU ($(wc -l < "$out/$tag.gdb.qemu" | tr -d ' ') lines): breakpoints, steps, registers, memory, a variable written, write, read and access watchpoints"
done

# ---- 2. interrupting a running target ------------------------------------------
# gdb is sent SIGINT, as a terminal's ^C does, and sends the stub 0x03.
cat > "$out/interrupt.gdb" <<'EOF'
set pagination off
set confirm off
break main
continue
delete
set var spin = 1
set var idle = 1
continue
print spin
print idle
set var spin = 0
continue
print idle
EOF
# QEMU's core stays asleep in the WFI when the debugger resumes it; a
# part's wakes, as halting is a wake-up event, and so does EmbSim's
{ cat "$out/interrupt.gdb"; printf 'set var idle = 0\ncontinue\n'; } > "$out/wfi.gdb"
echo kill >> "$out/interrupt.gdb"
interrupt_run() {       # PORT ELF OUT [SCRIPT]
    "$GDB" -nx -batch -ex "file $2" -ex "target remote localhost:$1" \
        -x "${4:-$out/interrupt.gdb}" > "$3" 2>&1 &
    gp=$!
    pids="$pids $gp"
    for k in 1 2; do
        sleep 2
        kill -INT $gp 2>/dev/null
    done
    i=0
    while kill -0 $gp 2>/dev/null && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
    kill -9 $gp 2>/dev/null
}
next_port; qp=$port; next_port; ep=$port
# shellcheck disable=SC2086
qemu_at $qp "$out/m3.elf" $M3Q
sim_at $ep "$out/m3.elf" $M3E
interrupt_run $qp "$out/m3.elf" "$out/int.qemu-raw"
interrupt_run $ep "$out/m3.elf" "$out/int.embsim-raw"
for w in qemu embsim; do
    sed 's/^0x[0-9a-f]* in main ()/main ()/' "$out/int.$w-raw" > "$out/int.$w"
done
same interrupt "$out/int.qemu" $qp "$out/int.embsim" $ep
[ "$(grep -c '^Program received signal SIGINT' "$out/int.embsim")" = 2 ] ||
    { cat "$out/int.embsim"; fail "interrupt: two SIGINT stops expected"; }
want "$out/int.embsim" '^\$3 = 1' "interrupt: the session did not reach the WFI"
next_port; ep=$port
sim_at $ep "$out/m3.elf" $M3E
interrupt_run $ep "$out/m3.elf" "$out/wfi.embsim" "$out/wfi.gdb"
want "$out/wfi.embsim" 'exited with code 0107\]' "interrupt: the program did not run on from the WFI"
echo "embsim-gdb: interrupting the target (0x03), in a loop and in a WFI nothing can wake, as on QEMU; resumed, the WFI completes as a part's does"

# ---- 3. lldb ---------------------------------------------------------------------
if command -v "$LLDB" >/dev/null 2>&1; then
    next_port; qp=$port; next_port; ep=$port
    # shellcheck disable=SC2086
    qemu_at $qp "$out/m4.elf" $M4Q
    # shellcheck disable=SC2086
    sim_at $ep "$out/m4.elf" $M4E
    for p in $qp $ep; do
        bounded 120 "$out/lldb.$p" "$LLDB" --batch -o "target create $out/m4.elf" \
            -o "gdb-remote localhost:$p" -o 'breakpoint set -n sum_table' \
            -o continue -o 'register read' -o 'register read r0 r1 pc sp lr' \
            -o 'memory read -s4 -fx -c8 0x20000000' -o step \
            -o 'frame variable' -o kill
    done
    same lldb "$out/lldb.$qp" $qp "$out/lldb.$ep" $ep
    want "$out/lldb.embsim" 'stop reason = breakpoint 1.1' "lldb: no breakpoint stop"
    want "$out/lldb.embsim" 'd15 = ' "lldb: no FPU registers"
    want "$out/lldb.embsim" '0x20000000: 0x00000001 0x00000002 0x0000000e' "lldb: memory"
    echo "embsim-gdb: lldb (gdb-remote): breakpoint, registers with the FPU's, memory and a step as on QEMU"
else
    echo "embsim-gdb: no lldb here: its check did not run"
fi

# ---- 4. EmbDBG's client ------------------------------------------------------------
if [ -x "$EMBDBG" ]; then
    for cfg in "m3|$M3Q|$M3E" "m4|$M4Q|$M4E"; do
        IFS='|' read tag qa ea <<EOF
$cfg
EOF
        next_port; qp=$port; next_port; ep=$port
        # shellcheck disable=SC2086
        qemu_at $qp "$out/$tag.elf" $qa
        # shellcheck disable=SC2086
        sim_at $ep "$out/$tag.elf" $ea
        for p in $qp $ep; do
            printf '%s\n' 'break compute' continue regs bt locals 'print counter' \
                stepi step 'mem 0x20000000 32' 'set r0 7' regs delete \
                'break sum_table' continue 'print n' delete continue quit \
                > "$out/embdbg.cmds"
            i=0
            while [ $i -lt 50 ]; do
                IN=$out/embdbg.cmds bounded 60 "$out/embdbg.$tag.$p" \
                    "$EMBDBG" "$out/$tag.elf" remote "localhost:$p"
                grep -q '^connected to' "$out/embdbg.$tag.$p" && break
                i=$((i + 1)); sleep 0.1
            done
        done
        same "embdbg.$tag" "$out/embdbg.$tag.$qp" $qp "$out/embdbg.$tag.$ep" $ep
        t=$out/embdbg.$tag.embsim
        want "$t" '^stopped at .*compute' "embdbg $tag: no stop in compute"
        want "$t" '^r0    00000000  r1    00000002' "embdbg $tag: compute(0, 2)'s arguments"
        want "$t" '^r0 = 0x7' "embdbg $tag: a register write"
        want "$t" '^n = 8' "embdbg $tag: sum_table's argument"
        want "$t" 'the target exited' "embdbg $tag: the run to the end"
    done
    echo "embsim-gdb: EmbDBG's remote client on EmbSim is as on QEMU, on m3 and m4"
else
    echo "embsim-gdb: no embdbg built: its check did not run"
fi

# ---- 5. gdb's load -------------------------------------------------------------------
# The stub holds the VARIANT image (exit 81); gdb loads the other one
# (exit 71) and runs it.
printf 'set pagination off\nset confirm off\nload\ncontinue\n' > "$out/load.gdb"
next_port; qp=$port; next_port; ep=$port
# shellcheck disable=SC2086
qemu_at $qp "$out/m3v.elf" $M3Q
sim_at $ep "$out/m3v.elf" $M3E
gdb_run $qp "$out/m3.elf" "$out/load.gdb" "$out/load.qemu-raw"
gdb_run $ep "$out/m3.elf" "$out/load.gdb" "$out/load.embsim-raw"
same load "$out/load.qemu-raw" $qp "$out/load.embsim-raw" $ep
want "$out/load.embsim" '^Loading section .text' "load: nothing loaded"
want "$out/load.embsim" 'exited with code 0107\]' "load: the loaded image did not run (exit 71)"
echo "embsim-gdb: gdb's load programs the flash and the loaded image runs, as on QEMU"

# ---- 6. EmbSim only ------------------------------------------------------------------
# the counts at a breakpoint, and the monitor's other commands
cat > "$out/monitor.gdb" <<'EOF'
set pagination off
set confirm off
info mem
maint print xml-tdesc
break sum_table
continue
monitor insns
monitor cycles
monitor stats
monitor help
monitor no-such-command
monitor reset
monitor insns
info registers pc
kill
EOF
for cfg in "m3|$M3E|thumbv7m" "m4|$M4E|thumbv7em"; do
    IFS='|' read tag ea x <<EOF
$cfg
EOF
    next_port; ep=$port
    # shellcheck disable=SC2086
    sim_at $ep "$out/$tag.elf" $ea
    gdb_run $ep "$out/$tag.elf" "$out/monitor.gdb" "$out/monitor.$tag"
    t=$out/monitor.$tag
    [ "$(sim_status $ep)" = 0 ] || fail "$tag: kill: EmbSim's status is $(sim_status $ep), not 0"
    want "$t" '^\[Inferior 1 (process 1) killed\]' "$tag: kill"
    bp=$(sed -n 's/^Breakpoint 1 at \(0x[0-9a-f]*\): .*/\1/p' "$t")
    n=$(sed -n '/^Breakpoint 1, sum_table/,$p' "$t" | grep -m1 '^[0-9][0-9]*$')
    c=$(sed -n '/^Breakpoint 1, sum_table/,$p' "$t" | grep '^[0-9][0-9]*$' | sed -n 2p)
    [ -n "$bp" ] && [ -n "$n" ] && [ -n "$c" ] || { cat "$t"; fail "$tag: no counts from the monitor"; }
    want "$t" "^$n instructions, $c cycles (est.)" "$tag: monitor stats is not insns and cycles"
    want "$t" '^cycles  *the estimated cycles so far' "$tag: monitor help"
    want "$t" "unknown monitor command 'no-such-command'" "$tag: an unknown monitor command"
    want "$t" '^embsim: reset' "$tag: monitor reset"
    [ "$(sed -n '/^embsim: reset/,$p' "$t" | grep -m1 '^[0-9][0-9]*$')" = 0 ] ||
        fail "$tag: the count is not 0 after monitor reset"
    # without a debugger: as many instructions, the same cycles; and the
    # next instruction is the breakpoint's
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/$tag.elf" $ea --max-insns $n --count "$out/count.$tag" > /dev/null 2>&1
    [ "$(sed -n 2p "$out/count.$tag")" = "$c" ] ||
        fail "$tag: $n instructions are $(sed -n 2p "$out/count.$tag") cycles without a debugger, $c with"
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/$tag.elf" $ea --max-insns $((n + 1)) --trace "$out/trace.$tag" > /dev/null 2>&1
    last=0x$(tail -1 "$out/trace.$tag" | cut -d: -f1 | sed 's/^0*//')
    [ $((last)) = $((bp)) ] ||
        fail "$tag: instruction $((n + 1)) is at $last, not at the breakpoint $bp"
    echo "embsim-gdb: $tag: monitor insns, cycles, stats, help, reset; $n instructions and $c cycles to the breakpoint, as a run without a debugger counts them"
done
# the memory map and the target description gdb received
t=$out/monitor.m3
want "$t" '0x00000000 0x00040000 flash blocksize 0x400 nocache' "m3: flash at 0 is not in the memory map"
want "$t" '0x20000000 0x20010000 rw nocache' "m3: SRAM is not in the memory map"
want "$t" 'org.gnu.gdb.arm.m-profile' "m3: the target description"
want "$t" 'name="basepri"' "m3: basepri in the target description"
grep -q 'org.gnu.gdb.arm.vfp' "$t" && fail "m3: an FPU in the M3's target description"
t=$out/monitor.m4
want "$t" '0x00000000 0x00400000 rw nocache' "m4: the MPS2's SSRAM at 0 is not ram in the memory map"
want "$t" 'org.gnu.gdb.arm.vfp' "m4: no FPU in the M4's target description"
want "$t" 'name="d15"' "m4: no d15 in the target description"
echo "embsim-gdb: the memory map (flash and RAM) and the target descriptions (the FPU on the M4 only) as gdb received them"

# monitor reset keeps what load wrote; monitor reload brings the file back
for how in reset:0107 reload:0121; do
    cmd=${how%%:*}; code=${how#*:}
    printf 'set pagination off\nset confirm off\nload\nmonitor %s\ncontinue\n' "$cmd" > "$out/$cmd.gdb"
    next_port; ep=$port
    sim_at $ep "$out/m3v.elf" $M3E
    gdb_run $ep "$out/m3.elf" "$out/$cmd.gdb" "$out/$cmd.out"
    want "$out/$cmd.out" "exited with code $code\]" "monitor $cmd after load: not exit $((0$code))"
done
echo "embsim-gdb: monitor reset keeps the image gdb loaded into flash; monitor reload reads the file again"

# a lockup is a SIGSEGV stop; detach lets the target run on to its end
printf 'set pagination off\nset confirm off\nbreak main\ncontinue\ndelete\nset var trap = 1\ncontinue\ncontinue\nkill\n' > "$out/lockup.gdb"
next_port; ep=$port
sim_at $ep "$out/m3.elf" $M3E
gdb_run $ep "$out/m3.elf" "$out/lockup.gdb" "$out/lockup.out"
[ "$(grep -c '^Program received signal SIGSEGV' "$out/lockup.out")" = 2 ] ||
    { cat "$out/lockup.out"; fail "a lockup is not a SIGSEGV stop, twice"; }
printf 'set pagination off\nset confirm off\nbreak compute\ncontinue\ndetach\n' > "$out/detach.gdb"
next_port; ep=$port
sim_at $ep "$out/m3.elf" $M3E
gdb_run $ep "$out/m3.elf" "$out/detach.gdb" "$out/detach.out"
want "$out/detach.out" '^\[Inferior 1 (process 1) detached\]' "detach"
[ "$(sim_status $ep)" = 71 ] || fail "after detach the run did not end with 71 but $(sim_status $ep)"
echo "embsim-gdb: a lockup stops as SIGSEGV; after detach the target runs to its end (71); kill ends EmbSim with 0"

# without --gdb-wait the image runs, and a debugger attaches to it where
# it is: in a loop, or in a WFI nothing can wake (where it waits for one)
for v in spin:m3s idle:m3i; do
    var=${v%%:*}; img=${v#*:}
    printf 'set pagination off\nset confirm off\nprint %s\nset var %s = 0\ncontinue\n' \
        "$var" "$var" > "$out/attach.gdb"
    next_port; ep=$port
    RUN=1 sim_at $ep "$out/$img.elf" $M3E
    sleep 1
    gdb_run $ep "$out/$img.elf" "$out/attach.gdb" "$out/attach.$var"
    want "$out/attach.$var" '^\$1 = 1' "attach ($var): the image was not where it waits"
    want "$out/attach.$var" 'exited with code 0107\]' "attach ($var): it did not run on to its end"
done
echo "embsim-gdb: without --gdb-wait, a debugger attaches to the running image (a loop, and a WFI nothing wakes)"

# ---- 7. RISC-V ---------------------------------------------------------------------
# gdb-fw.c for RV32 (soft-float: an integer `real`) and RV64 with D (a
# float one) on virt, QEMU's stub against EmbSim's. The session is the
# M-profile one with RISC-V's registers where it names the core's own:
# mstatus, mcause, mtvec, priv and mie, and the general and float groups
# for every register (the CSR group is QEMU's long list of the
# supervisor's and hypervisor's, which EmbSim does not have).
Q32=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
Q64=${EMBCC_QEMU_RISCV64:-qemu-system-riscv64}
# qemu_rv PORT QEMU ELF: QEMU's virt halted at reset with its stub
qemu_rv() {
    sh tests/harness/qrun.sh 120 "$2" -M virt -bios none -m 8 -nographic \
        -kernel "$3" -S -gdb "tcp::$1" > /dev/null 2>&1 &
    pids="$pids $!"
}
if command -v "$Q32" >/dev/null 2>&1 && command -v "$Q64" >/dev/null 2>&1 &&
   "$GDB" -nx -batch -ex 'set architecture riscv:rv64' 2>&1 | grep -q 'riscv:rv64'; then
    RVE="--board virt --ram-size 8M"
    RUN=0                       # a function's prefix assignment above can persist
    "$EMBCC" --target=riscv32-unknown-elf -g -O0 -c $src/gdb-fw.c -o "$out/rv32.o" &&
    "$EMBLD" -e reset -Ttext 0x80000000 -Tstack 0x80800000 "$out/rv32.o" -o "$out/rv32.elf" &&
    "$EMBCC" --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -g -O0 \
        -c $src/gdb-fw.c -o "$out/rv64.o" &&
    "$EMBLD" -e reset -Ttext 0x80000000 -Tstack 0x80800000 "$out/rv64.o" -o "$out/rv64.elf" ||
        fail "the RISC-V images do not build"
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/rv32.elf" $RVE > /dev/null 2>&1; st=$?
    [ $st = 71 ] || fail "rv32.elf exits $st without a debugger, not 71"
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/rv64.elf" $RVE > /dev/null 2>&1; st=$?
    [ $st = 65 ] || fail "rv64.elf exits $st without a debugger, not 65"
    sed -e 's/^print \$xpsr$/print $mstatus/' -e 's/^print \$msp$/print $mcause/' \
        -e 's/^print \$psp$/print $mtvec/' -e 's/^print \$control$/print $priv/' \
        -e 's/^print \$primask$/print $mie/' \
        -e 's/^info all-registers$/info registers\
info registers float/' "$out/session.gdb" > "$out/rv-session.gdb"
    for cfg in "rv32|$Q32|0134" "rv64|$Q64|0114"; do
        IFS='|' read tag q code <<EOT
$cfg
EOT
        next_port; qp=$port; next_port; ep=$port
        qemu_rv $qp "$q" "$out/$tag.elf"
        # shellcheck disable=SC2086
        sim_at $ep "$out/$tag.elf" $RVE
        gdb_run $qp "$out/$tag.elf" "$out/rv-session.gdb" "$out/$tag.gdb-qemu"
        gdb_run $ep "$out/$tag.elf" "$out/rv-session.gdb" "$out/$tag.gdb-embsim"
        same "$tag.gdb" "$out/$tag.gdb-qemu" $qp "$out/$tag.gdb-embsim" $ep
        t=$out/$tag.gdb-embsim
        want "$t" '^0x0*1000 in ?? ()' "$tag: not stopped at the reset ROM"
        want "$t" '^Breakpoint 1, compute (a=0, b=2)' "$tag: no stop at compute(0, 2)"
        want "$t" '^Value returned is \$1 = 1' "$tag: finish did not return 1"
        want "$t" '^\$3 = {x = 10, y = -4}' "$tag: the variable was not changed"
        want "$t" '^New value = 11' "$tag: the write watchpoint did not stop twice"
        want "$t" '^Hardware read watchpoint 3: table\[3\]' "$tag: no read watchpoint stop"
        want "$t" '^Value = 4' "$tag: the read watchpoint's value"
        want "$t" '^Hardware access (read/write) watchpoint 5: scale' "$tag: no access watchpoint stop"
        want "$t" '^a0  ' "$tag: no general registers"
        want "$t" '^fcsr  ' "$tag: no floating-point registers"
        want "$t" "exited with code $code\]" "$tag: the program did not run on to exit $code"
        [ "$(sim_status $ep)" = $((0$code)) ] ||
            fail "$tag: EmbSim's exit status is $(sim_status $ep), not $((0$code))"
        echo "embsim-gdb: $tag: gdb's session on EmbSim is its session on QEMU ($(wc -l < "$out/$tag.gdb.qemu" | tr -d ' ') lines): breakpoints, steps, registers (the FPU's), memory, a variable written, write, read and access watchpoints"
    done

    # interrupting the running target, in a loop and in a WFI nothing wakes
    next_port; qp=$port; next_port; ep=$port
    qemu_rv $qp "$Q64" "$out/rv64.elf"
    # shellcheck disable=SC2086
    sim_at $ep "$out/rv64.elf" $RVE
    interrupt_run $qp "$out/rv64.elf" "$out/rvint.qemu-raw"
    interrupt_run $ep "$out/rv64.elf" "$out/rvint.embsim-raw"
    for w in qemu embsim; do
        sed 's/^0x[0-9a-f]* in main ()/main ()/' "$out/rvint.$w-raw" > "$out/rvint.$w"
    done
    same rv-interrupt "$out/rvint.qemu" $qp "$out/rvint.embsim" $ep
    [ "$(grep -c '^Program received signal SIGINT' "$out/rvint.embsim")" = 2 ] ||
        { cat "$out/rvint.embsim"; fail "rv64 interrupt: two SIGINT stops expected"; }
    echo "embsim-gdb: rv64: interrupting the target (0x03), in a loop and in a WFI nothing can wake, as on QEMU"

    # EmbSim only: the counts at a breakpoint, the target description
    next_port; ep=$port
    # shellcheck disable=SC2086
    sim_at $ep "$out/rv64.elf" $RVE
    gdb_run $ep "$out/rv64.elf" "$out/monitor.gdb" "$out/monitor.rv64"
    t=$out/monitor.rv64
    n=$(sed -n '/^Breakpoint 1, sum_table/,$p' "$t" | grep -m1 '^[0-9][0-9]*$')
    c=$(sed -n '/^Breakpoint 1, sum_table/,$p' "$t" | grep '^[0-9][0-9]*$' | sed -n 2p)
    bp=$(sed -n 's/^Breakpoint 1 at \(0x[0-9a-f]*\): .*/\1/p' "$t")
    [ -n "$n" ] && [ -n "$c" ] && [ -n "$bp" ] || { cat "$t"; fail "rv64: no counts from the monitor"; }
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/rv64.elf" $RVE --max-insns $n --count "$out/count.rv64" > /dev/null 2>&1
    [ "$(sed -n 2p "$out/count.rv64")" = "$c" ] ||
        fail "rv64: $n instructions are $(sed -n 2p "$out/count.rv64") cycles without a debugger, $c with"
    # shellcheck disable=SC2086
    "$EMBSIM" "$out/rv64.elf" $RVE --max-insns $((n + 1)) --trace "$out/trace.rv64" > /dev/null 2>&1
    last=0x$(tail -1 "$out/trace.rv64" | cut -d: -f1 | sed 's/^0*//')
    [ $((last)) = $((bp)) ] ||
        fail "rv64: instruction $((n + 1)) is at $last, not at the breakpoint $bp"
    want "$t" 'org.gnu.gdb.riscv.cpu' "rv64: the target description"
    want "$t" 'org.gnu.gdb.riscv.fpu' "rv64: the FPU in the target description"
    want "$t" 'name="mcycle"' "rv64: mcycle in the target description"
    want "$t" '0x0000000080000000 0x0000000080800000 rw nocache' "rv64: RAM is not in the memory map"
    echo "embsim-gdb: rv64: $n instructions and $c cycles to the breakpoint, as a run without a debugger counts them; the target description and the memory map"

    # a trap with nowhere to go is a SIGSEGV stop
    next_port; ep=$port
    # shellcheck disable=SC2086
    sim_at $ep "$out/rv32.elf" $RVE
    gdb_run $ep "$out/rv32.elf" "$out/lockup.gdb" "$out/rvlockup.out"
    [ "$(grep -c '^Program received signal SIGSEGV' "$out/rvlockup.out")" = 2 ] ||
        { cat "$out/rvlockup.out"; fail "rv32: a trap with nowhere to go is not a SIGSEGV stop, twice"; }
    echo "embsim-gdb: rv32: a trap with nowhere to go stops as SIGSEGV"
else
    echo "embsim-gdb: no qemu-system-riscv32/64 or no RISC-V gdb: the RISC-V checks did not run"
fi

# ---- 8. AVR --------------------------------------------------------------------------
# gdb-fw.c for the ATmega328P, with the AVR harness (whose boot calls main
# and then waits for ever: the session ends at main's return, printing
# what main returns, and kills the target). The session is the M-profile
# one without the watchpoints -- QEMU's AVR stub closes the connection at
# the first -- and with SREG and the PC where it prints the core's own
# registers.
QAVR=${EMBCC_QEMU_AVR:-qemu-system-avr}
# qemu_avr PORT ELF: QEMU's uno halted at reset with its stub
qemu_avr() {
    sh tests/harness/qrun.sh 120 "$QAVR" -M uno -nographic -bios "$2" \
        -S -gdb "tcp::$1" > /dev/null 2>&1 &
    pids="$pids $!"
}
if command -v "$QAVR" >/dev/null 2>&1 &&
   "$GDB" -nx -batch -ex 'set architecture avr:5' 2>&1 | grep -q 'avr:5'; then
    AH=$out/avr-h; mkdir -p "$AH"
    "$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$AH/boot.o" &&
    "$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$AH/io.o" &&
    "$EMBCC" --target=avr -Os -c lib/rt/avr.c -o "$AH/rt.o" &&
    "$EMBCC" --target=avr -g -O0 -c $src/gdb-fw.c -o "$out/avr.o" &&
    EMBCC_AVR_HARNESS=$AH sh tests/harness/avr/link.sh "$out/avr.elf" "$out/avr.o" \
        > /dev/null 2>&1 || fail "the AVR image does not build"
    "$EMBSIM" "$out/avr.elf" --board uno > /dev/null 2>&1 ||
        fail "avr.elf does not run to the harness's loop on EmbSim"
    ret=$(grep -n '^    return acc;' $src/gdb-fw.c | cut -d: -f1)
    awk -v ret="$ret" '
        /^(watch|rwatch|awatch) / { skip = 1; next }
        skip && ($0 == "continue" || $0 == "bt") { next }
        { skip = 0 }
        $0 == "print $xpsr" { l[n++] = "print $sreg"; next }
        $0 == "print $msp" { l[n++] = "print/x $pc"; next }
        $0 == "print $psp" || $0 == "print $control" || $0 == "print $primask" { next }
        $0 == "info all-registers" { l[n++] = "info registers"; next }
        { l[n++] = $0 }
        END { for (i = 0; i < n - 1; i++) print l[i]
              print "break gdb-fw.c:" ret; print "continue"; print "print acc"; print "kill" }
    ' "$out/session.gdb" > "$out/avr-session.gdb"
    next_port; qp=$port; next_port; ep=$port
    qemu_avr $qp "$out/avr.elf"
    sim_at $ep "$out/avr.elf" --board uno
    gdb_run $qp "$out/avr.elf" "$out/avr-session.gdb" "$out/avr.gdb-qemu"
    gdb_run $ep "$out/avr.elf" "$out/avr-session.gdb" "$out/avr.gdb-embsim"
    same avr.gdb "$out/avr.gdb-qemu" $qp "$out/avr.gdb-embsim" $ep
    t=$out/avr.gdb-embsim
    want "$t" '^0x00000000 in __vectors ()' "avr: not stopped at the reset vector"
    want "$t" '^Breakpoint 1, compute (a=0, b=2)' "avr: no stop at compute(0, 2)"
    want "$t" '^Value returned is \$1 = 1' "avr: finish did not return 1"
    want "$t" '^\$3 = {x = 10, y = -4}' "avr: the variable was not changed"
    want "$t" '^SREG  ' "avr: no SREG among the registers"
    want "$t" '= 92$' "avr: main did not see the variable written (acc 92)"
    want "$t" '^\[Inferior 1 (process 1) killed\]' "avr: kill"
    [ "$(sim_status $ep)" = 0 ] || fail "avr: kill: EmbSim's status is $(sim_status $ep), not 0"
    echo "embsim-gdb: avr: gdb's session on EmbSim is its session on QEMU ($(wc -l < "$out/avr.gdb.qemu" | tr -d ' ') lines): breakpoints, finish, steps, registers, memory, a variable written and seen"

    # interrupting the running target: in a loop, and asleep
    next_port; qp=$port; next_port; ep=$port
    qemu_avr $qp "$out/avr.elf"
    sim_at $ep "$out/avr.elf" --board uno
    interrupt_run $qp "$out/avr.elf" "$out/avrint.qemu-raw"
    interrupt_run $ep "$out/avr.elf" "$out/avrint.embsim-raw"
    for w in qemu embsim; do
        sed 's/^0x[0-9a-f]* in main ()/main ()/' "$out/avrint.$w-raw" > "$out/avrint.$w"
    done
    same avr-interrupt "$out/avrint.qemu" $qp "$out/avrint.embsim" $ep
    [ "$(grep -c '^Program received signal SIGINT' "$out/avrint.embsim")" = 2 ] ||
        { cat "$out/avrint.embsim"; fail "avr interrupt: two SIGINT stops expected"; }
    echo "embsim-gdb: avr: interrupting the target (0x03), in a loop and asleep, as on QEMU"

    # EmbSim only: the counts at a breakpoint, the description
    next_port; ep=$port
    sim_at $ep "$out/avr.elf" --board uno
    gdb_run $ep "$out/avr.elf" "$out/monitor.gdb" "$out/monitor.avr"
    t=$out/monitor.avr
    n=$(sed -n '/^Breakpoint 1, sum_table/,$p' "$t" | grep -m1 '^[0-9][0-9]*$')
    c=$(sed -n '/^Breakpoint 1, sum_table/,$p' "$t" | grep '^[0-9][0-9]*$' | sed -n 2p)
    [ -n "$n" ] && [ -n "$c" ] || { cat "$t"; fail "avr: no counts from the monitor"; }
    "$EMBSIM" "$out/avr.elf" --board uno --max-insns $n --count "$out/count.avr" > /dev/null 2>&1
    [ "$(sed -n 2p "$out/count.avr")" = "$c" ] ||
        fail "avr: $n instructions are $(sed -n 2p "$out/count.avr") cycles without a debugger, $c with"
    want "$t" 'org.gnu.gdb.avr.cpu' "avr: the target description"
    echo "embsim-gdb: avr: $n instructions and $c cycles to the breakpoint, as a run without a debugger counts them; the target description"

    # EmbSim only: a write watchpoint, which stops after the store as
    # gdb's AVR target expects (QEMU's stub has none to compare with)
    printf 'set pagination off\nset confirm off\nbreak compute\ncontinue\ndelete\nwatch counter\ncontinue\ncontinue\ndelete\nbreak gdb-fw.c:%s\ncontinue\nprint acc\nkill\n' \
        "$ret" > "$out/avrwatch.gdb"
    next_port; ep=$port
    sim_at $ep "$out/avr.elf" --board uno
    gdb_run $ep "$out/avr.elf" "$out/avrwatch.gdb" "$out/avrwatch.out"
    t=$out/avrwatch.out
    want "$t" '^New value = 3' "avr: the write watchpoint's first stop"
    want "$t" '^New value = 11' "avr: the write watchpoint's second stop"
    want "$t" '= 71$' "avr: main's result after the watchpoints (acc 71)"
    echo "embsim-gdb: avr: a write watchpoint stops twice, after each store"
else
    echo "embsim-gdb: no qemu-system-avr or no AVR gdb: the AVR checks did not run"
fi
