"""Input/oracle regressions; actual GPU controls require an exact local native scene."""

from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "re"))
import ggx_directional_controls as controls
from ggx_directional_replay import compare_float_outputs


class GgxDirectionalControlTests(unittest.TestCase):
    def test_fixture_preserves_vertex_and_sets_a_constant_view(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            for name in ("native.vert.spv", "vertex-0.bin", "vertex-1.bin", "push-data.bin", "buffer-3.bin"):
                (source / name).write_bytes(b"unchanged captured interface")
            config = ["captured state"] * 20
            for i in range(5):
                config[13 + i] = f"1280 720 {100 if i == 3 else 37} 3 4 5 6"
            textures, table = controls.material_atlas(tile=8)
            output = root / "fixture"
            controls.write_fixture(source, output, config, textures, table)
            self.assertEqual((output / "native.vert.spv").read_bytes(),
                             (source / "native.vert.spv").read_bytes())
            self.assertEqual((output / "replay.txt").read_text().splitlines()[0],
                             "5 40 56 4 0 0 1 4 0 2")
            camera = np.fromfile(output / "buffer-1.bin", dtype="<f4")
            np.testing.assert_array_equal(camera[[3, 7, 11, 15]], [0, 0, -1, 1])
            np.testing.assert_array_equal(camera[[32, 37, 42]], [1, 1, 1])
            self.assertEqual((output / "texture-3.bin").stat().st_size, 40 * 56 * 4)

    def test_material_extremes_wrap_and_specular_colors_are_encoded(self):
        textures, table = controls.material_atlas(tile=16)
        self.assertEqual(textures[0].shape, (112, 80, 4))
        self.assertEqual(set(np.unique(textures[2][..., 0])), {0, 64, 128, 192, 255})
        self.assertEqual(set(np.unique(textures[2][..., 1])), {0, 64})
        self.assertEqual(table[5, 0, 3], 1e6)
        for column, swatch in enumerate(controls.SWATCHES):
            np.testing.assert_array_equal(textures[0][8, column * 16 + 8, :3], swatch)
        for mode in dict.fromkeys((*controls.MODES, *controls.CALIBRATION_MODES)):
            self.assertTrue(np.isfinite(controls.reference_specular(textures, table, mode)).all())

    def test_all_four_lobes_and_optional_flags_are_exercised(self):
        textures, table = controls.angular_atlas(width=64, stripe=2)
        for row in range(4):
            self.assertTrue(np.any(table[row, 8 + row, :3]))
        self.assertEqual(table.view("<u4")[2, 16, 0], 1)
        self.assertEqual(table.view("<u4")[3, 16, 1], 1)
        np.testing.assert_array_equal(table.view("<u4")[11, 16, :2], [0, 0])
        self.assertTrue(np.any(table[11, 10:12, :3]))
        for mode in dict.fromkeys((*controls.MODES, *controls.CALIBRATION_MODES)):
            expected = controls.reference_specular(textures, table, mode)
            self.assertTrue(np.isfinite(expected).all())
            self.assertFalse(np.any(expected[20:24]))

    def test_backfacing_microfacet_response_is_zero(self):
        textures, table = controls.angular_atlas(width=256, stripe=2)
        backfacing = textures[1][..., 2] < 128
        native = controls.reference_specular(textures, table, "native")
        self.assertGreater(native[backfacing].max(), 0)
        for mode in dict.fromkeys((*controls.MODES[1:], *controls.CALIBRATION_MODES[1:])):
            self.assertFalse(np.any(controls.reference_specular(textures, table, mode)[backfacing]))

    def test_calibration_covers_native_widths_and_roughness_only_tradeoff(self):
        sweep = controls.calibration_sweep()
        self.assertEqual(sweep["smoothness_byte_count"], 256)
        self.assertEqual(sweep["native_light_widths"], [2, 50, 1e6])
        self.assertLess(sweep["maximum_reference_peak_relative_error"], 1e-12)
        self.assertLess(sweep["maximum_ndf_half_height_error"], 1e-11)
        row = next(row for row in sweep["representative_rows"]
                   if row["smoothness_byte"] == 128 and row["native_light_width"] == 50)
        normal, grazing = row["angles"][0], row["angles"][2]
        self.assertGreater(grazing["roughness_only_peak_match_alpha_squared"],
                           normal["roughness_only_peak_match_alpha_squared"])
        self.assertGreater(grazing["roughness_only_peak_match_ndf_half_angle_degrees"],
                           row["native_and_width_ggx_ndf_half_angle_degrees"])
        self.assertLess(normal["unscaled_ggx_width_peak_ratio_to_native"], 1)
        self.assertGreater(grazing["unscaled_ggx_width_peak_ratio_to_native"], 1)

    def test_authored_mapping_matches_native_at_mirror_for_all_four_lobes(self):
        angles = np.radians([0, 30, 60, 80, 85, 89])
        encoded = controls.encoded_normals(
            np.stack((np.sin(angles), np.zeros(len(angles)), np.cos(angles)), axis=-1))
        normal = encoded.astype(np.float64) / 255 * 2 - 1
        normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
        for slot in range(4):
            for width in (2, 50, 1e6):
                textures = [np.full((len(angles), 5, 4), 255, dtype="u1") for _ in range(3)]
                table = np.zeros((225, 18, 4), dtype="<f4")
                for row in range(len(angles)):
                    controls.lighting_profile(table, row, slot, 35, width)
                    textures[0][row] = (128, 128, 128, row)
                    textures[1][row, :, :3] = encoded[row]
                    textures[2][row, :, 0] = [0, 64, 128, 192, 255]
                # Exact mirrors test the identity; GPU controls separately test float32 light storage.
                table = table.astype(np.float64)
                table[:len(angles), slot, :3] = 2 * normal[:, 2:3] * normal - [0, 0, 1]
                native = controls.reference_specular(textures, table, "native")
                authored = controls.reference_specular(textures, table, "ggx-width-authored")
                np.testing.assert_allclose(authored, native, rtol=1e-10, atol=1e-10)

    def test_calibration_atlases_cover_all_width_smoothness_combinations(self):
        textures, table = controls.material_atlas(tile=8, rows=controls.CALIBRATION_MATERIAL_ROWS)
        self.assertEqual(textures[0].shape[0], 16 * 8)
        for width in (2, 50, 1e6):
            rows = [i for i, row in enumerate(controls.CALIBRATION_MATERIAL_ROWS) if row[2] == width]
            self.assertEqual({int(textures[2][i * 8, 0, 0]) for i in rows}, {0, 64, 128, 192, 255})
            self.assertTrue(np.all(table[rows, 0, 3] == width))
        textures, table = controls.angular_atlas(width=64, stripe=2, rows=controls.CALIBRATION_ANGULAR_ROWS)
        self.assertEqual(textures[0].shape[0], len(controls.CALIBRATION_ANGULAR_ROWS) * 2)
        for mode in controls.CALIBRATION_MODES:
            self.assertTrue(np.isfinite(controls.reference_specular(textures, table, mode)).all())

    def test_authored_mapping_preserves_width_candidate_shape_without_output_clipping(self):
        textures, table = controls.angular_atlas(width=512, stripe=2)
        width = controls.reference_specular(textures, table, "ggx-width")
        authored = controls.reference_specular(textures, table, "ggx-width-authored")
        for row in (0, 1, 2, 3, 4, 5, 6):
            a, b = width[row * 2], authored[row * 2]
            active = a > 1e-8
            ratio = b[active] / a[active]
            np.testing.assert_allclose(ratio, np.full_like(ratio, ratio[0]), rtol=1e-12)
        self.assertGreater(authored.max(), 1)

    def test_dark_controls_do_not_require_a_fake_specular_change(self):
        result = np.zeros((2, 2, 3, 4), dtype="<f4")
        result[..., 3] = 1
        self.assertEqual(compare_float_outputs(
            result, result.copy(), require_specular_change=False)["specular_changed_pixels"], 0)
        with self.assertRaisesRegex(ValueError, "does not exercise"):
            compare_float_outputs(result, result.copy())

    def test_oracle_does_not_accept_incorrect_specular(self):
        textures, table = controls.material_atlas(tile=8)
        h, w = textures[0].shape[:2]
        results = {}
        for mode in dict.fromkeys((*controls.MODES, *controls.CALIBRATION_MODES)):
            result = np.zeros((2, h, w, 4), dtype="<f4")
            result[..., 3] = 1
            result[1, ..., :3] = controls.reference_specular(textures, table, mode)
            results[mode] = result
        controls.check_control(textures, table, results)
        results["ggx-width"][1, 0, 0, 0] += 1
        with self.assertRaisesRegex(ValueError, "independent oracle"):
            controls.check_control(textures, table, results)


if __name__ == "__main__":
    unittest.main()
