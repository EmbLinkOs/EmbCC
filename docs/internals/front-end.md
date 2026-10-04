# The front end

This page describes how EmbCC turns source text into the checked tree
that IR generation reads: the preprocessor, the lexer, the C parser,
semantic analysis, the type system, the C++ front end, and how each
stage reports and recovers from errors. It is for people who change the
language EmbCC accepts. It says where the code is and how it works; the
language itself is documented in the user's manual
([C language](../manual/c-language.md), [C++](../manual/cxx.md),
[Extensions](../manual/extensions.md)).

## The stages

For a C file the driver (`compile_unit` in `src/driver/main.c`) runs:

```text
src_read ──► cpp_process ──► parse_unit ──► sema_check ──► irgen
   bytes      text with        struct unit    the same tree,
              line markers     (the AST)      typed and checked
```

For a C++ file one stage is inserted after the preprocessor:

```text
src_read ──► cpp_process ──► cxx_translate ──► parse_unit ──► sema_check ──► irgen
                              C++ in, C text out
```

The C++ front end parses and analyses C++ and writes C; the C front end
then compiles that C exactly as it compiles a C file. Everything after
`cxx_translate` is shared.

| Stage | Entry point | Directory |
|---|---|---|
| Source input | `src_read` | `src/platform` |
| Preprocessor | `cpp_process` | `src/cpp` |
| Lexer | `lex_init`, `lex_next` | `src/lex` |
| C++ front end | `cxx_translate` | `src/cxx` |
| Parser | `parse_unit` | `src/parse` |
| Semantic analysis | `sema_check` | `src/sema` |
| Type system | `ty_*` | `src/sema/type.c` |

The stages after preprocessing stop the compile when they report an
error: if `cx_nerrors`, `parse_error_count()` or `sema_error_count()` is
nonzero after its stage, the driver prints
`compilation terminated: N errors` and nothing downstream runs.

## Source input

Every source byte the front end reads goes through `src_read` in the
platform layer. By default it reads the file; a language server installs
its own provider with `src_set_provider` so that unsaved editor buffers
are compiled as they are. Both the main file and every `#include` are
read this way. The driver registers the main file's text with
`diag_register_source` so diagnostics can quote its lines, and the
preprocessor registers each header it opens.

## The preprocessor

`cpp_process(path, src, incdirs, nincdirs)` in `src/cpp/cpp.c` is a
text-to-text pass. It returns the expanded unit as one string in which
GNU line markers (`# LINE "FILE"`) record where each line came from; the
lexer reads the markers, so every token keeps the file and line the
programmer wrote. `embcc -E` prints this string.

### Structure

- `process_file` reads one file a logical line at a time
  (`read_logical_line` splices backslash-newlines and strips comments),
  keeps the conditional stack, and dispatches directives. A non-directive
  line in a live region is macro-expanded by `expand_text` and appended
  to the output.
- Macros are a linked list of `struct macro` (name, parameters, body,
  an `expanding` flag that stops self-reference, and a `builtin` flag
  for predefined ones). Function-like macros are expanded by
  `expand_funclike`, which collects arguments, substitutes them
  (`subst_body`, with `#` and `##`), handles `__VA_ARGS__` and
  `__VA_OPT__`, and rescans.
- `#if` expressions are evaluated by `eval_if` and a recursive-descent
  evaluator (`eval_or` and below) over `long` values, with `defined`,
  `__has_include`, `__has_include_next`, `__has_builtin`,
  `__has_attribute`, `__has_cpp_attribute`, `__has_c_attribute`,
  `__has_feature` and `__has_extension` (`eval_has`).
- `#include` and `#include_next` search the `-I` and system directories
  in order (`do_include`); `#include_next` resumes after the directory
  the current file was found in. `#embed` is handled by `do_embed`.
- `__FILE__` and `__LINE__` are expanded by the preprocessor itself.
  `__func__` and `__FUNCTION__` are names semantic analysis resolves.

Limits: 16 macro parameters (`MAX_MACRO_PARAMS`), 50 levels of
`#include` (`MAX_INCLUDE_DEPTH`) and 64 levels of conditionals
(`MAX_COND_DEPTH`).

### Directives

| Directive | Handling |
|---|---|
| `#define`, `#undef` | Macro table. |
| `#if`, `#ifdef`, `#ifndef`, `#elif`, `#elifdef`, `#elifndef`, `#else`, `#endif` | Conditional stack. |
| `#include`, `#include_next` | `do_include`. |
| `#embed` | `do_embed`. |
| `#line`, `# N "FILE"` | Rewrites the line marker. |
| `#error` | Fatal error with the message. |
| `#warning` | A warning; the compile continues. |
| `#pragma pack(...)`, `_Pragma("pack(...)")` | Forwarded to the parser as the marker `__embcc_pack(ARGS)`, which no program can declare. Refused in C++. |
| `#pragma weak NAME [= TARGET]` | Forwarded as `__embcc_weak(NAME[, TARGET])`; the parser records it in `unit->weaks` and sema applies it before merging declarations (`apply_pragma_weak`). Refused in C++. |
| `#pragma once` | Records the file's path and text in `cpp->once`; `do_include` skips a file that matches either. |
| `#pragma push_macro`, `pop_macro` | A stack of saved definitions in `cpp->pushed`. |
| any other `#pragma` | Discarded. |

`do_pragma` handles both the directive and the `_Pragma` operator.
`__COUNTER__`, `__DATE__`, `__TIME__`, `__FILE_NAME__`, `__BASE_FILE__`
and `__INCLUDE_LEVEL__` are computed at each use (`dynamic_macro`), as
`__FILE__` and `__LINE__` are.

### Predefined macros

`cpp_process` loads the selected target's table first (`predef_table`
in `src/arch/predef.c`), then defines its own:

- `__EMBCC__ 1`, `__STDC__ 1`, `__STDC_HOSTED__ 1`, and for C
  `__STDC_VERSION__ 201710L`;
- the GNU spellings real headers use (`__extension__`, `__restrict`,
  `__restrict__`, `__inline`, `__inline__`) as empty macros;
- for C++, `__cplusplus` for the `-std=` year, the `__cpp_*` feature
  macros for the features EmbCC implements, `__GNUC__ 16`,
  `__GNUG__ 16`, and `__EXCEPTIONS`/`__cpp_exceptions` unless
  `-fno-exceptions`.

Then the `-D` and `-U` options are applied in command-line order.

Each target has two generated tables, one for C (`predef.c`) and one
for C++ (`predef_cxx.c`), in `src/arch/<arch>/` (RISC-V's are in
`src/arch/riscv32/` and `src/arch/riscv64/`, ARMv8-M's in
`src/arch/thumbv8m/`). They are produced by `tools/gen-predef.sh` from a
reference compiler's `-dM -E` output and are not edited by hand.
`--dump-predef` prints the table for the target and options given.

### Dependencies

The preprocessor records every header it opens (`cpp_dep_count`,
`cpp_dep_path`, `cpp_dep_is_system`); the driver writes them for `-M`,
`-MM`, `-MD` and `-MMD`. Directories added with `-isystem`, and EmbCC's
own include directories, are system directories
(`cpp_set_system_dirs`): warnings inside their headers are suppressed
and `-MM` omits them.

## The lexer

`src/lex/lex.c` turns preprocessed text into tokens on demand: the
parser holds a `struct lexer` and calls `lex_next` to advance; the
current token is `lx.tok`. The parser saves and restores the whole
`struct lexer` by value when it needs to look further ahead.

A `struct token` carries its kind, `line`, and 1-based `col`, and for
literals the decoded value:

- integer constants: `num` and the type flags `num_long`, `num_llong`,
  `num_uns`, `char_lit`;
- floating constants: `fnum`, `fnum_is_float`, `fnum_is_ld` (with the
  digits kept in `text` for exact `long double` conversion) and
  `fnum_is_imag`;
- strings: the bytes in `text`, the element count in `num`,
  `str_width` (1, 2 or 4) and `str_prefix`.

String and character literals are decoded into `struct litch` elements
(`lit_decode`) and encoded at the literal's final width (`lit_encode`):
UTF-8 for narrow strings, UTF-16 for `u""`, UTF-32 for `L""` and `U""`.
Adjacent literals are concatenated as elements and encoded once, after
the widest prefix is known. `lit_char_value` gives a character
constant's value, reading a plain character as the target's plain
`char`.

Two keyword tables exist: `keywords[]` for C and `cxx_keywords[]`, which
`lex_init_mode(..., 1)` adds for C++. A line marker `# N "FILE"` changes
the lexer's file and line and produces no token. Any other `#` is
refused (`stray '#' (preprocessor directives are handled before the
lexer)`). Identifiers may contain the UTF-8 characters C11 Annex D
allows (`lex_ident_utf8`).

The lexer has no error recovery: a malformed token (an unterminated
comment, an empty character constant) is a fatal error.

## The parser

`parse_unit(file, src)` in `src/parse/parse.c` is a recursive-descent
parser for C with GNU extensions. It returns a `struct unit`; see
[The AST](#the-ast).

### Organization

- **Declarations.** `parse_top` handles one external declaration.
  Declaration specifiers are read by `parse_type_spec`; declarators by
  `declarator`, `parse_stars`, `parse_array_dims` and
  `parse_fn_params`; `struct`, `union` and `enum` bodies by
  `parse_tagged`, `parse_struct_body` and `parse_enum_body`, which lay
  out the type as soon as its body is read (`ty_struct_layout`).
  Initializers are parsed by `parse_initializer`, with designators and
  brace elision (`elide_walk`).
- **The typedef ambiguity.** Typedef names and tags are known during
  parsing: the parser keeps the tables (`find_typedef`, `find_tag`) and
  returns them in `unit->typedefs` and `unit->tags`.
- **Expressions.** `parse_expr` descends through `parse_comma`,
  `parse_cond`, `parse_level` (the binary-operator precedence table),
  `parse_unary`, `parse_postfix` and `parse_primary`. Compound
  assignment `a op= b` is built as `EXPR_COMPOUND`.
- **Statements.** `parse_stmt` and `parse_block`. `case` and `default`
  are markers in the statement list of the `switch` body, not nested
  nodes.
- **Constant expressions during parsing.** Array bounds, bit-field
  widths, enumerator values and `_Static_assert` are folded in the parser
  (`size_fold`), because types depend on them.
- **Attributes.** `parse_attributes` reads `__attribute__((...))` and
  `[[...]]`. Each attribute name has an entry in `attr_table` with a
  disposition: `ATTR_HONOURED` (recorded and acted on), `ATTR_REFUSED`
  (an error naming the attribute, because ignoring it would change the
  generated code), `ATTR_NOOP` (accepted; there is nothing for it to do
  in EmbCC) or `ATTR_WARNED` (accepted, with a warning that what it asks
  for is not done). A name not in the table is warned about under
  `-Wattributes` and ignored. To add an attribute, add a table entry and
  a field in `struct attrs`, and carry it onto the AST node.
- **EmbCC's own forms.** The parser accepts constructs only the C++
  lowering writes: `__builtin_eh_region { ... } __builtin_eh_landing
  (exc, sel, actions...) { pad }` (`STMT_EHREGION`),
  `__builtin_eh_typeid(ti)` and `__attribute__((embcc_sret))` on a
  parameter, and the `__embcc_pack(...)` marker the preprocessor writes
  for `#pragma pack`.

### The AST

The tree is defined in `src/parse/ast.h`.

| Structure | Meaning |
|---|---|
| `struct unit` | The translation unit: lists of functions, globals, enumerators and file-scope `asm` blocks, plus the tags and typedefs the parser saw. |
| `struct func` | One function declaration or definition. Later declarations are merged into the first (canonical) node during semantic analysis and marked `absorbed`. Holds the parameters, attributes, the body, and fields later stages fill (`nvars`, `var_tys`, `code_off`, `code_len`, `stack_bytes`, unwind facts). |
| `struct global` | One file-scope object, merged the same way. Holds the type, linkage and attributes, and after semantic analysis its initializer as a byte image (`init_bytes`) plus relocations (`relocs`, `struct greloc`). The driver fills its section placement. |
| `struct stmt` | A statement (`enum stmt_kind`). |
| `struct expr` | An expression (`enum expr_kind`). Semantic analysis sets `ty` on every node. |
| `struct initelem` | One leaf of an aggregate initializer, flattened by semantic analysis to (offset, type, value), with bit-field position. |
| `struct asm_stmt`, `struct asm_operand` | Extended `asm`: template, outputs, inputs and clobbers. Semantic analysis resolves each operand's register. |
| `struct topasm` | A file-scope `__asm__` block (or a naked function's body), assembled by `src/arch/x86_64/topasm.c` on x86-64 and AArch64 and by `src/as/gas.c` on the embedded targets. |
| `struct econst`, `struct tagdef`, `struct typedefent` | Enumerators, tags and typedefs. |

Every node carries `line` and `col`; `struct func` and `struct global`
also carry the `file` of their declaration, which for a declaration in a
header is the header.

Binary operators are `enum binop`. `B_LAND` and `B_LOR` never reach code
generation as operators: IR generation lowers them to branches.

`embcc inspect tokens`, `inspect ast`, `inspect symbols` and
`inspect types` print the token stream, the checked tree, the unit's
declarations and the layout of its structures
(`src/driver/inspect.c`).

## Semantic analysis

`sema_check(u)` in `src/sema/sema.c` resolves names, checks the unit,
and rewrites the tree so that IR generation does not have to make
decisions. Its output contract:

- every expression node has a type (`ty`);
- every implicit conversion C performs is an explicit `EXPR_CAST` node,
  so IR generation never infers a width or signedness;
- every variable reference has a frame-slot index (`var_index`) or a
  global (`gref`), and every direct call its `callee`;
- every function has `nvars` and the type of each frame slot
  (`var_tys`);
- every aggregate initializer is flattened to `struct initelem` leaves,
  and every static initializer is lowered to a byte image with
  relocations.

### Order of work

1. `merge_decls` and `merge_globals` merge every later declaration of a
   name into its first node, checking that the declarations agree and
   applying C's linkage rules. `check_aliases` checks `alias`
   attributes.
2. `lower_globals` evaluates static initializers into `init_bytes` and
   `relocs`.
3. Each function definition is checked in source order by `check_func`:
   parameters enter the scope, the body is checked by `check_stmt` and
   `check_expr`, then the function gets its slot table. Checking in
   source order keeps C's rule that a name is usable only after its
   declaration.
4. `-Wunused-function` is reported, and a `static` function that is
   called but never defined is an error.

### Scopes and frame slots

A function's scope (`struct scope`) is an array of `struct vardef`
entries that are never removed: an entry's index is the variable's
frame slot for the whole function. When a block ends its entries are
deactivated, and lookup runs backward so an inner declaration shadows
an outer one. Semantic analysis also creates hidden slots (compound
literals, the size of a VLA, the saved stack pointer of a VLA); their
names begin with `<` and are never reported as unused.

A `static` local becomes a global of its own and keeps a slot index
that has no storage. A local whose alignment exceeds what the stack
guarantees (`target_stack_align()`) is given indirect storage
(`var_indirect`): its slot holds a pointer to suitably aligned memory.

### What else is here

| File or function | Responsibility |
|---|---|
| `const_fold`, `const_fold128`, `const_fold_f`, `const_fold_ld` | Constant expressions: integer, `__int128` (with `w128.c`), floating and `long double`. |
| `src/sema/ldfloat.c` | Exact `long double` constants in the target's format (x87 80-bit or binary128), independent of the host's `long double`. |
| `src/sema/w128.c` | 128-bit integer arithmetic on two 64-bit halves, so EmbCC's own source needs no `__int128`. |
| `cx_*` in `sema.c` | `_Complex` arithmetic, lowered to operations on the real and imaginary parts; complex `*` and `/` call `__mulXc3` and `__divXc3`. |
| `check_atomic_call`, `atomic_builtin` | The `__atomic_*` and `__sync_*` builtins. `atomic_builtin` classifies a name once for both semantic analysis and IR generation. |
| `builtin_bitop`, `sema_has_builtin` | The bit builtins, and the answer `__has_builtin` gives in C. |
| `asm_resolve_reg*` | Extended `asm` constraints to registers, per target. |
| `src/sema/format.c` | `-Wformat` for functions with a `format` attribute. |
| `src/sema/uninit.c` | `-Wuninitialized` and `-Wmaybe-uninitialized`, a data-flow walk over each function body. |

## The type system

Types are `struct type` (`src/sema/type.h`). The kinds are
`TY_VOID`, `TY_BOOL`, `TY_CHAR`, `TY_SHORT`, `TY_INT`, `TY_LONG`,
`TY_FLOAT`, `TY_DOUBLE`, `TY_LDOUBLE`, `TY_INT128`, `TY_PTR`,
`TY_ARRAY`, `TY_STRUCT` and `TY_FUNC`.

- `long long` is `TY_LONG` with `is_llong` set. `ty_equal` treats
  `long` and `long long` as the same type when they have the same size,
  and as different types when they do not (ILP32 and AVR).
- Unions are `TY_STRUCT` with `is_union`. `_Complex T` is a `TY_STRUCT`
  with `is_complex`, laid out as `struct { T __real__, __imag__; }`;
  code that handles aggregates must exclude it where it treats it as
  arithmetic. `ty_complex` returns one node per element type.
- `volatile` and `_Atomic` are flags on a copy of the type
  (`ty_volatile`, `ty_atomic`); `canon` points from a copy to the
  original, and structure equality compares canonical nodes. `const` is
  not part of `struct type`: the parser records whether a declared
  object is `const`, which decides its placement.
- A VLA is a `TY_ARRAY` with `vla_len` set and `count` 0; `vla_size`
  is the hidden slot that holds its size at run time. `ty_size` of a VLA
  is 0. `ty_is_vm` tests for a variably modified type.
- The basic types are interned (`ty_base`); pointer and array types are
  allocated per use. Each structure tag has one node, completed in place
  by `ty_struct_layout`.

### Sizes and the data model

Sizes come from the target's data model in `src/arch/target.c`, never
from the host: `ty_size` asks `target_int_size`, `target_long_size`,
`target_double_size`, `target_ldouble_size` and `target_ptr_size`.
`ty_align` is a scalar's size, capped by `target_max_scalar_align()` (1
on AVR); an aggregate's alignment is its strictest member's.
`ty_int_of_size(n, uns)` returns the integer type of exactly `n` bytes,
or `NULL` where the target has none (16 bytes without `__int128`).

| Target | `int` | `long` | pointer | `double` | `long double` | plain `char` |
|---|---|---|---|---|---|---|
| x86-64 | 4 | 8 | 8 | 8 | 16 (x87 80-bit) | signed |
| AArch64 | 4 | 8 | 8 | 8 | 16 (binary128) | unsigned |
| AArch64 Darwin | 4 | 8 | 8 | 8 | 8 | signed |
| ARMv7-M, ARMv8-M | 4 | 4 | 4 | 8 | 8 | unsigned |
| RV32 | 4 | 4 | 4 | 8 | 16 (binary128) | unsigned |
| RV64 | 4 | 8 | 8 | 8 | 16 (binary128) | unsigned |
| AVR | 2 | 4 | 2 | 4 | 4 | signed |

`-fsigned-char` and `-funsigned-char` override the `char` column
(`target_set_char_signed`). `ty_is_xldouble` is true only where
`long double` is wider than `double`; elsewhere `long double` is lowered
as the type it shares a format with while remaining a distinct C type.

### Structure layout

`ty_struct_layout(t, members, n, packed, user_align, pack)` assigns
member offsets, the size and the alignment, following GCC's layout:

- each member is placed at its alignment; `packed` makes every member's
  alignment 1; a member's own `aligned(N)` raises it even inside a
  packed structure; `pack` (`#pragma pack(N)`) caps every member's
  alignment at `N`;
- bit-fields share a running bit position; a bit-field does not span
  more alignment units of its type than the type itself occupies
  (GCC's `excess_unit_span`), and a zero-width bit-field rounds up to
  the next unit; in a packed structure a bit-field may cross its unit,
  and `bf_bytes` then gives the number of bytes it spans;
- an unnamed bit-field raises the structure's alignment only where
  `target_anon_bitfield_aligns()` says so (AAPCS and AAPCS64);
- `nat_align` records the alignment the members give, before the
  structure's own `aligned(N)`; the size is rounded up to the final
  alignment.

`embcc inspect types FILE` prints the result.

### Classification helpers

These answer ABI questions from a type. IR generation calls them and
records the answers on `IR_CALL` arguments and on the function, so
backends never walk types (see [EmbIR](ir.md#call-arguments-struct-ir_arg)).

| Function | Answers |
|---|---|
| `ty_classify(t, cls)` | System V x86-64 classification: the number of eightbytes (1 or 2) with `CLASS_INTEGER`, `CLASS_SSE` or `CLASS_NONE` (padding only) for each, or 0 for MEMORY. A structure over 16 bytes, one with an unaligned field, or one containing a `long double` wider than `double` is MEMORY. `__int128` is two INTEGER eightbytes. |
| `ty_hfa(t, &esz)` | AAPCS64 homogeneous floating-point aggregate: 1 to 4 members of one floating type (no bit-fields), returned with the member size, or 0. |
| `ty_aapcs64_byref(t)` | A composite over 16 bytes that is not an HFA: passed as a pointer to a copy, returned through `x8`. |
| `ty_natural_align(t)` | The ARM procedure-call standards' natural alignment: a structure's `nat_align`, an array's element's, otherwise `ty_align`. |
| `ty_x87_struct(t)`, `ty_x87_ret(t)` | x86-64: a structure that is exactly one `long double` (returned in `st0`), and a `long double _Complex` (returned in `st0`/`st1`). |
| `ty_promote`, `ty_arith_common` | The integer promotions and the usual arithmetic conversions, shared by the parser (for `typeof`) and semantic analysis. They follow the target's sizes, so on AVR `unsigned short` promotes to `unsigned int`, and on ILP32 `long` with `unsigned int` gives `unsigned long`. |
| `ty_name` | A type's spelling for diagnostics (four rotating buffers, so one message can name two types). |

## The C++ front end

`src/cxx` implements C++ by lowering it to C (decision D-013). Its one
public entry point is `cxx_translate(file, src)` in
`src/cxx/translate.h`: preprocessed C++ in, C text out. Everything else
is internal and declared in `src/cxx/cxx.h`.

```text
cx_tokenize ──► cx_parse_unit ──► cx_emit_unit
 tok.c           parse.c, expr.c,   emit.c, mangle.c
                 class.c, template.c, ...
```

1. `cx_tokenize` (`tok.c`) lexes the whole unit up front with the C
   lexer in C++ mode, into the array `cx_toks`, each token with its
   file. The parser needs the whole array: member function bodies
   defined inside a class are parsed after the class is complete, and
   templates are instantiated by replaying their tokens.
2. `cx_parse_unit` parses with semantic analysis interleaved, because
   in C++ whether a name is a type decides how the following tokens
   parse. Declarations are entered into their scope as soon as their
   declarator is read.
3. `cx_emit_unit` (`emit.c`) writes the typed trees as C. Only what is
   used is written: inline functions, implicit special members and
   internal variables are emitted from a worklist when something emitted
   refers to them. Line markers in the output point diagnostics from the
   C front end back at the C++ source. `embcc --emit-c FILE.cc` prints
   the result.

| File | Responsibility |
|---|---|
| `tok.c` | The token array, the cursor (`cx_pos`), diagnostics at a token (`cx_error`). |
| `type.c` | C++ types (`struct cty`): construction, identity, sizes, promotions. |
| `scope.c` | Scopes (`struct cscope`) and name lookup; symbols (`struct csym`) in one hash table keyed by scope and name. |
| `parse.c` | Declarations and statements. |
| `expr.c` | Expressions, implicit conversion sequences, overload resolution, initialization. |
| `class.c` | Class layout, special members, construction. |
| `vtable.c` | Virtual tables, virtual bases, VTTs, in g++'s order. |
| `template.c` | Templates: instantiation by replaying tokens with the parameters bound, argument deduction, SFINAE. |
| `concepts.c` | Requires-clauses, requires-expressions and concepts. |
| `consteval.c` | Constant evaluation: an interpreter over the front end's trees. |
| `traits.c` | The type-trait intrinsics libstdc++ uses (`__is_class` and the rest). |
| `coro.c` | Coroutines. |
| `access.c` | Access control (`private`, `protected`, friends); `-fno-access-control` turns it off. |
| `mangle.c` | Itanium C++ ABI name mangling. |
| `emit.c` | The C output. |

C++ types are separate from C's: `struct cty` keeps apart what C does
not (`char`, `signed char` and `unsigned char`; `long` and `long long`;
references; `bool`; `nullptr_t`; classes with members), and only
`emit.c` turns them into C types. The main structures are `struct cfunc`
(a function), `struct cclass`, `struct ctemplate`, `struct cexpr` and
`struct cstmt`.

How C++ constructs reach C:

- a reference is a pointer, dereferenced at each use; a member function
  takes `this` first; constructors and destructors are functions called
  on the object's address (complete-object and base-object variants);
- temporaries are locals declared at their full-expression and
  destroyed at its end, or at the end of their block when bound to a
  local reference; every exit from a scope destroys what the scope
  constructed;
- a namespace-scope object with a dynamic initializer is initialized by
  one function per unit placed in `.init_array`, with its destructor
  registered through `__cxa_atexit`;
- `try` and `catch` become `__builtin_eh_region` statements whose landing
  pads compare the selector against `__builtin_eh_typeid(...)` and call
  the `__cxa_*` runtime; the C front end turns them into IR exception
  regions;
- inline functions and template instances are emitted with
  `__attribute__((weak))`.

The C++ front end's `ct_size` and `ct_align` use LP64 sizes on every
target. On ARMv7-M, RV32 and AVR, `sizeof` evaluated in C++ code
therefore gives LP64 answers (`sizeof(long)` is 8), and exception
handling fails in the C front end with
`a landing pad's selector must be a long lvalue`.

`__has_builtin` in a C++ unit is answered by `cxx_has_builtin`. The
preprocessor is told it is preprocessing C++ (`cpp_set_cxx`,
`predef_set_cxx`) so that it loads the C++ predefined-macro table.

## Errors and recovery

Diagnostics are records built through the API in `src/driver/util.h`
and rendered once by `src/driver/diag.c`, as caret text or as JSON. A
diagnostic may carry notes, a source range, fix-its (`diag_fixit_at`)
and an explain id (`diag_set_id("E0002")`), which `embcc --explain`
looks up. A warning that an option controls is raised with
`diag_warn_opt(file, line, col, NAME, ...)`; `NAME` must be an entry in
`g_warns[]` in `diag.c`.

No stage calls `exit`. A fatal error unwinds to the boundary the driver
installs with `fatal_set_boundary` (a `setjmp`), and the driver returns
1 for the unit. `diag_fatal` reports and unwinds; `fatal_unwind` unwinds
after a diagnostic that is already recorded; `internal_error` reports a
compiler bug.

Each stage recovers differently:

| Stage | Recovery |
|---|---|
| Preprocessor | None. An error (`#error`, a missing header, a malformed directive) is fatal. |
| Lexer | None. A malformed token is fatal. |
| C++ front end | `cx_error` unwinds to the statement being parsed in a block, or to the declaration being parsed at namespace scope, and parsing resumes after it (`cx_resync`). Once parsing is over (instantiation, emission) an error is fatal. |
| Parser | `parse_error_at` records the error and unwinds to a recovery point: the statement being parsed in a block, or the external declaration at file scope. `resync` then skips past the next `;` at the same brace depth, or to the closing `}`. A missing `;` is not unwound at all: the parser reports it with a fix-it inserting `;` (id `E0002`) and continues as if it were there. |
| Semantic analysis | Each statement in a block is a recovery point (`check_stmt`): an error is reported and checking continues with the next statement, keeping what the failed statement declared. A name reported undeclared once is not reported again in the same function. Conflicting declarations and redefinitions are fatal. |

After a stage that recovers, the driver checks its error count and stops
before the next stage, so a later stage never runs on a tree with
errors. `-fmax-errors=N` stops after `N` errors.

## Where to make common changes

| Change | Where |
|---|---|
| A new keyword | `keywords[]` or `cxx_keywords[]` in `src/lex/lex.c`, a `TOK_KW_` value in `lex.h`, and the parser. |
| A new attribute | `attr_table` and `struct attrs` in `src/parse/parse.c`; the AST field it sets; the stage that acts on it. |
| A new builtin function | The name in `sema_has_builtin`'s list or one of the derived families (`atomic_builtin`, `builtin_bitop`), its typing in `check_expr`, and its lowering in `src/ir/irgen.c`. |
| A new warning | An entry in `g_warns[]` in `src/driver/diag.c`, and a `diag_warn_opt` call. |
| A new predefined macro | Regenerate the table with `tools/gen-predef.sh`, or define it in `cpp_process` if it describes EmbCC rather than the target. |
| A target data-model question | A function in `src/arch/target.c` with a row in `g_model[]`, never a test of the architecture at the use site. |
