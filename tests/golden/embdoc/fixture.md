# embdoc — the fixture: every construct the docs use

A paragraph with a soft
line break, *emphasis*, _emphasis_, **strong**, ***both***, and
`inline <code> & "quotes"`. Not emphasis: snake_case_name, foo_bar_, 2 * 3 * 4,
a_b_c and \*escaped\*. A backslash at the end is a hard break\
and so are two spaces  
at the end of a line.

Prose that mentions <stdio.h>, <arch> and <b>bold</b> shows the angle
brackets; <script>alert(1)</script> is text, never a script, and so is
<img src=x onerror=alert(1)>. AT&T keeps its ampersand; &copy; and &#169;
are entities. A comment is not shown: <!-- hidden note --> here.

<!-- UNVERIFIED: a comment block,
     over two lines, which GitHub does not show either -->

<a id="explicit"></a>
## Hello, World! (`t1_*` encoders)

### `-fdiagnostics-color[=WHEN]`, `-fno-diagnostics-color`

### The calling convention *(GCC, unverified)*

### C89–C23 & <stdio.h>: what's left

### D-001: Snake_Case and 2.5 points

## Usage

## Usage

## Usage-1

## Closing hashes ##

## Links

- [Same page](#hello-world-t1_-encoders), [an explicit anchor](#explicit)
  and [a duplicate heading](#usage-1).
- [Another document](../../../docs/manual/targets.md#predefined-macros)
  and [its top](../../../docs/manual/invoking.md).
- [The contents](../../../docs/README.md), whose page is an index.html.
- [A source file](../../../src/driver/main.c),
  [a test script](../../run.sh), [the Makefile](../../../Makefile) and
  [a directory](../../../src/opt/).
- [An external page](https://example.com/a_b?x=1&y=2 "A title") and a bare
  URL, https://example.com/path_(x), in a sentence.
- [Code in a link: `x < y`](../../../docs/manual/optimization.md#-o-levels).

## Lists

1. First, with a continuation
   line indented to the content.
2. Second, with a nested list:
   - a bullet
     - a deeper bullet
   - another bullet
3. Third, with a code block inside:

   ```c
   int x = a < b && c > d;
   ```

7) A new list: a different delimiter, starting at 7.
8) Eight.

- Loose item one.

- Loose item two,
lazily continued.

* A star list after a dash list is a new list.

## Tables

| Left | Centre | Right | None |
|:-----|:------:|------:|------|
| `a \| b` | **bold** | 1.0 | [link](#tables) |
| <x> | _em_ | 22 | |

Name | Value
--- | ---
no outer pipes | still a table

## Code

```sh
echo "<html>" && cat a.txt | grep '&amp;'
```

~~~text
a tilde fence, with ``` inside
~~~

````
a four-backtick fence holding
```c
a three-backtick one
```
````

    an indented code block <here>
    over two lines

## Quotes and breaks

> A quote with **strong** text
> and a list:
>
> - one
> - two

---

The end.
