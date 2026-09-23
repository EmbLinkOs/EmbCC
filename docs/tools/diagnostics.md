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
| **T2** | **Error recovery.** Both front ends keep going after an error — synchronising at statement and declaration boundaries — so one run reports every independent problem instead of the first. A recovery must never produce a *wrong* later diagnostic: each is either suppressed or real. | done (C and C++) — tests/golden/diagnostics-recovery.sh |
| **T3** | **Fix-its that apply.** `-fdiagnostics-parseable-fixits` (GCC's line format) and `embcc --fix`, which rewrites the file. Producers: a misspelt name, a missing `;`, `.` for `->` (and back), the member the type actually has, and the header that declares a C library name. | done — tests/golden/diagnostics-fix.sh: the fixed file compiles |
| **T4** | **The driver GCC and Clang users already know.** Dependency generation, `-fsyntax-only`, `--help`, `-dumpmachine`, and warning groups over real analyses (unused variable/parameter/function, shadow, sign-compare, uninitialised, format), each with its `-Wno-` and its name in the diagnostic. Still to come: `-S`, `@file`, `-###`, and fallthrough. | done for those — tests/golden/driver-deps.sh, warnings.sh and format-check.sh (gcc agrees on which code warns) |
| **T5** | **`embls`, the language server.** LSP over stdio, C and C++: diagnostics as you type, completion (members after `.`/`->`, locals, globals, keywords), hover, go-to-definition, document symbols. Still to come: find references, signature help, rename, `#include` completion, cross-file indexing. | done (first five, both languages) — tests/golden/embls.sh drives a whole session |
| **T6** | **Past the bar.** `embcc --explain <id>` — a stable id per diagnostic, printed with it, and an entry with the rule, a worked example, the fix and the citation. Suggestions come from the index rather than edit distance alone (the member you meant, on the type you have; the header that declares the name). `embld --doctor` says why a link failed, for every undefined symbol at once. | tests/golden/diagnostics-explain.sh, incl. "every id printed has an entry"; tests/golden/embld-doctor.sh |

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

The C++ front end recovers the same way, at declarations and at statements,
with one difference that matters: it restores the parser's own state
(`parse_save`/`parse_restore` — scope, current function, template context)
before resynchronising, since a longjmp out of a class body would otherwise
leave the parser inside it. Once parsing is over and instantiation has
begun there is no statement boundary to resume at, so an error there ends
the compile, deliberately.

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

C++ is indexed by the C++ front end rather than the C one: the child
tokenizes and parses with `cx_parse_unit` and walks its tables, so the
members offered after a `.` are the class's own — its data and the
functions you can call, without the ones the compiler generated for you —
and hover shows the signature as written, not the mangled name. Which front
end runs is decided by the file's suffix, as the driver decides it.

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

The producers are the ones whose fix is not in doubt: the name the front
end already suggested, a missing `;`, `.` where the value is a pointer (and
`->` where it is not), the member the type actually has — suggested from
that type's own members, not from a dictionary — and the header that
declares a C library name (`'malloc' is declared in <stdlib.h>`, with the
`#include` as the edit). The last one is knowledge rather than a guess:
the C library's surface is fixed by the standard. The second changes how the
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
| `-Wuninitialized` | `-Wall` | a local read on a path that never wrote it |
| `-Wmaybe-uninitialized` | `-Wall` | a local read where only some paths wrote it |
| `-Wformat` | `-Wall` | a `printf` or `scanf` format that disagrees with the arguments beside it |

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

## T6 — `embld --doctor`, why the link failed

A linker prints `undefined reference to 'foo'` and stops at the first one.
That names the symptom. The cause is already in the inputs, and nobody
reads them for you: `embld --doctor *.o *.a` does, and answers every
undefined symbol in one run.

It scans each object's symbol table, and every member of each archive —
the question is what *could* define the name, not what the link happened to
pull in — then, per undefined symbol, says which of these it is:

| what the inputs show | what it says |
| --- | --- |
| another unit defines it, but `static` | names that unit: the definition exists and is private, so drop the `static` or move the caller |
| it is a C library name | names the header that declares it (`strlen` → `<string.h>`) and the library a freestanding link does not add |
| `__cxa_*`, `_Unwind_*`, `__cxxabiv1`, `_ZSt*` | the C++ runtime: link libsupc++ and libstdc++ |
| `_ZTV…` / `_ZTI…` | the **key function** rule — a vtable is emitted with the first non-inline virtual function, so a key function that is only declared leaves no unit emitting it |
| a mangled member function | a member declared in the class and never defined looks exactly like this |
| nothing at all | is a source file missing from the link, or a library? |

The C++ cases need the name read back, so the doctor carries enough Itanium
demangling to turn `_ZTV5Shape` into `vtable for Shape` and
`_ZNK5Shape4areaEv` into `Shape::area() const`, CV-qualifiers included.
Anything more elaborate — templates, substitutions — is left mangled rather
than guessed at, since a wrong name is worse than a raw one.

The ordering of those cases is itself a judgement. libsupc++'s own
`_ZTVN10__cxxabiv117__class_type_infoE` *is* a `_ZTV` symbol, and the
key-function rule is true of it and useless: it sends the reader looking
for a virtual function they never wrote. The runtime check runs first.

## T4 — `-Wuninitialized`, the first analysis with a lattice

Every warning before this one is a check on a single construct: this
declaration, that comparison. `int x; if (c) x = 1; return x;` is not that
kind of question. It compiles, links, runs, and returns whatever the frame
held, and answering it means knowing what every path reaching the read has
done.

So `src/sema/uninit.c` is a real dataflow analysis, run over the statement
tree once semantic analysis has bound every name to a frame slot. Each slot
holds one of three values — written on no path, every path, some path — and
the walk joins them wherever control rejoins: after an `if`, around a loop's
back edge, at each `case` label (which is another way in, carrying the state
at the switch head, so a switch with no `default` leaves a slot unwritten).
A read of a slot written on no path is `-Wuninitialized`; on some,
`-Wmaybe-uninitialized`. One warning per variable, because the second read
is the same bug seen again.

**The bar is false positives, not coverage.** A missed bug is still a bug
the programmer can find; an invented one is a bug they cannot. The analysis
was built against EmbCC's own 50 source files — code gcc compiles `-Werror`
clean, where every warning is by definition a false positive. The first run
produced 36. What each one turned out to be is the interesting part:

- **20 were `va_list ap; va_start(ap, fmt);`**. Spelled as a call,
  `va_start` looks like a read of an uninitialized `va_list`; it is a
  *write* — irgen takes the address. Now treated as one.
- **13 were slot 0, seen through a function's name.** `fprintf` is an
  `EXPR_VAR` whose `fref` is set and whose `gref` is not, so `var_index`
  kept its default of 0 — a real slot, belonging to some other variable.
  A bound name now has to match the slot's name.
- **1 was `a && (sh = f()) >= 0 && g(sh)`.** Taken pairwise, the last
  operand is analysed from a state where the middle one might not have run.
  But it runs only if the middle one did: the chain of one operator is now
  walked whole, each operand seeing the state in which all before it ran.
- **1 was `... else no();`**, where `no()` leaves through `longjmp`. No
  analysis can see that, so `no()` now says `__attribute__((noreturn))`,
  and the walk honours it (reusing sema's own `is_noreturn_call`).
- **1 was a guarded loop-carried value**: `for (…) { if (i) use(w); w = i; }`
  — written late, read early on a later turn, under a test that is false on
  the first. The test is the guard, and the analysis cannot relate it to the
  iteration count; gcc is silent here too. A MAYBE that the back edge alone
  produced, read under a test inside that loop, is not reported. Unguarded,
  the same shape is a first-iteration read and still is.

It now warns nowhere in EmbCC's own source, and the golden asserts that.

**Against the referee.** On the four-case file in
tests/golden/warnings-uninit.sh, gcc `-O2` agrees on three: the definite
read, the loop that may not run, and the switch with no `default`. It misses
`if (c) x = 1; return x;` at every `-O` level — clang reports that one as
`-Wsometimes-uninitialized`, so it is a true positive gcc happens not to
find. gcc catches one EmbCC does not: a read through a pointer to a local
whose address was taken. That is deliberate. `&x` hands the slot to code the
walk is not looking at, and gcc only sees it by working after inlining.
Both differences are asserted, so either one changing shows up as a failure.

**Two bugs fell out of writing it**, both older than the analysis and both
affecting diagnostics that already shipped:

- `-fsyntax-only` did not imply `-c`, so it ran to the link step and failed
  with "cannot link" instead of checking the file. It implies it now, as in
  GCC.
- A function prototyped in a header and defined in a `.c` reported *every*
  diagnostic about its body against the **header**, with the `.c` file's
  line numbers. `merge_decls` moved the definition's body into the canonical
  node but left the prototype's file and line on it. The canonical node now
  takes the definition's location, because after the merge it *is* the
  definition. This was wrong for unused-variable, unused-parameter, shadow,
  sign-compare and missing-return too.

And one gap: a trailing `__attribute__((noreturn))` — GCC's usual spelling
— was parsed at `fn_tail` and then dropped; only `weak` was copied onto the
function node. Both `noreturn` and `nothrow` now survive it.

## T4 — `-Wformat`, and where the knowledge lives

`printf("%d\n", some_long)` compiles with nothing to say, and then reads
four bytes where eight were passed. A variadic call has no prototype for
its tail, so nothing in the language relates the format string to the
arguments — but both are right there at the call, and the types are
known. Only nobody was comparing them.

**Which functions to check is not a list inside the compiler.** A
function says so itself, with
`__attribute__((format(printf, n, m)))` — GCC's spelling for thirty
years, already on every declaration in a real `<stdio.h>`, and now on
ours. A compiler that knew the name `printf` would hold that knowledge
in two places and still know nothing about anyone's own `log()`.

**What is compared is the PROMOTED type**, because that is what actually
lands in the variadic tail. A `float` arrives as a `double` and a
`short` as an `int`, so `printf("%f", 1.0f)` and `printf("%d", (short)x)`
are both correct and neither is reported. sema has already applied the
default argument promotions by the time the check runs, so it reads the
types as they will be passed rather than as they were written. scanf is
the mirror: nothing is promoted through a pointer, so plain `%f` writes
a `float` and `%lf` a `double`, and `sscanf(s, "%f", &a_double)` is
caught.

**A signedness mismatch is deliberately not reported.** `printf("%d", 3u)`
and `printf("%x", 3)` have the same size and the same representation for
every value that reaches them. GCC reports them; in a codebase of any
size the result is noise, and noise inside `-Wall` trains the reader to
skip the category — which costs the warnings that do matter. What is
reported is a difference the machine can see: a wrong size, a wrong
class, a pointer where a number goes, `%s` with something that is not a
pointer to characters, or a count that does not match.

**C++ is checked too**, through the lowering rather than beside it: the
attribute is carried into the emitted C, so one implementation serves
both front ends. The indices move with it, and exactly one thing moves
them. `this` does not — GCC defines a non-static member's arguments as
counting from two *because* of it, so the source already accounts for
the parameter lowering makes explicit. A hidden return slot does, since
that one is this compiler's own and comes before `this`.
`tests/golden/format-check.sh` has a member function of each kind.

**The judge is gcc**, on the same file with the same attributes, and the
invariant runs one way: every line EmbCC reports, gcc must report too.
The golden holds sixteen wrong calls that must all be caught and twenty
correct ones that must all be silent — and the second list is the one
that found the bugs. Checking a scanf field's pointee against a size
reported `sscanf(s, "%s", buf)`, because `%s` takes a character *buffer*
rather than a pointer to one object; a checker that says that is
unusable, and the fix is that `%s`, `%c` and `%[` ask nothing about what
they point at.

## T5 — references, rename, signature help, `#include`

grep finds a *name*. An editor needs the *thing*. The difference shows up
the moment two things share a spelling, which in C they constantly do: a
member `p.x` and a local `x`, a parameter `total` in one function and the
file-scope `total` in another, the word `total` in a comment and inside a
string literal.

So the index now records, besides every declaration, every **place a name
is used** — emitted from the parse tree, not from the text. An identifier
inside a comment or a string is not a node, so it cannot be found; a member
name after `.` is a different node from a variable of the same spelling, so
the two never collide. On this file:

```c
struct Point { int x; int y; };
static int total;
int scale(struct Point p, int n) {
    int x = p.x * n;
    total = total + x;
    return x;
}
int other(int total) { return total + 1; }
int main(void) {
    /* total in a comment, and "total" in a string */
    return scale(q, 3) + total;
}
```

references on the file-scope `total` gives its declaration and the three
real uses, and leaves out `other`'s parameter, the comment and the string.
references on that parameter gives two locations, in `other` alone.
references on the local `x` gives three, and `p.x` is not among them.

**Rename is that set with a new spelling**, which is the only reason it is
safe to offer: it is exactly as correct as the reference list, and it
includes the declaration, because a rename that leaves the declaration
behind does not compile.

**Signature help** reads the signature out of the index — the one the
parser built, parameter names and all — splits its parameter list, and
counts the commas at the cursor's paren depth to say which argument is
being typed.

**`#include` completion** lists the headers under the same `-I` and
`-isystem` directories the index was built with, so a name it offers is a
name that will resolve. A partial path (`sys/`) searches inside it.

C++ gets the other five answers but not these three: `struct cexpr` carries
a line and no column, and a rename needs the column. That is the next step
there, not a limitation of the approach — the C++ front end resolves a name
to its declaration *pointer* at parse time, which is stronger than the
name-and-scope matching the C side does.

**Three bugs came out of building it**, all older than the feature:

- `lookup_at` fell back to a local or parameter that was **not in scope**
  when nothing else matched. Hovering the file-scope `total` reported the
  parameter of a function three declarations away. An out-of-scope local is
  now skipped outright, which also fixes hover and go-to-definition.
- A parameter was indexed at its **function's line, column 1**, because
  `struct func` never recorded where each name was written. Renaming one
  would have overwritten the first character of the signature. The parser
  now records each parameter name's own line and column, `merge_decls`
  carries the definition's over, and the rename edits exactly the right
  bytes.
- `flags_load` ran on every keystroke and **appended**, so the `-I` list
  grew by a full copy per edit until it hit its 64-entry cap and silently
  dropped whatever came after. It clears first now.

## Remarks — why the compiler did what it did

Diagnostics say what is wrong with the program. **Remarks say what the
compiler decided about it**, and they are recorded by the pass that decided,
at the moment it decided (vision R2). That is the whole design: `embcc why`
is a query over records, never an engine that re-derives a reason afterwards.

    $ embcc why not-inlined prog.c -O2
    variadic (prog.c:20): not-inlined
      because callee-is-varargs
      decided by the inline pass
    fp (prog.c:20): not-inlined
      because returns-floating-point
      decided by the inline pass
    big (prog.c:20): not-inlined
      because callee-too-large — 74 instructions, budget 24
      decided by the inline pass

Three refusals, three different reasons. Before this, `inlinable()` returned
`0` from a dozen places and every one of them meant something else; the
information existed for a microsecond and was thrown away. Naming those
branches — not writing the API — was the work.

A remark is data, and text is the last step (§13): `-fremarks` prints the
text form during an ordinary compile, `-fremarks=json` the record form, and
both are off by default because a pass that always built strings would slow
every compile for a report almost nobody wants.

`reason` is the part that must stay stable — it is what a query matches and
what a person learns. `detail` is free text and may be improved at will.

**The answer says when it has none.** The inliner is an `-O2` pass, so at
`-O0` there is nothing recorded — and an empty list would read as "there was
no reason":

    $ embcc why not-inlined prog.c -O0
    nothing recorded: no pass made a 'not-inlined' decision
    (remarks come from the passes that RAN -- an optimization
    decision needs -O2)

### The producers

| pass | decisions it records |
|---|---|
| `inline` | `inlined` / `not-inlined`, with the reason out of twelve and the size that settled it |
| `mem2reg` | `promoted-to-register` / `kept-in-memory` **per variable**, at that variable's own declaration line |
| `sroa` | `split-into-scalars` / `kept-whole` **per aggregate**, with the reason it is still one object |
| `sccp` | `branch-always-jumps` / `branch-never-jumps` when a condition folds to a constant |
| `opt` | `optimized`: what the whole fixpoint came to, as instructions before → after |
| `regalloc` | `spilled-to-stack`: how many values missed a register, out of how many, against how many registers exist |

**"Why is my variable still on the stack?"** is the question `mem2reg`
answers, and five different causes used to leave the same zero behind:

    $ embcc why kept-in-memory prog.c -O2
    vol (prog.c:7): kept-in-memory
      because declared-volatile
    addressed (prog.c:8): kept-in-memory
      because address-is-taken
    agg (prog.c:9): kept-in-memory
      because not-a-scalar-integer-pointer-or-float

Each points at its own declaration line, which needed `struct ir_dbgvar` to
carry one — R3 work that fell out of R2, because a remark about a variable
has to point at the variable and not at the function containing it.

The last of those has a follow-up question — *an aggregate cannot be
promoted, so is it at least being taken apart?* — and `sroa` answers it in
the same shape, one line per object with the reason it is still one:

    $ embcc why kept-whole prog.c -O2
    punned (prog.c:14): kept-whole
      because two-accesses-overlap-at-different-widths
    leaks (prog.c:22): kept-whole
      because address-escapes

"Overlap at different widths" is the union read as a `double` and as two
`int`s; "address escapes" is the address reaching a callee, a global or
anything else that could keep it. Neither is a missed optimization — split
either one and the program writes a value and reads a different one.

**A remark states the fact it is sure of.** SCCP knows a branch folded; it
does *not* know whether the programmer's condition was true, because `if (c)`
lowers to `BRZ c -> else` and a *taken* branch is therefore a *false*
condition. Reporting "condition is always true" there would be exactly
backwards, so the remark reports the IR fact and the golden asserts that the
source-polarity wording never appears.

**Still silent:** local CSE, global CSE, DCE, copy propagation, load
elimination and store forwarding. These run inside a fixpoint where `changed`
doubles as loop control, so counting them means restructuring the loop rather
than adding a line — the per-function `opt` summary covers their aggregate
effect until then. The aarch64 backend has no register allocator (memory-model
codegen), so `spilled-to-stack` correctly never appears on that target.
