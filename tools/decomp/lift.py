#!/usr/bin/env python3
"""从 Ghidra 反编译转储（*.so.c）里按名字/行号抽取函数体与交叉定位。

用法:
  python tools/decomp/lift.py <dump.c> --find REGEX            列出命中的函数名与行号
  python tools/decomp/lift.py <dump.c> --name FUN_0030e70c     打印该函数体
  python tools/decomp/lift.py <dump.c> --at 273150             打印该行所在函数
  python tools/decomp/lift.py <dump.c> --find REGEX --body     命中即连函数体一起打印
"""
import argparse
import io
import re
import sys

SIG = re.compile(r"^[A-Za-z_][\w \t\*]*\b([\w]+)\s*\(")
END = re.compile(r"^\}\s*$")


def load(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().split("\n")


def index(lines):
    """返回 [(start, end, name)]，按 start 升序。"""
    funcs = []
    i = 0
    n = len(lines)
    while i < n:
        m = SIG.match(lines[i])
        if m and not lines[i].startswith(("typedef", "struct", "union", "enum", "//")):
            j = i + 1
            while j < n and not END.match(lines[j]):
                j += 1
            funcs.append((i, j, m.group(1)))
            i = j
        i += 1
    return funcs


def enclosing(funcs, lineno):
    lo, hi = 0, len(funcs) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        s, e, name = funcs[mid]
        if s <= lineno <= e:
            return funcs[mid]
        if lineno < s:
            hi = mid - 1
        else:
            lo = mid + 1
    return best


def show(lines, func, out):
    s, e, name = func
    out.write("/* ===== %s  L%d-%d ===== */\n" % (name, s + 1, e + 1))
    out.write("\n".join(lines[s:e + 1]))
    out.write("\n\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--find")
    ap.add_argument("--name")
    ap.add_argument("--at", type=int)
    ap.add_argument("--body", action="store_true")
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument("--list", action="store_true", help="列出全部函数名")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    lines = load(a.dump)
    funcs = index(lines)

    if a.list:
        for s, e, name in funcs:
            print("%d\t%s" % (s + 1, name))
        return
    if a.name:
        for s, e, name in funcs:
            if name == a.name:
                show(lines, (s, e, name), sys.stdout)
        return
    if a.at:
        f = enclosing(funcs, a.at - 1)
        if f:
            show(lines, f, sys.stdout)
        else:
            print("第 %d 行不在任何函数内" % a.at)
        return
    if a.find:
        rx = re.compile(a.find)
        hits = 0
        seen = set()
        for idx, line in enumerate(lines):
            if not rx.search(line):
                continue
            f = enclosing(funcs, idx)
            key = f[2] if f else ("inline@%d" % (idx + 1))
            if key in seen and not a.body:
                continue
            seen.add(key)
            hits += 1
            if hits > a.limit:
                break
            if f and a.body:
                show(lines, f, sys.stdout)
            else:
                print("L%d [%s] %s" % (idx + 1, key, line.strip()[:160]))
        print("-- 命中函数 %d 个（上限 %d）" % (min(hits, a.limit), a.limit), file=sys.stderr)


if __name__ == "__main__":
    main()
