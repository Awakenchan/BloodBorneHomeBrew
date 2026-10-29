#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import os
import re
from pathlib import Path

def find(dump, va):
    path = Path(dump) / f'{va & ~0xfffff:07x}.c'
    if not path.exists():
        return None
    text = path.read_text(encoding='utf-8', errors='replace')
    m = re.search(rf'^// {va:x} \S+\n', text, re.M)
    if not m:
        return None
    end = re.search(r'^// [0-9a-f]+ \S+\n', text[m.end():], re.M)
    return text[m.start():m.end() + end.start() if end else len(text)]

def callees(body):
    return sorted({int(x, 16) for x in re.findall(r'FUN_([0-9a-f]{8})', body)})

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('address', nargs='+')
    ap.add_argument('--dump', default=os.environ.get('LANTERN_DUMP', 'E:/re-tools/work/dump'))
    ap.add_argument('--depth', type=int, default=0)
    a = ap.parse_args()
    seen = set()
    todo = [(int(x, 16), 0) for x in a.address]
    while todo:
        va, depth = todo.pop(0)
        if va in seen:
            continue
        seen.add(va)
        body = find(a.dump, va)
        print(body if body else f'// {va:x} not in the dump\n')
        if body and depth < a.depth:
            todo += [(c, depth + 1) for c in callees(body) if c not in seen]
