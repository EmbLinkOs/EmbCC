# EmbCC as a programmer's tool

What a compiler owes the person writing the code, not just the machine
running it: every reason the code does not build (not only the first), in a
form an editor can act on; a fix offered where the fix is knowable; and the
same understanding served live — completion, types under the cursor, where
a name comes from. GCC and Clang set the bar; this is the plan to reach it
and then go past it, on EmbCC's own terms (one tree, no LLVM, C99 source).

The order is forced by dependency: nothing downstream works until a
diagnostic is a *value* rather than a line printed on the way out, and
until the front ends can keep going after an error.

| | what | proven by |
|---|---|---|
| **T1** | **The diagnostic engine.** A diagnostic is a record: severity, location, source range, notes, fix-its, the option that controls it. Buffered, then rendered — text exactly as before, or `-fdiagnostics-format=json` (GCC's schema, so existing tools read it). `-fdiagnostics-color`, `-fmax-errors`, `-w`, `-Werror`. First fix-its: the name suggestions the front ends already compute. | done — tests/golden/diagnostics-json.sh; the text goldens unchanged |
| **T2** | **Error recovery.** The C front end keeps going after an error — synchronising at statement and declaration boundaries — so one run reports every independent problem instead of the first. Then the C++ front end. A recovery must never produce a *wrong* later diagnostic: each is either suppressed or real. | a file with N independent errors reports N |
| **T3** | **Fix-its that apply.** `-fdiagnostics-parseable-fixits` (GCC's line format) and `embcc --fix`, which rewrites the file. Fix-its for the mechanical cases: a missing `;`, a missing `&`/`*`, `.` for `->`, an unspelled `struct` tag, a misspelt name, a missing `#include` for a known declaration. | before/after files in a golden, and a fixed file compiles |
| **T4** | **The driver GCC and Clang users already know.** A real option table (`--help` generated from it), `-M`/`-MM`/`-MD`/`-MMD`/`-MF`/`-MT`/`-MP`, `-fsyntax-only`, `-S`, `@file`, `--version`, `-dumpmachine`, `-x`, `-###`. And warnings that mean something: `-Wall`/`-Wextra` as groups over real analyses (unused, shadowed, uninitialised, sign-compare, fallthrough, format), each with its `-Wno-` and its name printed in the diagnostic. | gcc's own option spellings on EmbCC, and each warning's golden |
| **T5** | **`embls`, the language server.** LSP over stdio on a tolerant parse: diagnostics as you type, completion (members after `.`/`->`, locals, globals, keywords, `#include` paths), hover (type, declaration, comment), go-to-definition, find references, signature help, document symbols, rename. | an LSP conversation transcript test, and it drives a real editor |
| **T6** | **Past the bar.** `embcc --explain <id>`: what the error means, why it fired *here*, and the smallest edit that fixes it. Suggestions that use the index rather than edit distance alone (the member you meant, on the type you have; the header that declares the name). `embcc doctor`: why a link failed, in terms of symbols and the units that needed them. | worked examples, each a test |

## T1 — the engine (done)

`src/driver/diag.c`. A diagnostic is built, not printed:

```c
struct diag {
    int level;                 /* error, warning, note */
    struct diag_loc loc;       /* file, line, col, end_col (0: infer) */
    char *msg;
    const char *option;        /* the -W that controls it, or NULL */
    struct diag_fixit *fixits; /* replace [col, end_col) with a string */
    struct diag *children;     /* notes attached to it */
};
```

Every existing `diag_*` call builds one of these; a note attaches to the
diagnostic before it. They are held until the process ends (an `atexit`
flush, so the ~300 places that `exit(1)` after an error need no change) and
rendered once, as text or as JSON.

Text is what it always was — heading, source line, caret, `~` under the rest
of the token — plus, when a fix-it is attached, the replacement under the
caret, as GCC prints it. JSON is GCC's `-fdiagnostics-format=json` schema: a
top-level array, each diagnostic with `kind`, `message`, `option`,
`column-origin`, `locations` (`caret`, `start`, `finish`), `fixits`
(`start`, `next`, `string`) and `children`. An editor that reads GCC reads
EmbCC.

What the flags do: `-fdiagnostics-format=text|json`,
`-fdiagnostics-color=auto|always|never` (`-fno-diagnostics-color`),
`-fmax-errors=N` (stop after N; 0 = no limit), `-w` (drop warnings),
`-Werror` (warnings become errors, and the exit status follows).

The first fix-its come from the suggestions the front ends already make:
`'fooo' is not declared` → note `did you mean 'foo'?` with a fix-it
replacing the identifier. In JSON that is a `fixits` entry an editor can
apply without parsing English.
