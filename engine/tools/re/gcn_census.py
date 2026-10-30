# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import re
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import shader_files as sf


def load_names(path):
    names = {}
    text = Path(path).read_text()
    for match in re.finditer(r"enum class Opcode(\w+) : u32 \{(.*?)\};", text, re.S):
        table = {}
        for name, value in re.findall(r"(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)", match.group(2)):
            table[int(value, 0)] = name
        names[match.group(1)] = table
    return names


def decode(code):
    at = 0
    out = []
    while at + 4 <= len(code):
        word = struct.unpack_from("<I", code, at)[0]
        top = word >> 23
        size = 4
        if word >> 25 == 0x3f:
            enc, op = "VOP1", (word >> 9) & 0xff
            if (word & 0x1ff) == 255:
                size = 8
        elif word >> 25 == 0x3e:
            enc, op = "VOPC", (word >> 17) & 0xff
            if (word & 0x1ff) == 255:
                size = 8
        elif word >> 31 == 0:
            enc, op = "VOP2", (word >> 25) & 0x3f
            if (word & 0x1ff) == 255 or op in (32, 33):
                size = 8
        elif top == 0x17d:
            enc, op = "SOP1", (word >> 8) & 0xff
            if (word & 0xff) == 255:
                size = 8
        elif top == 0x17e:
            enc, op = "SOPC", (word >> 16) & 0x7f
            if (word & 0xff) == 255 or ((word >> 8) & 0xff) == 255:
                size = 8
        elif top == 0x17f:
            enc, op = "SOPP", (word >> 16) & 0x7f
        elif word >> 28 == 0xb:
            enc, op = "SOPK", (word >> 23) & 0x1f
        elif word >> 30 == 2:
            enc, op = "SOP2", (word >> 23) & 0x7f
            if (word & 0xff) == 255 or ((word >> 8) & 0xff) == 255:
                size = 8
        elif word >> 27 == 0x18:
            enc, op = "SMRD", (word >> 22) & 0x1f
            if not (word >> 8) & 1 and (word & 0xff) == 255:
                size = 8
        elif word >> 26 == 0x34:
            enc, op = "VOP3", (word >> 17) & 0x1ff
            size = 8
        elif word >> 26 == 0x32:
            enc, op = "VINTRP", (word >> 16) & 3
        elif word >> 26 == 0x36:
            enc, op = "DS", (word >> 18) & 0xff
            size = 8
        elif word >> 26 == 0x38:
            enc, op = "MUBUF", (word >> 18) & 0x7f
            size = 8
        elif word >> 26 == 0x3a:
            enc, op = "MTBUF", (word >> 16) & 7
            size = 8
        elif word >> 26 == 0x3c:
            enc, op = "MIMG", (word >> 18) & 0x7f
            size = 8
        elif word >> 26 == 0x3e:
            enc, op = "EXP", 0
            size = 8
        else:
            raise ValueError(f"unknown word {word:08x} at {at}")
        out.append((enc, op))
        at += size
    return out


def main():
    ap = argparse.ArgumentParser(description="Which GCN instructions the game's shaders use")
    ap.add_argument("game")
    ap.add_argument("opcodes", help="an opcode name table")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    names = load_names(args.opcodes)
    root = Path(args.game)
    seen = {}
    for path in sorted((root / "dvdroot_ps4/shader").glob("*")):
        for name, blob in sf.walk_container(path.name, path.read_bytes()):
            for start, end, shader_hash, kind in sf.find_programs(blob):
                seen.setdefault(shader_hash, blob[start:end])
    eboot = root / "eboot.elf"
    blob = eboot.read_bytes()
    for start, end, shader_hash, kind in sf.find_programs(blob):
        seen.setdefault(shader_hash, blob[start:end])
    counts = Counter()
    per_program = Counter()
    failed = 0
    for shader_hash, code in seen.items():
        try:
            insts = decode(code)
        except ValueError as error:
            failed += 1
            print(f"{shader_hash:016x}: {error}", file=sys.stderr)
            continue
        counts.update(insts)
        per_program.update(set(insts))
    print(f"{len(seen)} programs, {failed} failed to decode, {sum(counts.values())} instructions, "
          f"{len(counts)} distinct")
    if args.list:
        for key in sorted(counts):
            enc, op = key
            table = names.get(enc, {})
            if enc == "VOP3":
                table = names.get("VOP3", {})
            print(f"{enc:7} {op:4} {table.get(op, '?'):28} {counts[key]:8} {per_program[key]:6}")


if __name__ == "__main__":
    main()
