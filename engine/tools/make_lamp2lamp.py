# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import os
import struct
import sys
import zlib

ANCHOR = 16
MINIMUM = 24


def unpack(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"DCX\0":
        raise SystemExit(f"not a DCX file: {path}")
    dca = raw.find(b"DCA\0")
    size = struct.unpack(">I", raw[dca + 4:dca + 8])[0]
    return zlib.decompress(raw[dca + size:])


def operations(base, target):
    index = {}
    for at in range(0, max(len(base) - ANCHOR, 0), 4):
        index.setdefault(base[at:at + ANCHOR], []).append(at)
    result = []
    pending = bytearray()
    at = 0
    following = 0
    while at < len(target):
        best, best_length = -1, 0
        candidates = [following] if following + ANCHOR <= len(base) and base[following:following + ANCHOR] == target[at:at + ANCHOR] else []
        candidates += index.get(target[at:at + ANCHOR], [])[:64]
        for start in candidates:
            length = ANCHOR
            while start + length < len(base) and at + length < len(target) and base[start + length] == target[at + length]:
                length += 1
            if length > best_length:
                best, best_length = start, length
        if best_length >= MINIMUM:
            if pending:
                result.append(("insert", bytes(pending)))
                pending.clear()
            result.append(("copy", best, best_length))
            at += best_length
            following = best + best_length
        else:
            pending.append(target[at])
            at += 1
    if pending:
        result.append(("insert", bytes(pending)))
    return result


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: make_lamp2lamp.py <mod folder> <game folder> <output>")
    mod, game, output = sys.argv[1:]
    rows = []
    for root, _, files in os.walk(mod):
        for name in files:
            if name.endswith(".dcx"):
                rows.append(os.path.relpath(os.path.join(root, name), mod).replace("\\", "/"))
    rows.sort()
    body = bytearray(struct.pack("<I", len(rows)))
    added = 0
    for relative in rows:
        base = unpack(os.path.join(game, relative))
        target = unpack(os.path.join(mod, relative))
        ops = operations(base, target)
        name = relative.encode()
        body += struct.pack("<H", len(name)) + name
        body += hashlib.sha256(base).digest() + hashlib.sha256(target).digest()
        body += struct.pack("<II", len(target), len(ops))
        inserted = 0
        for op in ops:
            if op[0] == "copy":
                body += struct.pack("<II", op[1], op[2])
            else:
                body += struct.pack("<II", 0xFFFFFFFF, len(op[1])) + op[1]
                inserted += len(op[1])
        added += inserted
        print(f"{relative}: {len(target)} bytes, {len(ops)} ops, {inserted} new")
    packed = zlib.compress(bytes(body), 9)
    with open(output, "wb") as out:
        out.write(b"L2LP" + struct.pack("<I", len(body)) + packed)
    print(f"{len(rows)} files, {added} new bytes, {len(packed) + 8} bytes written to {output}")


main()
