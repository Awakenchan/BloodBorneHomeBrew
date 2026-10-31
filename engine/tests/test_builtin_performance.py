"""Verify built-in performance defaults and resolution-safe migration from the old toggles."""
from paths import ROOT
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import patches


class BuiltinPerformanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.operations = [
            (int(offset, 16), int(crc, 16), bytes.fromhex(value))
            for offset, crc, value in re.findall(
                r'\{0x([0-9a-f]+), 0x([0-9a-f]+)u, "([0-9a-f]+)"\},',
                (ROOT / 'start/performance.inc').read_text())
        ]

    def test_all_legacy_behavior_and_original_byte_guards_are_preserved(self):
        name = 'Performance Patch (perf increase)'
        root = ET.parse(ROOT / 'patches/Bloodborne.xml').getroot()
        meta = next(m for m in root.iter('Metadata') if m.get('Name') == name)
        guards = json.loads((ROOT / 'patches/Bloodborne-1.09-sites.json').read_text())[name]
        expected = [
            (int(line.get('Address'), 16) - patches.EBOOT_BASE, crc, bytes.fromhex(line.get('Value')))
            for line, crc in zip(meta.iter('Line'), guards, strict=True)
        ]
        self.assertEqual(len(expected), 168)
        self.assertEqual(self.operations, expected)

    def test_grid_size_tracks_output_not_internal_resolution(self):
        for output in ((1920, 1080), (2560, 1440), (3840, 2160), (3440, 1440)):
            with self.subTest(output=output), \
                 patch.object(patches, 'original_bytes', return_value=b'original'), \
                 patch.object(patches.zlib, 'crc32', side_effect=[crc for _, crc, _ in self.operations]):
                writes = dict(patches.performance_writes(b'', output))
            for offset, _, value in self.operations:
                if offset in (0x2295cb6, 0x2295cc0):
                    self.assertEqual(writes[offset][:6], value[:6])
                    self.assertEqual(struct.unpack('<I', writes[offset][6:])[0],
                                     output[0 if offset == 0x2295cb6 else 1])
                else:
                    self.assertEqual(writes[offset], value)

    def test_incompatible_executable_applies_no_partial_defaults(self):
        with patch.object(patches, 'original_bytes', return_value=b'wrong'):
            self.assertEqual(patches.performance_writes(b'', (2560, 1440)), [])

    def test_legacy_toggles_are_managed(self):
        for name in ('Performance Patch (perf increase)', '1080p Light Grid (READ NOTES)',
                     '1440p Light Grid (READ NOTES)', '4k Light Grid (READ NOTES)'):
            self.assertTrue(patches.managed_builtin(name))

    @unittest.skipUnless(os.environ.get('BB_TEST_EBOOT'), 'requires a local verified 1.09 ELF')
    def test_native_start_matches_defaults_without_xml_toggle(self):
        executable = ROOT / 'out/bb-start.exe'
        with tempfile.TemporaryDirectory(dir=ROOT / 'out', prefix='performance-test-') as directory:
            out = ROOT / 'out' / directory
            shutil.copyfile(os.environ['BB_TEST_EBOOT'], out / 'eboot.elf')
            ini = out / 'lantern.ini'
            ini.write_text('effect_dof=1\neffect_ssao=1\neffect_motion_blur=1\neffect_game_aa=1\n'
                           'effect_dynamic_shadows=1\neffect_ssr=1\n')
            config = out / 'patches.json'
            config.write_text(json.dumps({'enabled': [
                'Bloodborne.xml/Performance Patch (perf increase)',
                'Bloodborne.xml/1440p Light Grid (READ NOTES)']}))
            env = dict(os.environ, BB_GAME_HEAPS='0', BB_GX_REGION_MB='160', BB_RENDER_POOLS='0',
                       BB_OTHER_EDITION='0', BB_ONLINE='0')

            def compile(output, legacy=False):
                result = subprocess.run([str(executable), '--test-patches', str(out), str(ROOT),
                                         str(ini), str(out), '30', '', output, '',
                                         str(config) if legacy else ''],
                                        env=env, capture_output=True, text=True, timeout=30, check=True)
                self.assertIn('Engine performance: built-in', result.stdout)
                blob = (out / 'patches.bin').read_bytes()
                count, = struct.unpack_from('<Q', blob, 16)
                at, writes = 24, {}
                for _ in range(count):
                    offset, size = struct.unpack_from('<QQ', blob, at)
                    at += 16
                    for i, value in enumerate(blob[at:at+size]):
                        writes[offset+i] = value
                    at += size
                self.assertEqual(at, len(blob))
                return writes

            for output in ((1920, 1080), (2560, 1440), (3840, 2160), (3440, 1440)):
                text = f'{output[0]}x{output[1]}'
                writes = compile(text)
                self.assertEqual(writes, compile(text, legacy=True))
                for offset, _, value in self.operations:
                    if offset in (0x2295cb6, 0x2295cc0):
                        value = value[:6] + struct.pack('<I', output[0 if offset == 0x2295cb6 else 1])
                    if offset == 0x22c4fd7:
                        value = b'\x01'  # The explicit SSR switch wins over the old defaults.
                    self.assertEqual(bytes(writes[offset+i] for i in range(len(value))), value)


if __name__ == '__main__':
    unittest.main()
