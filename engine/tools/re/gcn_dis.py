# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gcn_census as gc


def state_code(path):
    data = Path(path).read_bytes()
    runtime_size, ud_count, code_dwords = struct.unpack_from("<III", data, 24)
    at = 48 + runtime_size + ud_count * 4
    return data[at:at + code_dwords * 4]


def reg(code):
    if code < 104:
        return f"s{code}"
    names = {106: "vcc", 107: "vcc_hi", 124: "m0", 126: "exec", 127: "exec_hi", 128: "0", 253: "scc", 255: "lit",
             240: "0.5", 241: "-0.5", 242: "1.0", 243: "-1.0", 244: "2.0", 245: "-2.0", 246: "4.0", 247: "-4.0"}
    if code in names:
        return names[code]
    if 129 <= code <= 192:
        return str(code - 128)
    if 193 <= code <= 208:
        return str(192 - code)
    if code >= 256:
        return f"v{code - 256}"
    return f"?{code}"


def dump(code, names):
    at = 0
    while at + 4 <= len(code):
        w = struct.unpack_from("<I", code, at)[0]
        w2 = struct.unpack_from("<I", code, at + 4)[0] if at + 8 <= len(code) else 0
        enc, op = gc.decode(code[at:at + 8])[0]
        name = names.get(enc, {}).get(op, "?")
        size = 4
        text = ""
        if enc in ("VOP3", "DS", "MUBUF", "MTBUF", "MIMG", "EXP"):
            size = 8
        if enc == "VOP1":
            size = 8 if (w & 0x1ff) == 255 else 4
            text = f"v{(w >> 17) & 255}, {reg(w & 0x1ff)}"
        elif enc == "VOPC":
            size = 8 if (w & 0x1ff) == 255 else 4
            text = f"{reg(w & 0x1ff)}, v{(w >> 9) & 255}"
        elif enc == "VOP2":
            size = 8 if (w & 0x1ff) == 255 or op in (32, 33) else 4
            text = f"v{(w >> 17) & 255}, {reg(w & 0x1ff)}, v{(w >> 9) & 255}"
        elif enc == "SOP1":
            size = 8 if (w & 0xff) == 255 else 4
            text = f"{reg((w >> 16) & 0x7f)}, {reg(w & 0xff)}"
        elif enc == "SOP2":
            size = 8 if (w & 0xff) == 255 or ((w >> 8) & 0xff) == 255 else 4
            text = f"{reg((w >> 16) & 0x7f)}, {reg(w & 0xff)}, {reg((w >> 8) & 0xff)}"
        elif enc == "SOPC":
            size = 8 if (w & 0xff) == 255 or ((w >> 8) & 0xff) == 255 else 4
            text = f"{reg(w & 0xff)}, {reg((w >> 8) & 0xff)}"
        elif enc == "SOPK":
            text = f"{reg((w >> 16) & 0x7f)}, {w & 0xffff:#x}"
        elif enc == "SOPP":
            simm = struct.unpack("<h", struct.pack("<H", w & 0xffff))[0]
            text = f"-> {at + 4 + simm * 4:#x}" if "BRANCH" in name else f"{w & 0xffff:#x}"
        elif enc == "SMRD":
            size = 8 if not (w >> 8) & 1 and (w & 0xff) == 255 else 4
            text = f"s{(w >> 15) & 127}, s{((w >> 9) & 63) * 2}, {w & 255:#x} imm={(w >> 8) & 1}"
        elif enc == "VOP3":
            text = f"d{w & 255} sd{(w >> 8) & 127} {reg(w2 & 511)}, {reg((w2 >> 9) & 511)}, {reg((w2 >> 18) & 511)} abs={(w >> 8) & 7} neg={(w2 >> 29) & 7}"
        elif enc == "VINTRP":
            text = f"v{(w >> 18) & 255}, v{w & 255}, attr{(w >> 10) & 63}.{(w >> 8) & 3}"
        elif enc == "EXP":
            text = f"target={(w >> 4) & 63} en={w & 15} compr={(w >> 10) & 1} done={(w >> 11) & 1} vm={(w >> 12) & 1} v{w2 & 255},v{(w2 >> 8) & 255},v{(w2 >> 16) & 255},v{(w2 >> 24) & 255}"
        elif enc in ("MUBUF", "MTBUF"):
            text = f"off={w & 0xfff} offen={(w >> 12) & 1} idxen={(w >> 13) & 1} glc={(w >> 14) & 1} v{w2 & 255} data=v{(w2 >> 8) & 255} s{((w2 >> 16) & 31) * 4} slc={(w2 >> 22) & 1} soff={reg((w2 >> 24) & 255)}"
        elif enc == "MIMG":
            text = f"dmask={(w >> 8) & 15} v{w2 & 255} data=v{(w2 >> 8) & 255} t=s{((w2 >> 16) & 31) * 4} s=s{((w2 >> 21) & 31) * 4} da={(w >> 14) & 1}"
        elif enc == "DS":
            text = f"off0={w & 255} off1={(w >> 8) & 255} gds={(w >> 17) & 1} addr=v{w2 & 255} d0=v{(w2 >> 8) & 255} d1=v{(w2 >> 16) & 255} vd=v{(w2 >> 24) & 255}"
        print(f"{at:#06x} {name:26} {text}")
        at += size


def main():
    names = gc.load_names(sys.argv[2])
    dump(state_code(sys.argv[1]), names)


if __name__ == "__main__":
    main()
