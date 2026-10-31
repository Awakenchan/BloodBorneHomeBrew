# SPDX-License-Identifier: GPL-2.0-or-later
import re
import struct
import unittest
from pathlib import Path

class SsrIntensityTest(unittest.TestCase):
    def test_composite_multiplier(self):
        header = (Path(__file__).resolve().parents[1] / "gpu" / "shadps4" /
                  "video_core" / "renderer_vulkan" / "vk_ssr_composite_patch.h")
        source = header.read_text().split("kSsrCompositeSpv{", 1)[1]
        words = [int(value, 16) for value in re.findall(r"0x[0-9a-f]+", source)]
        instructions = []
        cursor = 5
        while cursor < len(words):
            size, opcode = words[cursor] >> 16, words[cursor] & 0xffff
            self.assertGreater(size, 0)
            instructions.append((opcode, words[cursor + 1:cursor + size]))
            cursor += size
        self.assertEqual(cursor, len(words))
        constants = {args[1]: args[2] for op, args in instructions if op == 43}
        push_variables = {args[1] for op, args in instructions
                          if op == 59 and args[2] == 9}

        accesses = [args for op, args in instructions if op == 65 and
                    args[2] in push_variables and len(args) == 4 and
                    constants.get(args[3]) == 11]
        self.assertEqual(len(accesses), 1)
        push_types = {args[0] for op, args in instructions if op == 59 and
                      args[1] in push_variables}
        structs = {args[2] for op, args in instructions if op == 32 and
                   args[0] in push_types}
        self.assertTrue(any(op == 72 and args[0] in structs and
                            args[1:] == [11, 35, 120] for op, args in instructions))
        loaded = next(args[1] for op, args in instructions
                      if op == 61 and args[2] == accesses[0][1])
        intensity = next(args[1] for op, args in instructions
                         if op == 124 and args[2] == loaded)
        uses = [args for op, args in instructions if op == 133 and intensity in args[2:]]
        self.assertEqual(len(uses), 3)
        definitions = {args[1]: (op, args) for op, args in instructions
                       if op in (129, 133)}
        for multiply in uses:
            delta = multiply[2] if multiply[3] == intensity else multiply[3]
            self.assertEqual(definitions[delta][0], 133)
            adds = [args for op, args in instructions
                    if op == 129 and multiply[1] in args[2:]]
            self.assertEqual(len(adds), 1)
            add = adds[0]
            environment = add[2] if add[3] == multiply[1] else add[3]
            for strength in (0.0, 0.5, 1.0, 3.0):
                for background, reflection, confidence in (
                        (2.0, 8.0, 0.75), (4.0, 1.0, 0.2), (3.0, 6.0, 0.0)):
                    values = {environment: background,
                              delta: confidence * (reflection - background),
                              intensity: strength}
                    values[multiply[1]] = values[multiply[2]] * values[multiply[3]]
                    result = values[add[2]] + values[add[3]]
                    self.assertAlmostEqual(
                        result, background + strength * confidence * (reflection - background))
                    if strength == 0 or confidence == 0:
                        self.assertEqual(result, background)

if __name__ == "__main__":
    unittest.main()
