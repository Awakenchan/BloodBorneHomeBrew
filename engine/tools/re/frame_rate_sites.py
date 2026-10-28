#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RBP, X86_REG_RIP, X86_REG_RSI
from game_patches import BASE, ONE_THIRTIETH, XML, Eboot, f32

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'gpu' / 'shim' / 'engine' / 'frame_rate_sites.inc'
OUT_LISTS = ROOT / 'gpu' / 'shim' / 'engine' / 'frame_rate_lists.inc'
SPRINT = 'Sprint Fix (High FPS)'
FRAME_MODE_TIMES = (0x2434840, 0x243485a, 0x243487e, 0x24348b3)
WAIT_SLEEP = 0x25629f0
LIMITER_CAVE = 0x2c012cc
STATUS_STEP_SECOND = (0x1cbc511, bytes.fromhex('37b70603'), bytes.fromhex('f3e3a903'))
LISTS = ('60 FPS++', '90 FPS++', 'Uncap FPS++')
FRAME_TIME = 0x575a908
EFFECT_SECOND_STEP = 0x4d27c4c
HIGH_STEPS = (0xf383ca, 0x4d29170)
EFFECT_STEP = 0x1cbdb42
EFFECT_PREFIX = bytes.fromhex('c7857cffffff')
COUNT_SIXTH = 0x1929676
COUNT_THIRD = 0x192c405
THREE_VECTOR = 0x4cf6420
NEG_THREE = 0x4d27fd4
THREE = 0x4d27fe0
EASE_DISTANCE = 0x4d25e68
EASE_CHASE = 0x4d25e94
STEP_CONTEXT_VTABLE = 0x56efd40
HELD_AT_TARGET = (0x4c88688, 0x4d26c9c, 0x4d270e4, 0x4d28418, 0x1bf9b74)


def lists(path, names=LISTS):
    root = ET.parse(path).getroot()
    out = {}
    for m in root.iter('Metadata'):
        if m.get('Name') in names:
            out[m.get('Name')] = {int(l.get('Address'), 16): bytes.fromhex(l.get('Value').replace('0x', ''))
                                  for l in m.iter('Line')}
    missing = set(names) - set(out)
    if missing:
        raise SystemExit(f'{path}: no {", ".join(sorted(missing))}')
    return out


def in_code(eboot, address):
    return eboot.function(address) is not None


def step_context(eboot, imm):
    start = eboot.function(imm)
    if start is None:
        return False
    disasm = Cs(CS_ARCH_X86, CS_MODE_64)
    disasm.detail = True
    code = list(disasm.disasm(eboot.read(start, imm + 16 - start), start))
    at = next((i for i, ins in enumerate(code) if ins.address < imm < ins.address + ins.size), None)
    if at is None or at + 1 >= len(code):
        return False
    store = code[at]
    if store.bytes[0] != 0xc7 or store.address + store.size != imm + 4 or store.bytes[1] not in (0x45, 0x85):
        return False
    mem = store.operands[0].mem
    if mem.base != X86_REG_RBP or mem.index != 0:
        return False
    head = mem.disp - 8
    passed = code[at + 1]
    if not (passed.mnemonic == 'lea' and passed.operands[0].type == X86_OP_REG and passed.operands[0].reg == X86_REG_RSI
            and passed.operands[1].mem.base == X86_REG_RBP and passed.operands[1].mem.disp == head):
        return False
    vtable_reg = None
    for ins in reversed(code[max(0, at - 6):at]):
        if (ins.mnemonic == 'mov' and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.base == X86_REG_RBP
                and ins.operands[0].mem.disp == head and ins.operands[1].type == X86_OP_REG):
            vtable_reg = ins.operands[1].reg
            break
    if vtable_reg is None:
        return False
    base = None
    for ins in reversed(code[max(0, at - 24):at]):
        if ins.mnemonic == 'add' and ins.operands[0].type == X86_OP_REG and ins.operands[0].reg == vtable_reg:
            if ins.operands[1].type != X86_OP_IMM or ins.operands[1].imm != 0x10:
                return False
        if (ins.mnemonic == 'lea' and ins.operands[0].type == X86_OP_REG and ins.operands[0].reg == vtable_reg
                and ins.operands[1].mem.base == X86_REG_RIP):
            base = ins.address + ins.size + ins.operands[1].mem.disp
            break
    return base is not None and base + 0x10 == STEP_CONTEXT_VTABLE


def sites(eboot, found):
    sixtieth = f32(1 / 60)
    common = None
    for name in ('60 FPS++', '90 FPS++'):
        steps = {a for a, d in found[name].items() if d == sixtieth}
        common = steps if common is None else common & steps
    common -= {FRAME_TIME, EFFECT_SECOND_STEP}
    out = []
    for address in sorted(common):
        if eboot.read(address, 4) != ONE_THIRTIETH:
            raise SystemExit(f'{address:#x}: not 1/30 in the eboot')
        if address in HELD_AT_TARGET:
            out.append((address, 'TargetStep'))
        elif in_code(eboot, address) and step_context(eboot, address):
            out.append((address, 'ContextStep'))
        else:
            out.append((address, 'Step'))
    for address in HIGH_STEPS:
        if eboot.read(address, 4) != ONE_THIRTIETH or found['90 FPS++'].get(address) != f32(1 / 90):
            raise SystemExit(f'{address:#x}: not the high-rate step')
        out.append((address, 'HighStep'))
    effect = found['90 FPS++'].get(EFFECT_STEP)
    if not effect or effect[:6] != EFFECT_PREFIX:
        raise SystemExit(f'{EFFECT_STEP:#x}: not the effect step store')
    out.append((EFFECT_STEP + 6, 'EffectStep'))
    for address, kind, sixty in ((COUNT_SIXTH, 'CountSixth', 10), (COUNT_THIRD, 'CountThird', 20)):
        if found['60 FPS++'].get(address) != struct.pack('<i', sixty):
            raise SystemExit(f'{address:#x}: not the frame count')
        out.append((address, kind))
    if found['60 FPS++'].get(THREE_VECTOR) != f32(*[3 / 60] * 4):
        raise SystemExit(f'{THREE_VECTOR:#x}: not 3/60')
    out += [(THREE_VECTOR + 4 * i, 'Three') for i in range(4)]
    if found['60 FPS++'].get(NEG_THREE) != f32(-3 / 60) or found['60 FPS++'].get(THREE) != f32(3 / 60):
        raise SystemExit('the 3/60 pair moved')
    out += [(NEG_THREE, 'NegThree'), (THREE, 'Three')]
    out += [(EASE_DISTANCE, 'EaseDistance'), (EASE_CHASE, 'EaseChase')]
    ninety = found['90 FPS++']
    for at in FRAME_MODE_TIMES:
        if patched(eboot, ninety, at, 9) != bytes.fromhex('41c7442418') + f32(1 / 90):
            raise SystemExit(f'{at:#x}: not a frame mode time')
        out.append((at + 5, 'FrameModeTime'))
    if patched(eboot, ninety, WAIT_SLEEP, 5) != b'\xbf' + struct.pack('<I', 990000 // 90):
        raise SystemExit(f'{WAIT_SLEEP:#x}: not the wait sleep')
    out.append((WAIT_SLEEP + 1, 'WaitSleep'))
    cave = patched(eboot, ninety, LIMITER_CAVE, 13)
    cap = struct.pack('<I', 10000)
    if cave[:2] != b'\x81\xff' or cave[6:9] != b'\x7e\x05\xbf' or cave[2:6] != cap or cave[9:13] != cap:
        raise SystemExit(f'{LIMITER_CAVE:#x}: not the limiter cap')
    out += [(LIMITER_CAVE + 2, 'LimiterCap'), (LIMITER_CAVE + 9, 'LimiterCap')]
    return out


def patched(eboot, writes, address, length):
    out = bytearray(eboot.read(address, length))
    for at, data in writes.items():
        for i, b in enumerate(data):
            if address <= at + i < address + length:
                out[at + i - address] = b
    return bytes(out)


def prefix(eboot, found, address, kind):
    if kind == 'EffectStep':
        return EFFECT_PREFIX[-4:]
    if kind in ('FrameModeTime', 'WaitSleep', 'LimiterCap'):
        return patched(eboot, found['90 FPS++'], address - 4, 4)
    return eboot.read(address - 4, 4) if in_code(eboot, address) else b''


def render(eboot, found, table):
    lines = ['// SPDX-License-Identifier: GPL-3.0-or-later']
    for address, kind in table:
        code = in_code(eboot, address) or kind == 'EffectStep'
        before = prefix(eboot, found, address, kind)
        bytes_ = ', '.join(f'0x{b:02x}' for b in before) if before else '0, 0, 0, 0'
        lines.append(f'{{0x{address - BASE:07x}, SiteKind::{kind}, {"true" if code else "false"}, '
                     f'{"true" if before else "false"}, {{{bytes_}}}}},')
    return '\n'.join(lines) + '\n'


def render_lists(eboot, found, sprint):
    ids = {name: i for i, name in enumerate(LISTS + (SPRINT,))}
    every = dict(found)
    every[SPRINT] = sprint
    blob, entries = bytearray(), []
    for name, writes in every.items():
        rows = [(at, eboot.read(at, len(data)), data) for at, data in writes.items()]
        if name != SPRINT:
            at, original, data = STATUS_STEP_SECOND
            if eboot.read(at, 4) != original:
                raise SystemExit(f'{at:#x}: the status step moved')
            rows.append(STATUS_STEP_SECOND)
        for at, original, data in sorted(rows):
            entries.append((ids[name], at - BASE, len(data), len(blob)))
            blob += original + data
    lines = ['// SPDX-License-Identifier: GPL-3.0-or-later',
             'constexpr std::uint8_t kListBlob[] = {']
    for i in range(0, len(blob), 24):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in blob[i:i + 24]) + ',')
    lines.append('};')
    lines.append('constexpr ListWrite kListWrites[] = {')
    lines += [f'    {{{l}, 0x{o:07x}, {n}, {a}}},' for l, o, n, a in entries]
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('eboot', nargs='?', default=str(ROOT / 'out' / 'eboot.elf'))
    parser.add_argument('--xml', default=str(XML))
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    eboot = Eboot(args.eboot)
    found = lists(args.xml)
    table = sites(eboot, found)
    text = render(eboot, found, table)
    sprint = lists(args.xml, (SPRINT,))[SPRINT]
    list_text = render_lists(eboot, found, sprint)
    if args.write:
        OUT.write_text(text, encoding='utf-8', newline='\n')
        OUT_LISTS.write_text(list_text, encoding='utf-8', newline='\n')
        print(f'{OUT}: {len(table)} sites; {OUT_LISTS}: {len(found)} lists and the sprint fix')
        return 0
    for path, want in ((OUT, text), (OUT_LISTS, list_text)):
        if not path.exists() or path.read_text(encoding='utf-8') != want:
            print(f'{path} differs from what the eboot and {args.xml} give (--write rebuilds it)')
            return 1
    print(f'{OUT}: {len(table)} sites hold; {OUT_LISTS} holds')
    return 0


if __name__ == '__main__':
    sys.exit(main())
