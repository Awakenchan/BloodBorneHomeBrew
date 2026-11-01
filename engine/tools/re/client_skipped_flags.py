#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Flags a guest misses because event scripts jump over them on a client.

Usage: client_skipped_flags.py <game folder>/dvdroot_ps4/event <progress_flags.inc>

An event with 1003[105] (jump by multiplayer state) skips what follows on a client, so a guest
never sets those flags itself: shared progress has to carry them. Lists every flag set after that
jump that the shared list does not hold. The files are read as in emevd_trophies.py.
"""
import glob
import os
import re
import struct
import sys
import zlib


def events(path):
    d = zlib.decompress(open(path, 'rb').read()[0x4c:])
    count, at, _, instructions_at = struct.unpack_from('<4Q', d, 0x10)
    args_at = struct.unpack_from('<Q', d, 0x10 + 13 * 8)[0]
    for e in range(count):
        event, n, first = struct.unpack_from('<3Q', d, at + 0x30 * e)
        rows = []
        for k in range(n):
            bank, ident, size, offset = struct.unpack_from('<IIQq', d, instructions_at + first + 0x20 * k)
            arg = d[args_at + offset:args_at + offset + size]
            rows.append((bank, ident, arg))
        yield event, rows


def main():
    text = open(sys.argv[2]).read()
    ranges = [(int(a), int(b)) for a, b, _ in re.findall(r'\{(\d+)u,\s*(\d+)u,\s*(\d+)\}', text)]
    shared = lambda flag: any(a <= flag <= b for a, b in ranges)
    missing = {}
    checked = 0
    for path in sorted(glob.glob(os.path.join(sys.argv[1], '*.emevd.dcx'))):
        name = os.path.basename(path).split('.')[0]
        for event, rows in events(path):
            jump = next((i for i, r in enumerate(rows) if r[:2] == (1003, 105)), None)
            if jump is None:
                continue
            checked += 1
            for bank, ident, arg in rows[jump:]:
                if (bank, ident) == (2003, 2) and len(arg) >= 5 and arg[4]:
                    flag = struct.unpack_from('<i', arg)[0]
                    if flag > 0 and not shared(flag):
                        missing.setdefault(flag, []).append(f'{name}:{event}')
    print(f'{checked} events with a client jump, {len(missing)} flag(s) set after it not shared')
    for flag in sorted(missing):
        print(flag, ' '.join(missing[flag][:3]))


if __name__ == '__main__':
    main()
