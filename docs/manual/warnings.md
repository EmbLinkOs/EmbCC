# Warning reference

Every warning EmbCC has, generated from the compiler's own table by `embcc --reference=warnings`. A warning names the option that controls it, `[--warn=NAME]`; `--no-warn=NAME` turns it off and `--error=NAME` makes it an error. [Diagnostics](diagnostics.md) says how they are reported and shows each one.

**On by default** means on unless `--no-warn=NAME` or `--no-warnings` is given. A warning in **neither set** is on only when named.

| Warning | Diagnoses | Turned on by |
|---|---|---|
| `--warn=address` | A function name tested in an `if` condition, or compared with a null pointer constant | `--warn=all` |
| `--warn=arith-conversion` | As `--warn=conversion`, also for arithmetic whose operands each fit the target type | neither set |
| `--warn=attributes` | An attribute EmbCC does not know, or one it accepts but does not implement | on by default |
| `--warn=cast-qual` | A cast that removes `const` or `volatile` from the type a pointer points to | neither set |
| `--warn=conversion` | An implicit conversion that may change an integer's value, or an integer converted to a floating type that cannot hold all its values | neither set |
| `--warn=deprecated-declarations` | A use of a function or variable declared `deprecated` | on by default |
| `--warn=discarded-qualifiers` | A pointer conversion that drops the pointed-to type's `const` | on by default |
| `--warn=div-by-zero` | Integer division or remainder by a constant zero | on by default |
| `--warn=double-promotion` | A `float` implicitly converted to `double`: an operand, a `double` parameter, a variadic argument | neither set |
| `--warn=float-conversion` | An implicit conversion from a floating type to an integer or to a narrower floating type that may change the value | `--warn=conversion` |
| `--warn=format` | A `printf`- or `scanf`-style format that disagrees with its arguments | `--warn=all` |
| `--warn=format-nonliteral` | A format that is not a string literal, with arguments after it (not for a `va_list` function such as `vprintf`) | `--warn=format=2` |
| `--warn=format-security` | A format that is not a string literal, with no arguments after it | `--warn=format=2` |
| `--warn=implicit-fallthrough` | A `case` whose statements can run into the next label with no `__attribute__((fallthrough));`, `[[fallthrough]];` or fall-through comment | `--warn=extra` |
| `--warn=logical-op` | `a && a` or `a \|\| a` with identical operands | `--warn=extra` |
| `--warn=maybe-uninitialized` | A local variable read on a path where only some paths wrote it | `--warn=all` |
| `--warn=missing-declarations` | A function with external linkage defined with no declaration before it (when `--warn=missing-prototypes` is off) | neither set |
| `--warn=missing-prototypes` | A function with external linkage defined with no prototype before it | neither set |
| `--warn=multichar` | A character constant of more than one character, `'ab'` | on by default |
| `--warn=nested-externs` | An `extern` declaration inside a function body | neither set |
| `--warn=old-style-declaration` | A storage class or `inline` after the type specifiers: `const static int t[]`, `int static n` | `--warn=extra` |
| `--warn=overflow` | An integer constant converted to a type that cannot hold it as a signed or as an unsigned value: `unsigned char c = 300;` | on by default |
| `--warn=parentheses` | A comparison as an unparenthesized operand of `&`, `\|` or `^`: `reg & MASK == 0` | `--warn=all` |
| `--warn=pedantic` | A construct the C standard `--std` names does not have: a later standard's (`0b101` before C23, `_Bool` in C89) or a GNU extension (`({ ... })`, `case 1 ... 3`, `x ?: y`), except after `__extension__`. `--pedantic` turns it on and `--pedantic-errors` makes it an error; either also takes C23's keywords away from a GNU dialect before C23 | neither set |
| `--warn=prio-ctor-dtor` | A `constructor` or `destructor` priority from 0 to 100, which the implementation reserves | on by default |
| `--warn=redundant-decls` | A second declaration of a function or `extern` object in the same scope | neither set |
| `--warn=return-mismatch` | A `return` with a value in a `void` function, under `--std=c89` or `gnu89` (from C99 on, and under `--pedantic-errors`, it is an error) | on by default |
| `--warn=shadow` | A local declaration that hides a local variable, a parameter or a file-scope variable | neither set |
| `--warn=shift-count-overflow` | A constant shift count that is negative or not less than the operand width | `--warn=all` |
| `--warn=sign-compare` | A comparison that converts a possibly negative signed operand to unsigned | `--warn=extra` |
| `--warn=sign-conversion` | An implicit conversion between signed and unsigned integer types that may change the sign of the value | `--warn=conversion` |
| `--warn=strict-prototypes` | A function declarator with `()` instead of `(void)` | neither set |
| `--warn=switch-default` | A `switch` with no `default` label | neither set |
| `--warn=type-limits` | An unsigned value compared `< 0` or `>= 0` | `--warn=extra` |
| `--warn=undef` | An identifier in `#if` that no macro defines (evaluated as 0), outside an operand `&&`, `\|\|` or `?:` skips | neither set |
| `--warn=uninitialized` | A local variable read before any path wrote it | `--warn=all` |
| `--warn=unused-function` | A `static` function that is defined and never used | `--warn=all` |
| `--warn=unused-parameter` | A function parameter the body never uses | `--warn=extra` |
| `--warn=unused-result` | A discarded result of a function declared `warn_unused_result` or `[[nodiscard]]` | on by default |
| `--warn=unused-variable` | A local variable that is never used | `--warn=all` |
| `--warn=vla` | A variable length array, or a `typedef` of one | neither set |
| `--warn=windows-abi` | A compile for a Windows target, whose ABI EmbCC does not yet fully implement | on by default |

GCC's spellings work too: `-WNAME`, `-Wno-NAME`, `-Werror=NAME` ([Migrating from GCC](migrating-from-gcc.md)).
