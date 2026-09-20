#!/bin/sh
# embls, the language server (docs/TOOLING.md T5): an editor's questions
# answered by the compiler itself. The session below is a real LSP
# conversation over stdio — initialize, didOpen, completion, hover,
# definition, documentSymbol — and every answer is checked against what the
# front end knows, not against a fixture.
#
# What makes the answers worth having: they come from EmbCC's own
# preprocessor and parser, so the members offered after `.` are the members
# the compiler sees, and the diagnostics shown as you type are the ones the
# build will print.
set -eu
echo "TEST-MARKER embls"
. "$(dirname "$0")/../lib.sh"

command -v python3 >/dev/null 2>&1 || { echo "skipped: python3 absent"; exit 0; }
EMBLS=${EMBLS:-./embls}
[ -x "$EMBLS" ] || { echo "skipped: embls is not built (make embls)"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/embls
rm -rf "$out"; mkdir -p "$out"

cat > "$out/demo.c" << 'EOF'
struct Point { int x; int y; char *label; };
static int total;

int distance2(struct Point a, struct Point b)
{
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    return dx * dx + dy * dy;
}

int main(void)
{
    struct Point origin = { 0, 0, "origin" };
    total = distance2(origin, origin);
    return total;
}
EOF

cat > "$out/broken.c" << 'EOF'
struct Point { int x; int y; };
int area(struct Point p)
{
    int width = p.x;
    int height = p.z;
    return width * heigth;
}
EOF

EMBCC_ABS=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
EMBLS_ABS=$(cd "$(dirname "$EMBLS")" && pwd)/$(basename "$EMBLS")
export EMBCC_ABS EMBLS_ABS

python3 - "$out" << 'PY' || exit 1
import json, os, subprocess, sys
out = sys.argv[1]
srv = subprocess.Popen([os.environ["EMBLS_ABS"]], stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE,
                       env={**os.environ, "EMBLS_EMBCC": os.environ["EMBCC_ABS"]})

def send(o):
    b = json.dumps(o).encode()
    srv.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
    srv.stdin.flush()

def recv():
    hdr = b""
    while b"\r\n\r\n" not in hdr:
        c = srv.stdout.read(1)
        if not c:
            raise SystemExit("embls closed the connection")
        hdr += c
    n = int([l for l in hdr.decode().split("\r\n")
             if l.lower().startswith("content-length")][0].split(":")[1])
    return json.loads(srv.stdout.read(n))

def open_doc(path):
    uri = "file://" + os.path.abspath(path)
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
          "params": {"textDocument": {"uri": uri, "text": open(path).read()}}})
    return uri, recv()["params"]["diagnostics"]

def ask(method, uri, line, ch):
    send({"jsonrpc": "2.0", "id": 7, "method": method,
          "params": {"textDocument": {"uri": uri},
                     "position": {"line": line, "character": ch}}})
    return recv()["result"]

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
caps = recv()["result"]["capabilities"]
for c in ("completionProvider", "hoverProvider", "definitionProvider",
          "documentSymbolProvider"):
    assert caps.get(c), ("missing capability", c)
print("initialize: completion, hover, definition, symbols")

# A file that compiles has nothing to say about it.
uri, diags = open_doc(out + "/demo.c")
assert diags == [], diags
print("a correct file: no diagnostics")

# Completion after `a.` is the struct's own members, in order, and nothing
# else -- not locals, not keywords.
items = [i["label"] for i in ask("textDocument/completion", uri, 5, 15)["items"]]
assert items == ["x", "y", "label"], items
details = {i["label"]: i["detail"]
           for i in ask("textDocument/completion", uri, 5, 15)["items"]}
assert details["label"] == "char *", details
print("completion after `.`: %s (with types)" % ", ".join(items))

# In a body: the function's own parameters and locals are offered, another
# function's locals are not.
labels = [i["label"] for i in ask("textDocument/completion", uri, 6, 8)["items"]]
for want in ("a", "b", "dx", "dy", "distance2", "total", "struct"):
    assert want in labels, (want, labels[:20])
assert "origin" not in labels, "a local of main is offered inside distance2"
print("completion in a body: parameters, locals, globals, keywords; "
      "another function's locals stay out")

# Hover and definition answer from the parse, not from the text.
h = ask("textDocument/hover", uri, 13, 14)["contents"]["value"]
assert "int distance2(struct Point a, struct Point b)" in h, h
d = ask("textDocument/definition", uri, 13, 14)
assert d["range"]["start"]["line"] == 3, d      # 0-based: the definition
print("hover: the real signature;  definition: its line")

syms = [s["name"] for s in ask("textDocument/documentSymbol", uri, 0, 0)]
assert "distance2" in syms and "main" in syms and "total" in syms, syms
print("documentSymbol: %s" % ", ".join(syms))

# A file with two mistakes reports both, where they are, with the
# suggestion under the second.
uri2, diags2 = open_doc(out + "/broken.c")
lines = sorted(d["range"]["start"]["line"] + 1 for d in diags2)
assert lines == [5, 6], lines
msgs = " ".join(d["message"] for d in diags2)
assert "has no member 'z'" in msgs, msgs
assert "'heigth' is not declared" in msgs, msgs
assert "did you mean 'height'?" in msgs, msgs
assert all(d["severity"] == 1 for d in diags2), diags2
print("a broken file: both errors, at their lines, with the suggestion")

# Completion still works there -- an editor is most useful while the file
# is still wrong.
items2 = [i["label"] for i in ask("textDocument/completion", uri2, 4, 19)["items"]]
assert items2 == ["x", "y"], items2
print("completion while the file is broken: %s" % ", ".join(items2))

send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
recv()
send({"jsonrpc": "2.0", "method": "exit", "params": {}})
srv.wait(timeout=10)
PY
echo "embls answered a whole session"
