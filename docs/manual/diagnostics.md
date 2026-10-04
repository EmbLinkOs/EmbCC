# Diagnostics

This page describes how EmbCC reports problems in a program: the format
of an error or warning, how many errors one run reports, the JSON form an
editor reads, fix-it hints and `--fix`, the `--explain` entries behind the
`E0001`-style ids, every `-W` option, and the optimization remarks that
say what the optimizer decided. It is for anyone who compiles code with
EmbCC or integrates it into an editor or build system.

> **Section ids.** Comments in the EmbCC sources cite
> "docs/manual/diagnostics.md T1" through "T6". Those ids refer to
> sections of this page: T1 is [the diagnostic format](#t1), T2 is
> [error recovery](#t2), T3 is [fix-its](#t3), T4 is
> [the warning options](#t4), T5 is [the language server](#t5), and T6 is
> [`--explain`](#t6) together with [`embld --doctor`](#embld-doctor).
> Dependency generation (`-M`, `-MD`, ...) and `-fsyntax-only`, which the
> old T4 also covered, are described in [Invoking EmbCC](invoking.md).

<a id="t1"></a>

## The diagnostic format

### Anatomy of a diagnostic

Every diagnostic is written to standard error. In the default text
format it looks like this:

```c
int area(int w, int h)
{
    int height = h;
    return w * heigth;
}
```

```text
embcc: area.c:4:16: error: 'heigth' is not declared in 'area' — for a call, add a prototype or define it first [E0001]
      return w * heigth;
                 ^~~~~~
embcc: area.c:4:16: note: did you mean 'height'?
      return w * heigth;
                 ^~~~~~
                 height
embcc: compilation terminated: 1 error
```

The parts are, in order:

| Part | Meaning |
|---|---|
| `embcc:` | Every diagnostic line starts with the program name. |
| `area.c:4:16:` | File, line and column. Columns count bytes from 1. The column is omitted when it is not known (some diagnostics are tied to a whole line); the line is omitted when the diagnostic is about the file as a whole. A diagnostic with no file names `<embcc>`. |
| `error:` | The severity: `error`, `warning` or `note`. |
| message | What is wrong. |
| `[-Wname]` | Printed after a diagnostic that a `-W` option controls (a warning, or an error under `-Werror` or `-Werror=name`). `-Wno-name` turns it off. |
| `[E0001]` | Printed after a diagnostic that has an [explanation](#t6): `embcc --explain E0001` prints it. |

When the source text of the file is available, the diagnostic is
followed by the source line (indented by two spaces) and a caret line.
The `^` marks the column; `~` underlines the rest of the identifier or
number the caret lands on, or the range the front end recorded. Tabs in
the source line are kept in the caret line so that the caret stays
aligned. A diagnostic without a column shows the source line with no
caret.

A diagnostic with a [fix-it](#t3) prints the replacement text on a
further line, under the place it goes.

### Severities

| Severity | Meaning |
|---|---|
| `error` | The program is not valid, or EmbCC cannot compile it. The compile fails and the exit status is 1. |
| `warning` | The program is valid but probably wrong. The compile continues. Most warnings are controlled by a [`-W` option](#t4); `-w` drops them and `-Werror` turns them into errors. |
| `note` | Extra information about the error or warning printed immediately before it: where an earlier declaration is, a suggested spelling, the header that declares a name, the macro an expansion came from, or the template instantiation being processed. A note does not affect the exit status. |

Optimization **remarks** are not diagnostics. They are off by default and
are described in [Optimization remarks](#remarks).

### Notes EmbCC attaches

| Note | Attached to |
|---|---|
| `did you mean 'NAME'?` | An undeclared identifier whose spelling is close to a name in scope, or a member name close to a member the structure has. Carries a fix-it. |
| `'NAME' is declared in <HEADER>` | An undeclared identifier that is a C library name EmbCC knows (see [Which diagnostics carry fix-its](#fixit-producers)). Carries a fix-it that inserts the `#include` at line 1. |
| `insert ';' here` | A missing semicolon. Points at the end of the previous token and carries a fix-it. |
| `use '.' here`, `use '->' here` | `->` applied to a structure value, or `.` applied to a pointer to a structure. Carries a fix-it. |
| `the one it hides is here` | A `-Wshadow` warning. |
| `'NAME' is declared here, with no initializer` | A `-Wuninitialized` or `-Wmaybe-uninitialized` warning. |
| `expanded from macro 'NAME'` | Any error or warning whose column falls on the name of a macro invoked on that line, or, when the column is unknown, whose line invokes a macro. Printed as a heading only, without a source line. |
| `in the instantiation of ...` | A C++ error inside a template being instantiated. Up to 12 such notes are printed, innermost first. |
| `this is a bug in EmbCC, not in the program being compiled` | An `internal error:`. |

### When diagnostics are printed

EmbCC records diagnostics while it compiles and prints them all once the
compile ends (or stops at an error it cannot recover from). They appear
in the order they were found, each followed by its notes.

A small number of messages bypass this mechanism and are printed
immediately as plain text:

- driver messages about the command line itself, such as an unknown `-W`
  or `-Werror=` name, `-pedantic`, or a `-std=` value that is accepted but
  not enforced;
- `escape sequence out of range ...; truncated, as gcc does`, from the
  lexer;
- `the optimizer stopped after N rounds on 'F' without converging`, from
  the optimizer.

These carry no `[-W...]` tag, are not affected by `-w` or `-Werror`, and
do not appear in [JSON output](#json).

### Color

#### `-fdiagnostics-color[=WHEN]`

Color the text output with ANSI escape sequences. `WHEN` is `auto`,
`always` or `never`; `-fdiagnostics-color` with no value means `always`.
Any other value is treated as `auto`. The default is `auto`: color is
used when standard error is a terminal.

With color, the location and the message are bold, `error:` is bold red,
`warning:` bold magenta, `note:` bold cyan, and the caret line and fix-it
text bold green. EmbCC does not read `GCC_COLORS` or `NO_COLOR`.

#### `-fno-diagnostics-color`

The same as `-fdiagnostics-color=never`.

### Exit status

| Status | Meaning |
|---|---|
| 0 | No errors. Warnings do not change the status. |
| 1 | At least one error, including a warning turned into an error by `-Werror` or `-Werror=NAME`. No output file is left (see [`-Werror`](#-werror)). |

With `--fix`, the status is 0 whenever at least one fix-it was applied
(see [`--fix`](#fix)).

### Internal errors

An inconsistency inside the compiler is reported as

```text
embcc: <embcc>: error: internal error: ...
embcc: <embcc>: note: this is a bug in EmbCC, not in the program being compiled
```

with no source location, because the problem is in EmbCC's state rather
than at a place in the program. Please report it with the input that
caused it.

<a id="t2"></a>

## How many errors one run reports

### Error recovery

EmbCC reports every independent error it can in one run, rather than
stopping at the first.

- **Parsing.** After a syntax error the parser skips to the next
  statement in the current block, or to the next external declaration,
  and carries on. A missing `;` is a special case: the parser continues as
  if the semicolon were present, so a file missing several semicolons
  reports each of them.
- **Semantic analysis.** An error inside a function body abandons the
  statement that contains it and continues with the next statement.
  A name already reported as undeclared in a function is not reported
  again in that function.
- **C++.** The C++ front end recovers the same way, at declarations and
  at statements. Once parsing is finished and template instantiation has
  begun, an error ends the compile.

Stages do not run on a broken program. If parsing reported errors,
semantic analysis does not start; if semantic analysis reported errors,
no code is generated. The run then ends with

```text
embcc: compilation terminated: N errors
```

N counts every error found, including repeated uses of an undeclared
name that were not printed, so it can be larger than the number of
`error:` lines above it.

Some errors end the compile at once, with no summary line:

- semantic errors outside a function body, such as an invalid
  file-scope initializer;
- `control may reach the end of 'F'` ([E0008](#t6));
- command-line and file errors (`cannot open file`, an unknown option);
- errors from code generation, the assembler and object writers (an
  unsupported construct for the target, for example).

The following file shows recovery: three syntax errors are reported in
one run.

```c
int f(void)
{
    int a = 1
    int b = 2
    return a + b;
}
int g(void) { return ) ; }
```

```text
embcc: semi.c:4:5: error: expected ';' before 'int' [E0002]
      int b = 2
      ^~~
embcc: semi.c:3:14: note: insert ';' here
      int a = 1
               ^
               ;
embcc: semi.c:5:5: error: expected ';' before 'return' [E0002]
      return a + b;
      ^~~~~~
embcc: semi.c:4:14: note: insert ';' here
      int b = 2
               ^
               ;
embcc: semi.c:7:22: error: expected an expression, got ')'
  int g(void) { return ) ; }
                       ^
embcc: compilation terminated: 3 errors
```

### `-fmax-errors=N`

Stop after N errors. When the limit is reached EmbCC prints the errors so
far and

```text
embcc: compilation terminated due to -fmax-errors=N
```

and exits with status 1. Warnings turned into errors by `-Werror` or
`-Werror=NAME` count toward the limit. `0` means no limit, which is the
default. The value is read with `atoi`, so a value that is not a number
also means no limit.

EmbCC does not accept Clang's spelling `-ferror-limit=N` or GCC's
`-Wfatal-errors` (the first is an unknown argument, the second an unknown
warning name).

<a id="json"></a>

## Machine-readable output

### `-fdiagnostics-format=FORMAT`

Select the output format. `FORMAT` is `text` (the default) or `json`.
Any other value is refused:

```text
embcc: unknown diagnostic format 'xml' (text, json)
```

`json` writes GCC's JSON diagnostic format to standard error: one JSON
array holding every diagnostic of the run, with notes nested under the
diagnostic they belong to. A tool that reads GCC's
`-fdiagnostics-format=json` reads EmbCC's.

```sh
embcc -fsyntax-only -fdiagnostics-format=json area.c
```

```text
[
  {"kind": "error", "message": "'heigth' is not declared in 'area' — for a call, add a prototype or define it first", "id": "E0001", "column-origin": 1, "locations": [{"caret": {"file": "area.c", "line": 4, "column": 16, "display-column": 16, "byte-column": 16}, "finish": {"file": "area.c", "line": 4, "column": 21, "display-column": 21, "byte-column": 21}}], "children": [
      {"kind": "note", "message": "did you mean 'height'?", "column-origin": 1, "locations": [{"caret": {"file": "area.c", "line": 4, "column": 16, "display-column": 16, "byte-column": 16}, "finish": {"file": "area.c", "line": 4, "column": 21, "display-column": 21, "byte-column": 21}}], "fixits": [{"start": {"file": "area.c", "line": 4, "column": 16, "display-column": 16, "byte-column": 16}, "next": {"file": "area.c", "line": 4, "column": 22, "display-column": 22, "byte-column": 22}, "string": "height"}]}
  ]}
]
```

Each element of the array is an object with these members:

| Member | Present | Value |
|---|---|---|
| `kind` | always | `"error"`, `"warning"` or `"note"`. A warning turned into an error by `-Werror` or `-Werror=NAME` has kind `"error"`. |
| `message` | always | The message text, without the `[-W...]` and `[E....]` tags. |
| `option` | warnings a `-W` option controls | The option, for example `"-Wunused-variable"`. Kept when `-Werror` makes the diagnostic an error. |
| `id` | diagnostics with an explanation | The [explain id](#t6), for example `"E0001"`. EmbCC's own member; GCC has no equivalent. |
| `column-origin` | always | `1`: columns count from 1. |
| `locations` | always | An array with one object. Its `caret` is the point the diagnostic is about. Its `finish` is the last column of the marked range (inclusive) and is present only when the column is known. |
| `fixits` | when the diagnostic has fix-its | An array of edits. Each replaces the text from `start` up to but not including `next` (both on the same line) with `string`. Insertion has `start` equal to `next`. |
| `children` | when there are notes | The notes, each an object of this same shape. |

Each position is an object with `file`, `line`, `column`,
`display-column` and `byte-column`. All three column values are the same
byte column; EmbCC does not compute display widths for tabs or multibyte
characters. A diagnostic about a whole line has `column` 0 and no
`finish`; one about a whole file also has `line` 0.

Points to be aware of when consuming the output:

- When a compile produces no diagnostics, nothing at all is written, not
  even `[]`. Treat empty standard error as an empty array.
- In JSON mode the `compilation terminated` line is not printed; the
  array is the whole of the diagnostic output.
- Driver messages about the command line (an unknown `-W` name, for
  example) are written as plain text before the array. Read from the first
  `[`.
- `-fdiagnostics-parseable-fixits` has no effect in JSON mode; the fix-its
  are in the `fixits` members.

### Using JSON diagnostics from an editor

The usual way for an editor to get diagnostics is to run a syntax-only
compile of the buffer with the project's flags and parse the array:

```sh
embcc -fsyntax-only -fdiagnostics-format=json -I include -DCONFIG=1 file.c
```

`-fsyntax-only` runs the preprocessor, the parser and semantic analysis
and writes no output file. The editor maps each element to its own
diagnostic: `kind` to a severity, `caret` and `finish` to a range
(converting the 1-based columns to whatever the editor counts from), and
`children` to related information. `fixits` can be offered as code
actions: each is a self-contained text edit that needs no parsing of the
message.

EmbCC's language server works this way; see
[the language server](#t5).

<a id="t3"></a>

## Fix-its

A fix-it is an edit attached to a diagnostic or to one of its notes: a
replacement of a span of one source line by new text. Fix-its are
offered only where the edit is determined by the diagnostic. They appear
in three forms: under the caret in the text output, in the `fixits`
member of the JSON output, and in GCC's parseable line format.

<a id="fixit-producers"></a>

### Which diagnostics carry fix-its

| Diagnostic | Edit |
|---|---|
| `'NAME' is not declared in 'F' ...` ([E0001](#t6)), when a name in scope is within a small edit distance | Replace the identifier with the suggested name (on the `did you mean` note). |
| `'NAME' is not declared ...`, when NAME is a known C library function or macro | Insert `#include <HEADER>` and a newline at line 1, column 1. |
| `struct S has no member 'm'` ([E0004](#t6)), when S has a member with a close spelling | Replace the member name with the suggested one. |
| `expected ';' before ...` ([E0002](#t6)) | Insert `;` after the previous token. |
| `'->' needs a pointer to a struct/union, got struct S (use '.' on a value)` | Replace `->` with `.`. |
| `'.' needs a struct/union, got struct S * (use '->' through a pointer)` | Replace `.` with `->`. |

The C library names that produce a header suggestion are:

| Header | Names |
|---|---|
| `<stdio.h>` | `printf`, `fprintf`, `sprintf`, `snprintf`, `puts`, `putchar`, `fputs`, `fopen`, `fclose`, `fread`, `fwrite`, `fgets`, `scanf`, `sscanf`, `perror`, `fflush`, `fseek`, `ftell` |
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `exit`, `abort`, `atoi`, `atol`, `strtol`, `strtoul`, `strtod`, `qsort`, `bsearch`, `getenv`, `rand`, `srand` |
| `<string.h>` | `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strdup`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strerror` |
| `<math.h>` | `sqrt`, `pow`, `fabs`, `sin`, `cos`, `tan`, `log`, `log2`, `log10`, `exp`, `floor`, `ceil`, `round`, `fmod` |
| `<ctype.h>` | `isalpha`, `isdigit`, `isalnum`, `isspace`, `isupper`, `islower`, `toupper`, `tolower` |
| `<assert.h>` | `assert` |
| `<time.h>` | `time`, `clock`, `strftime`, `localtime` |
| `<errno.h>` | `errno` |
| `<unistd.h>` | `write`, `read`, `close`, `lseek`, `unlink` |
| `<fcntl.h>` | `open` |

A name suggestion (`did you mean`) is chosen by spelling distance. It is
usually, but not always, the intended name; review the edit.

### `-fdiagnostics-parseable-fixits`

After each diagnostic in the text output, print its fix-its (and those of
its notes) in GCC's machine-readable form, one per line:

```text
fix-it:"FILE":{LINE:COL-LINE:NEXT}:"TEXT"
```

The edit replaces columns `COL` up to but not including `NEXT` on line
`LINE` with `TEXT`. A `"` or `\` in `TEXT` is preceded by `\`. For the
file above:

```text
fix-it:"area.c":{4:16-4:22}:"height"
```

The `#include` fix-it's text ends in a newline, which is written as a
literal newline rather than escaped.
<!-- The literal newline is how src/driver/diag.c render_fixits_parseable
     prints it at this commit; GCC writes \n. Reported to the lead. -->

<a id="fix"></a>

### `--fix`

Apply the fix-its to the source files instead of only printing them.
`--fix` implies `-fsyntax-only`: no object file is written. The
diagnostics are printed as usual, then each file named by a fix-it is
rewritten in place:

```text
embcc: semi_fix.c: applied 2 fixes
embcc: 2 fixes applied; compile again
```

The rules are:

- Edits are applied from the end of the file backwards, so earlier
  positions stay valid.
- At most one edit is applied per line. Two edits on one line could
  overlap; the second one is left for the next run.
- At most 512 fix-its are collected per run.
- A file with no fix-it is not touched. When nothing could be fixed the
  driver prints `embcc: nothing to fix automatically`.

The exit status is 0 if at least one fix was applied, even though the
compile itself failed; otherwise it is the status of the compile. Run the
compiler again after `--fix` to see what remains.

<a id="t6"></a>

## Explaining a diagnostic: `--explain`

### `--explain ID`, `--explain=ID`

Print the explanation for the diagnostic id `ID` to standard output and
exit. An explanation states the rule, shows the mistake and the fix side
by side, and cites the paragraph of the C standard the rule comes from.
`ID` is case-insensitive (`e0004` works). An unknown id prints

```text
embcc: no explanation for 'E9999'; `embcc --explain` lists what there is
```

and exits with status 1. `--explain` with no id lists every entry.

```sh
embcc --explain E0004
```

```text
E0004: a struct or union has no such member

The member name is looked up in the type of the expression to the left of
the `.` or `->`, and that type has no member of that name.

    struct Point { int x; int y; };
    int f(struct Point p) { return p.z; }   /* E0004 */
...
C99 6.5.2.3.
```

Diagnostics that have an explanation print their id at the end of the
message (`[E0004]`) and carry it in the JSON `id` member.

### Diagnostic ids

| Id | Explanation title | Printed with |
|---|---|---|
| `E0001` | a name is used that was never declared | `'NAME' is not declared in 'F' — for a call, add a prototype or define it first` |
| `E0002` | a statement or declaration is missing its ';' | `expected ';' before TOKEN` |
| `E0003` | a value of one type is used where another is required | `CONTEXT: cannot convert FROM to TO`, where a structure or union is passed as an argument, returned, or used as an initializer for a different type. CONTEXT is `argument`, `return` or `initialization`. |
| `E0004` | a struct or union has no such member | `struct S has no member 'NAME'` |
| `E0005` | an object is declared of a type whose size is not known | `'NAME' has incomplete type TYPE`, for an object defined in a block without an initializer |
| `E0006` | a function is called with the wrong number of arguments | No diagnostic prints this id. The error for a wrong argument count is `this call needs N arguments, got M`. |
| `E0007` | a value is assigned to something that cannot be assigned to | No diagnostic prints this id. The related errors are `cannot assign to an array` and `assignment target must be a variable, *pointer, or member`. |
| `E0008` | control reaches the end of a function that must return a value | `control may reach the end of 'F' — every path must end in a return statement` |

`E0006` and `E0007` have explanation entries but are not attached to any
diagnostic. An assignment to a `const`-qualified object, the example in
the `E0007` entry, is the error `assignment of read-only 'NAME' (its
type is TYPE)`; see [Const qualification](c-language.md#const-qualification).

`E0008` is an error, not a warning, and it ends the compile. It applies
to every function with a non-`void` return type except `main`. Reaching
the closing brace of `main` returns 0, as C99 5.1.2.2.3 specifies, so
`int main(void) { }` is a valid program.

<a id="t4"></a>

## Warning options

`--help-warnings` lists every warning EmbCC has and the group each
belongs to.

### Controlling warnings

Options are processed from left to right. Each one sets the state of
the warnings it names, and a later option overrides an earlier one. In
particular, `-Wno-unused-variable -Wall` leaves `-Wunused-variable` on,
because `-Wall` comes later; write `-Wall -Wno-unused-variable`. (GCC
gives a specific option precedence over a group regardless of order;
EmbCC does not.)

#### `-Wall`

Enable `-Waddress`, `-Wformat`, `-Wmaybe-uninitialized`, `-Wparentheses`,
`-Wshift-count-overflow`, `-Wuninitialized`, `-Wunused-function` and
`-Wunused-variable`.

#### `-Wextra`, `-W`

Enable `-Wlogical-op`, `-Wsign-compare`, `-Wtype-limits` and
`-Wunused-parameter`. `-Wextra` does not imply `-Wall`. `-W` is an older
spelling of `-Wextra`.

#### `-WNAME`

Enable the warning `NAME`. If EmbCC has no warning of that name, the
option is accepted, has no effect, and the driver prints

```text
embcc: warning: -Wcast-align is not a warning EmbCC has, so it turns nothing on (--help-warnings lists them)
```

This message is printed immediately, as plain text; `-w` does not
suppress it and `-Werror` does not make it an error.

#### `#pragma GCC diagnostic`

A file can set warnings for a region of itself with `#pragma GCC
diagnostic` (or `#pragma clang diagnostic`, or the `_Pragma` operator
form); see [`#pragma GCC diagnostic`](extensions.md#pragma-gcc-diagnostic).
A pragma overrides the command line for the warnings it names, from its
position on.

#### `-Wno-NAME`

Disable the warning `NAME`. A name EmbCC does not have is accepted
silently and has no effect.

#### `-Werror`

Turn every warning into an error. The diagnostic is printed with
`error:` instead of `warning:` and keeps its `[-Wname]` tag, and the
exit status becomes 1. Warnings that no option controls (a macro
redefinition, `#warning`) are turned into errors too.

A warning turned into an error does not stop the compile: the remaining
diagnostics are still reported. The compile has failed, though, and it
leaves no output file: the object, assembly file or executable it was
writing is removed, together with any file of that name an earlier
compile left, so `make` builds it again on the next run. A dependency
file from `-MD` or `-MMD` and a `.su` file from `-fstack-usage` are still
written. A warning that is not an error still lets the object be
written.

#### `-Wno-error`

Undo an earlier `-Werror`.

#### `-Werror=NAME`, `-Wno-error=NAME`

`-Werror=NAME` turns the warning `NAME` on and reports it as an error,
whether or not `-Werror` is given. `-Wno-error=NAME` keeps `NAME` a
warning when `-Werror` is given; it does not turn the warning on. For
the warning it names, either option overrides `-Werror` and `-Wno-error`
wherever they appear on the command line:

```sh
embcc -Wall -Werror -Wno-error=unused-variable -c f.c   # -Wunused-variable stays a warning
embcc -Werror=format -c f.c                            # -Wformat on, and an error
```

`-Wno-NAME` after `-Werror=NAME` turns the warning off again, and `-w`
suppresses it. The warnings no option controls follow `-Werror` alone. A
`NAME` that is not a warning EmbCC has is reported and ignored:

```text
embcc: warning: -Werror=cast-align names no warning EmbCC has (--help-warnings lists them)
```

Like the unknown-warning message, it is printed immediately as plain
text.

#### `-w`

Suppress all warnings that go through the diagnostic engine, including
those `-Werror` would have turned into errors. `-w` takes effect wherever
it appears on the command line. See [Limitations](#warning-limitations)
for the notes `-w` does not suppress.

#### `-Wsystem-headers`

Report warnings in system headers. By default a warning whose location
is in a system header is dropped, because the header is not the
project's to fix. Errors are never dropped. A header is a system header
when it was found through an `-isystem` directory or one of EmbCC's own
include directories, or was included by another system header.
`-Wno-system-headers` is accepted and has no effect.

#### `-pedantic`, `-pedantic-errors`, `-Wpedantic`

EmbCC compiles one C dialect, C11 with the GNU extensions, and has no
diagnostics for extensions to ISO C; see
[C language support](c-language.md). `-pedantic` and `-pedantic-errors`
are accepted with a warning that they turn nothing on:

```text
embcc: warning: -pedantic: EmbCC has no diagnostics for extensions to ISO C, so this turns nothing on
```

The message is printed immediately as plain text; `-w` does not suppress
it and `-Werror` does not make it an error. `-pedantic-errors` makes no
diagnostic an error. `-Wpedantic` is an unknown warning name and turns
nothing on.

### Summary of warnings

| Option | Diagnoses | Enabled by |
|---|---|---|
| `-Waddress` | A function name tested in an `if` condition, or compared with a null pointer constant | `-Wall` |
| `-Wattributes` | An attribute EmbCC does not know, or one it accepts but does not implement | default |
| `-Wdeprecated-declarations` | A use of a function or variable declared `deprecated` | default |
| `-Wdiscarded-qualifiers` | A pointer conversion that drops the pointed-to type's `const` | default |
| `-Wdiv-by-zero` | Integer division or remainder by a constant zero | default |
| `-Wformat` | A `printf`- or `scanf`-style format that disagrees with its arguments | `-Wall` |
| `-Wlogical-op` | `a && a` or `a \|\| a` with identical operands | `-Wextra` |
| `-Wmaybe-uninitialized` | A local variable read on a path where only some paths wrote it | `-Wall` |
| `-Wparentheses` | A comparison as an unparenthesized operand of `&`, `\|` or `^` | `-Wall` |
| `-Wshadow` | A local declaration that hides a local variable, a parameter or a file-scope variable | none |
| `-Wshift-count-overflow` | A constant shift count that is negative or not less than the operand width | `-Wall` |
| `-Wsign-compare` | A comparison that converts a possibly negative signed operand to unsigned | `-Wextra` |
| `-Wtype-limits` | An unsigned value compared `< 0` or `>= 0` | `-Wextra` |
| `-Wuninitialized` | A local variable read before any path wrote it | `-Wall` |
| `-Wunused-function` | A `static` function that is defined and never used | `-Wall` |
| `-Wunused-parameter` | A function parameter the body never uses | `-Wextra` |
| `-Wunused-result` | A discarded result of a function declared `warn_unused_result` or `[[nodiscard]]` | default |
| `-Wunused-variable` | A local variable that is never used | `-Wall` |
| `-Wwindows-abi` | A compile for a Windows target, whose ABI EmbCC does not yet fully implement | default |

"default" means the warning is on unless `-Wno-NAME` or `-w` is given.
"none" means only `-WNAME` enables it.

### Warning reference

#### `-Waddress`

Warn when the name of a function is used where its address is always
non-null: as the condition of an `if`, or compared with a null pointer
constant using `==` or `!=`. This is almost always a missing call. A
function declared `__attribute__((weak))` is exempt, because its address
can be null. A function pointer variable is never reported. Enabled by
`-Wall`.

```c
void tick(void);
int f(void) { return tick == 0; }
```

```text
embcc: addr.c:2:22: warning: the address of 'tick' is never null, so this is always false -- a call may be missing [-Waddress]
  int f(void) { return tick == 0; }
                       ^~~~
```

In an `if`, the message is `... so this branch is always taken -- a call
may be missing`.

#### `-Wattributes`

Warn about an `__attribute__` that has no effect. Two cases are reported:

- an attribute EmbCC does not know:
  `attribute 'NAME' is not one EmbCC knows, and is ignored`;
- an attribute EmbCC accepts but does not implement, currently `error`
  and `warning`: `__attribute__((error)) is accepted but does nothing
  here: ...`, followed by the reason.

On by default. Attributes that EmbCC refuses outright are errors, not
warnings; see [Extensions](extensions.md).

```c
int odd(void) __attribute__((frobnicate));
```

```text
embcc: attr.c:1: warning: attribute 'frobnicate' is not one EmbCC knows, and is ignored [-Wattributes]
  int odd(void) __attribute__((frobnicate));
```

#### `-Wdeprecated-declarations`

Warn when a function or variable declared with
`__attribute__((deprecated))` or `[[deprecated]]` is called, read, or
has its address taken. The message is `'NAME' is deprecated`; the text
argument of `deprecated("...")` is not printed. On by default.

```c
int old(void) __attribute__((deprecated));
int f(void) { return old(); }
```

```text
embcc: dep.c:2:22: warning: 'old' is deprecated [-Wdeprecated-declarations]
  int f(void) { return old(); }
                       ^~~
```

#### `-Wdiscarded-qualifiers`

Warn when a pointer to a `const`-qualified type is converted, without a
cast, to a pointer whose pointed-to type is not `const`: in an
initialization, an assignment, an argument or a `return`. A store through
the result would modify a read-only object. On by default, as in GCC.

```c
void take(char *p);
void f(const char *s) { take(s); }
```

```text
embcc: q.c:2:30: warning: argument discards the 'const' qualifier of const char * [-Wdiscarded-qualifiers]
  void f(const char *s) { take(s); }
                               ^
```

#### `-Wdiv-by-zero`

Warn about an integer division or remainder whose right operand is a
constant expression equal to zero: `division by zero`. On by default.

```c
int f(int x) { return x / 0; }
```

```text
embcc: div.c:1:23: warning: division by zero [-Wdiv-by-zero]
  int f(int x) { return x / 0; }
                        ^
```

#### `-Wformat`

Check direct calls to functions declared with
`__attribute__((format(printf, N, M)))` or
`__attribute__((format(scanf, N, M)))` whose format argument is a string
literal. The declarations in EmbCC's `<stdio.h>` carry the attribute, and
a program's own functions can use it. Enabled by `-Wall`.

The check compares each conversion with the argument it consumes, after
the default argument promotions, so `printf("%f", 1.0f)` and
`printf("%d", (short)x)` are correct and are not reported. For `scanf`,
nothing is promoted through the pointer: `%f` writes a `float` and `%lf`
a `double`. It reports:

- an integer conversion whose argument has a different size
  (`%d reads 4 bytes, but this argument is long, which is 8`);
- an integer conversion given a pointer or a non-integer, a floating
  conversion given a non-floating argument, `%Lf`-style mismatches
  between `double` and `long double`;
- `%s` given something that is not a pointer to characters, and a
  pointer conversion given a non-pointer;
- a `scanf` conversion whose pointer points at the wrong class or size
  of object;
- a `*` width or precision whose argument is not an integer;
- an unknown conversion character (`'%y' is not a conversion this format
  understands`); the argument count is not checked after one;
- too few or too many arguments for the format.

A difference in signedness alone (`printf("%d", 3u)`, `printf("%x", -1)`)
is not reported. When `M` is 0 (a function taking a `va_list`), only the
format string itself is checked.

```c
#include <stdio.h>
void f(long n) { printf("%d\n", n); }
```

```text
embcc: fmt.c:2:33: warning: %d reads 4 bytes, but this argument is long, which is 8 [-Wformat]
  void f(long n) { printf("%d\n", n); }
                                  ^
```

#### `-Wlogical-op`

Warn when both operands of `&&` or `||` are the same expression: the
same variable, the same chain of member accesses, or the same number,
with no side effects. Enabled by `-Wextra`.

```c
int f(int x) { return x || x; }
```

```text
embcc: logic.c:1:23: warning: both operands of '||' are the same expression [-Wlogical-op]
  int f(int x) { return x || x; }
                        ^
```

#### `-Wmaybe-uninitialized`

Warn when a local variable is read at a point that some, but not all,
paths reach with a value written: a write inside an `if` with no `else`,
a loop that may run zero times, a `switch` with no `default`. Each
variable is reported once, with a note at its declaration. Enabled by
`-Wall`. See `-Wuninitialized` for what the analysis covers.

```c
int g(int c)
{
    int y;
    if (c)
        y = 1;
    return y;
}
```

```text
embcc: maybe.c:6:12: warning: 'y' may be used uninitialized [-Wmaybe-uninitialized]
      return y;
             ^
embcc: maybe.c:3:9: note: 'y' is declared here, with no initializer
      int y;
          ^
```

A value carried around a loop (written late in the body and read
earlier in it on the next iteration) is not reported when the read is
under a test inside that loop, because such a test is usually the guard
that makes the read safe.

#### `-Wparentheses`

Warn when a comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`) is an operand
of a bitwise `&`, `|` or `^` without parentheses around it. Because
comparisons bind more tightly, `reg & MASK == 0` means
`reg & (MASK == 0)`. Enabled by `-Wall`. Other cases GCC's
`-Wparentheses` covers, such as an assignment used as a condition, are
not reported.

```c
int f(unsigned reg) { return reg & 0x4 == 0; }
```

```text
embcc: paren.c:1:30: warning: comparison binds tighter than '&' here: this is 'a & (b == c)', not '(a & b) == c' [-Wparentheses]
  int f(unsigned reg) { return reg & 0x4 == 0; }
                               ^~~
```

The message always shows `==`, whichever comparison operator was used.

#### `-Wshadow`

Warn when a declaration in a block hides a local variable or parameter
that is still in scope, or when a local variable or a parameter hides a
file-scope variable declared before the function, with a note at the
hidden declaration. A local that has the name of a function is not
reported, as with GCC and Clang. Not enabled by `-Wall` or `-Wextra`.
The message for a file-scope variable is `declaration of 'NAME' shadows
a global declaration`.

```c
int f(int n)
{
    int total = 0;
    for (int i = 0; i < n; i++) {
        int total = i;
        (void)total;
    }
    return total;
}
```

```text
embcc: shadow.c:5:13: warning: declaration of 'total' shadows an earlier one [-Wshadow]
          int total = i;
              ^~~~~
embcc: shadow.c:3:9: note: the one it hides is here
      int total = 0;
          ^~~~~
```

When the hidden name is a parameter the message is `... shadows a
parameter`.

#### `-Wshift-count-overflow`

Warn when the right operand of `<<` or `>>` is a constant that is
negative or not less than the width in bits of the promoted left
operand. Such a shift is undefined, and the hardware typically masks the
count rather than producing zero. Enabled by `-Wall`.

```c
int f(int x) { return x << 32; }
```

```text
embcc: shift.c:1:23: warning: shift count 32 is not less than the width of int (32 bits) [-Wshift-count-overflow]
  int f(int x) { return x << 32; }
                        ^
```

A negative count gives `shift count -1 is negative`.

#### `-Wsign-compare`

Warn when a comparison between a signed and an unsigned integer is
performed in an unsigned type, so that a negative value compares as a
large one. A signed operand that is a non-negative constant is not
reported. Enabled by `-Wextra`.

```c
int f(int i, unsigned n) { return i < n; }
```

```text
embcc: sign.c:1:35: warning: comparison between int and unsigned int [-Wsign-compare]
  int f(int i, unsigned n) { return i < n; }
                                    ^
```

#### `-Wtype-limits`

Warn when a value of unsigned type is compared with the constant 0 in a
way whose result the type already decides: `u < 0` (always false) and
`u >= 0` (always true), in either operand order. Enabled by `-Wextra`.
Other range comparisons, such as a `char` compared with 300, are not
reported.

```c
int f(unsigned u) { return u < 0; }
```

```text
embcc: limits.c:1:28: warning: comparison of unsigned int with 0 is always false: it has no negative values [-Wtype-limits]
  int f(unsigned u) { return u < 0; }
                             ^
```

#### `-Wuninitialized`

Warn when a local variable is read at a point that no path reaches with
a value written. Each variable is reported once, with a note at its
declaration. Enabled by `-Wall`.

The analysis follows the control flow of the function body: `if`,
loops, `break` and `continue`, `switch` and `case` labels, `&&`, `||` and
`?:`, `return`, and calls to functions declared `noreturn`.
`va_start(ap, ...)` counts as a write of `ap`. It checks scalar local
variables only; arrays, structures, `static` locals and globals are not
checked. It also does not check:

- a variable whose address is taken (`&x`), because writes through the
  pointer are not followed;
- a variable used as an operand of an `asm` statement;
- any variable in a function that contains a `goto`, a label, or a
  label address (`&&label`): such a function is not analysed at all.

```c
int f(int c)
{
    int x;
    return x + c;
}
```

```text
embcc: uninit.c:4:12: warning: 'x' is used uninitialized [-Wuninitialized]
      return x + c;
             ^
embcc: uninit.c:3:9: note: 'x' is declared here, with no initializer
      int x;
          ^
```

`(void)x` counts as a read: it is reported if `x` was never written.
<!-- Observed at this commit: `int voided; (void)voided;` gives
     -Wuninitialized. GCC and Clang do not warn on (void)x. Reported to
     the lead. -->

#### `-Wunused-function`

Warn about a `static` function that is defined and never called or
referred to. A non-`static` function is never reported, because another
translation unit may call it, and neither is `main`. A function declared
`__attribute__((unused))` or `__attribute__((used))` is exempt, and so is
a `static inline` function defined in a header, as with Clang. Enabled by
`-Wall`. The location has a line but no column.

```c
static int helper(int x) { return x; }
```

```text
embcc: func.c:1: warning: unused function 'helper' [-Wunused-function]
  static int helper(int x) { return x; }
```

#### `-Wunused-parameter`

Warn about a named parameter that the function body never uses. Enabled
by `-Wextra`. The location has a line but no column. Writing `(void)name;`
in the body marks the parameter as used, and so does
`__attribute__((unused))` or `[[maybe_unused]]` on it, written before its
type, between the type and the name, or after the name
(`int f(int x __attribute__((unused)))`).

```c
int f(int a, int b) { return a; }
```

```text
embcc: param.c:1: warning: unused parameter 'b' [-Wunused-parameter]
  int f(int a, int b) { return a; }
```

#### `-Wunused-result`

Warn when a call to a function declared `__attribute__((warn_unused_result))`
or `[[nodiscard]]` is an expression statement on its own, so its value is
discarded. Casting the call to `void` marks the discard as intended. On by
default.

```c
int must(void) __attribute__((warn_unused_result));
void f(void) { must(); }
```

```text
embcc: result.c:2:16: warning: result of 'must' is discarded, and it is declared warn_unused_result [-Wunused-result]
  void f(void) { must(); }
                 ^~~~
```

#### `-Wunused-variable`

Warn about a local variable that is declared and never referred to.
A variable that is assigned but never read counts as used. A variable
declared `__attribute__((unused))` is exempt, as are names the compiler
introduces for its own purposes. Enabled by `-Wall`.

```c
int f(int a)
{
    int unused;
    return a;
}
```

```text
embcc: var.c:3:9: warning: unused variable 'unused' [-Wunused-variable]
      int unused;
          ^~~~~~
```

#### `-Wwindows-abi`

Issued once for every object file written for a Windows target
(`--target=x86_64-windows-gnu` or `x86_64-w64-mingw32`): the code EmbCC
generates for Windows is not yet fully the Microsoft x64 ABI. `long` is
8 bytes and `wchar_t` 4 (Windows has 4 and 2), and `rsi`, `rdi`, `xmm6`
and `xmm7` are not preserved across calls, so objects EmbCC compiles
agree with each other but not with code built by other compilers. On by
default. The warning carries a file name but no line, and is not issued
for `-fsyntax-only`, `-E` or `-S`. See [Targets](targets.md) for the
state of the Windows target.

```text
embcc: win.c: warning: x86_64-windows-gnu is not yet the Microsoft x64 ABI: `long` is 8 bytes and wchar_t 4 (Windows has 4 and 2), and rsi, rdi, xmm6 and xmm7 are not preserved across a call: objects EmbCC compiles agree with each other and with nothing else [-Wwindows-abi]
```

### Warnings no option controls

These warnings have no `-W` option and no `[-W...]` tag. `-w` suppresses
them and `-Werror` turns them into errors; `-Wno-...` cannot turn them
off individually.

| Message | Cause |
|---|---|
| `macro 'NAME' redefined` | A `#define` that changes an existing macro's definition. The new definition replaces the old one. |
| `#warning: TEXT` | A `#warning` directive. |
| `section("NAME") on function 'F' is ignored` | C++ only: a `section` attribute on a function. |

The lexer's `escape sequence out of range` warnings and the optimizer's
non-convergence warning are printed outside the diagnostic engine (see
[When diagnostics are printed](#t1)); no option, including `-w`, affects
them.

### Warnings in C++

The analyses behind the warnings run in the C front end, over the C that
EmbCC lowers C++ to. For a C++ compile:

- `-Wunused-variable`, `-Wunused-parameter`, `-Wunused-function`,
  `-Wshadow` and `-Wsign-compare` are switched off, whatever the command
  line says, because the lowered code contains names and conversions the
  programmer did not write.
- The other warnings report what survives lowering in a recognizable
  form. `-Wformat`, `-Wuninitialized`, `-Wmaybe-uninitialized`,
  `-Wdiv-by-zero`, `-Wshift-count-overflow` and `-Wtype-limits` report on
  C++ source. The C++ front end handles attributes itself, so
  `-Wattributes`, `-Wdeprecated-declarations` and `-Wunused-result` do not
  report for C++ declarations; `-Wparentheses`, `-Waddress` and
  `-Wlogical-op` do not report either.
- The line and column of a warning can be wrong, because they come from
  the lowered code.
<!-- The C++ coverage list above is from compiling sample programs at this
     commit (cxw.cc, cxattr.cc in docwork/diagnostics), not from a rule in
     the source. Lines were off by several lines in one sample. -->

See [C++ support](cxx.md).

<a id="warning-limitations"></a>

### Limitations

- A note that belongs to a warning that is not printed (because its
  option is off, because of `-w`, or because it is in a system header) is
  still printed. With `-Wuninitialized` on and `-Wmaybe-uninitialized`
  off, for example, the `'y' is declared here, with no initializer` note of
  the suppressed warning appears on its own. In JSON it is attached to
  whichever diagnostic came before it.
- `__attribute__((unused))` is honoured on local variables and functions,
  not on parameters.
- The C++ limitations described in [Warnings in C++](#warnings-in-c).

<a id="unsupported-warning-options"></a>

### Options EmbCC does not implement

The driver keeps no list of GCC or Clang warning options to accept and
ignore. Instead, every option that starts with `-W` is handled by the
rules above, with these consequences:

| Option | What EmbCC does |
|---|---|
| `-WNAME` for a name not in the [summary](#summary-of-warnings) (`-Wconversion`, `-Wcast-align`, `-Wpedantic`, `-Wunused`, `-Weverything`, `-Wfatal-errors`, ...) | Accepted; prints `embcc: warning: -WNAME is not a warning EmbCC has, so it turns nothing on (--help-warnings lists them)`; no effect. |
| `-Wno-NAME` for a name not in the summary (`-Wno-unused`, `-Wno-system-headers`, ...) | Accepted silently; no effect. `-Wno-unused` does **not** turn off the `-Wunused-*` warnings. |
| `-Werror=NAME`, `-Wno-error=NAME` for a name not in the summary | Accepted; prints `embcc: warning: -Werror=NAME names no warning EmbCC has (--help-warnings lists them)`; no effect. |
| `-Wp,ARGS` | Not passed to the preprocessor. Treated as an unknown warning name: the `is not a warning EmbCC has` message is printed and the arguments are ignored. |
| `-Wl,ARGS`, `-Wa,ARGS` | Options for the link and the assembler, not warnings; see [Invoking EmbCC](invoking.md#assembler-and-linker-options). |
| `-pedantic`, `-pedantic-errors` | Accepted; prints `embcc: warning: -pedantic: EmbCC has no diagnostics for extensions to ISO C, so this turns nothing on`; no effect. |

<a id="remarks"></a>

## Optimization remarks

A remark records a decision an optimization pass made and the reason for
it, at the point where the pass made it. Remarks are not collected unless
asked for, because building them costs time on every compile.

EmbCC does not implement Clang's `-Rpass=`, `-Rpass-missed=` and
`-Rpass-analysis=` options or `-fsave-optimization-record`; they are
unknown arguments. Use `-fremarks` and `embcc why`.

### `-fremarks`

Collect remarks during the compile and print them to standard error when
it ends, one per line, in the order the passes made them:

```text
FILE:LINE: remark: DECISION 'SUBJECT': DETAIL [PASS/REASON]
```

For example, at `-O2`:

```text
rem.c:11: remark: inlined 'add': 4 instructions into sum, budget 24 [inline/small-enough]
rem.c:12: remark: not-inlined 'variadic' [inline/callee-is-varargs]
rem.c:6: remark: kept-in-memory 'vol' [mem2reg/declared-volatile]
rem.c:7: remark: kept-in-memory 'addressed' [mem2reg/address-is-taken]
rem.c:13: remark: branch-never-jumps 'sum': the condition folded to 1, so one arm is unreachable [sccp/condition-is-a-constant]
rem.c:11: remark: unrolled 'sum': 8 copies of a 8-instruction body, the original kept for the remainder [unroll/counted-loop]
```

`REASON` is a stable code meant to be matched by tools and searched for.
`DETAIL` is free text giving the facts that settled the decision, and may
change between releases. Most remarks come from passes that run only
when optimizing; at `-O0` there are few or none.

### `-fremarks=json`

The same remarks as a JSON array on standard error. Each element has
`pass`, `decision`, `subject` and `reason`; `detail` when there is one;
and `location` (an object with `file` and `line`) when the remark has a
location.

```text
[
  {"pass": "inline", "decision": "inlined", "subject": "add", "reason": "small-enough", "detail": "4 instructions into sum, budget 24", "location": {"file": "rem.c", "line": 11}},
  {"pass": "inline", "decision": "not-inlined", "subject": "variadic", "reason": "callee-is-varargs", "location": {"file": "rem.c", "line": 12}},
  ...
]
```

The remark array is separate from the diagnostic array of
`-fdiagnostics-format=json`; when both are requested, standard error
holds both.

### Remark producers

| Pass | Decisions | Reasons |
|---|---|---|
| `inline` | `inlined`, `not-inlined` | `small-enough`, `always_inline`; `callee-too-large`, `not-a-sole-callee-at-O1`, `callee-not-defined-here`, `would-be-recursive`, `callee-is-noinline`, `callee-is-weak`, `callee-is-varargs`, `callee-computes-in-__int128`, `callee-has-exception-regions`, `returns-a-struct`, `parameter-is-not-a-simple-scalar`, `returns-a-value-wider-than-a-vreg`, `callee-has-inline-asm`, `callee-uses-va_start`, `callee-has-a-vla`, `callee-uses-a-computed-goto`, `callee-calls-a-struct-returning-function` |
| `mem2reg` | `promoted-to-register`, `kept-in-memory` (one per local variable, at its declaration) | `scalar-and-never-addressed`; `address-is-taken`, `declared-volatile`, `read-is-volatile`, `write-is-volatile`, `read-is-partial-or-extending`, `write-is-partial`, `not-a-scalar-integer-pointer-or-float`, `not-4-or-8-bytes`, `type-unknown` |
| `sroa` | `split-into-scalars`, `kept-whole` (one per aggregate local) | `address-never-escapes`; `address-escapes`, `address-held-in-a-reassigned-temp`, `declared-volatile`, `access-is-volatile`, `access-is-not-1-2-4-or-8-bytes`, `access-runs-outside-the-object`, `two-accesses-overlap-at-different-widths`, `too-many-pieces`, `too-large-to-split`, `is-a-wide-scalar`, `is-a-variable-length-array` |
| `sccp` | `branch-always-jumps`, `branch-never-jumps` | `condition-is-a-constant` |
| `unroll` | `unrolled` | `counted-loop` |
| `switch-thread` | `threaded` | `state-known` |
| `regalloc` | `spilled-to-stack`, `split` | `no-register-free`, `loop-live-range` |
| `opt` | `optimized`, `rewrote`, `inferred`, `hoisted`, `rotated`, `vectorized`, `strength-reduced`, `recognized-memcpy`, `recognized-memzero`, `dropped` | `fixpoint-reached`, `pass-counts`, `attr/const`, `attr/pure`, `licm/loop-invariant`, `licm/bottom-tested-loop`, `vec/constant-trip-count`, `vec/runtime-trip-count`, `ivsr/address`, `idiom/loop`, `unreachable` |

The SCCP decisions describe the branch in the intermediate code: a
branch that "always jumps" corresponds to a source condition that is
false when the condition is the test of an `if`, because an `if` is
lowered to a jump over its body. The remark does not translate this back
to the source condition.

`optimized` reports a function's instruction count before and after
optimization; `rewrote` reports, per function, how many changes the
passes that make no remark of their own (value numbering, CSE, load
reuse, copy propagation, dead code and dead stores, selects, partial
redundancy elimination) made.

### `embcc why DECISION [SUBJECT] FILE [OPTIONS]`

Compile `FILE` with `OPTIONS` (any ordinary compile options, such as
`-O2`, `-I` or `--target=`), collect the remarks, and print to standard
output the ones whose decision is `DECISION` and, when `SUBJECT` is
given, whose subject is `SUBJECT`. No output file is written. `why` must
be the first argument. The word after `DECISION` is taken as `SUBJECT`
when it neither starts with `-` nor contains a `.` and at least one more
argument follows it.

```sh
embcc why kept-in-memory vol rem.c -O2
```

```text
vol (rem.c:6): kept-in-memory
  because declared-volatile
  decided by the mem2reg pass
```

When nothing matches, `why` says so instead of printing an empty answer:

```text
nothing recorded: no pass made a 'not-inlined' decision
(remarks come from the passes that RAN -- an optimization
decision needs -O2)
```

The exit status is 0 whether or not anything matched, and 1 for a usage
error. The `dropped` decision is recorded only under `-fremarks`, so
`embcc why dropped` does not find it.
<!-- main.c records opt/dropped only when want_remarks is set, not for
     `why`. Reported to the lead. -->

### `embcc inspect`

`embcc inspect STAGE FILE [OPTIONS]` prints what one stage of the
compiler built (`tokens`, `pp`, `ast`, `symbols`, `types`, `ir`, `cfg`,
`callgraph`) instead of compiling. It shows the result of the decisions
that remarks explain; see [Invoking EmbCC](invoking.md).

## Related tools

<a id="t5"></a>

### The language server

`embls` is EmbCC's language server: it speaks the Language Server
Protocol over standard input and output. For diagnostics it runs
`embcc -fsyntax-only -fdiagnostics-format=json` over the editor's buffer
and publishes each error and warning, with its notes appended to the
message, so the editor shows what the build will report. It does not
offer the fix-its as code actions. It also answers completion, hover,
go-to-definition, references, rename, signature help and document
symbols from EmbCC's own front end. It reads compile flags from
`compile_flags.txt` in the file's directory or above it, and from the
`EMBLS_FLAGS` environment variable. See [embls](tools/embls.md).

<a id="embld-doctor"></a>

### `embld --doctor`

`embld --doctor FILES...` reads the objects and archives a link would
use and explains, for every undefined symbol at once, why it is
undefined: a definition that exists but is `static`, a C library function
that needs a library the link does not add, the C++ runtime, a missing
key function for a vtable, a member function declared and never defined,
or no definition anywhere. See [embld](tools/embld.md).

```sh
embld --doctor main.o util.o
```

```text
embld: undefined: helper
  wanted by main.o
  util.o does define it — but as `static`, which keeps it inside that
  unit. Drop the `static`, or move the caller into that file.
embld: doctor: 1 symbol undefined
```
