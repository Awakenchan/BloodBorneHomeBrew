"""Run with one or more local original directional .spv paths; no game data is embedded."""

import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "re"))
import ggx_directional_patch as patch

ORIGINALS = [Path(value) for value in sys.argv[1:]]
sys.argv[1:] = []


class GgxDirectionalPatchTests(unittest.TestCase):
    def test_unknown_and_already_patched_inputs_are_rejected(self):
        with self.assertRaises(ValueError):
            patch.check_original(b"")
        for source in ORIGINALS:
            changed = bytearray(source.read_bytes())
            changed[-1] ^= 1
            with self.assertRaises(ValueError):
                patch.check_original(changed)
            with tempfile.TemporaryDirectory() as temporary:
                result = Path(temporary) / "ggx.spv"
                patch.generate(source, result, "ggx-width")
                with self.assertRaises(ValueError):
                    patch.check_original(result.read_bytes())

    def test_native_is_default_and_bit_exact(self):
        for source in ORIGINALS:
            with tempfile.TemporaryDirectory() as temporary:
                result = Path(temporary) / "native.spv"
                patch.generate(source, result)
                self.assertEqual(result.read_bytes(), source.read_bytes())
                with self.assertRaises(FileExistsError):
                    patch.generate(source, result, "ggx-width")
                self.assertEqual(result.read_bytes(), source.read_bytes())
                with self.assertRaises(FileExistsError):
                    patch.generate(source, source)

    def test_all_explicit_calibrations_validate(self):
        for source in ORIGINALS:
            with tempfile.TemporaryDirectory() as temporary:
                outputs = []
                for mode in patch.GGX_MODES:
                    result = Path(temporary) / f"{mode}.spv"
                    patch.generate(source, result, mode)
                    outputs.append(result.read_bytes())
                self.assertEqual(len(set(outputs)), len(patch.GGX_MODES))

    def test_authored_normalization_is_opt_in_and_has_no_output_clamp(self):
        self.assertNotIn("ggx_reference_gain", patch.ggx_function("ggx-width"))
        self.assertNotIn("ggx_reference_gain", patch.ggx_function("ggx-peak"))
        authored = patch.ggx_function("ggx-width-authored")
        self.assertIn("OpFMul %f32_id %ggx_raw_lit %ggx_reference_gain", authored)
        self.assertNotIn("FClamp %ggx_raw_lit", authored)
        self.assertNotIn("FMin %ggx_raw_lit", authored)

    def test_replacement_sites_must_match(self):
        with self.assertRaises(ValueError):
            patch.patch_assembly("", "ggx-width")
        with self.assertRaises(ValueError):
            patch.patch_assembly("", "invalid")

    def test_width_mapping_covers_native_exponent_range(self):
        for i in range(4096):
            exponent = math.exp2(13 * i / 4095)
            half_cosine_squared = math.exp2(-2 / exponent)
            alpha_squared = (1 - half_cosine_squared) / (math.sqrt(2) - half_cosine_squared)
            self.assertGreater(alpha_squared, 0)
            self.assertLess(alpha_squared, 1)
            relative_height = alpha_squared ** 2 / (
                1 + (alpha_squared - 1) * half_cosine_squared) ** 2
            self.assertAlmostEqual(relative_height, 0.5, delta=1e-11)


if __name__ == "__main__":
    if not ORIGINALS:
        raise SystemExit("Provide local native directional shader fixtures; empty fixture runs are not tests")
    unittest.main()
