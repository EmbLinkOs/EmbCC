# Sourced by the inline-asm goldens: an inline asm template's directives
# -- .ascii/.asciz/.string, the alignments, the data directives and %c --
# refereed byte for byte by llvm-mc.
#
#   asmdir_referee OUT TARGET "MC-ARGS" FILLER STEP LINES [SECTION]
#
# LINES is a file of template lines (one statement each). They become a C
# function's asm, `%c0` and `%c1` standing for the constants -5 and 24, its
# "i" operands; llvm-mc gets the same lines with the numbers written in.
# The comparison runs once per PHASE: an asm of 0, 1, 2... FILLERs (an
# instruction or a datum of STEP bytes) comes first, then the template,
# which opens with a 4-byte marker -- so across the runs the template
# starts at every STEP multiple modulo 16, and an alignment padded
# relative to the template, not to the section, shows up in all but one
# of them. llvm-mc's input puts the marker at the same offset (`.zero K`),
# and from the marker on the two must be the same bytes. SECTION is the text section's name for
# llvm-objcopy --dump-section (.text, or __TEXT,__text for Mach-O), which
# reads ELF, Mach-O and COFF alike. Returns non-zero
# after saying what differed.
#
# ASMDIR_CFLAGS adds to EmbCC's command line (-march=rv32ima); ASMDIR_MC_SED
# is a sed script for llvm-mc's copy of the lines, where llvm-mc reads a
# directive otherwise than GNU as (AVR's .align, a byte count to llvm-mc).
#
# Needs MC and OBJCOPY (llvm-mc, llvm-objcopy) and EMBCC.
asmdir_referee() {
    _o=$1; _t=$2; _mc=$3; _fill=$4; _step=$5; _lines=$6; _sec=${7:-.text}
    _n=$((16 / _step))
    [ "$_n" -ge 1 ] || _n=1
    _r=0
    while [ "$_r" -lt "$_n" ]; do
        _mk=$(printf 'Q%02dZ' "$_r")
        # the fillers are an asm of their own, before the template: what
        # moves is where the TEMPLATE starts in the section
        : > "$_o/asmdir-$_r.fill"
        _k=0
        while [ "$_k" -lt "$_r" ]; do
            printf '%s\n' "$_fill" >> "$_o/asmdir-$_r.fill"; _k=$((_k + 1))
        done
        { printf '.ascii "%s"\n' "$_mk"; cat "$_lines"; } > "$_o/asmdir-$_r.s"
        { printf 'void f(void)\n{\n'
          if [ "$_r" -gt 0 ]; then
              printf '    __asm__ volatile(\n'
              sed 's/\\/\\\\/g; s/"/\\"/g; s/^/        "/; s/$/\\n"/' "$_o/asmdir-$_r.fill"
              printf '    );\n'
          fi
          printf '    __asm__ volatile(\n'
          sed 's/\\/\\\\/g; s/"/\\"/g; s/^/        "/; s/$/\\n"/' "$_o/asmdir-$_r.s"
          printf '        : : "i"(-5), "i"(24));\n}\n'; } > "$_o/asmdir-$_r.c"
        # shellcheck disable=SC2086
        "$EMBCC" --target="$_t" ${ASMDIR_CFLAGS:-} -O2 -c "$_o/asmdir-$_r.c" -o "$_o/asmdir-$_r.o" \
            2> "$_o/asmdir-$_r.err" || {
            echo "$_t: a template of directives does not compile:"
            head -3 "$_o/asmdir-$_r.err"; return 1; }
        "$OBJCOPY" --dump-section="$_sec=$_o/asmdir-$_r.bin" \
            "$_o/asmdir-$_r.o" "$_o/asmdir-$_r.tmp" || return 1
        # where the marker landed: the template's offset in the section
        _at=$(od -An -v -tx1 "$_o/asmdir-$_r.bin" | tr -d ' \n' |
              awk -v m="$(printf '%s' "$_mk" | od -An -tx1 | tr -d ' \n')" '{
                  for (i = 1; i + length(m) - 1 <= length($0); i += 2)
                      if (substr($0, i, length(m)) == m) { print (i - 1) / 2; exit }
              }')
        [ -n "$_at" ] || { echo "$_t: the template's marker is not in the object"; return 1; }
        { [ "$_at" -gt 0 ] && printf '.zero %s\n' "$_at"
          sed 's/%c0/-5/g; s/%c1/24/g' "$_o/asmdir-$_r.s" |
              sed "${ASMDIR_MC_SED:-p;d}"; } > "$_o/asmdir-$_r.mc.s"
        # shellcheck disable=SC2086
        "$MC" $_mc -filetype=obj "$_o/asmdir-$_r.mc.s" -o "$_o/asmdir-$_r.mc.o" \
            2> "$_o/asmdir-$_r.mc.err" || {
            echo "$_t: llvm-mc rejected the lines:"; head -3 "$_o/asmdir-$_r.mc.err"
            return 1; }
        "$OBJCOPY" --dump-section="$_sec=$_o/asmdir-$_r.ref" \
            "$_o/asmdir-$_r.mc.o" "$_o/asmdir-$_r.tmp" || return 1
        _len=$(($(wc -c < "$_o/asmdir-$_r.ref") - _at))
        _ours=$(dd if="$_o/asmdir-$_r.bin" bs=1 skip="$_at" count="$_len" 2>/dev/null |
                od -An -v -tx1 | tr -d ' \n')
        _theirs=$(dd if="$_o/asmdir-$_r.ref" bs=1 skip="$_at" count="$_len" 2>/dev/null |
                  od -An -v -tx1 | tr -d ' \n')
        if [ "$_ours" != "$_theirs" ]; then
            echo "$_t: the directives at section offset $_at are not llvm-mc's bytes:"
            echo "     | ours   $_ours"
            echo "     | theirs $_theirs"
            return 1
        fi
        _r=$((_r + 1))
    done
    echo "$_t: $(wc -l < "$_lines" | tr -d ' ') directive lines at $_n phases are llvm-mc's bytes"
    return 0
}
