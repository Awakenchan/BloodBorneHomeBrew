# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

GCN_FIRST = b"\xff\x03\xeb\xbe"
FOOTER = b"OrbShdr"


def undcx(data):
    if data[:4] != b"DCX\0":
        return data
    at = data.find(b"DCA\0")
    if at < 0:
        raise ValueError("no DCA block")
    size = struct.unpack(">I", data[at + 4:at + 8])[0]
    return zlib.decompress(data[at + size:])


def bnd4_entries(data):
    if data[:4] != b"BND4":
        return
    count = struct.unpack_from("<I", data, 0x0c)[0]
    header_size = struct.unpack_from("<Q", data, 0x10)[0]
    entry_size = struct.unpack_from("<Q", data, 0x20)[0]
    unicode_names = data[0x30] != 0
    for i in range(count):
        at = header_size + i * entry_size
        csize = struct.unpack_from("<Q", data, at + 8)[0]
        usize = struct.unpack_from("<Q", data, at + 0x10)[0]
        if entry_size >= 0x24:
            offset = struct.unpack_from("<I", data, at + 0x18)[0]
            name_at = struct.unpack_from("<I", data, at + 0x20)[0]
        else:
            offset = struct.unpack_from("<I", data, at + 0x18)[0]
            name_at = struct.unpack_from("<I", data, at + 0x1c)[0]
        if unicode_names:
            end = name_at
            while data[end:end + 2] != b"\0\0":
                end += 2
            name = data[name_at:end].decode("utf-16-le")
        else:
            name = data[name_at:data.index(b"\0", name_at)].decode("cp932")
        yield name, data[offset:offset + csize]


def walk_container(name, data):
    data = undcx(data)
    if data[:4] == b"BND4":
        for sub, blob in bnd4_entries(data):
            yield from walk_container(name + "/" + sub.replace("\\", "/").split("/")[-1], blob)
    else:
        yield name, data


def find_programs(data):
    out = []
    at = 0
    while True:
        at = data.find(FOOTER, at)
        if at < 0:
            return out
        length = struct.unpack_from("<I", data, at + 8)[0] >> 8
        shader_hash = struct.unpack_from("<Q", data, at + 16)[0]
        kind = (struct.unpack_from("<I", data, at + 8)[0] >> 2) & 15
        first = data.rfind(GCN_FIRST, 0, at)
        if first >= 0:
            out.append((first, first + length, shader_hash, kind))
        at += 7


def program_hash(code):
    return hashlib.sha1(code).hexdigest()


def read_state(path):
    data = path.read_bytes()
    if data[:4] != b"BBS1":
        return None
    stage_hw, stage_sw, shader_hash = struct.unpack_from("<IIQ", data, 8)
    runtime_size, ud_count, code_dwords = struct.unpack_from("<III", data, 24)
    at = 48 + runtime_size + ud_count * 4
    return stage_hw, shader_hash, data[at:at + code_dwords * 4]


def main():
    ap = argparse.ArgumentParser(description="Lists the GCN programs in the game's shader bundles")
    ap.add_argument("game", help="game folder, CUSA03173")
    ap.add_argument("--scan-all", action="store_true", help="also scan every .dcx and the eboot")
    ap.add_argument("--scan-code", action="store_true", help="also scan the eboot and the modules")
    ap.add_argument("--states", help="cache folder: every .bbs must be found in the game's files")
    ap.add_argument("--out", help="folder to write the programs to (game data, keep it out of git)")
    ap.add_argument("--json", help="write the table to this file")
    args = ap.parse_args()

    root = Path(args.game)
    files = sorted((root / "dvdroot_ps4/shader").glob("*"))
    table = {}
    entries = 0
    per_ext = {}
    for path in files:
        for name, blob in walk_container(path.name, path.read_bytes()):
            entries += 1
            for start, end, shader_hash, kind in find_programs(blob):
                ext = name.rsplit(".", 1)[-1]
                per_ext[ext] = per_ext.get(ext, 0) + 1
                entry = table.setdefault(shader_hash, {"source": name, "dwords": (end - start) // 4,
                                                       "ext": ext, "kind": kind, "copies": 0,
                                                       "sha": program_hash(blob[start:end])})
                entry["copies"] += 1
                if args.out:
                    out = Path(args.out)
                    out.mkdir(parents=True, exist_ok=True)
                    (out / f"{shader_hash:016x}.{ext}").write_bytes(blob[start:end])
    print(f"{len(files)} bundles, {entries} files inside, {sum(per_ext.values())} programs, "
          f"{len(table)} distinct by the shader hash, {per_ext}")
    counts = {}
    for entry in table.values():
        counts[entry["ext"]] = counts.get(entry["ext"], 0) + 1
    print("distinct per kind of file:", counts, "instructions dwords:",
          sum(e["dwords"] for e in table.values()))

    other = {}
    if args.scan_all or args.scan_code:
        eboot = root / "eboot.elf"
        scan = [eboot] if eboot.exists() else []
        scan += sorted((root / "sce_module").glob("*"))
        if args.scan_all:
            scan += [p for p in (root / "dvdroot_ps4").rglob("*.dcx") if "shader" not in p.parts]
        for index, path in enumerate(scan):
            try:
                blobs = list(walk_container(path.name, path.read_bytes()))
            except Exception as error:
                print(f"  {path}: {error}", file=sys.stderr)
                continue
            for name, blob in blobs:
                for start, end, shader_hash, kind in find_programs(blob):
                    if shader_hash not in table:
                        other.setdefault(shader_hash, {"source": name, "offset": start,
                                                       "dwords": (end - start) // 4, "kind": kind,
                                                       "sha": program_hash(blob[start:end])})
                        if args.out:
                            (Path(args.out) / f"{shader_hash:016x}.elf").write_bytes(blob[start:end])
            if index % 200 == 0:
                print(f"  {index} of {len(scan)} files, {len(other)} programs elsewhere", flush=True)
        print(f"{len(other)} further programs outside the bundles")
        for key, entry in list(other.items())[:80]:
            print(f"  {key:016x} {entry}")

    if args.states:
        missing = []
        different = 0
        programs = {}
        for path in sorted(Path(args.states).glob("*.bbs")):
            state = read_state(path)
            if state is not None:
                programs[state[1]] = (path.name, state[0], state[2])
        for shader_hash, (name, stage, code) in programs.items():
            entry = table.get(shader_hash) or other.get(shader_hash)
            if entry is None:
                missing.append((name, stage, code))
            elif entry["sha"] != program_hash(code):
                different += 1
        print(f"{len(programs)} distinct programs in the states, {len(missing)} not in the files, "
              f"{different} with other code under the same hash")
        for name, stage, code in missing[:60]:
            print(f"  {name} stage {stage} {len(code) // 4} dwords")

    if args.json:
        Path(args.json).write_text(json.dumps(table, indent=0))


if __name__ == "__main__":
    main()
