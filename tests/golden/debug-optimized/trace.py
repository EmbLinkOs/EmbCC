# gdb -batch -x trace.py, with TRACE_PORT, TRACE_SRC (a basename), TRACE_LINES
# ("12,13,20-31"), TRACE_OUT and TRACE_MAX in the environment: connect to the
# stub, break on each of those lines of the source, and at every stop record
# the function, the line, which visit of that line it is, and every parameter
# and local in scope -- its value as gdb prints it, or None when gdb says it is
# optimized out. Written as JSON lines to TRACE_OUT.
import gdb, json, os

port = os.environ["TRACE_PORT"]
src = os.environ["TRACE_SRC"]
mx = int(os.environ.get("TRACE_MAX", "4000"))
lines = []
for part in os.environ["TRACE_LINES"].split(","):
    if "-" in part:
        a, b = part.split("-")
        lines += range(int(a), int(b) + 1)
    elif part:
        lines.append(int(part))

gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set print pretty off")
gdb.execute("set print address on")
gdb.execute("target remote :" + port)
for l in lines:
    try:
        gdb.execute("break %s:%d" % (src, l), to_string=True)
    except gdb.error:
        pass
# where a harness goes once main returns: the trace ends there (a board that
# then waits forever would never stop again)
for f in ("_exit", "exit", "abort"):
    try:
        gdb.execute("break " + f, to_string=True)
    except gdb.error:
        pass

out = open(os.environ["TRACE_OUT"], "w")
seen = {}
for n in range(mx):
    try:
        gdb.execute("continue", to_string=True)
    except gdb.error:
        break
    try:
        f = gdb.selected_frame()
    except gdb.error:
        break
    sal = f.find_sal()
    if not sal.symtab or os.path.basename(sal.symtab.filename) != src:
        break
    fn = f.name()
    key = "%s:%d" % (fn, sal.line)
    k = seen.get(key, 0)
    seen[key] = k + 1
    vals = {}
    decl = {}
    try:
        blk = f.block()
    except RuntimeError:
        blk = None
    while blk is not None:
        for sym in blk:
            if not (sym.is_variable or sym.is_argument) or sym.name in vals:
                continue
            decl[sym.name] = 0 if sym.is_argument else sym.line
            try:
                v = sym.value(f)
                if v.is_optimized_out:
                    vals[sym.name] = None
                else:
                    ptr = v.type.strip_typedefs().code == gdb.TYPE_CODE_PTR
                    s = str(v)
                    if ptr:   # stack addresses differ between builds: null or not
                        s = "ptr:" + ("null" if int(v) == 0 else "set")
                    vals[sym.name] = s
            except Exception as e:
                vals[sym.name] = "ERR " + str(e)[:60]
        if blk.function is not None:
            break
        blk = blk.superblock
    out.write(json.dumps({"key": key, "line": sal.line, "k": k, "pc": int(f.pc()),
                         "vals": vals, "decl": decl}) + "\n")
out.close()
