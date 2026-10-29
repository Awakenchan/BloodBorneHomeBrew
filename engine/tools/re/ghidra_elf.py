#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
import struct
from pathlib import Path

import numpy as np

from eboot import Eboot

PT_LOAD, PT_NULL = 1, 0
R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE = 1, 6, 7, 8

def dynamic_tags(raw, off):
    tags = {}
    for i in range(4096):
        t, v = struct.unpack_from('<qQ', raw, off + 16 * i)
        if t == 0:
            break
        tags.setdefault(t, v)
    return tags

def import_names(root):
    text = (root / 'src/import_names.inc').read_text()
    return {nid.split('#')[0]: name for nid, name in re.findall(r'\{"([^"]+)","([^"]+)"\}', text)}

def build(eboot_path, out_dir, root):
    e = Eboot(eboot_path)
    raw = bytearray(e.raw)
    phoff, = struct.unpack_from('<Q', raw, 0x20)
    phnum, = struct.unpack_from('<H', raw, 0x38)
    dynamic = dynlib = None
    for i in range(phnum):
        kind, = struct.unpack_from('<I', raw, phoff + 56 * i)
        off, = struct.unpack_from('<Q', raw, phoff + 56 * i + 8)
        if kind == 2:
            dynamic = off
        elif kind == 0x61000000:
            dynlib = off
        if kind != PT_LOAD:
            struct.pack_into('<I', raw, phoff + 56 * i, PT_NULL)
    struct.pack_into('<H', raw, 0x10, 2)
    struct.pack_into('<B', raw, 0x07, 0)

    tags = dynamic_tags(raw, dynamic)
    strtab = dynlib + tags[0x61000035]
    symtab = dynlib + tags[0x61000039]
    nsyms = tags[0x6100003f] // 24
    known = import_names(root)
    symbols = []
    for i in range(nsyms):
        name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', raw, symtab + 24 * i)
        nid = bytes(raw[strtab + name:strtab + name + 128]).split(b'\0')[0].decode('latin-1')
        base = nid.split('#')[0]
        symbols.append((nid, known.get(base), shndx, value))

    def file_offset(va):
        for sva, off, filesz, _, _ in e.loads:
            if sva <= va and va + 8 <= sva + filesz:
                return off + va - sva
        return None

    labels = []
    patched = 0
    for va, value in e.relative.items():
        off = file_offset(va)
        if off is not None:
            struct.pack_into('<Q', raw, off, value)
            patched += 1

    def label_for(index):
        nid, name, shndx, value = symbols[index]
        safe = re.sub(r'[^A-Za-z0-9_]', '_', nid.split('#')[0])
        return nid, name or f'imp_{safe}', shndx, value

    tables = [(tags[0x6100002f], tags[0x61000031]), (tags[0x61000029], tags[0x6100002d])]
    got = {}
    for start, size in tables:
        table = np.frombuffer(e.raw, '<u8', size // 8, dynlib + start).reshape(-1, 3)
        for va, info, addend in table.tolist():
            kind, index = info & 0xffffffff, info >> 32
            if kind not in (R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT):
                continue
            nid, name, shndx, value = label_for(index)
            if shndx and value:
                off = file_offset(va)
                if off is not None:
                    struct.pack_into('<Q', raw, off, (value + addend) & (1 << 64) - 1)
                    patched += 1
                continue
            got[va] = name
            labels.append((va, 'slot', name, nid))

    text = e.text
    pos = np.nonzero((text[:-6] == 0xff) & (text[1:-5] == 0x25))[0]
    disp = text[pos + 2].astype(np.int64) | (text[pos + 3].astype(np.int64) << 8) | \
        (text[pos + 4].astype(np.int64) << 16) | (text[pos + 5].astype(np.int64) << 24)
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    stub_va = pos.astype(np.int64) + e.text_va
    dest = stub_va + 6 + disp
    stubs = 0
    for s, d in zip(stub_va.tolist(), dest.tolist()):
        if d in got:
            labels.append((s, 'stub', got[d], ''))
            stubs += 1

    for nid, name, shndx, value in symbols:
        if shndx and value:
            labels.append((value, 'export', name or re.sub(r'[^A-Za-z0-9_]', '_', nid.split('#')[0]), nid))

    out_dir.mkdir(parents=True, exist_ok=True)
    elf = out_dir / 'eboot_ghidra.elf'
    elf.write_bytes(bytes(raw))
    with open(out_dir / 'functions.csv', 'w') as f:
        for va, size in e.funcs:
            f.write(f'{va:x},{size:x}\n')
    with open(out_dir / 'labels.csv', 'w') as f:
        for va, kind, name, nid in sorted(labels):
            f.write(f'{va:x},{kind},{name},{nid}\n')
    named = sum(1 for _, n, _, _ in symbols if n)
    print(f'{elf}: {len(e.funcs)} functions, {patched} pointers relocated, '
          f'{len(got)} import slots, {stubs} stubs, {named}/{len(symbols)} symbols named')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('eboot')
    ap.add_argument('out_dir')
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    a = ap.parse_args()
    build(a.eboot, Path(a.out_dir), Path(a.root))
