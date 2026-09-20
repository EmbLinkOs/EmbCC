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
| **T2** | **Error recovery.** The C front end keeps going after an error — synchronising at statement and declaration boundaries — so one run reports every independent problem instead of the first. A recovery must never produce a *wrong* later diagnostic: each is either suppressed or real. | done (C) — tests/golden/diagnostics-recovery.sh; the C++ front end is still first-error |
| **T3** | **Fix-its that apply.** `-fdiagnostics-parseable-fixits` (GCC's line format) and `embcc --fix`, which rewrites the file. Producers so far: a misspelt name, a missing `;`. Still to come: `.` for `->`, an unspelled `struct` tag, a missing `#include` for a known declaration. | done — tests/golden/diagnostics-fix.sh: the fixed file compiles |
| **T4** | **The driver GCC and Clang users already know.** Dependency generation, `-fsyntax-only`, `--help`, `-dumpmachine`, and warning groups over real analyses (unused variable/parameter/function, shadow, sign-compare), each with its `-Wno-` and its name in the diagnostic. Still to come: `-S`, `@file`, `-###`, and more analyses (uninitialised, fallthrough, format). | done for those — tests/golden/driver-deps.sh and warnings.sh (gcc agrees on which code warns) |
| **T5** | **`embls`, the language server.** LSP over stdio: diagnostics as you type, completion (members after `.`/`->`, locals, globals, keywords), hover, go-to-definition, document symbols. Still to come: find references, signature help, rename, `#include` completion, cross-file indexing. | done (first five) — tests/golden/embls.sh drives a whole session |
| **T6** | **Past the bar.** `embcc --explain <id>` — done: a stable id per diagnostic, printed with it, and an entry with the rule, a worked example, the fix and the citation. Still to come: suggestions that use the index rather than edit distance alone (the member you meant, on the type you have; the header that declares the name), and `embcc doctor` for why a link failed. | tests/golden/diagnostics-explain.sh, incl. "every id printed has an entry" |

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

## T2 — recovery (done, for C)

A syntax error is recorded and the parser resumes at the nearest recovery
point: the next statement inside a block, or the next external declaration.
Resynchronising means skipping to the `;` that ends the broken construct or
the `}` that ends its block, and it always consumes at least one token, so
recovery cannot spin on the token it failed at. Semantic analysis does the
same per statement, which matters more in practice — most of the reasons a
file will not build are semantic (a name not declared, a member that does
not exist, an argument that does not fit).

Two rules keep the extra diagnostics honest:

- **Nothing downstream runs on a broken tree.** If the parse reported
  errors, semantic analysis never starts; if semantic analysis reported
  any, code generation never starts. The compile ends with `compilation
  terminated: N errors`. A later pass on a half-built tree would invent
  diagnostics that are not about the program.
- **A repeated cause is reported once.** A name misspelt once is usually
  used several times; the first use reports it, the rest are counted and
  quiet.

What a failed statement already added to the scope is left there
deliberately: dropping a half-declared variable would turn one error into a
crowd of "not declared" ones below it.

`-fmax-errors=N` stops after N, as GCC does.

## T4 — the driver, first half (done)

`-M`, `-MM`, `-MD`, `-MMD`, `-MF`, `-MT`, `-MQ`, `-MP`: the make rule
naming what the file included, which is how a C project knows to rebuild
when a header changes. The preprocessor records every header it opened, in
order, each marked with whether it came from a system directory — which is
what `-MM` leaves out. System-ness is inherited: `stdio.h` finds `_ansi.h`
beside itself, through no `-isystem` directory at all, and it is still a
system header.

The rule is checked three ways (tests/golden/driver-deps.sh): it names
every header gcc's own `-M` names, `make` itself accepts the file, and
`-MM` drops the system ones. EmbCC's list can be *longer* than gcc's, and
here it is: EmbCC defines no `__GNUC__`, so newlib's `__GNUC_PREREQ` tests
take their portable branch and really do include `<limits.h>`. The rule
must describe the compile that happened.

`-fsyntax-only` runs the front end and writes nothing — what an editor
asks for, and what a build's "does this still compile" step wants.
`--help` lists the options in the spellings GCC and Clang use;
`-dumpmachine` prints the target `--target` chose.

## T5 — the language server (first five answers)

`tools/embls/embls.c`, built by `make embls`. It speaks LSP over stdio and
answers from the compiler itself rather than from a second parser that
would drift from it:

- **diagnostics** by running `embcc -fsyntax-only
  -fdiagnostics-format=json` over the buffer, so what the editor shows is
  what the build will say — notes included, under the message;
- **completion, hover, definition, symbols** from an index built by
  EmbCC's own preprocessor and parser over the buffer: the members offered
  after `.` are the members of the struct the compiler sees, with their
  types; the names offered in a body are that function's parameters and
  locals (another function's are not), plus globals, typedefs,
  enumerators and keywords.

The parse runs in a **forked child**, which is what makes it safe to use a
compiler front end this way: a front end ends the process where it cannot
continue, and a server must not end. The child writes what it learned down
a pipe; if it dies early the parent has a smaller index and the next
keystroke tries again.

A header the editor was not told how to find is not fatal either
(`cpp_set_tolerant`, which only a tool sets — for the compiler a missing
header is an error): the rest of the file is still worth understanding,
and completion keeps working while the diagnostics honestly say the header
is missing.

Flags come from `compile_flags.txt` beside the file or above it (clangd's
format, so a project set up for clangd works here), plus `EMBLS_FLAGS`.

Editor setup: point the client at the `embls` binary, no arguments. For
example, in Neovim:

```lua
vim.lsp.start({ name = "embls", cmd = { "/path/to/EmbCC/embls" },
                root_dir = vim.fn.getcwd() })
```

## T3 — fix-its that apply (done)

`-fdiagnostics-parseable-fixits` prints GCC's line form,
`fix-it:"FILE":{LINE:COL-LINE:NEXT}:"TEXT"`, and `embcc --fix` performs
the edits itself — in the files the fix-its name, from the end backwards so
earlier positions stay valid, at most one edit a line (two edits to one
line could overlap, and a wrong edit is worse than none). A file with no
fix-it is left exactly as it was, and `--fix` says so rather than
pretending.

The producers today are the two whose fix is never in doubt: the name the
front end already suggested, and a missing `;`. The second changes how the
parser recovers — rather than resynchronising, it carries on as if the
semicolon were there, so a file missing two of them reports both and
`--fix` inserts both in one pass. The position is remembered so the same
insertion cannot repeat and spin.

Every parser diagnostic is now recoverable: the 37 that still ended the
compile (a declaration's, which have a line but no column) join the 62
that already recovered.

## T6 — `--explain` (done)

A C compiler tells you what is wrong where it noticed. That is not the same
as telling you what the rule is. `src/driver/explain.c` holds an entry per
diagnostic worth explaining — the rule, the mistake and the fix side by
side, and the paragraph of C99 it comes from — and the diagnostic prints
its id, so the next step is in front of the reader:

    error: struct Point has no member 'z' [E0004]
    $ embcc --explain E0004

Eight entries to begin with, on the diagnostics people actually hit: an
undeclared name, a missing `;`, a conversion that does not exist, a member
that does not exist, an object of a type with no size, the wrong number of
arguments, assigning to what cannot be assigned, and a path that does not
return. The ids reach editors too — JSON diagnostics carry `"id"`.

The invariant the golden enforces: every id the compiler can print has an
entry, so an id is never a dead end.

## T4 — warnings that mean something (first five)

`-Wall` and `-Wextra` are groups over real analyses, not a verbosity dial.
Each warning has a name, is turned off by `-Wno-NAME`, and prints that name
so the reader knows what controls it. `--help-warnings` lists them with
their groups.

| warning | group | what it finds |
|---|---|---|
| `-Wunused-variable` | `-Wall` | a local nothing reads or writes |
| `-Wunused-function` | `-Wall` | a `static` function nobody calls (no other unit can) |
| `-Wunused-parameter` | `-Wextra` | a parameter the body never touches |
| `-Wshadow` | — | a declaration that hides one still in scope, with a note at the one it hides |
| `-Wsign-compare` | `-Wextra` | a comparison the usual conversions turn unsigned, where the signed side can be negative |

The golden's last check is the one that matters: on the same file, with the
same flags, EmbCC warns where gcc warns — a warning nobody else raises is a
false positive, and one only gcc raises is a gap.

Two rules keep them quiet where they should be:

- **A system header's warnings are not the project's to fix.** The
  preprocessor already knew which headers came from a system directory (the
  `-MM` work); the engine now drops warnings from them, as GCC does, unless
  `-Wsystem-headers` asks.
- **C++ does not warn about the C it lowers to.** These analyses run in the
  C front end, over lowered code where a template instantiated from a header
  is attributed to the `.cc` that instantiated it. A warning pointing at the
  wrong line is worse than none, so they are C-only until the C++ front end
  grows its own.

Found on the way: every semantic diagnostic named the *unit's* file, so an
error inside a header was reported against the `#include` line. Semantic
analysis now reports against the file of the function it is checking, which
is what a header's own errors always deserved.
