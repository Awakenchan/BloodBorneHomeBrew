#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys

from eboot import Eboot

HERE = os.path.dirname(os.path.abspath(__file__))

CONTEXT = {
    'Dispatch': (0x21695c0, 'dcb.dispatch'),
    'DispatchIndirect': (0x2169640, 'dcb.dispatchIndirect'),
    'Draw': (0x21696c0, 'dcb.drawIndexAuto'),
    'DrawIndexed': (0x2169780, 'dcb.drawIndexOffset'),
    'DrawInstanced': (0x2169840, 'dcb.drawIndexAuto'),
    'DrawIndexedInstanced': (0x2169900, 'dcb.drawIndexOffset'),
    'DrawInstancedIndirect': (0x21699d0, 'dcb.drawIndirect'),
    'DrawIndexedInstancedIndirect': (0x2169ab0, 'dcb.drawIndexIndirect'),
    'Commit': (0x2169ba0, None),
}
FLUSH_OUTPUT_MERGER = 0x26d2230
POST_DRAW = 0x26cbd50
COMMAND_FLUSH = 0x26af700
FINISH_COMMAND_LIST = (0x26d5850, 0x216aca0)
EXECUTE_COMMAND_LIST = (0x21679a0, 0x216a7d0)
EXECUTE_COMMAND_LISTS = (0x21679e0, 0x216a980)
SUBMIT_SEGMENT = 0x26b0b00
FLIP_CHAIN = (0x26afb30, 0x26b0ce0, 0x26b0e30)
PRESENT = (0x26d5dd0, 0x26d6e50, 0x33d0)
CREATE_BUFFER = (0x2165c10, 0x58)
UPLOAD_POOL = (0x26a9b50, 0x216b0f0, 0x30, 0xff0)
OCCLUSION_QUERY = 0x1077660
PIPELINE_DRAIN = 0x26b09e0
TABLE_RECLAIM = 0x26aa860
FLUSH_WAIT = 0x11d7030
CE_WRITERS = (0x11fd190, 0x11fd3e0)
YEBIS_CONTEXT_VTABLE = (0x530a380, 133, {50: 0x2a41320})
YEBIS_GNM = {'draw index': 0x11fbc40, 'draw auto': 0x11fbb50, 'dispatch': 0x11fbd10}
PPFX = {'bind framebuffer': 0xbb68f0, 'apply effects': 0xbb6980}
FLIP_PASS = 0x21d5f80
CUE = (0x1092e80, 0x1093150)
FRAME_TIMER = 0x2034770


def gnm_names():
    names = {}
    for line in open(os.path.join(HERE, 'gnm_names.txt')):
        part = line.split()
        if len(part) >= 2 and part[0].startswith('0x'):
            names[int(part[0], 16)] = part[1]
    return names


def immediates(e, f):
    out = set()
    for i in e.disasm(f):
        for tok in i.op_str.replace(',', ' ').replace('[', ' ').replace(']', ' ').split():
            if tok.startswith('0x'):
                try:
                    out.add(int(tok, 16))
                except ValueError:
                    pass
    return out


def main(path):
    e = Eboot(path)
    names = gnm_names()
    fails = []

    def check(ok, what):
        print(f"{'OK  ' if ok else 'FAIL'}  {what}")
        if not ok:
            fails.append(what)

    def calls(f):
        return [t for _, t in e.calls_in(f)]

    for name, (f, writer) in CONTEXT.items():
        check(e.is_function(f), f'context {name} at 0x{f:x}')
        targets = calls(f)
        if writer:
            check(any(names.get(t) == writer for t in targets), f'  {name} writes {writer}')
        check(COMMAND_FLUSH in targets, f'  {name} ends with the command flush')
    for name in ('Draw', 'DrawIndexed', 'DrawInstanced', 'DrawIndexedInstanced', 'Commit'):
        check(FLUSH_OUTPUT_MERGER in calls(CONTEXT[name][0]), f'  {name} flushes the output merger')
    for name in ('DrawInstancedIndirect', 'DrawIndexedInstancedIndirect', 'Commit'):
        check(POST_DRAW in calls(CONTEXT[name][0]), f'  {name} runs the post-draw')

    check(FINISH_COMMAND_LIST[1] in calls(FINISH_COMMAND_LIST[0]), 'FinishCommandList calls its body')
    check(EXECUTE_COMMAND_LIST[1] in calls(EXECUTE_COMMAND_LIST[0]), 'ExecuteCommandList calls its body')
    check(EXECUTE_COMMAND_LISTS[1] in calls(EXECUTE_COMMAND_LISTS[0]), 'ExecuteCommandLists calls its body')
    check(e.is_function(SUBMIT_SEGMENT), f'segment submit at 0x{SUBMIT_SEGMENT:x}')
    check(FLIP_CHAIN[1] in calls(FLIP_CHAIN[0]) and FLIP_CHAIN[2] in calls(FLIP_CHAIN[1]), 'flip: close, submit and flip chained')
    present = e.disasm(PRESENT[0])
    check(any(i.mnemonic == 'jmp' and i.op_str == hex(PRESENT[1]) for i in present) and
          any(hex(PRESENT[2]) in i.op_str for i in present), 'Present tail-jumps to the swap chain at context+0x33d0')
    check(CREATE_BUFFER[1] in immediates(e, CREATE_BUFFER[0]), 'CreateBuffer makes a 0x58-byte holder')
    pool, device, node, block = UPLOAD_POOL
    imm = immediates(e, pool)
    check(node in imm and block in imm, 'upload pool: 0x30-byte nodes in 0xff0-byte blocks')
    check(any(e.function(c)[0] == device for c in e.callers(pool)), f'upload pool made by the device at 0x{device:x}')
    for name, f in (('occlusion query writer', OCCLUSION_QUERY), ('pipeline drain', PIPELINE_DRAIN),
                    ('table reclaim', TABLE_RECLAIM), ('flush wait', FLUSH_WAIT), ('flip pass', FLIP_PASS),
                    ('frame timer', FRAME_TIMER), ('CUE pre-draw', CUE[0]), ('CUE post-draw', CUE[1]),
                    ('CE writers start', CE_WRITERS[0]), ('CE writers end', CE_WRITERS[1])):
        check(e.is_function(f), f'{name} at 0x{f:x}')
    table, count, slots = YEBIS_CONTEXT_VTABLE
    entries = [e.ptr(table + 8 * i) for i in range(count)]
    check(all(e.is_function(x) for x in entries), f'YEBIS context vtable 0x{table:x}: {count} methods')
    for slot, f in slots.items():
        check(entries[slot] == f, f'  slot {slot} (ClearRenderTargetView) is 0x{f:x}')
    for name, f in {**YEBIS_GNM, **PPFX}.items():
        check(e.is_function(f), f'{name} at 0x{f:x}')

    print(f"\n{len(fails)} failed")
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'E:/BloodborneDev/engine/out/eboot.elf'))
