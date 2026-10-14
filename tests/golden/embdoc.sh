#!/bin/sh
# embdoc: the documentation as a website.
#
# The renderer first, on a fixture that holds every construct docs/ uses:
# headings whose anchors must be GitHub's slugs (or every file.md#section
# link written against GitHub breaks), table alignment, nested and loose
# lists, code and raw HTML escaped -- <stdio.h> and <script> are text --
# .md#anchor links made .html#anchor, other repository files sent to
# GitHub. Then the link checker, on a small tree with one link broken in
# each way it knows, and on the real docs, where it must find nothing.
# Last the site the real docs make: a page per document, the index, the
# search index, and nothing on any page that embdoc did not write.
set -u
echo "TEST-MARKER embdoc"
. "$(dirname "$0")/../lib.sh"
EMBDOC=${EMBDOC:-$EMBCC_ROOT/embdoc}
[ -x "$EMBDOC" ] || { echo "FAIL: $EMBDOC is not built (make embdoc)"; exit 1; }
out=$EMBCC_ROOT/tests/golden/out/embdoc
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
cd "$EMBCC_ROOT" || fail "cannot cd to $EMBCC_ROOT"

# ---- the renderer ------------------------------------------------------
fx=tests/golden/embdoc/fixture.md
"$EMBDOC" md "$fx" > "$out/fixture.html" 2> "$out/fixture.err" ||
    fail "embdoc md exits $?"
[ -s "$out/fixture.err" ] && { cat "$out/fixture.err"; fail "embdoc md warned about the fixture"; }
# the page on one line, so a structure that spans lines is one string
tr '\n' ' ' < "$out/fixture.html" > "$out/fixture.line"
has()   { grep -qF -- "$1" "$out/fixture.line" || fail "$2: no $1"; }
hasnt() { ! grep -qF -- "$1" "$out/fixture.line" || fail "$2: $1"; }

# GitHub's slugs: lower case, punctuation (and the em dash) dropped, each
# space a hyphen, code's text kept, a repeat numbered -- and a -1 that is
# taken already skipped
has '<h1 id="embdoc--the-fixture-every-construct-the-docs-use">' "em dash in a slug"
has '<h2 id="hello-world-t1_-encoders">' "punctuation and code in a slug"
has '<h3 id="-fdiagnostics-colorwhen--fno-diagnostics-color">' "option heading slug"
has '<h3 id="the-calling-convention-gcc-unverified">' "emphasis in a slug"
has '<h3 id="c89c23--stdioh-whats-left">' "en dash, & and <stdio.h> in a slug"
has '<h3 id="d-001-snake_case-and-25-points">' "underscore and dot in a slug"
has '<h2 id="usage">' "first Usage"
has '<h2 id="usage-1">Usage<' "repeated heading"
has '<h2 id="usage-1-1">Usage-1<' "a heading that is a taken slug"
has '<h2 id="closing-hashes">Closing hashes<a' "closing #s"

# inlines
has '<em>emphasis</em>, <em>emphasis</em>, <strong>strong</strong>, <em><strong>both</strong></em>' "emphasis"
has 'snake_case_name, foo_bar_, 2 * 3 * 4, a_b_c and *escaped*' "intraword _ and spaced * are text"
has '<code>inline &lt;code&gt; &amp; &quot;quotes&quot;</code>' "inline code escaped"
has 'hard break<br /> and so are two spaces<br /> at the end' "hard breaks"

# raw HTML is text, comments are dropped, entities are kept
has 'mentions &lt;stdio.h&gt;, &lt;arch&gt; and &lt;b&gt;bold&lt;/b&gt;' "<stdio.h> in prose"
has '&lt;script&gt;alert(1)&lt;/script&gt;' "a script in prose"
has '&lt;img src=x onerror=alert(1)&gt;' "an img in prose"
hasnt '<script' "markup passed through"
hasnt '<img' "markup passed through"
hasnt '<b>' "markup passed through"
hasnt 'hidden note' "an inline comment shown"
hasnt 'UNVERIFIED' "a comment block shown"
has 'AT&amp;T keeps its ampersand; &copy; and &#169;' "entities"
has '<a id="explicit"></a> <h2' "an explicit anchor"

# links: .md#anchor to .html#anchor, repository files to GitHub
gh=https://github.com/EmbLinkOs/EmbCC
has '<a href="#hello-world-t1_-encoders">Same page</a>' "same-page anchor"
has '<a href="../../../docs/manual/targets.html#predefined-macros">' ".md#anchor"
has '<a href="../../../docs/manual/invoking.html">' ".md"
has '<a href="../../../docs/index.html">The contents</a>' "README.md as index.html"
has "<a href=\"$gh/blob/main/src/driver/main.c\">" "a source file"
has "<a href=\"$gh/blob/main/tests/run.sh\">" "a test script"
has "<a href=\"$gh/blob/main/Makefile\">" "the Makefile"
has "<a href=\"$gh/tree/main/src/opt\">" "a directory"
has '<a href="https://example.com/a_b?x=1&amp;y=2" title="A title">An external page</a>' "an external link"
has '<a href="https://example.com/path_(x)">https://example.com/path_(x)</a>, in' "a bare URL"
has '<a href="../../../docs/manual/optimization.html#-o-levels">Code in a link: <code>x &lt; y</code></a>' "code in a link"
grep -q 'href="[^"]*\.md[#"]' "$out/fixture.html" && fail "an href still names a .md"
"$EMBDOC" md "$fx" --repo-url https://example.org/r/ 2>/dev/null |
    grep -qF '<a href="https://example.org/r/blob/main/tests/run.sh">' ||
    fail "--repo-url is not used for repository files"

# lists: nesting by indentation, loose and tight, a block inside an item
has '<li><p>Second, with a nested list:</p> <ul> <li>a bullet <ul> <li>a deeper bullet</li> </ul> </li> <li>another bullet</li> </ul> </li>' "nested lists"
has '<li><p>First, with a continuation line indented to the content.</p>' "a continuation line"
has '<li><p>Third, with a code block inside:</p> <div class="codeblock" data-lang="c"><pre><code class="language-c">int x = a &lt; b &amp;&amp; c &gt; d;' "code in a list item"
has '<ol start="7"> <li>A new list: a different delimiter, starting at 7.</li> <li>Eight.</li> </ol>' "an ordered list's start"
has '<li><p>Loose item two, lazily continued.</p>' "a loose list, a lazy line"
has '<ul> <li>A star list after a dash list is a new list.</li> </ul>' "a new bullet is a new list"

# tables: alignment, markup in cells, \| a pipe in a cell
has '<th style="text-align:left">Left</th> <th style="text-align:center">Centre</th> <th style="text-align:right">Right</th> <th>None</th>' "table alignment"
has '<td style="text-align:left"><code>a | b</code></td> <td style="text-align:center"><strong>bold</strong></td> <td style="text-align:right">1.0</td> <td><a href="#tables">link</a></td>' "table cells"
has '<td style="text-align:left">&lt;x&gt;</td>' "raw HTML in a cell"
has '<td>no outer pipes</td> <td>still a table</td>' "a table without outer pipes"

# code blocks
has '<code class="language-sh">echo &quot;&lt;html&gt;&quot; &amp;&amp; cat a.txt | grep '"'"'&amp;amp;'"'"'' "a sh block escaped"
has '<code class="language-text">a tilde fence, with ``` inside' "a ~~~ fence"
has '<pre><code>a four-backtick fence holding ```c a three-backtick one ``` </code></pre>' "a ```` fence"
has '<pre><code>an indented code block &lt;here&gt; over two lines </code></pre>' "indented code"
has '<blockquote> <p>A quote with <strong>strong</strong> text and a list:</p> <ul> <li>one</li> <li>two</li> </ul> </blockquote> <hr />' "a quote and a rule"

# ...and all of it, byte for byte
cmp -s "$out/fixture.html" tests/golden/embdoc/fixture.html || {
    diff tests/golden/embdoc/fixture.html "$out/fixture.html" | head -20
    fail "the fixture renders differently from tests/golden/embdoc/fixture.html"
}
echo "  the fixture renders as expected"

# ---- the link checker -----------------------------------------------------
t=$out/tree
mkdir -p "$t/docs"
printf '# Tree\n\nSee [a](docs/a.md).\n' > "$t/README.md"
printf '# B\n\n## Target here\n' > "$t/docs/b.md"
cat > "$t/docs/a.md" <<'EOF'
# A

## Section one

[fine](b.md#target-here), [up](../README.md), [here](#section-one),
[bad anchor](b.md#nope), [missing](c.md), [not here](#section-two)
and [out](../../escape.md).
EOF
cat > "$out/check.want" <<'EOF'
docs/a.md:6: b.md#nope -> no anchor #nope in docs/b.md
docs/a.md:6: c.md -> no such file
docs/a.md:6: #section-two -> no anchor #section-two on this page
docs/a.md:7: ../../escape.md -> leaves the repository
EOF
"$EMBDOC" check --root "$t" > "$out/check.got" 2> "$out/check.err"
rc=$?
[ "$rc" -eq 1 ] || fail "embdoc check exits $rc on a tree with broken links, not 1"
cmp -s "$out/check.want" "$out/check.got" || {
    diff "$out/check.want" "$out/check.got"
    fail "embdoc check does not list the broken links as expected"
}
"$EMBDOC" check > "$out/check-docs.txt" 2>&1 || {
    cat "$out/check-docs.txt"
    fail "broken links in the documentation"
}
echo "  check: the tree's 4 broken links found; the docs: $(tail -1 "$out/check-docs.txt" | sed 's/^embdoc: //')"

# ---- the site ---------------------------------------------------------------
site=$out/site
"$EMBDOC" site --out "$site" --base-url /EmbCC/ > "$out/site.log" 2>&1 ||
    { cat "$out/site.log"; fail "embdoc site exits $?"; }
n=0
for f in README.md $(find docs -name '*.md' | sort) tools/*/README.md; do
    [ -f "$f" ] || continue
    case $f in
        README.md|*/README.md) h=${f%README.md}index.html ;;
        *) h=${f%.md}.html ;;
    esac
    [ -f "$site/$h" ] || fail "$f has no page $h"
    grep -qF "/edit/main/$f\"" "$site/$h" || fail "$h has no edit link to $f"
    n=$((n + 1))
done
pages=$(find "$site" -name '*.html' | wc -l | tr -d ' ')
[ "$pages" -eq $((n + 1)) ] ||
    fail "$pages pages for $n documents (one per document and 404.html)"
for f in search-index.json embdoc.css embdoc.js 404.html; do
    [ -s "$site/$f" ] || fail "the site has no $f"
done
# two scripts a page, both embdoc's: the theme's and embdoc.js
scripts=$(cat $(find "$site" -name '*.html') | grep -o '<script' | wc -l | tr -d ' ')
[ "$scripts" -eq $((2 * pages)) ] || fail "$scripts <script> tags in $pages pages"
grep -q 'href="docs/manual/invoking.html"' "$site/index.html" ||
    fail "index.html has no navigation"
grep -q 'href="/EmbCC/index.html"' "$site/404.html" ||
    fail "404.html's links do not start at --base-url"
if command -v python3 > /dev/null 2>&1; then
    python3 - "$site/search-index.json" "$n" <<'PY' || fail "search-index.json is not as it should be"
import json, sys
d = json.load(open(sys.argv[1]))
assert len(d['pages']) == int(sys.argv[2]), (len(d['pages']), sys.argv[2])
assert len(d['sections']) > len(d['pages'])
assert all(0 <= s['p'] < len(d['pages']) for s in d['sections'])
PY
fi
echo "  site: $n documents, $pages pages, $(du -sk "$site" | cut -f1) KB"
