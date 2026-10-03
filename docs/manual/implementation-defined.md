# C implementation-defined behavior

The C standard leaves a number of choices to each implementation and
requires the implementation to document them. This page records EmbCC's
choice for each one, in the order of C17 Annex J.3, per target where the
targets differ. It is for programmers who need to know exactly what a
construct does under EmbCC: porting code from GCC or Clang, sharing data
layouts with other compilers, or writing code for a particular board.
Calling conventions and object formats are in [Targets](targets.md);
library behavior in general is in [Libraries](libraries.md).

Each entry quotes the item from C17 Annex J.3 in italics, with the
clause it comes from, and then gives EmbCC's answer.

## Targets on this page

Where an answer differs by target, the columns and names below are used.
Every triple each name covers is listed in [Targets](targets.md).

| Name | Triples | Data model |
|---|---|---|
| x86-64 | `x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu` | LP64 |
| Apple arm64 | `aarch64-apple-darwin` | LP64, Apple's variant |
| AArch64 | `aarch64-elf`, `aarch64-emblink`, `aarch64-linux-gnu` | LP64 |
| Cortex-M | `thumbv7m-none-eabi`, `thumbv7em-none-eabi[hf]`, `thumbv8m.main-none-eabi[hf]` | ILP32 |
| RV32 | `riscv32-unknown-elf` | ILP32 |
| RV64 | `riscv64-unknown-elf` | LP64 |
| AVR | `avr` | 16-bit `int` and pointers |

`x86_64-apple-darwin` has the same answers as x86-64 throughout this
page. `x86_64-windows-gnu` uses the x86-64 data model, which is not
Microsoft's; see [Windows](targets.md#windows-coff).

## Translation

*How a diagnostic is identified (C17 3.10, 5.1.1.3).*

A diagnostic is a message on standard error that begins with `embcc:`,
names the file and line (and usually the column), and gives a severity
(`error`, `warning` or `note`). Some end with an identifier in brackets,
such as `[E0008]`, that `embcc --explain` describes. The format,
the machine-readable form and the exit status are described in
[Diagnostics](diagnostics.md#the-diagnostic-format).

*Whether each nonempty sequence of white-space characters other than
new-line is retained or replaced by one space character in translation
phase 3 (C17 5.1.1.2).*

Retained. A comment is replaced by one space. The difference is visible
only in the output of `-E`, which keeps the source's spacing.

## Environment

*The mapping between physical source file multibyte characters and the
source character set in translation phase 1 (C17 5.1.1.2).*

Source files are read as UTF-8. A UTF-8 byte-order mark at the start of a
file is skipped. A byte that does not begin a valid UTF-8 sequence is
taken as itself inside a string literal or character constant (see
[Characters](#characters)); elsewhere it is an error.

*The name and type of the function called at program startup in a
freestanding environment (C17 5.1.2.1).*

EmbCC supplies no startup code for the bare-metal targets (Cortex-M,
RV32, RV64, AVR, and `x86_64-elf`/`aarch64-elf` used for a kernel).
Execution begins at the entry symbol given to the linker (`embld -e SYM`,
`_start` by default), and the program's own startup code calls whatever
it chooses, conventionally `int main(void)`. See
[Startup code](embedded.md#startup-code-and-the-linkers-symbols).

*The effect of program termination in a freestanding environment
(C17 5.1.2.1).*

Whatever the program's startup code does when the function it called
returns. EmbCC adds nothing.

*An alternative manner in which the main function may be defined
(C17 5.1.2.2.1).*

`main` is compiled as an ordinary function. Besides the two standard
forms, `int main(int, char **, char **)` and `void main(void)` are
accepted without a diagnostic.

Reaching the closing brace of `main` returns 0, as C99 and later
specify. Every other function that returns a value must end each path
with a `return` statement; see
[Functions that can reach their closing brace](c-language.md#functions-that-can-reach-their-closing-brace).

*What constitutes an interactive device (C17 5.1.2.3).*

With EmbCC's C library, `stdout` is line-buffered when the operating
system backend reports that it is a terminal, and fully buffered
otherwise; `stderr` is unbuffered. See
[Libraries](libraries.md#behavior-worth-knowing).

*Whether a program can have more than one thread of execution in a
freestanding environment (C17 5.1.2.4).*

`thrd_create` creates threads on the Linux and EmbLinkOS targets and
returns `thrd_error` on every other target. A bare-metal program that
runs code from interrupt handlers does so outside the C threads model.

*The set of signals, their semantics, and their default handling;
signal values that correspond to a computational exception; signals
ignored at program startup (C17 7.14, 7.14.1.1).*

EmbCC's C library implements the C model only: `raise` calls the handler
directly, and nothing delivers a signal from outside the program. See
[Libraries](libraries.md#behavior-worth-knowing).

*The set of environment names and the method for altering the
environment list used by the `getenv` function (C17 7.22.4.6).*

`getenv` reads the `environ` vector that the startup code publishes.
There is no method for altering it: `setenv` and `unsetenv` are declared
but not defined.

*The manner of execution of the string by the `system` function
(C17 7.22.4.8).*

`system` never runs a command. `system(NULL)` returns 0, reporting that
no command processor is available; with a command it returns −1.

## Identifiers

*Which additional multibyte characters may appear in identifiers and
their correspondence to universal character names (C17 6.4.2).*

The characters listed in C11 Annex D, written in UTF-8, may appear in an
identifier; the combining marks Annex D excludes from the first position
may not begin one. Such an identifier is written to the object file as
its UTF-8 bytes:

```c
int café = 3;           /* the symbol is the five bytes "caf\xc3\xa9" */
```

A byte that is not part of an allowed character is an error:

```text
embcc: f.c:1: error: byte 0xcc is not part of a character C allows here (identifiers take UTF-8 letters, C11 Annex D)
```

Universal character names (`é`) are not accepted in identifiers, and
neither is `$`:

```text
embcc: f.c:1: error: character '\' is not supported yet
embcc: f.c:1: error: character '$' is not supported yet
```

*The number of significant initial characters in an identifier
(C17 5.2.4.1, 6.4.2).*

All characters are significant, in internal and external names alike.
There is no length limit.

## Characters

*The number of bits in a byte (C17 3.6).*

8, on every target. `CHAR_BIT` is 8.

*The values of the members of the execution character set (C17 5.2.1).*

The execution character set is UTF-8. The basic execution character set
has its ASCII values.

*The unique value of the member of the execution character set produced
for each of the standard alphabetic escape sequences (C17 5.2.2).*

| Escape | Value |
|---|---|
| `\a` | 7 |
| `\b` | 8 |
| `\f` | 12 |
| `\n` | 10 |
| `\r` | 13 |
| `\t` | 9 |
| `\v` | 11 |

The GNU escape `\e` is also accepted and has the value 27.

*The value of a `char` object into which has been stored any character
other than a member of the basic execution character set (C17 6.2.5).*

The byte stored, interpreted as plain `char`. A character outside ASCII
occupies more than one `char` in UTF-8, so it cannot be stored in one
`char` object.

*Which of `signed char` or `unsigned char` has the same range,
representation, and behavior as "plain" `char` (C17 6.2.5, 6.3.1.1).*

| x86-64 | Apple arm64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|
| `signed char` | `signed char` | `unsigned char` | `unsigned char` | `unsigned char` | `unsigned char` | `signed char` |

`-funsigned-char` and `-fsigned-char` override the default for the whole
translation unit, and `__CHAR_UNSIGNED__` is defined exactly when plain
`char` is unsigned (see
[Invoking EmbCC](invoking.md#-fsigned-char--funsigned-char)). The choice
also decides the value of character constants (below), of `#if`
expressions that use them, and the signedness of plain `char`
bit-fields.

AVR's default matches `clang --target=avr`. avr-gcc's documented default
is unsigned; pass `-funsigned-char` to match avr-gcc.

<!-- UNVERIFIED: the avr-gcc default is from avr-gcc's documentation as
     quoted in src/arch/target.c; there is no avr-gcc here to check it.
     Note src/arch/target.h's comment on TARGET_AVR still says char is
     UNSIGNED by default, which the data model in target.c contradicts. -->

*The mapping of members of the source character set (in character
constants and string literals) to members of the execution character set
(C17 6.4.4.4, 5.1.1.2).*

The identity. A UTF-8 character in a plain or `u8` string literal is
stored as the same UTF-8 bytes. A source byte that is not valid UTF-8 is
stored unchanged.

*The value of an integer character constant containing more than one
character or containing a character or escape sequence that does not map
to a single-byte execution character (C17 6.4.4.4).*

A character constant is one byte, read as plain `char`: `'\377'` is −1
where plain `char` is signed and 255 where it is unsigned.

A constant of more than one character is refused, and so is a single
character that takes more than one byte in UTF-8:

```text
embcc: f.c:1: error: a character constant holds one character (multi-character constants are not supported)
embcc: f.c:1: error: character U+00E9 does not fit in one byte; write it as a wide constant (L'...')
```

An octal or hexadecimal escape whose value does not fit in a byte keeps
its low 8 bits, with a warning:

```text
embcc: f.c:1: warning: escape sequence out of range for a character constant; truncated, as gcc does
```

The same truncation, with a corresponding warning, applies to an escape
in a string literal whose value does not fit the literal's element.

*The value of a wide character constant containing more than one
multibyte character or a single multibyte character that maps to
multiple members of the extended execution character set, or containing
a multibyte character or escape sequence not represented in the extended
execution character set (C17 6.4.4.4).*

A wide character constant with more than one character (`L'ab'`) is
refused with the multi-character error above. An `L'...'` or `U'...'`
constant has the code point of its character as its value. A `u'...'`
constant whose character needs a UTF-16 surrogate pair is refused:

```text
embcc: f.c:1: error: U+1F600 needs two UTF-16 code units and cannot be one u'' constant
```

An escape too large for the constant's type keeps its low bits, with a
warning.

*The current locale used to convert a wide character constant or a wide
string literal into wide character codes (C17 6.4.4.4, 6.4.5).*

No locale is consulted. The source is decoded as UTF-8 and each
character's code point is the wide character code.

*Whether differently-prefixed wide string literal tokens can be
concatenated and, if so, the treatment of the resulting multibyte
character sequence (C17 6.4.5).*

An unprefixed literal concatenated with a prefixed one takes the prefix
(`L"a" "b"` is `L"ab"`). Two different prefixes are refused:

```text
embcc: f.c:1:16: error: concatenating u"" and L"" literals is not supported
```

*The value of a string literal containing a multibyte character or
escape sequence not represented in the execution character set
(C17 6.4.5).*

Every Unicode character is represented. A universal character name is
encoded at the literal's width: UTF-8 in a plain or `u8` literal, UTF-16
(with surrogate pairs) in a `u` literal, UTF-32 in a `U` or `L` literal.
An out-of-range numeric escape is truncated as described above.

*The encoding of any of `wchar_t`, `char16_t`, and `char32_t` where the
corresponding standard encoding macro (`__STDC_ISO_10646__`,
`__STDC_UTF_16__`, or `__STDC_UTF_32__`) is not defined (C17 6.10.8.2).*

None of the three macros is defined. `char16_t` holds UTF-16 code units,
`char32_t` holds UTF-32, and `wchar_t` holds UTF-32 code points.

| Type | x86-64 | Apple arm64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|---|
| `wchar_t` | `int` | `int` | `unsigned int` | `unsigned int` | `int` | `int` | `int` (2 bytes) |
| `wint_t` | `unsigned int` | `int` | `unsigned int` | `int` | `unsigned int` | `unsigned int` | `int` (2 bytes) |
| `char16_t` | `unsigned short` | `unsigned short` | `unsigned short` | `unsigned short` | `unsigned short` | `unsigned short` | `unsigned int` (2 bytes) |
| `char32_t` | `unsigned int` | `unsigned int` | `unsigned int` | `unsigned int` | `unsigned int` | `unsigned int` | `unsigned long` (4 bytes) |

A wide string literal's elements have the type its prefix names: `L`
`wchar_t` (two bytes on AVR), `u` `char16_t` and `U` `char32_t`. A
character above U+FFFF in a two-byte literal (`u""`, and `L""` on AVR)
is written as a UTF-16 surrogate pair.

## Integers

*Any extended integer types that exist in the implementation
(C17 6.2.5).*

`__int128` and `unsigned __int128` (also spelled `__int128_t` and
`__uint128_t`), 16 bytes with 16-byte alignment, on x86-64, Apple arm64,
AArch64 and RV64. On RV64 the type can be declared and measured with
`sizeof`, but no operation on a value of it compiles:

```text
embcc: f.c:1: error: the RV64 backend cannot lower a 128-bit value yet (function f) [ldvar w=16 size=16]
```

On Cortex-M, RV32 and AVR the type does not exist:

```text
embcc: f.c:1:10: error: __int128 does not exist on this target (it needs 64-bit registers; use long long)
```

C23's `_BitInt(N)` is not supported.

*Whether signed integer types are represented using sign and magnitude,
two's complement, or ones' complement, and whether the extraordinary
value is a trap representation or an ordinary value (C17 6.2.6.2).*

Two's complement, with no padding bits. The value with only the sign bit
set (`INT_MIN` and its counterparts) is an ordinary value. No integer
type has trap representations.

*The rank of any extended integer type relative to another extended
integer type with the same precision (C17 6.3.1.1).*

There is only one extended integer type of each precision.

*The result of, or the signal raised by, converting an integer to a
signed integer type when the value cannot be represented in an object of
that type (C17 6.3.1.3).*

The value is reduced modulo 2^N, where N is the width of the type; no
signal is raised. `(signed char)200` is −56 and `(short)0x18000` is
−32768. The same rule applies in constant expressions.

*The results of some bitwise operations on signed integers (C17 6.5).*

The bitwise operators act on the two's complement representation. A
right shift of a negative value is arithmetic: the sign bit is copied
into the vacated positions, so `-17 >> 2` is −5 and `-1 >> 31` is −1, on
every target and in constant expressions.

Shifts by a negative count or by the width of the type or more, and left
shifts of negative values, are undefined behavior, not
implementation-defined. `-fsanitize=shift` traps on a count that is out
of range; a left shift of a negative value is not checked (see
[Run-time checks](optimization.md#run-time-checks--fsanitize)).

### Signed integer overflow

Signed overflow is undefined behavior in C. EmbCC defines it: signed
arithmetic wraps in two's complement, and no optimization at any level
assumes that overflow cannot happen. EmbCC therefore always behaves as
GCC and Clang do with `-fwrapv`. For example, at `-O2`:

```c
int inc(int x) { return x + 1; }        /* inc(INT_MAX) == INT_MIN */
int gt(int x)  { return x + 1 > x; }    /* gt(INT_MAX) == 0; not folded to 1 */
```

`-fwrapv` is accepted and changes nothing. `-fno-wrapv`, `-ftrapv`,
`-fstrict-overflow` and `-fno-strict-overflow` are not accepted. To find
overflow at run time, use `-fsanitize=signed-integer-overflow`. See
[Optimization](optimization.md#signed-integer-overflow).

Wrapping applies on every target at the width of the type. On AVR,
where `int` is 16 bits, `int` and `unsigned int` arithmetic wraps at 16
bits inside an expression, not only when the result is stored:

```c
unsigned half(unsigned x) { return (x + 1) / 2; }   /* half(0xFFFF) == 0 */
long next(int x)          { return x + 1; }         /* next(32767) == -32768 */
```

## Floating point

EmbCC's floating types use the IEEE 754 binary formats:

| Type | x86-64 | Apple arm64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|---|
| `float` | binary32 | binary32 | binary32 | binary32 | binary32 | binary32 | binary32 |
| `double` | binary64 | binary64 | binary64 | binary64 | binary64 | binary64 | binary32 |
| `long double` | x87 80-bit extended | binary64 | binary128 | binary64 | binary128 | binary128 | binary32 |
| Arithmetic | SSE2; x87 for `long double` | FP/SIMD | FP/SIMD; `long double` in software | software, or single-precision VFP with `-mfpu=` | software | software | software |

On RV32 and RV64, `long double` has the binary128 size and format, and
constant expressions of that type are evaluated during translation, but
no run-time operation on a `long double` value compiles (the same
`cannot lower a 128-bit value yet` error as for `__int128`).

*The accuracy of the floating-point operations and of the library
functions in `<math.h>` and `<complex.h>` that return floating-point
results (C17 5.2.4.2.2).*

The arithmetic operators and conversions are correctly rounded IEEE 754
operations, in hardware or in the compiler runtime (`lib/rt`), which
rounds to nearest, ties to even, and supports subnormals. The accuracy of
the library functions is given in
[Libraries](libraries.md#behavior-worth-knowing).

*The accuracy of the conversions between floating-point internal
representations and string representations performed by the library
functions in `<stdio.h>`, `<stdlib.h>`, and `<wchar.h>` (C17 7.21.6,
7.22.1.3, 7.29.4.1.1).*

`printf` converts exactly and rounds correctly; `strtod` is exact for
most inputs and otherwise within one unit in the last place. Details are
in [Libraries](libraries.md#behavior-worth-knowing).

*The rounding behaviors characterized by non-standard values of
`FLT_ROUNDS` (C17 5.2.4.2.2).*

There are none. `FLT_ROUNDS` is the constant 1 (to nearest). EmbCC never
changes the rounding mode, and `FLT_ROUNDS` does not follow a change
made with `fesetround`. The C library supports `fesetround` on x86-64
and AArch64 only.

*The evaluation methods characterized by non-standard negative values of
`FLT_EVAL_METHOD` (C17 5.2.4.2.2).*

There are none. Every operation is evaluated in the type of its operands:
EmbCC uses SSE2, not the x87 unit, for `float` and `double` on x86-64.
`FLT_EVAL_METHOD` is 0 on x86-64, Apple arm64 and AArch64.

On Cortex-M, RV32, RV64 and AVR, `<float.h>` defines `FLT_EVAL_METHOD` as
`__FLT_EVAL_METHOD__`, which is not predefined on those targets. In `#if`
it evaluates to 0; in an expression it is an error:

```text
embcc: f.c:4:11: error: '__FLT_EVAL_METHOD__' is not declared in '<global initializer>' — for a call, add a prototype or define it first [E0001]
```

<!-- Reported to the lead: the generated thumb/riscv/avr predefined-macro
     tables (from clang -dM, which treats __FLT_EVAL_METHOD__ specially)
     lack __FLT_EVAL_METHOD__, and include/float.h relies on it. -->

*The direction of rounding when an integer is converted to a
floating-point number that cannot exactly represent the original value
(C17 6.3.1.4).*

To nearest, ties to even. This holds for conversions at run time and for
those evaluated during translation.

*The direction of rounding when a floating-point number is converted to
a narrower floating-point number (C17 6.3.1.5).*

To nearest, ties to even.

*How the nearest representable value or the larger or smaller
representable value immediately adjacent to the nearest representable
value is chosen for certain floating constants (C17 6.4.4.2).*

A floating constant without a suffix is the nearest `double`. A constant
with the `f` suffix is converted to the nearest `double` and that value
rounded to `float`. A `long double` constant (suffix `l`) is converted
from its spelling at the target's `long double` precision.

*Whether and how floating expressions are contracted when not disallowed
by the `FP_CONTRACT` pragma; the default state for the `FP_CONTRACT`
pragma (C17 6.5, 7.12.2).*

Floating expressions are never contracted: `a * b + c` is a multiply and
an add on every target, and EmbCC emits no fused multiply-add
instruction. `#pragma STDC FP_CONTRACT` is accepted and has no effect.

*The default state for the `FENV_ACCESS` pragma (C17 7.6.1).*

`#pragma STDC FENV_ACCESS` and `#pragma STDC CX_LIMITED_RANGE` are
accepted and have no effect; EmbCC compiles every program as if
`FENV_ACCESS` were off. At `-O1` and above, the optimizer evaluates `+`,
`-`, `*`, `/`, negation and the comparisons on `float` and `double`
constants during translation, rounding to nearest, ties to even, so a
rounding mode set with `fesetround` does not apply to them. It leaves an
operation whose operand or result is a NaN, and all `long double`
arithmetic, to run time, and it does not reassociate floating-point
arithmetic. See [Optimization](optimization.md#floating-point).

*Additional floating-point exceptions, rounding modes, environments, and
classifications, and their macro names (C17 7.6, 7.12).*

None.

## Arrays and pointers

*The result of converting a pointer to an integer or vice versa
(C17 6.3.2.3).*

A pointer converted to an integer of the same width keeps its bit
pattern, and converting it back yields the original pointer. Converted
to a wider integer it is zero-extended; to a narrower one it is
truncated to the low bits. An integer converted to a pointer is first
converted to an integer type of the pointer's width (sign-extended if it
is signed and narrower, truncated if it is wider), and the pointer has
that bit pattern.

On AVR, a pointer to a function holds the function's word address in
program memory, which is half its byte address; see
[AVR](targets.md#avr).

*The size of the result of subtracting two pointers to elements of the
same array (C17 6.5.6).*

The result has type `ptrdiff_t`:

| x86-64 | Apple arm64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|
| `long`, 8 bytes | `long`, 8 bytes | `long`, 8 bytes | `int`, 4 bytes | `int`, 4 bytes | `long`, 8 bytes | `int`, 2 bytes |

## Hints

*The extent to which suggestions made by using the `register`
storage-class specifier are effective (C17 6.7.1).*

None. At block scope, `register` is accepted and does not influence
register allocation. Its one use is the GNU form
`register T x __asm__("REG")`, which binds a variable to a register for
an inline-assembly operand. Taking the address of a `register` variable
is accepted without a diagnostic.

`register` on a parameter or at file scope is refused:

```text
embcc: f.c:1:7: error: 'register' is not supported yet (see docs/manual/c-language.md)
```

*The extent to which suggestions made by using the `inline` function
specifier are effective (C17 6.7.4).*

None. Inlining is decided by the optimizer from the size of the callee,
at `-O2` and `-Os`; the `inline` keyword has no influence. Use
`__attribute__((always_inline))` or `__attribute__((noinline))` to
direct it (see [Optimization](optimization.md#inlining)).

A function defined `inline` without `static` or `extern` is emitted as an
ordinary external definition, as GNU C89 `inline` was, and not as a C99
inline definition. Two translation units that each define the same such
function therefore both define its symbol. Use `static inline` for
functions defined in headers.

## Structures, unions, enumerations, and bit-fields

*Whether a "plain" `int` bit-field is treated as a `signed int` bit-field
or as an `unsigned int` bit-field (C17 6.7.2, 6.7.2.1).*

As `signed int`, on every target. A plain `char` bit-field has the
signedness of plain `char` on the target. For example, after
`struct { int f : 3; } s; s.f = 7;`, `s.f` is −1.

*Allowable bit-field types other than `_Bool`, `signed int`, and
`unsigned int` (C17 6.7.2.1).*

Every integer type: plain, signed and unsigned `char`, `short`, `long`,
`long long`, `__int128` where it exists, and enumerated types. A
bit-field of enumerated type has the signedness of the enumerated type
(see below); an enumeration whose values all fit `int` is `int`, so its
bit-fields are signed. The width may not exceed the width of the
declared type, which on AVR is 16 for `int`:

```text
embcc: f.c:1: error: a bitfield must have integer type, not float
embcc: f.c:4: error: bitfield 'a' width 30 exceeds its type unsigned int
embcc: f.c:1: error: a named bitfield 'x' cannot have width 0
```

*Whether atomic types are permitted for bit-fields (C17 6.7.2.1).*

An `_Atomic`-qualified bit-field is accepted without a diagnostic.

<!-- UNVERIFIED: whether an access to an _Atomic bit-field is performed
     atomically. Not checked; consider refusing it. -->

*Whether a bit-field can straddle a storage-unit boundary (C17 6.7.2.1).*

The layout is GCC's, and agrees with each target's ABI. A bit-field may
not span more alignment units of its declared type than an object of that
type does. Where alignment equals size, which is every type on every
target except AVR, this means a bit-field never crosses a boundary of a
unit the size of its declared type; one that would is moved to the next
such unit. On AVR, where every alignment is 1, a bit-field may straddle
byte boundaries. In a `packed` structure, bit-fields are packed without
regard to their type's units.

*The order of allocation of bit-fields within a unit (C17 6.7.2.1).*

From the least significant bit upward, in declaration order. Every target
is little-endian, so the first bit-field occupies the low bits of the
lowest-addressed byte.

A zero-width bit-field moves the next member to the next unit boundary of
its declared type's alignment. An unnamed bit-field, of any width, raises
the alignment of the structure on AArch64 and Cortex-M, as AAPCS and
AAPCS64 require; on x86-64, Apple arm64 and RISC-V it does not, as the
System V, Apple and RISC-V ABIs require.

`embcc inspect types FILE.c` prints the layout EmbCC chose for each
structure: offsets, bit positions and padding. Some examples, as
size/alignment in bytes:

| Structure | x86-64, Apple arm64, RV32, RV64 | AArch64, Cortex-M | AVR |
|---|---|---|---|
| `struct { char c; int i : 4; }` | 4/4, `i` in bits 8–11 | 4/4, `i` in bits 8–11 | 2/1, `i` in bits 8–11 |
| `struct { char c; unsigned : 4; char d; }` | 3/1 | 4/4 | 3/1 |
| `struct { char a; int : 0; char b; }` | 5/1, `b` at offset 4 | 8/4, `b` at offset 4 | 2/1, `b` at offset 1 |
| `struct { unsigned short a : 12, b : 12; }` | 4/2, `b` at offset 2 | 4/2, `b` at offset 2 | 3/1, `b` in bits 12–23 |

*The alignment of non-bit-field members of structures (C17 6.7.2.1).*

Each member is aligned to the alignment of its type, given in
[Architecture](#architecture); on AVR every alignment is 1. A structure's
alignment is the largest alignment of its members, and its size is
rounded up to a multiple of that.

`__attribute__((packed))` lowers every member's alignment to 1.
`__attribute__((aligned(N)))` and `_Alignas` raise a member's or a
structure's alignment. `#pragma pack(N)` caps every member's alignment at
`N` (see [Preprocessing directives](#preprocessing-directives)). A
structure must have at least one member; an empty structure is refused:

```text
embcc: f.c:1:14: error: a struct/union needs at least one member
```

*The integer type compatible with each enumerated type (C17 6.7.2.2).*

For an enumeration without a fixed underlying type, the first of these
types that can represent the value of every enumeration constant, on
every target:

| Type | Chosen when |
|---|---|
| `int` | every value fits `int` |
| `unsigned int` | no value is negative, and every value fits `unsigned int` |
| `unsigned long` | no value is negative, and every value fits `unsigned long` |
| `unsigned long long` | no value is negative |
| `long` | every value fits `long` |
| `long long` | otherwise |

The enumeration constants have the enumerated type, as in C23. An
enumeration whose values all fit `int` is `int` even when no value is
negative, where GCC and Clang choose `unsigned int`; so
`(enum e)-1 < 0` is true under EmbCC. A wider enumeration has the type
GCC and Clang give it: one with no negative value that needs more than
32 bits is `unsigned long` on x86-64, Apple arm64, AArch64 and RV64, and
`unsigned long long` on Cortex-M and RV32. On AVR, one with no negative
value whose largest value is from 32768 to 65535 is `unsigned int` (2
bytes), and one that needs more than 16 bits is `unsigned long` (4
bytes). An enumeration named again by its tag has the same type.

A value of 2^63 or more is that value, not a negative one:
`enum { X = 0xffffffffffffffff }` is an `unsigned long` enumeration on
the 64-bit targets. An enumeration with such a value and a negative one,
or with a constant that would follow `LLONG_MAX` or `ULLONG_MAX`, has no
type that holds every value, and is refused, as GCC refuses it:

```text
embcc: f.c:1: error: the enumeration's values run from -1 to 9223372036854775808, which no integer type holds
embcc: f.c:1: error: enumerator 'B' would be one past LLONG_MAX, which no integer type the enum can have holds
```

A C23 fixed underlying type (`enum e : unsigned char { ... }`) is
supported: the enumerated type and its enumeration constants have that
type. A value the type cannot represent is refused:

```text
embcc: f.c:1: error: enumerator 'B' is 256, which the underlying type unsigned char cannot represent
```

`-fshort-enums` is refused, and so is a `packed` or `aligned` attribute on
an enumeration:

```text
embcc: error: -fshort-enums is not supported; EmbCC would emit ordinary code and the flag's promise would not hold
embcc: f.c:1:32: error: a packed or aligned enum is not supported (EmbCC's enums are always int-sized)
```

## Qualifiers

*What constitutes an access to an object that has volatile-qualified
type (C17 6.7.3).*

Each read or write of a `volatile` object that the abstract machine
performs is one load or store of the object, at the width of its type,
in program order relative to other `volatile` accesses. Accesses are not
removed, merged, combined with neighbouring accesses, moved out of a
loop or vectorized. A compound assignment (`reg |= 4`) is one read and
one write. A `volatile` object whose value is not used (`(void)reg;`) is
still read.

This holds at every optimization level for every `volatile` object:
objects with static, thread or automatic storage duration, whether or
not their address is taken, and any object accessed through a pointer to
a `volatile` type, such as a memory-mapped register
(`*(volatile unsigned *)0x40000000`).

```c
extern volatile unsigned short reg;
void f(void) { reg = 1; reg = 1; (void)reg; reg |= 4; }   /* 2 stores, 1 load, then load and store */
```

A `volatile` local variable is kept in its stack slot, never in a
register. A delay loop `for (volatile int i = 0; i < n; i++) ;` reads
and writes `i` in memory on each iteration, and a `volatile` local
changed between `setjmp` and `longjmp` has its new value after the
`longjmp` (C17 7.13.2.1).

A bit-field member of a `volatile` structure, or a `volatile` bit-field,
is accessed through the whole unit of its declared type (four bytes for
an `unsigned` bit-field, one byte for an `unsigned char` one). A read is
one load of the unit; two reads in one expression are two loads. A
store is one load of the unit and one store of it with the field
replaced. The value of the assignment is the value stored, converted to
the bit-field's type; the unit is not read again.

See also [Optimization](optimization.md#volatile).

## Preprocessing directives

*The locations within `#pragma` directives where header name
preprocessing tokens are recognized (C17 6.4, 6.4.7).*

None; no pragma EmbCC recognizes takes a header name.

*How sequences in both forms of header names are mapped to headers or
external source file names (C17 6.4.7).*

The characters between the delimiters are taken literally, with no
escape processing, as a path that is appended to each directory searched
(`DIR/NAME`). This is done even when the name begins with `/`, so an
absolute path in a header name is not found, except by
`#include "..."` in a file that was named on the command line without a
directory. A header name may be at most 255 characters.

*The places that are searched for an included `< >` delimited header,
and how the places are specified or the header is identified; how the
named source file is searched for in an included `" "` delimited header
(C17 6.10.2).*

For `#include "file"`, the directory of the file containing the
directive is searched first. Then, for both forms, the `-I` and
`-isystem` directories are searched in command-line order, followed by
EmbCC's own header directories. `#include_next` continues the search
after the directory in which the current file was found. The exact list,
`-nostdinc`, and the 16-directory limit are described in
[Search order](invoking.md#search-order); `--print-search-dirs` shows
EmbCC's own directories.

*The method by which preprocessing tokens (possibly resulting from macro
expansion) in a `#include` directive are combined into a header name
(C17 6.10.2).*

The operand of `#include` is not macro-expanded. It must begin with `"`
or `<`; any other form is refused:

```text
embcc: f.c:2: error: malformed #include
```

*The nesting limit for `#include` processing (C17 6.10.2).*

50 levels. Deeper nesting is an error:

```text
embcc: rec.h:1: error: #include nested too deeply
```

*Whether the value of a character constant in a constant expression that
controls conditional inclusion matches the value of the same character
constant in the execution character set; whether the value of a
single-character character constant in such an expression may have a
negative value (C17 6.10.1).*

It matches, and it may be negative: a character constant in `#if` has the
value it has in the program, so `#if '\377' < 0` is true on x86-64, Apple
arm64 and AVR (and with `-fsigned-char`) and false on the other targets.

*Whether the `#` operator inserts a `\` character before the `\`
character that begins a universal character name in a character
constant or string literal (C17 6.10.3.2).*

It does: `#x` applied to `'é'` produces `"'\\u00e9'"`.

*The behavior on each recognized non-STDC `#pragma` directive
(C17 6.10.6).*

EmbCC acts on `#pragma pack`, `#pragma once`, `#pragma push_macro`,
`#pragma pop_macro` and `#pragma weak`; the last four behave as in GCC
and are described in [Extensions](extensions.md#pragmas). `#pragma pack`
sets the maximum alignment of the members of structures defined after
it:

| Form | Effect |
|---|---|
| `#pragma pack(N)` | Cap member alignment at `N`, which must be 1, 2, 4, 8 or 16. |
| `#pragma pack()` | Remove the cap. |
| `#pragma pack(push)`, `#pragma pack(push, N)` | Save the current value, then optionally set `N`. |
| `#pragma pack(pop)` | Restore the saved value. |
| `#pragma pack(show)` | Accepted; has no effect. |

The arguments are not macro-expanded. The cap applies to a member's
`aligned` attribute as well, and to the units that bit-fields are placed
in. The operator form `_Pragma("pack(...)")` is equivalent. Refused forms
each have their own error:

```text
embcc: f.c:1: error: #pragma pack wants 1, 2, 4, 8 or 16
embcc: f.c:3: error: #pragma pack inside a struct body is not supported: put it before the struct
embcc: f.c:1: error: #pragma pack(push, name) is not supported
embcc: f.c:1: error: #pragma pack(pop) without a matching push
```

In C++, `#pragma pack` is refused; use `__attribute__((packed))`.

Every other pragma, in `#pragma` or `_Pragma` form, is discarded without
a diagnostic. This includes:

- `#pragma GCC ...` and `#pragma clang ...` (diagnostic control,
  visibility, and the rest).
- `#pragma STDC FP_CONTRACT`, `FENV_ACCESS` and `CX_LIMITED_RANGE`, which
  have no effect (see [Floating point](#floating-point)).

There is no warning for an ignored pragma; `-Wunknown-pragmas` is not a
warning EmbCC has.

*The definitions for `__DATE__` and `__TIME__` when respectively, the
date and time of translation are not available (C17 6.10.8.1).*

EmbCC does not define `__DATE__` or `__TIME__` at all, nor `__TIMESTAMP__`
or `__COUNTER__`. A use of one reaches the compiler as an undeclared
identifier:

```text
embcc: f.c:1:17: error: '__DATE__' is not declared in '<global initializer>' — for a call, add a prototype or define it first [E0001]
```

Define them with `-D` if a program needs them, for example
`-D__DATE__='"Jan  1 1970"'`.

### Preprocessor limits

| Limit | EmbCC | C17 minimum |
|---|---|---|
| Nesting of `#include` | 50: `#include nested too deeply` | 15 |
| Nesting of conditional inclusion | 64: `conditionals nested too deeply` | 63 |
| Parameters in one macro definition | 16: `too many macro parameters` | 127 |

The limit of 16 macro parameters is below the standard's minimum.

### Predefined macros that describe the implementation

| Macro | Value |
|---|---|
| `__STDC__` | `1` |
| `__STDC_VERSION__` | `201710L` (C only) |
| `__STDC_HOSTED__` | `1`, on every target, with or without `-ffreestanding` |
| `__EMBCC__` | `1` |
| `__GNUC__`, `__clang__` | not defined |
| `__STDC_IEC_559__`, `__STDC_IEC_559_COMPLEX__` | not defined |
| `__STDC_ISO_10646__`, `__STDC_UTF_16__`, `__STDC_UTF_32__` | not defined |
| `__STDC_NO_ATOMICS__`, `__STDC_NO_COMPLEX__`, `__STDC_NO_THREADS__`, `__STDC_NO_VLA__` | not defined |
| `__STDC_LIB_EXT1__` | not defined |

`embcc --target=TRIPLE --dump-predef` lists the target-specific
predefined macros; see [Targets](targets.md).

## Library functions

*The library's implementation-defined behavior (C17 7.1 to 7.31).*

The items below describe EmbCC's C library, `lib/libc`, used on x86-64
(EmbLinkOS and Linux) and by bare-metal programs that link it. The
library's scope and the rest of its behavior are in
[Libraries](libraries.md#the-c-library-liblibc).

| Item | EmbCC's C library |
|---|---|
| Null pointer constant `NULL` (7.19) | `((void *)0)` |
| `max_align_t` (7.19) | not declared in C; declared by `<stddef.h>` in C++ only |
| Output of a failed `assert` (7.2.1.1) | `FILE:LINE: FUNCTION: Assertion `EXPR' failed.` on standard error, then `abort()` |
| `abort` (7.22.4.1) | flushes open streams and ends the program with status 134; it does not raise `SIGABRT` |
| `EXIT_SUCCESS`, `EXIT_FAILURE` (7.22.4.4) | 0 and 1 |
| `malloc(0)`, `calloc` of zero bytes (7.22.3) | a unique non-null pointer |
| `realloc(p, 0)` (7.22.3.5) | frees `p` and returns a null pointer |
| `RAND_MAX` (7.22.2) | 2147483647 |
| `errno` values (7.5) | the traditional POSIX numbers |
| Locales (7.11) | one: `"C"`. `setlocale` accepts `"C"`, `"POSIX"` and `""` |
| Multibyte encoding (7.22.7, 7.29.6) | UTF-8; overlong forms, surrogates and values above U+10FFFF are rejected |
| `MB_LEN_MAX` (5.2.4.2.1) | 1, although the multibyte encoding is UTF-8 |
| `MB_CUR_MAX` (7.22) | not defined by `<stdlib.h>` |
| `printf` of a null `%s`, `%p` (7.21.6.1) | `(null)`, `(nil)` |
| `system` (7.22.4.8) | runs nothing; returns 0 for `system(NULL)` and −1 otherwise |
| Time zone (7.27) | none: `localtime` is `gmtime` |
| `fesetround`, floating-point exception flags (7.6) | x86-64 and AArch64 only |

<!-- Reported to the lead: MB_LEN_MAX is 1 in include/limits.h while the
     library's multibyte encoding is UTF-8 (MB_CUR_MAX must not exceed
     MB_LEN_MAX), and MB_CUR_MAX is missing from lib/libc/include/stdlib.h
     (`int a = MB_CUR_MAX;` is error E0001). -->

## Architecture

*The values or expressions assigned to the macros specified in the
headers `<float.h>`, `<limits.h>`, and `<stdint.h>` (C17 5.2.4.2,
7.20.2, 7.20.3).*

They follow from the sizes and formats on this page: the integer limits
from the type widths and two's complement, the `<float.h>` values from
the IEEE formats. Each header defines its macros from predefined macros
(`__INT_MAX__`, `__DBL_MANT_DIG__`, `__SIZE_TYPE__`, ...), which
`--dump-predef` lists for each target. Values that differ between
targets include:

| Macro | x86-64 | Apple arm64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|---|
| `CHAR_MIN` | −128 | −128 | 0 | 0 | 0 | 0 | −128 |
| `INT_MAX` | 2^31−1 | 2^31−1 | 2^31−1 | 2^31−1 | 2^31−1 | 2^31−1 | 32767 |
| `LONG_MAX` | 2^63−1 | 2^63−1 | 2^63−1 | 2^31−1 | 2^31−1 | 2^63−1 | 2^31−1 |
| `SIZE_MAX` | 2^64−1 | 2^64−1 | 2^64−1 | 2^32−1 | 2^32−1 | 2^64−1 | 65535 |
| `LDBL_MANT_DIG` | 64 | 53 | 113 | 53 | 113 | 113 | 24 |
| `DBL_MANT_DIG` | 53 | 53 | 53 | 53 | 53 | 53 | 24 |
| `WCHAR_MIN` | −2^31 | −2^31 | 0 | 0 | −2^31 | −2^31 | −32768 |
| `FLT_EVAL_METHOD` | 0 | 0 | 0 | 0 in `#if` only | 0 in `#if` only | 0 in `#if` only | 0 in `#if` only |
| `FLT_ROUNDS` | 1 | 1 | 1 | 1 | 1 | 1 | 1 |

*The result of attempting to indirectly access an object with automatic
or thread storage duration from a thread other than the one with which it
is associated (C17 6.2.4).*

The access is an ordinary memory access; EmbCC adds nothing to prevent
or detect it. On Cortex-M, RV32, RV64 and AVR, a `__thread` object has
one instance shared by all threads; see [Targets](targets.md#summary).

*The number, order, and encoding of bytes in any object (when not
explicitly specified in this International Standard) (C17 6.2.6.1).*

Bytes are 8 bits. Every target is little-endian: the least significant
byte of a multi-byte object is at the lowest address. Integers are two's
complement and floating types use the IEEE formats in
[Floating point](#floating-point). `_Bool` is one byte holding 0 or 1. A
complex type is laid out as two elements of its real type, real part
first. A pointer is an address of the size in the table below.

*The value of the result of the `sizeof` and `_Alignof` operators
(C17 6.5.3.4).*

Size and alignment in bytes, written `size/alignment`:

| Type | x86-64 | Apple arm64 | AArch64 | Cortex-M | RV32 | RV64 | AVR |
|---|---|---|---|---|---|---|---|
| `_Bool`, `char` | 1/1 | 1/1 | 1/1 | 1/1 | 1/1 | 1/1 | 1/1 |
| `short` | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/2 | 2/1 |
| `int`, and enumerated types whose values fit `int` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 2/1 |
| `long` | 8/8 | 8/8 | 8/8 | 4/4 | 4/4 | 8/8 | 4/1 |
| `long long` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/1 |
| `__int128` | 16/16 | 16/16 | 16/16 | — | — | 16/16 | — |
| pointers, `size_t`, `ptrdiff_t`, `intptr_t` | 8/8 | 8/8 | 8/8 | 4/4 | 4/4 | 8/8 | 2/1 |
| `intmax_t` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/1 |
| `float` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/1 |
| `double` | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 8/8 | 4/1 |
| `long double` | 16/16 | 8/8 | 16/16 | 8/8 | 16/16 | 16/16 | 4/1 |
| `_Complex float` | 8/4 | 8/4 | 8/4 | 8/4 | 8/4 | 8/4 | 8/1 |
| `_Complex double` | 16/8 | 16/8 | 16/8 | 16/8 | 16/8 | 16/8 | 8/1 |
| `_Complex long double` | 32/16 | 16/8 | 32/16 | 16/8 | 32/16 | 32/16 | 8/1 |
| `wchar_t` | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 4/4 | 2/1 |

"—" means the type does not exist on that target. Any other enumerated
type has the size and alignment of the integer type it is compatible
with (see
[Structures, unions, enumerations, and bit-fields](#structures-unions-enumerations-and-bit-fields)).
The same table, with the ABI details that go with it, is in
[Data models](targets.md#data-models).

*Whether any extended alignments are supported and the contexts in which
they are supported; valid alignment values other than those returned by
an `_Alignof` expression for fundamental types (C17 6.2.8).*

The largest fundamental alignment (`__BIGGEST_ALIGNMENT__`) is 16 on
x86-64, Apple arm64, AArch64, RV32 and RV64, 8 on Cortex-M and 1 on AVR.

Extended alignments are supported with `_Alignas` and
`__attribute__((aligned(N)))`, where `N` is an integer constant
expression:

- for objects with static storage duration, at least up to 4096 (the
  section is given the alignment);
- for structure members, which raises the structure's alignment;
- for local arrays, structures and unions on every target except AVR.

A local scalar may be aligned up to the stack's alignment, which is 16
bytes, or 8 on Cortex-M. A larger alignment is refused:

```text
embcc: f.c:1:32: error: 'x' needs 32-byte alignment and the stack only guarantees 16: supported for an array or a struct, not yet for a scalar
```

On AVR a local with an alignment greater than 1 is refused:

```text
embcc: f.c:7: error: the AVR backend cannot lower a local with __attribute__((aligned)): AVR's stack pointer has no known alignment, so a frame slot cannot be given one yet (function f)
```

Valid alignments are powers of two. Any other value is refused, by
`_Alignas` and by the `aligned` attribute; `_Alignas(0)` is accepted and
has no effect, as the standard specifies:

```text
embcc: f.c:1: error: _Alignas requires a constant power of two
embcc: f.c:1: error: aligned wants a constant power of two
```

## Summary of departures from the standard

These are the places, described above, where EmbCC does not do what C17
requires:

- Universal character names in identifiers are refused
  ([Identifiers](#identifiers)).
- On Cortex-M, RISC-V and AVR, `FLT_EVAL_METHOD` cannot be used in an
  expression ([Floating point](#floating-point)).
- An enumeration constant outside the range of `int` is accepted
  without a diagnostic, as C23 allows
  ([Structures](#structures-unions-enumerations-and-bit-fields)).
- `__DATE__` and `__TIME__` are not defined, and a macro may have at most
  16 parameters ([Preprocessing directives](#preprocessing-directives)).
- `max_align_t` is not declared in C, `MB_CUR_MAX` is not defined, and
  `MB_LEN_MAX` is 1 ([Library functions](#library-functions)).
