#!/usr/bin/env python3
"""Line-based delta reduction.

usage: reduce.py FILE.c TEST.sh
TEST.sh is run with the candidate file's path as $1; exit 0 means
"still interesting". The candidate must also parse (clang -fsyntax-only).
The reduced file is written to FILE.c.min.c.
"""
import subprocess
import sys


def interesting(lines, test, path):
    with open(path, "w") as f:
        f.write("".join(lines))
    if subprocess.run(["clang", "-fsyntax-only", "-w", path],
                      capture_output=True).returncode != 0:
        return False
    return subprocess.run(["sh", test, path], capture_output=True).returncode == 0


def main():
    src, test = sys.argv[1], sys.argv[2]
    lines = open(src).read().splitlines(keepends=True)
    cand = src + ".cand.c"
    if not interesting(lines, test, cand):
        sys.exit("the original is not interesting")
    n = max(1, len(lines) // 2)
    while True:
        changed = False
        i = 0
        while i < len(lines):
            trial = lines[:i] + lines[i + n:]
            if any("/*K*/" in l for l in lines[i:i + n]):
                i += n                    # a line the program needs defined
                continue
            if trial and interesting(trial, test, cand):
                lines = trial
                changed = True
            else:
                i += n
        if n == 1 and not changed:
            break
        if not changed:
            n = max(1, n // 2)
    with open(src + ".min.c", "w") as f:
        f.write("".join(lines))
    print("reduced to %d lines" % len(lines))


if __name__ == "__main__":
    main()
