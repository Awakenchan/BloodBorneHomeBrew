#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import sys
import re
import struct
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

def imports(game, modules):
    sys.path.insert(0, str(ROOT / 'scripts'))
    from link_modules import module
    loaded = [('eboot.bin', module(game / 'eboot.bin'))] + [(m, module(game / 'sce_module' / m)) for m in modules]
    exported = set()
    for _, m in loaded[1:]:
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity']:
                exported.add((s['identity'][0], s['identity'][1][0]))
    out = {}
    for filename, m in loaded:
        for s in m['symbols']:
            if s['section'] or not s['identity']:
                continue
            nid, (library, _), _ = s['identity']
            if (nid, library) in exported:
                continue
            out.setdefault(nid, (library, set()))[1].add(filename)
    return out

def names():
    text = (ROOT / 'src/import_names.inc').read_text()
    return {nid.split('#')[0]: sym for nid, sym in re.findall(r'\{"([^"]+)","([^"]+)"\}', text)}

def providers():
    found = defaultdict(list)
    sym_files = defaultdict(list)
    for path in sorted((ROOT / 'src/sys').glob('*.c')):
        if path.name == 'net_offline.c':
            continue
        for nid in re.findall(r'\{"([A-Za-z0-9+\-]{11})",\s*"', path.read_text(encoding='utf-8')):
            found[nid].append(path.relative_to(ROOT).as_posix())
    for path in sorted((ROOT / 'gpu').rglob('*.cpp')):
        if 'third_party' in path.parts:
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        for nid in re.findall(r'LIB_FUNCTION\(\s*"([A-Za-z0-9+\-]{11})"', text):
            found[nid].append(path.relative_to(ROOT).as_posix())
    return found, sym_files

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', nargs='?', default='E:/bloodbrone/CUSA03173')
    ap.add_argument('--modules', default='libc.prx,libSceFios2.prx')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    imp = imports(Path(a.game), [m for m in a.modules.split(',') if m])
    sym = names()
    found, sym_files = providers()
    table = defaultdict(lambda: defaultdict(list))
    for nid, (lib, users) in sorted(imp.items(), key=lambda x: (x[1][0], sym.get(x[0], x[0]))):
        name = sym.get(nid, nid)
        where = found.get(nid) or sym_files.get(name) or ['unresolved']
        table[lib][where[0]].append(name if users == {'eboot.bin'} else f"{name} ({', '.join(sorted(users))})")
    total = sum(len(v) for f in table.values() for v in f.values())
    print(f'{total} imports')
    for lib in sorted(table, key=lambda l: -sum(len(v) for v in table[l].values())):
        files = table[lib]
        print(f'\n{lib}: {sum(len(v) for v in files.values())}')
        for f, fns in sorted(files.items(), key=lambda x: -len(x[1])):
            print(f'  {len(fns):4} {f}' + (': ' + ', '.join(fns) if a.list else ''))

if __name__ == '__main__':
    main()
