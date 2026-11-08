# Warning reference

Every warning EmbCC has, generated from the compiler's own table by `embcc --reference=warnings`. A warning names the option that controls it, `[--warn=NAME]`; `--no-warn=NAME` turns it off and `--error=NAME` makes it an error. [Diagnostics](diagnostics.md) says how they are reported and shows each one.

**On by default** means on unless `--no-warn=NAME` or `--no-warnings` is given. A warning in **neither set** is on only when named.

| Warning | Diagnoses | Turned on by |
|---|---|---|
| `--warn=address` | A function name tested in an `if` condition, or compared with a null pointer constant | `--warn=all` |
| `--warn=analyzer-double-free` | Memory freed twice | `--warn=analyze` |
| `--warn=analyzer-malloc-leak` | Memory a function allocated and lost: no pointer to it is left when it returns | `--warn=analyze` |
| `--warn=analyzer-null-dereference` | A null pointer dereferenced | `--warn=analyze` |
| `--warn=analyzer-possible-null-dereference` | The result of an allocation dereferenced before it was checked for NULL | `--warn=analyze` |
| `--warn=analyzer-use-after-free` | Memory used after it was freed | `--warn=analyze` |
| `--warn=arith-conversion` | As `--warn=conversion`, also for arithmetic whose operands each fit the target type | neither set |
| `--warn=attributes` | An attribute EmbCC does not know, or one it accepts but does not implement | on by default |
| `--warn=cast-align` | A pointer cast to a type with a stricter alignment (`(int *)` of a `char *`) on a target where a misaligned access traps: ARMv6-M, v8-M Baseline, Xtensa, MIPS, SPARC, TriCore | neither set |
| `--warn=cast-align-strict` | As `--warn=cast-align`, on every target (`-Wcast-align=strict`) | neither set |
| `--warn=cast-qual` | A cast that removes `const` or `volatile` from the type a pointer points to | neither set |
| `--warn=cert-arr30` | CERT C ARR30-C (high): an array subscript a constant out of the array's bounds | `--warn=cert` |
| `--warn=cert-dcl30` | CERT C DCL30-C (high): the address of an automatic object returned | `--warn=cert` |
| `--warn=cert-dcl37` | CERT C DCL37-C (low): a declaration of a reserved identifier (_X..., __...) | `--warn=cert` |
| `--warn=cert-env33` | CERT C ENV33-C (high): a call of system | `--warn=cert` |
| `--warn=cert-err33` | CERT C ERR33-C (high): the result of a function that reports errors by it (malloc, fopen, ...) ignored | `--warn=cert` |
| `--warn=cert-exp30` | CERT C EXP30-C (medium): an object modified twice, or modified and read, without a sequence point between | `--warn=cert` |
| `--warn=cert-exp42` | CERT C EXP42-C (medium): memcmp of structures, which compares their padding | `--warn=cert` |
| `--warn=cert-exp46` | CERT C EXP46-C (low): a bitwise operator with a Boolean-like operand | `--warn=cert` |
| `--warn=cert-flp30` | CERT C FLP30-C (low): a floating-point loop counter | `--warn=cert` |
| `--warn=cert-mem30` | CERT C MEM30-C (high): a pointer used after it was passed to free | `--warn=cert` |
| `--warn=cert-mem34` | CERT C MEM34-C (high): free of memory that was not allocated (a local or a global) | `--warn=cert` |
| `--warn=cert-msc24` | CERT C MSC24-C (low): a deprecated or obsolescent function (gets, atoi, setbuf, rewind, asctime, ...) | `--warn=cert` |
| `--warn=cert-msc30` | CERT C MSC30-C (medium): rand, which is not good enough for anything that matters | `--warn=cert` |
| `--warn=cert-str30` | CERT C STR30-C (low): a string literal assigned to a pointer through which it may be modified | `--warn=cert` |
| `--warn=cert-str31` | CERT C STR31-C (high): gets, which cannot be told the size of its buffer | `--warn=cert` |
| `--warn=comment` | A `//` comment that a backslash at the end of its line continues on the next, or `/*` inside a `/* ... */` comment | `--warn=all` |
| `--warn=conversion` | An implicit conversion that may change an integer's value, or an integer converted to a floating type that cannot hold all its values | neither set |
| `--warn=coverage-mismatch` | `-fprofile-use` and a function that is not the one the profile was taken from: its source or its options changed; its profile is ignored | on by default |
| `--warn=deprecated-declarations` | A use of a function or variable declared `deprecated` | on by default |
| `--warn=discarded-qualifiers` | A pointer conversion that drops the pointed-to type's `const` | on by default |
| `--warn=div-by-zero` | Integer division or remainder by a constant zero | on by default |
| `--warn=double-promotion` | A `float` implicitly converted to `double`: an operand, a `double` parameter, a variadic argument | neither set |
| `--warn=float-conversion` | An implicit conversion from a floating type to an integer or to a narrower floating type that may change the value | `--warn=conversion` |
| `--warn=format` | A `printf`- or `scanf`-style format that disagrees with its arguments | `--warn=all` |
| `--warn=format-nonliteral` | A format that is not a string literal, with arguments after it (not for a `va_list` function such as `vprintf`) | `--warn=format=2` |
| `--warn=format-security` | A format that is not a string literal, with no arguments after it | `--warn=format=2` |
| `--warn=implicit-fallthrough` | A `case` whose statements can run into the next label with no `__attribute__((fallthrough));`, `[[fallthrough]];` or fall-through comment | `--warn=extra` |
| `--warn=implicit-function-declaration` | Under `--permissive`, a call to a function nothing declared, which C89 declared `extern int f();` | on by default |
| `--warn=implicit-int` | Under `--permissive`, a declaration with no type, which C89 read as `int`: `static x = 3;`, `f() { ... }` | on by default |
| `--warn=isr-critical-unbalanced` | A critical section (interrupts disabled) still open where the function returns | `--warn=analyze` |
| `--warn=isr-race-rmw` | A read-modify-write (x++, x \|= m, x = x + 1) of an object an interrupt handler also writes, outside a critical section: an interrupt between the read and the write loses its update | `--warn=analyze` |
| `--warn=isr-reentrancy` | A function that both an interrupt handler and the code it interrupts call, and that keeps state in a static object: an interrupt in the middle of it corrupts that state | `--warn=analyze` |
| `--warn=isr-shared-nonvolatile` | An object an interrupt handler writes and other code reads (or the other way), not volatile: the compiler may keep it in a register, and never see the change | `--warn=analyze` |
| `--warn=isr-torn-access` | An access wider than the machine reads or writes at once, of an object an interrupt handler also accesses, outside a critical section: one side can see half of the other's update | `--warn=analyze` |
| `--warn=isr-unsafe-call` | An interrupt handler, or a function it calls, calling one that is not safe there: it allocates, locks, blocks or does stdio | `--warn=analyze` |
| `--warn=logical-op` | `a && a` or `a \|\| a` with identical operands | `--warn=extra` |
| `--warn=maybe-uninitialized` | A local variable read on a path where only some paths wrote it | `--warn=all` |
| `--warn=misra-10.1` | MISRA C:2023 Rule 10.1 (required): an operand of an inappropriate essential type: Boolean in arithmetic, a signed operand of a bitwise or shift operator, unary minus of an unsigned one | `--warn=misra` |
| `--warn=misra-10.3` | MISRA C:2023 Rule 10.3 (required): a value assigned to an object of a narrower essential type, or of another essential type category | `--warn=misra` |
| `--warn=misra-10.4` | MISRA C:2023 Rule 10.4 (required): the operands of an arithmetic or comparison operator in different essential type categories (signed and unsigned) | `--warn=misra` |
| `--warn=misra-10.6` | MISRA C:2023 Rule 10.6 (required): a composite expression assigned to an object of a wider essential type | `--warn=misra` |
| `--warn=misra-10.8` | MISRA C:2023 Rule 10.8 (required): a composite expression cast to a wider essential type or another category | `--warn=misra` |
| `--warn=misra-11.1` | MISRA C:2023 Rule 11.1 (required): a conversion between a pointer to a function and any other type | `--warn=misra` |
| `--warn=misra-11.3` | MISRA C:2023 Rule 11.3 (required): a cast between pointers to different object types | `--warn=misra` |
| `--warn=misra-11.4` | MISRA C:2023 Rule 11.4 (advisory): a conversion between a pointer and an integer | `--warn=misra` |
| `--warn=misra-11.5` | MISRA C:2023 Rule 11.5 (advisory): a conversion from a pointer to void to a pointer to an object | `--warn=misra` |
| `--warn=misra-11.6` | MISRA C:2023 Rule 11.6 (required): a cast between a pointer to void and an arithmetic type | `--warn=misra` |
| `--warn=misra-11.7` | MISRA C:2023 Rule 11.7 (required): a cast between a pointer to an object and a non-integer arithmetic type | `--warn=misra` |
| `--warn=misra-11.8` | MISRA C:2023 Rule 11.8 (required): a cast that removes const or volatile from what a pointer points to | `--warn=misra` |
| `--warn=misra-11.9` | MISRA C:2023 Rule 11.9 (required): an integer 0 used as a null pointer constant (NULL is the only one) | `--warn=misra` |
| `--warn=misra-12.1` | MISRA C:2023 Rule 12.1 (advisory): operators of different precedence mixed without parentheses | `--warn=misra` |
| `--warn=misra-12.2` | MISRA C:2023 Rule 12.2 (required): a constant shift count outside 0 to one less than the operand's width | `--warn=misra` |
| `--warn=misra-12.3` | MISRA C:2023 Rule 12.3 (advisory): the comma operator | `--warn=misra` |
| `--warn=misra-13.3` | MISRA C:2023 Rule 13.3 (advisory): an increment or decrement in an expression with another side effect | `--warn=misra` |
| `--warn=misra-13.4` | MISRA C:2023 Rule 13.4 (advisory): the result of an assignment used as a value | `--warn=misra` |
| `--warn=misra-13.5` | MISRA C:2023 Rule 13.5 (required): a side effect in the right operand of && or \|\| | `--warn=misra` |
| `--warn=misra-13.6` | MISRA C:2023 Rule 13.6 (required): a side effect in the operand of sizeof | `--warn=misra` |
| `--warn=misra-14.1` | MISRA C:2023 Rule 14.1 (required): a loop counter of floating type | `--warn=misra` |
| `--warn=misra-14.3` | MISRA C:2023 Rule 14.3 (required): a controlling expression that is invariant (other than an infinite loop's) | `--warn=misra` |
| `--warn=misra-14.4` | MISRA C:2023 Rule 14.4 (required): a controlling expression that is not essentially Boolean | `--warn=misra` |
| `--warn=misra-15.1` | MISRA C:2023 Rule 15.1 (advisory): the goto statement | `--warn=misra` |
| `--warn=misra-15.2` | MISRA C:2023 Rule 15.2 (required): a goto that jumps backward | `--warn=misra` |
| `--warn=misra-15.4` | MISRA C:2023 Rule 15.4 (advisory): more than one break or goto leaving the same loop | `--warn=misra` |
| `--warn=misra-15.5` | MISRA C:2023 Rule 15.5 (advisory): a function with more than one point of exit, or one not at its end | `--warn=misra` |
| `--warn=misra-15.6` | MISRA C:2023 Rule 15.6 (required): the body of an iteration or selection statement that is not a compound statement | `--warn=misra` |
| `--warn=misra-15.7` | MISRA C:2023 Rule 15.7 (required): an if ... else if chain without a final else | `--warn=misra` |
| `--warn=misra-16.3` | MISRA C:2023 Rule 16.3 (required): a switch clause that does not end with an unconditional break | `--warn=misra` |
| `--warn=misra-16.4` | MISRA C:2023 Rule 16.4 (required): a switch statement without a default label | `--warn=misra` |
| `--warn=misra-16.5` | MISRA C:2023 Rule 16.5 (required): a default label that is neither the first nor the last of its switch | `--warn=misra` |
| `--warn=misra-16.6` | MISRA C:2023 Rule 16.6 (required): a switch statement with fewer than two clauses | `--warn=misra` |
| `--warn=misra-16.7` | MISRA C:2023 Rule 16.7 (required): a switch on an essentially Boolean expression | `--warn=misra` |
| `--warn=misra-17.1` | MISRA C:2023 Rule 17.1 (required): the features of <stdarg.h> | `--warn=misra` |
| `--warn=misra-17.2` | MISRA C:2023 Rule 17.2 (required): a function that calls itself, directly or through others of this unit | `--warn=misra` |
| `--warn=misra-17.7` | MISRA C:2023 Rule 17.7 (required): the value a non-void function returns, discarded without a cast to void | `--warn=misra` |
| `--warn=misra-17.8` | MISRA C:2023 Rule 17.8 (advisory): a function parameter modified | `--warn=misra` |
| `--warn=misra-18.4` | MISRA C:2023 Rule 18.4 (advisory): the +, -, += or -= operator applied to a pointer | `--warn=misra` |
| `--warn=misra-18.5` | MISRA C:2023 Rule 18.5 (advisory): a declaration with more than two levels of pointer nesting | `--warn=misra` |
| `--warn=misra-18.7` | MISRA C:2023 Rule 18.7 (required): a flexible array member | `--warn=misra` |
| `--warn=misra-18.8` | MISRA C:2023 Rule 18.8 (required): a variable-length array | `--warn=misra` |
| `--warn=misra-19.2` | MISRA C:2023 Rule 19.2 (advisory): the union keyword | `--warn=misra` |
| `--warn=misra-2.2` | MISRA C:2023 Rule 2.2 (required): an expression statement with no effect (dead code) | `--warn=misra` |
| `--warn=misra-2.6` | MISRA C:2023 Rule 2.6 (advisory): a label that no goto names | `--warn=misra` |
| `--warn=misra-2.7` | MISRA C:2023 Rule 2.7 (advisory): a function parameter that is not used | `--warn=misra` |
| `--warn=misra-21.10` | MISRA C:2023 Rule 21.10 (required): the time and date functions of <time.h> | `--warn=misra` |
| `--warn=misra-21.12` | MISRA C:2023 Rule 21.12 (required): the exception-handling features of <fenv.h> | `--warn=misra` |
| `--warn=misra-21.21` | MISRA C:2023 Rule 21.21 (required): the system function | `--warn=misra` |
| `--warn=misra-21.24` | MISRA C:2023 Rule 21.24 (required): the random number generator functions rand and srand | `--warn=misra` |
| `--warn=misra-21.3` | MISRA C:2023 Rule 21.3 (required): the memory allocation and deallocation functions of <stdlib.h> | `--warn=misra` |
| `--warn=misra-21.4` | MISRA C:2023 Rule 21.4 (required): setjmp and longjmp | `--warn=misra` |
| `--warn=misra-21.5` | MISRA C:2023 Rule 21.5 (required): the features of <signal.h> | `--warn=misra` |
| `--warn=misra-21.6` | MISRA C:2023 Rule 21.6 (required): the standard input/output functions | `--warn=misra` |
| `--warn=misra-21.7` | MISRA C:2023 Rule 21.7 (required): atof, atoi, atol and atoll | `--warn=misra` |
| `--warn=misra-21.8` | MISRA C:2023 Rule 21.8 (required): the termination functions of <stdlib.h> (abort, exit, _Exit, quick_exit) | `--warn=misra` |
| `--warn=misra-21.9` | MISRA C:2023 Rule 21.9 (required): bsearch and qsort | `--warn=misra` |
| `--warn=misra-7.1` | MISRA C:2023 Rule 7.1 (required): an octal constant other than 0 | `--warn=misra` |
| `--warn=misra-7.2` | MISRA C:2023 Rule 7.2 (required): an integer constant of unsigned type without a `u` or `U` suffix | `--warn=misra` |
| `--warn=misra-7.3` | MISRA C:2023 Rule 7.3 (required): the lowercase `l` in an integer constant's suffix | `--warn=misra` |
| `--warn=misra-7.4` | MISRA C:2023 Rule 7.4 (required): a string literal assigned to a pointer to non-const char | `--warn=misra` |
| `--warn=misra-8.10` | MISRA C:2023 Rule 8.10 (required): an inline function that is not static | `--warn=misra` |
| `--warn=misra-8.11` | MISRA C:2023 Rule 8.11 (advisory): an extern array declared without its size | `--warn=misra` |
| `--warn=misra-8.14` | MISRA C:2023 Rule 8.14 (required): the `restrict` qualifier | `--warn=misra` |
| `--warn=misra-8.2` | MISRA C:2023 Rule 8.2 (required): a function declared or defined without a prototype with named parameters | `--warn=misra` |
| `--warn=missing-declarations` | A function with external linkage defined with no declaration before it (when `--warn=missing-prototypes` is off) | neither set |
| `--warn=missing-profile` | `-fprofile-use` and no profile for this unit: the program was not run since `-fprofile-generate` built it, or its `.gcda` is somewhere else (`-fprofile-use=PATH`) | on by default |
| `--warn=missing-prototypes` | A function with external linkage defined with no prototype before it | neither set |
| `--warn=multichar` | A character constant of more than one character, `'ab'` | on by default |
| `--warn=nested-externs` | An `extern` declaration inside a function body | neither set |
| `--warn=null-dereference` | A pointer dereferenced where it is null on every path (`int *p = 0; *p`), or on some (`p = c ? q : 0; *p`); not a volatile access | neither set |
| `--warn=old-style-declaration` | A storage class or `inline` after the type specifiers: `const static int t[]`, `int static n` | `--warn=extra` |
| `--warn=overflow` | An integer constant converted to a type that cannot hold it as a signed or as an unsigned value: `unsigned char c = 300;` | on by default |
| `--warn=parentheses` | A comparison as an unparenthesized operand of `&`, `\|` or `^`: `reg & MASK == 0` | `--warn=all` |
| `--warn=pedantic` | A construct the C standard `--std` names does not have: a later standard's (`0b101` before C23, `_Bool` in C89) or a GNU extension (`({ ... })`, `case 1 ... 3`, `x ?: y`), except after `__extension__`. `--pedantic` turns it on and `--pedantic-errors` makes it an error; either also takes C23's keywords away from a GNU dialect before C23 | neither set |
| `--warn=prio-ctor-dtor` | A `constructor` or `destructor` priority from 0 to 100, which the implementation reserves | on by default |
| `--warn=redundant-decls` | A second declaration of a function or `extern` object in the same scope | neither set |
| `--warn=return-mismatch` | A `return` with a value in a `void` function, under `--std=c89` or `gnu89` (from C99 on, and under `--pedantic-errors`, it is an error) | on by default |
| `--warn=return-type` | Control that can reach the closing brace of a function returning a value, which C allows while no caller uses the value: the function returns 0 there (an integer or pointer result; a floating or aggregate one is undefined) [E0008] | on by default |
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

GCC's spellings work too: `-WNAME`, `-Wno-NAME`, `-Werror=NAME` ([Migrating from GCC](migrating-from-gcc.md)).
