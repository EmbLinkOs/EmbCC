#!/bin/sh
# Sources saved on Windows: every line ends CR LF, as MPLAB X, Keil and
# STM32CubeIDE projects often have them, and a few old files end lines
# with a lone CR. Each is one newline, as GCC reads it. A CR left in the
# line went into a #define's body, so FreeRTOSConfig.h's
#     #define configUSE_16_BIT_TICKS 0<CR>
# made FreeRTOS's `#if ( configUSE_16_BIT_TICKS == 1 )` an error, and a
# backslash before CR LF did not join the next line.
set -u
echo "TEST-MARKER crlf-sources"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/crlf-sources
rm -rf "${out:?}"; mkdir -p "$out/inc"
fail() { echo "FAIL: $*"; exit 1; }

# a header's macro in #if, a splice, a function-like macro, #pragma once
printf '#pragma once\r\n#define TICKS16 0\r\n#define SUM(a, b) \\\r\n    ((a) + (b))\r\nstatic const int once = 1;\r\n' > "$out/inc/cfg.h"
printf '#include "inc/cfg.h"\r\n#include "./inc/cfg.h"\r\n#if ( TICKS16 == 1 )\r\n#error wrong\r\n#elif TICKS16 == 0\r\nint ok = SUM(20, 22);\r\n#endif\r\n_Static_assert(SUM(20, 22) == 42, "the splice joined");\r\nint main(void) { return ok + once - 1; }\r\n' > "$out/a.c"
"$EMBCC" -c "$out/a.c" -o "$out/a.o" 2> "$out/a.err" ||
    { cat "$out/a.err"; fail "a CR LF source does not compile"; }
"$EMBCC" -E "$out/a.c" > "$out/a.i" || fail "-E of a CR LF source"
if od -c "$out/a.i" | grep -q '\\r'; then fail "-E passed a CR through"; fi

# a lone CR, and line numbers counted by line ends of both kinds
printf '#define M 5\rint z = M;\r\nint w = ;\n' > "$out/b.c"
"$EMBCC" -fsyntax-only "$out/b.c" 2> "$out/b.err" && fail "b.c's error was missed"
grep -q 'b.c:3:9: error: expected an expression' "$out/b.err" ||
    { cat "$out/b.err"; fail "the error is not at line 3 column 9"; }
echo "crlf-sources: CR LF and a lone CR are each one newline: in #define, #if, a splice, #pragma once and -E, with lines counted"
