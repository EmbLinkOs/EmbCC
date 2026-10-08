# avrshape.awk -- over `llvm-objdump -d --no-show-raw-insn --triple=avr`:
# for each function, print "NAME ok" when it touches memory through Z only
# inside `in r0, 0x3f; cli` ... `out 0x3f, r0` windows (at least one),
# calls nothing and has no sei; else "NAME BAD: why".
function done_fn() {
    if (fn == "") return
    if (why == "" && wins == 0) why = "no interrupts-off access"
    if (why == "" && open) why = "a window is never closed"
    print fn (why == "" ? " ok" : " BAD: " why)
}
/^[0-9a-f]+ <[^>]+>:$/ {
    done_fn(); fn = $2; gsub(/[<>:]/, "", fn)
    why = ""; open = 0; prev = ""; wins = 0; inwin_z = 0; next
}
fn != "" && /^ +[0-9a-f]+:/ {
    m = $2; ops = $0; sub(/^ +[0-9a-f]+:[ \t]+[a-z]+[ \t]*/, "", ops)
    if (m == "cli") {
        if (prev != "in r0, 0x3f") why = why "cli without SREG saved in r0; "
        open = 1; inwin_z = 0
    } else if (m == "out" && ops ~ /^0x3f, r0$/) {
        if (!open) why = why "SREG restored outside a window; "
        if (inwin_z) wins++
        open = 0
    } else if (m == "sei") {
        why = why "sei; "
    } else if (m ~ /^r?call$/) {
        why = why "a call; "
    } else if (m ~ /^(ld|ldd|st|std)$/ && ops ~ /Z/) {
        if (!open) why = why "Z access with interrupts on (" $0 "); "
        else inwin_z = 1
    }
    prev = m " " ops
}
END { done_fn() }
