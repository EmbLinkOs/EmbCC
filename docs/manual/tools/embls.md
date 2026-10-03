# embls — the EmbCC language server

This page is the reference for `embls`, EmbCC's language server. It is
for people who connect an editor to EmbCC and for people who write an
editor client for it. `embls` speaks the Language Server Protocol (LSP)
over standard input and output. It publishes the diagnostics that `embcc`
itself reports for the file being edited. It answers completion, hover,
go-to-definition, references, rename, signature help and document-symbol
requests from an index built by EmbCC's own preprocessor and parser. The
page covers every request `embls` answers, how it finds a project's
flags, what it writes, and where its answers are approximate.

## NAME

`embls` — a Language Server Protocol server for C and C++, backed by the
EmbCC front end

## SYNOPSIS

```text
embls
```

`embls` takes no arguments. It reads LSP messages from standard input and
writes responses and notifications to standard output. It stops when its
input ends or when it receives `exit`.

## DESCRIPTION

An editor starts `embls` as a child process and exchanges messages with
it over a pipe. `embls` keeps the text of every document the editor
opens. Each time a document is opened or changed, `embls` does three
things, in this order:

1. It reloads the project flags (see [Project flags](#project-flags)).
2. It rebuilds the document's index (see [The index](#the-index)).
3. It runs `embcc` over the document and publishes the diagnostics (see
   [Diagnostics](#diagnostics)).

All other requests are answered from the index of the document they
name. `embls` holds one index at a time: a request about a document
other than the one indexed last first reloads that document's flags and
rebuilds its index from its current text. `embls` handles one message at
a time. A request that
arrives while a document is being reindexed is answered after the
reindex. Cancellation (`$/cancelRequest`) is not implemented.

### Documents

`embls` asks for full-document synchronization (`textDocumentSync` is
`1`). On `textDocument/didChange` it takes the `text` of the last element
of `contentChanges` as the complete new content of the document. On
`textDocument/didClose` it does nothing: the document and its index stay,
and requests about it are still answered.

A `file://` URI is turned into a path by removing the scheme and decoding
`%XX` escapes.

The preprocessor and `embcc` read files, not editor buffers. `embls`
therefore writes the document's current text to a temporary file beside
the document and removes it once it has been read:

| File | Used for |
|---|---|
| `PATH.embls-tmp.c` | building the index |
| `PATH.embls-diag.c` or `PATH.embls-diag.cc` | running `embcc` for the diagnostics |

`PATH` is the document's path. Because the copy sits beside the
document, `#include "..."` finds the same headers it finds in a build.
The document's directory must be writable. When it is not, the
document's index is empty and no diagnostics are published for it.

### Project flags

`embls` looks for a file named `compile_flags.txt` in the document's
directory, then in each parent directory in turn, eight directories in
all. It reads the first one it finds. This is the file format clangd
reads:

- Each line is one command-line argument, passed to `embcc` as written.
- A trailing newline or carriage return is removed, and empty lines are
  skipped.
- There is no comment syntax and no quoting. An option and its value
  either share a line with no space between them (`-Iinclude`) or sit
  on two lines (`-I`, then `include`).

The value of the `EMBLS_FLAGS` environment variable, split at spaces, is
appended after the arguments from the file. Together they are limited
to 64 arguments; any after the 64th are ignored. Both are reloaded on
every open and change, so an edit to `compile_flags.txt` takes effect at
the next change to a document.

Relative paths in the flags are resolved against the working directory of
the `embls` process, not against the directory that holds
`compile_flags.txt`. Start `embls` in the directory the paths are
relative to, or write absolute paths. Absolute paths are also needed for
go-to-definition into those headers; see [Limitations](#limitations).

All the flags are passed to `embcc` for the diagnostics. The index uses
only the include directories: `-IDIR`, `-isystemDIR`, and `-I` or
`-isystem` followed by the directory as the next argument.

### Diagnostics

For the diagnostics, `embls` runs this command through `/bin/sh`:

```text
EMBCC -fsyntax-only -fdiagnostics-format=json -I 'DIR' 'FLAG'... -c 'TEMP' 2>&1
```

- `EMBCC` is the value of `EMBLS_EMBCC`. When that variable is unset or
  empty, it is `embcc`, found through `PATH`.
- `DIR` is the document's directory.
- The `FLAG`s are the project flags, in order.
- `TEMP` is the temporary copy of the document.

`embls` reads both output streams and parses the JSON array that starts
at the first `[`. The array's format is described in
[JSON diagnostics](../diagnostics.md#json). It then sends one
`textDocument/publishDiagnostics` notification for the document. That
notification lists every diagnostic in the array and replaces the
previous set. Each element of the array becomes one LSP diagnostic:

| LSP field | Value |
|---|---|
| `range` | on the line of the first location's `caret`, from the caret column to the `finish` column (one character when there is no `finish`), converted to 0-based |
| `severity` | `2` (Warning) when `kind` is `warning`; `1` (Error) for every other kind |
| `source` | `embcc` |
| `message` | the `message`, followed by a line `  note: TEXT` for each note in `children` |

A diagnostic that spans several lines is reported on its first line
only. The diagnostic's warning option, its `--explain` id and its fix-its
are not passed on. `embls` offers no code actions.

If `embcc` cannot be run, or prints no JSON array, the notification
carries an empty list.

Which language `embcc` compiles the document as depends on the suffix of
the temporary file, not on the document's own suffix. See
[C and C++](#c-and-c).

### The index

Every request other than the diagnostics is answered from the index. At
each open or change, `embls` forks a child process. The child runs
EmbCC's preprocessor and parser over the document and sends back, through
a pipe, one record for each declaration and each use of a name. The child
discards its own error output.

A front end may end its process when it meets input it cannot continue
past. Because the front end runs in the child, that ends only the child,
and `embls` keeps serving. The parser recovers from most syntax errors.
So a document that does not compile is still indexed, and completion
keeps working while the file is wrong. A function whose body cannot be
parsed may lose its local variables from the index. If the child stops
before it has written the index, the document's index is empty until the
document's next change. For example, an `#error` directive that is active
under the index's configuration (below) stops the child.

For a C document the index records:

- functions, with their signatures as the front end parsed them;
- file-scope variables, with their types;
- parameters and local variables, with their types and the range of
  lines of the function that declares them;
- `struct`, `union` and `enum` tags, typedef names, and enumerators with
  their values;
- the members of each structure and union, under the structure's tag;
- every use of a name inside a function body. A use is a variable, the
  function called by a call, or a member after `.` or `->`. An identifier
  in a comment or a string literal is not a use.

Macros are not recorded.

The index is always built with this configuration, whatever the project
flags say:

- The predefined macros are those of the x86-64 target, whatever
  `--target` selects. For example, `__x86_64__` is defined and `__AVR__`
  is not.
- Headers are searched for in the document's directory, and then in the
  `-I` and `-isystem` directories of the project flags, in order. The
  compiler's built-in include directories are not searched. Declarations
  from the C library headers, such as `printf` from `<stdio.h>`, are
  therefore not indexed unless a flag names their directory.
- A header that is not found is skipped. This is not an error for the
  index; the diagnostics still report the missing header.
- `-D`, `-U`, `-x`, `--target` and every other flag are ignored. Code
  under `#if` that depends on a macro defined with `-D` is indexed as if
  the macro were not defined.

### C and C++

The document's suffix decides which front end builds the index. The C++
front end is used for `.cc`, `.cpp`, `.cxx`, `.C`, `.c++`, `.hpp`, `.hh`
and `.hxx`. The C front end is used for every other suffix, `.h`
included.

A C++ document's index records:

- functions. A member function's signature is qualified with its class,
  as in `int Widget::size()`.
- classes, with the detail `class NAME`, `struct NAME` or `union NAME`.
- each class's data members and the member functions a caller can name,
  as its members. Constructors, destructors, and functions that are
  implicitly declared or deleted are excluded.
- namespace-scope variables, parameters and local variables.

It records no uses of names. In a C++ document,
`textDocument/references` and `textDocument/rename` therefore find only
the declaration.

The language of the diagnostics is decided by a separate rule. The
temporary file is named `.embls-diag.c` when the document's path contains
`.c` but not `.cc`, and `.embls-diag.cc` otherwise. `embcc` then picks
the language from that suffix. The two rules disagree for some suffixes:

| Document suffix | Index | Diagnostics |
|---|---|---|
| `.c` | C | C |
| `.cc`, `.C`, `.hh`, `.hpp`, `.hxx` | C++ | C++ |
| `.cpp`, `.cxx`, `.c++` | C++ | C |
| `.h`, and any other suffix | C | C++ |

The test applies to the whole path, so a directory name that contains
`.c` or `.cc` changes the result too. A `.cpp` file is diagnosed as C:
`embcc` reports its class definitions with errors such as
`expected a type before 'class'`. A `.h` file is diagnosed as C++, so a
C header that uses a C++ keyword as an identifier gets errors such as
`expected a name to declare before 'new'`. Give C++ sources the `.cc`
suffix to have them diagnosed as C++.

### Positions

Positions use LSP's 0-based line and character numbers. `embls` counts
characters in bytes, whereas the protocol counts UTF-16 code units. The
two agree on lines that contain only ASCII. On a line with multi-byte
characters, positions after the first such character are off by the
difference.

### Requests

`embls` answers the requests below. Every other request is answered with
a `null` result, and every other notification is ignored. A request about
a document that has not been opened is answered with `null`.

Hover, definition, references, prepare-rename and rename all start by
looking up the name at the cursor:

1. The name is the identifier the cursor is in or immediately after,
   taken from the document's text. A word inside a comment or a string
   is looked up like any other.
2. If a local variable or parameter of that spelling belongs to the
   function whose lines contain the cursor, that local wins.
3. Otherwise the result is the first declaration of that spelling the
   front end recorded. A prototype in a header comes before the
   function's definition in the document.

When nothing matches, the result is `null`.

#### `initialize`

The result advertises the capabilities listed under [OUTPUT](#output)
and the server information `{"name":"embls","version":"0.1"}`. The
request's parameters are not read.

#### `shutdown`, `exit`

`shutdown` is answered with `null`. `exit` ends `embls` with status 0,
whether or not `shutdown` came first.

#### `textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose`

The document notifications. See [Documents](#documents).

#### `textDocument/completion`

The trigger characters are `.` and `>`. The result is a completion list
with `isIncomplete` set to `false`. It is not filtered by what has
already been typed; the editor filters it. What the list holds depends on
the text before the cursor on the cursor's line:

- **On an `#include` line.** If the line, up to the cursor, is `#include`
  followed by `<` or `"` and the start of a name, the list holds the
  matching entries of the include directories. Those are the document's
  directory and then the `-I` and `-isystem` directories of the project
  flags. A `/` in the partial name searches the named subdirectory
  (`<sys/` lists the inside of each `sys` directory). Only these entries
  are offered: directories, files ending in `.h` or `.hpp`, and files
  with no `.` in their name, such as `vector`. Names that begin with `.`
  are skipped. The list holds at most 400 entries. Each item's `kind` is
  17 (File) or 19 (Folder), and its `detail` is the directory it was
  found in.
- **After `.` or `->`.** If the cursor follows `.` or `->`, with an
  optional partial member name between, the list holds the members of the
  operand's type and nothing else. Spaces are allowed between the operand
  and the operator, but not after it. The operand must be an identifier
  that names a local variable, a parameter or a file-scope variable
  whose type is a tagged structure, union or class, or a pointer to one.
  A typedef of a tagged structure qualifies. The list is empty when the
  operand is itself a member (`o.i.`) or when its type is an untagged
  structure (`typedef struct { ... } T;`). When the operand does not end
  in an identifier (`arr[0].`, `f()->`), the general list below is
  returned instead.
- **Anywhere else.** The general list holds everything in the index
  except members and uses. Local variables and parameters are included
  only when the cursor is on a line of the function that declares them.
  A name declared twice, such as a prototype and its definition, appears
  twice. In a C++ document the list also holds the functions the compiler
  declares implicitly, such as `operator new` and a class's implicit
  constructors and assignment operators. A fixed list of 41 C keywords,
  from `auto` to `_Static_assert`, ends the list, in C++ documents as
  well.

| What | `kind` | `detail` |
|---|---|---|
| function | 3 (Function) | its signature, such as `int distance2(struct point a, struct point b)` |
| variable, local, parameter | 6 (Variable) | its type |
| member | 5 (Field) | its type |
| tag, typedef, class | 7 (Class) | `struct NAME` (or `union`, `enum`, `class`) for a tag, the type for a typedef |
| enumerator | 20 (EnumMember) | `= VALUE` |
| keyword | 14 (Keyword) | none |

#### `textDocument/hover`

The result is Markdown content of this form:

````text
```c
DETAIL
```

KIND — FILE:LINE
````

- `DETAIL` is the signature or type shown in completion, or the name when
  there is none. A function declared with `(void)` is shown with `()`.
  The code fence is tagged `c` in C++ documents too.
- `KIND` is one of `function`, `variable`, `local`, `parameter`, `type`,
  `enumerator` or `member`.
- `FILE:LINE` is the 1-based line of the declaration. It is left out
  when no position is recorded: for tags, typedefs, enumerators and
  members in C, and for classes and data members in C++.

#### `textDocument/definition`

The result is one location, a zero-length range at the declaration found
by the lookup. A declaration is located at its name. The exception is a
parameter in a C++ document, which is located at the start of its
function's line. The result is `null` when no position is recorded; see
`textDocument/hover`.

#### `textDocument/references`

The result is a list of locations, one for each use of the declaration
found by the lookup. When `context.includeDeclaration` is true or absent,
the list starts with the declaration itself, followed by every other
file-scope declaration of the same name: a prototype, an `extern`
declaration in a header, the definition. A use is counted under these
rules:

- For a local variable or a parameter, a use counts only in the same
  function. Another function's local of the same spelling is a different
  name.
- For a file-scope name, a use counts except in a function where a local
  or parameter of the same spelling is declared.
- For a member, every use of a member of that spelling after `.` or `->`
  counts, whatever its structure type. A member never matches a variable
  of the same spelling, and a variable never matches a member.
- An identifier in a comment or a string literal is never a use.

Each range covers as many bytes as the name has. Uses are found only in
function bodies in the document and in the headers it includes. In a C++
document only the declaration is found.

#### `textDocument/prepareRename`

The result is the range of the identifier under the cursor, on the
cursor's line, when the lookup finds a declaration; otherwise it is
`null`. No placeholder is returned.

#### `textDocument/rename`

The result is a workspace edit with a `changes` map. The map holds every
declaration and every use that `textDocument/references` finds with
`includeDeclaration` true, each replaced by `newName`. Each edit is
listed under the URI of the file it changes, so a rename of a name
declared in a header edits the header and the document.

Before it answers, `embls` checks every edit against the text it would
change: the document's own text, the text of another open document, or
the file on disk. If any range does not hold the name, the rename is
refused and nothing is edited. A refusal is an error response with code
`-32803` (RequestFailed) and a message that starts
`cannot rename 'NAME':`. It is given for:

- a use that comes from the body of a macro (`... is not spelled there in
  the source (a macro expansion, or a declaration located only by its
  line)`);
- any name in a C++ document (`in a C++ document embls records
  declarations but not uses`);
- a structure or union member (`cannot rename member 'NAME': embls does
  not tell members of different structures apart`);
- a name whose declaration has no recorded position, such as an
  enumeration constant or a type (`where it is declared is not
  recorded`);
- a name that occurs in a header found through a relative `-I` path
  (`it occurs in FILE, a header found through a relative include
  path`).

`newName` is not checked; an empty `newName` gives `null`.

#### `textDocument/signatureHelp`

The trigger characters are `(` and `,`. `embls` scans back from the
cursor, on the cursor's line only, to the innermost unclosed `(`. It
counts the commas outside nested brackets on the way. The identifier
before that `(` names the function. The result holds the signature of
the first function of that name in the index, with each parameter as a
separate label. `activeParameter` is the comma count, kept at most at the
last parameter, so that a variadic call stays on `...`. The result is
`null` when the call's `(` is on an earlier line or the function is not
in the index.

#### `textDocument/documentSymbol`

The result is a flat list of the functions (kind 12) and file-scope
variables (kind 13) declared in the document itself. Declarations from
headers are not listed. In a C++ document, member functions are listed
too. Each entry has `name`, `kind`, a `detail` holding the signature or
type, and a `range` and `selectionRange`. `selectionRange` covers the
name. `range` runs from the start of the name's line to the end of the
name.

### Limitations

- Go-to-definition and hover find the first declaration recorded. When a
  header declares a function and the document defines it, they go to the
  header's prototype.
- Parameters in a C++ document are located at the start of their
  function's line, not at their name.
- `textDocument/rename` works in C documents only, and not for members,
  enumeration constants or types; see `textDocument/rename` for the
  refusals.
- A header found through a relative `-I` path is recorded under that
  relative path. Definitions and references in such a header are
  reported under the document's own URI instead, and a rename that would
  edit one is refused.
- A diagnostic located in an included header is shown in the document,
  at the header's line and column.
- The scope of a local variable is the whole function, not the block
  that declares it.
- There is no cross-file index. Only the document and the headers it
  includes are known; a function defined in another `.c` file is known
  only through its prototype in a header.

## OPTIONS

`embls` has no options. Command-line arguments are ignored, `--help` and
`--version` included: `embls --help` waits for LSP messages like `embls`
does. `embls` is configured through `compile_flags.txt` (see
[Project flags](#project-flags)) and the variables under
[ENVIRONMENT](#environment).

## OUTPUT

### Framing

Each message on standard input is a header block ended by an empty line,
followed by a JSON body. The `Content-Length` header gives the size of
the body in bytes; its name is matched without regard to case. Other
headers are ignored. A header block that has no `Content-Length` ends
`embls`, as does the end of the input.

Each message `embls` writes to standard output has the same form, with
`Content-Length` as its only header:

```text
Content-Length: 38\r\n
\r\n
{"jsonrpc":"2.0","id":3,"result":null}
```

A response carries the request's `id`, numeric or string, unchanged.
`embls` writes nothing to standard error. The output of `embcc` is read
by `embls` and never reaches the editor directly.

### Messages

The `initialize` result, with all the capabilities `embls` advertises:

```json
{"jsonrpc":"2.0","id":1,"result":{"capabilities":{"textDocumentSync":1,"completionProvider":{"triggerCharacters":[".",">"]},"hoverProvider":true,"definitionProvider":true,"referencesProvider":true,"renameProvider":{"prepareProvider":true},"signatureHelpProvider":{"triggerCharacters":["(",","]},"documentSymbolProvider":true},"serverInfo":{"name":"embls","version":"0.1"}}}
```

A diagnostics notification for a document with an undeclared name, where
`embcc` attached a suggestion as a note (the body is wrapped here):

```json
{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{
 "uri":"file:///home/me/proj/a.c","diagnostics":[{"range":{
 "start":{"line":1,"character":24},"end":{"line":1,"character":25}},
 "severity":1,"source":"embcc",
 "message":"'h' is not declared in 'main' — for a call, add a prototype or define it first\n  note: did you mean 'g'?"}]}}
```

A hover result for a function:

```json
{"jsonrpc":"2.0","id":2,"result":{"contents":{"kind":"markdown","value":"```c\nint square(int n)\n```\n\nfunction — /home/me/hello/hello.c:1"}}}
```

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | the input ended, `exit` was received, or a message had no `Content-Length` header |

`embls` exits with status 0 in every case, including an `exit` that was
not preceded by `shutdown`.

## ENVIRONMENT

### `EMBLS_EMBCC`

The compiler to run for diagnostics. The value is placed, unquoted, at
the start of the shell command shown under [Diagnostics](#diagnostics).
When the variable is unset or empty, `embcc` is used.

### `EMBLS_FLAGS`

Extra arguments, separated by spaces, appended after the arguments from
`compile_flags.txt`. Use it to try a flag without creating the file. A
later argument overrides an earlier one, as on the `embcc` command line;
for example, `-Wno-unused-variable` here turns off what `-Wall` in the
file turned on.

### `PATH`

Searched by `/bin/sh` for `embcc` when `EMBLS_EMBCC` is unset or empty.

## FILES

| File | Meaning |
|---|---|
| `compile_flags.txt` | the project's flags, one argument per line, in the document's directory or one of up to seven directories above it; see [Project flags](#project-flags) |
| `PATH.embls-tmp.c`, `PATH.embls-diag.c`, `PATH.embls-diag.cc` | temporary copies of the document `PATH`, written beside it at each open and change and removed after use; see [Documents](#documents) |

## EXAMPLES

Build `embls`. `make all` and `make install` build it too, and
`make install` copies it into `PREFIX/bin`:

```sh
make embls
```

Set up a Cortex-M3 firmware project whose headers are in `include/`.
In the project root, `compile_flags.txt` holds:

```text
--target=thumbv7m-none-eabi
-Iinclude
-Wall
```

Have the editor start `embls` with the project root as its working
directory, so that `include` is resolved against it. The diagnostics are then those of
`embcc --target=thumbv7m-none-eabi -Iinclude -Wall`. The index still uses
the x86-64 predefined macros, as described under
[The index](#the-index).

Try extra flags for one session without editing the file:

```sh
EMBLS_FLAGS='-Wextra -DDEBUG=1' embls
```

In this example, `-DDEBUG=1` affects the diagnostics only.

Point Neovim's built-in LSP client at `embls` for the current buffer:


```lua
vim.lsp.start({ name = "embls", cmd = { "embls" },
                root_dir = vim.fn.getcwd() })
```

Drive a session by hand from the shell. This script asks for the hover
text of `square` at its call:

```sh
msg() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
uri="file://$PWD/hello.c"
{
  msg '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
  msg '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"'"$uri"'","text":"int square(int n) { return n * n; }\nint main(void) { return square(3); }\n"}}}'
  msg '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"'"$uri"'"},"position":{"line":1,"character":26}}}'
  msg '{"jsonrpc":"2.0","id":3,"method":"shutdown"}'
  msg '{"jsonrpc":"2.0","method":"exit"}'
} | embls
```

`${#1}` counts characters, which equals the byte count only because the
messages are ASCII. Run in `/home/me/hello`, the script prints the
`initialize` result shown under [OUTPUT](#output), then the following
(the `\r\n` line ends are shown as plain line breaks):

```text
Content-Length: 126

{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///home/me/hello/hello.c","diagnostics":[]}}Content-Length: 146

{"jsonrpc":"2.0","id":2,"result":{"contents":{"kind":"markdown","value":"```c\nint square(int n)\n```\n\nfunction — /home/me/hello/hello.c:1"}}}Content-Length: 38

{"jsonrpc":"2.0","id":3,"result":null}
```

## SEE ALSO

[Diagnostics](../diagnostics.md) (the diagnostic format, the JSON form
`embls` reads, and [the language server](../diagnostics.md#t5)),
[Invoking EmbCC](../invoking.md#-fsyntax-only),
[C++ support](../cxx.md), [Targets](../targets.md),
[`embidx`](embidx.md), [Getting started](../getting-started.md)
