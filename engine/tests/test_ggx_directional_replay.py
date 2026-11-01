"""Synthetic gate regressions; these are not GPU equivalence or image-quality tests."""

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "re"))
import ggx_directional_replay as replay


class GgxDirectionalReplayTests(unittest.TestCase):
    def test_actual_compiled_modules_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / "capture"
            capture.mkdir()
            vertex = root / "supplied.vert.spv"
            fragment = root / "supplied.frag.spv"
            vertex.write_bytes(b"actual vertex module")
            fragment.write_bytes(b"actual fragment module")
            records = [["shader_key", "0", "abc"], ["shader_key", "3", "def"]]
            with self.assertRaisesRegex(ValueError, "Actual compiled module is missing"):
                replay.captured_modules(capture, records, vertex, fragment)
            (root / "compiled-abc.spv").write_bytes(fragment.read_bytes())
            (root / "compiled-def.spv").write_bytes(vertex.read_bytes())
            with self.assertRaisesRegex(ValueError, "pipeline state is missing"):
                replay.captured_modules(capture, records, vertex, fragment)
            pipeline = root / "pipeline-abc-def.txt"
            pipeline.write_text("actual native pipeline")
            selected, hashes = replay.captured_modules(capture, records, vertex, fragment)
            self.assertEqual(selected, pipeline)
            self.assertEqual(set(hashes), {0, 3})
            fragment.write_bytes(b"stale but otherwise supported cache alias")
            with self.assertRaisesRegex(ValueError, "rejecting stale cache alias"):
                replay.captured_modules(capture, records, vertex, fragment)
            fragment.write_bytes((root / "compiled-abc.spv").read_bytes())
            vertex.write_bytes(b"stale vertex cache alias")
            with self.assertRaisesRegex(ValueError, "rejecting stale cache alias"):
                replay.captured_modules(capture, records, vertex, fragment)

    def test_pipeline_key_does_not_mix_states_with_the_same_stages(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / "capture"
            capture.mkdir()
            vertex, fragment = root / "compiled-def.spv", root / "compiled-abc.spv"
            vertex.write_bytes(b"vertex")
            fragment.write_bytes(b"fragment")
            (root / "pipeline-abc-def.txt").write_text("wrong same-stage pipeline")
            records = [["shader_key", "0", "abc"], ["shader_key", "3", "def"],
                       ["pipeline_key", "123"]]
            with self.assertRaisesRegex(ValueError, "pipeline state is missing"):
                replay.captured_modules(capture, records, vertex, fragment)
            pipeline = root / "pipeline-123.txt"
            pipeline.write_text("correct per-pipeline state")
            self.assertEqual(replay.captured_modules(capture, records, vertex, fragment)[0], pipeline)

    def test_invalid_or_duplicate_shader_keys_are_rejected(self):
        for records in (
            [["shader_key", "0", "..\\external"]],
            [["shader_key", "0", "abc"], ["shader_key", "0", "def"]],
            [["shader_key", "0", "abc"]],
        ):
            with self.assertRaises(ValueError):
                replay.captured_modules(Path("unused"), records, Path("unused"), Path("unused"))

    def captured_fixture(self, root):
        capture = root / "capture"
        capture.mkdir()
        (capture / "complete.txt").write_text("complete")
        vertex, fragment = root / "compiled-def.spv", root / "compiled-abc.spv"
        vertex.write_bytes(b"actual vertex module")
        fragment.write_bytes(b"actual fragment module")
        pipeline = root / "pipeline-123.txt"
        pipeline.write_text(
            "version 1\npush_descriptors 1\ndynamic_vertex_input 1\nsubgroup_size 0\n"
            "multisample 1 0 0 0 0\nclip 0 1 1\nprovoking_last 0\n"
            "blend 0 1 6 1 0 1 0 0 15\nblend 1 1 6 1 0 1 0 0 15\n")
        records = [
            "shader_key 0 abc", "shader_key 3 def", "pipeline_key 123",
            "vs 8747a367", "ps d92266b9", "indices 4", "index_bytes 0",
            "base_vertex 0", "first_instance 0", "instances 1", "host_topology 4",
            "indexed 0", "depth_format 130", "host_viewport 0 1 2 -1 0 1",
            "host_scissor 0 0 2 1", "host_raster 0 1 0 0 0 1",
            "host_depth 1 0 3 0 0", "host_color_masks 15 15 0 0 0 0 0 0",
            "host_render_area 2 1",
        ]
        for binding, stage, local in ((0, 0, 0), (1, 0, 1), (2, 0, 2), (4, 3, 0)):
            records.extend((f"constant {binding} stage {stage} local {local} bytes 16",
                            f"buffer_type {binding} special 0"))
            (capture / f"constant-{binding}.bin").write_bytes(bytes([binding]) * 16)
        for i in (0, 1):
            records.extend((
                f"target {i} width 2 height 1 format 122",
                f"attribute {i} binding {i} offset {0 if i == 0 else 16} "
                f"format {109 if i == 0 else 103}",
                f"vertex {i} stride 24 bytes 96",
                f"sampler_host stage 0 local {i} min {i} mag {i} mip {i} u {2 if i == 0 else 0} "
                f"v {2 if i == 0 else 0} w {2 if i == 0 else 0} bias 0 min_lod 0 "
                f"max_lod 15.9960938 aniso {1 if i == 0 else 16} aniso_enable {i} "
                "compare_enable 0 compare_op 0 border 0",
            ))
            (capture / f"vertex-{i}.bin").write_bytes(bytes(range(96)))
            (capture / f"before-{i}.bin").write_bytes(bytes(8))
        for i in range(5):
            records.extend((
                f"gpu_image {i} base_mip 0 mip_count 1 format {100 if i == 3 else 37} "
                f"mapping {'3 1 1 2' if i == 3 else '3 4 5 6'}",
                f"gpu_mip {i} level 0 width 2 height 1 bytes 8",
            ))
            (capture / f"gpu-texture-{i}.bin").write_bytes(bytes(8))
        (capture / "metadata.txt").write_text("\n".join(records) + "\n")
        (capture / "push-data.bin").write_bytes(bytes(128))
        (capture / "before-depth.bin").write_bytes(bytes(8))
        return capture, vertex, fragment, pipeline

    def test_preparation_preserves_actual_modules_and_state(self):
        for fragment_uniforms, vertex_uniforms, expected in (
            ({1, 2}, {10}, "0 1 1 1"), (set(), {10}, "0 0 0 1"),
            (set(), set(), "0 0 0 0"),
        ):
            with self.subTest(descriptors=expected), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                capture, vertex, fragment, pipeline = self.captured_fixture(root)
                output = root / "replay"
                fragment_reflection = {"ubos": [{"binding": i} for i in fragment_uniforms]}
                vertex_reflection = {
                    "ubos": [{"binding": i} for i in vertex_uniforms],
                    "ssbos": [{"binding": i} for i in {10, 11} - vertex_uniforms],
                }

                def reflect(command, **kwargs):
                    return json.dumps(vertex_reflection if command[1] == str(vertex)
                                      else fragment_reflection)

                def generate(source, destination):
                    destination.write_bytes(source.read_bytes())

                with mock.patch.object(replay, "check_original"), \
                        mock.patch.object(replay, "tool", return_value="unused"), \
                        mock.patch.object(replay.subprocess, "check_output", side_effect=reflect), \
                        mock.patch.object(replay, "generate", side_effect=generate):
                    self.assertEqual(replay.prepare(capture, output, vertex, fragment), (2, 1))
                config = (output / "replay.txt").read_text().splitlines()
                self.assertEqual(config[0], "5 2 1 4 0 0 1 4 0 0")
                self.assertEqual(config[1:4], ["0 1 2 -1 0 1", expected, "1 3 0 130"])
                self.assertEqual(config[4:9], [
                    "0 1 0 0 0 1", "0 0 2 1", "0 1 1", "0", "1 0 0 0 0"])
                self.assertEqual((output / "native.vert.spv").read_bytes(), vertex.read_bytes())
                self.assertEqual((output / "native.frag.spv").read_bytes(), fragment.read_bytes())
                self.assertEqual((output / "native-pipeline.txt").read_bytes(), pipeline.read_bytes())
                self.assertEqual((output / "before-depth.bin").read_bytes(), bytes(8))
                self.assertEqual((output / "buffer-3.bin").read_bytes(), bytes([4]) * 16)

    def test_stale_alias_is_rejected_before_creating_a_fixture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture, vertex, fragment, _ = self.captured_fixture(root)
            stale = root / "cache-alias.spv"
            stale.write_bytes(b"stale module")
            output = root / "replay"
            with self.assertRaisesRegex(ValueError, "rejecting stale cache alias"):
                replay.prepare(capture, output, vertex, stale)
            self.assertFalse(output.exists())

    def test_unsupported_captured_states_are_rejected(self):
        for filename, original, unsupported in (
            ("metadata.txt", "host_depth 1 0 3 0 0", "host_depth 1 1 3 0 0"),
            ("metadata.txt", "host_render_area 2 1", "host_render_area 1 1"),
            ("metadata.txt", "host_raster 0 1 0 0 0 1", "host_raster 0 1 1 0 0 1"),
            ("pipeline-123.txt", "multisample 1 0 0 0 0", "multisample 2 0 0 0 0"),
            ("pipeline-123.txt", "blend 1 1 6 1 0 1 0 0 15", "blend 1 1 1 1 0 1 0 0 15"),
        ):
            with self.subTest(state=unsupported), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                capture, vertex, fragment, _ = self.captured_fixture(root)
                source = capture / filename if filename == "metadata.txt" else root / filename
                source.write_text(source.read_text().replace(original, unsupported))
                output = root / "replay"
                with mock.patch.object(replay, "check_original"), self.assertRaises(ValueError):
                    replay.prepare(capture, output, vertex, fragment)
                self.assertFalse(output.exists())

    def exercise(self, packed_change=None, candidate_change=None, compare=True, candidates=()):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture, output = root / "capture", root / "replay"
            capture.mkdir()
            packed = np.arange(4, dtype="<u4").reshape(2, 1, 2)
            for i in (0, 1):
                packed[i].tofile(capture / f"native-{i}.bin")
            floating = np.full((2, 1, 2, 4), 0.5, dtype="<f4")
            floating[0, 0, 0, 0] = 0
            floating[..., 3] = 1
            calls = []

            def prepare(*args):
                output.mkdir()
                return 2, 1

            def run(runner, fixture, shader, destination, is_float=False):
                calls.append((shader.name, is_float))
                if not is_float:
                    result = packed.copy()
                    if packed_change is not None:
                        result[packed_change, 0, 0] += 1
                else:
                    result = floating.copy()
                    if shader.name != "native.frag.spv":
                        result[1, ..., :3] += 0.1
                        if candidate_change is not None:
                            candidate_change(result, floating)
                result.tofile(destination)

            argv = ["ggx_directional_replay", str(capture), str(output),
                    "--vertex", "unused.vert.spv", "--fragment", "unused.frag.spv",
                    "--runner", "unused.exe"]
            if compare:
                argv += ["--mode", "compare"]
            for candidate in candidates:
                argv += ["--candidate", candidate]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(replay, "prepare", side_effect=prepare), \
                    mock.patch.object(replay, "replay", side_effect=run), \
                    mock.patch.object(replay, "generate") as generate, \
                    contextlib.redirect_stdout(io.StringIO()):
                try:
                    replay.main()
                except ValueError as error:
                    failure = str(error)
                else:
                    failure = None
            return failure, json.loads((output / "report.json").read_text()), calls, generate.call_count

    def test_one_pixel_in_either_target_blocks_ggx(self):
        for target in (0, 1):
            failure, report, calls, generated = self.exercise(packed_change=target)
            self.assertIn("Native replay is not exact", failure)
            self.assertFalse(report["native_baseline_exact"])
            self.assertEqual(report["native_packed_mismatched_pixels"][target], 1)
            self.assertEqual(report["ggx_scene_scoring"], "blocked")
            self.assertEqual(calls, [("native.frag.spv", False)])
            self.assertEqual(generated, 0)

    def test_signed_zero_diffuse_change_is_not_bit_exact(self):
        def change(result, baseline):
            result[0, 0, 0, 0] = -0.0
        failure, _, _, _ = self.exercise(candidate_change=change)
        self.assertIn("preserve native diffuse exactly", failure)

    def test_nonfinite_candidate_is_rejected(self):
        def change(result, baseline):
            result[1, 0, 0, 0] = np.inf
        failure, _, _, _ = self.exercise(candidate_change=change)
        self.assertIn("GGX must be finite", failure)

    def test_specular_blend_alpha_change_is_rejected(self):
        def change(result, baseline):
            result[1, 0, 0, 3] = 0
        failure, _, _, _ = self.exercise(candidate_change=change)
        self.assertIn("preserve native specular blend alpha exactly", failure)

    def test_inactive_specular_is_not_a_successful_comparison(self):
        def change(result, baseline):
            result[:] = baseline
        failure, _, _, _ = self.exercise(candidate_change=change)
        self.assertIn("does not exercise", failure)

    def test_both_candidates_follow_an_exact_native_baseline(self):
        failure, report, calls, generated = self.exercise()
        self.assertIsNone(failure)
        self.assertTrue(report["native_baseline_exact"])
        self.assertEqual(report["ggx_scene_scoring"], "verified")
        self.assertEqual(calls[:2], [("native.frag.spv", False), ("native.frag.spv", True)])
        self.assertEqual(generated, 2)
        for result in report["ggx"].values():
            self.assertTrue(result["diffuse_bit_exact"])
            self.assertEqual(result["specular_changed_pixels"], 2)

    def test_default_mode_never_generates_ggx(self):
        failure, report, calls, generated = self.exercise(compare=False)
        self.assertIsNone(failure)
        self.assertEqual(report["mode"], "native")
        self.assertEqual(report["ggx_scene_scoring"], "not_requested")
        self.assertEqual(len(calls), 2)
        self.assertEqual(generated, 0)

    def test_authored_candidate_is_explicit_and_still_native_first(self):
        failure, report, calls, generated = self.exercise(candidates=("ggx-width-authored",))
        self.assertIsNone(failure)
        self.assertEqual(set(report["ggx"]), {"ggx-width-authored"})
        self.assertEqual(calls[:2], [("native.frag.spv", False), ("native.frag.spv", True)])
        self.assertEqual(generated, 1)
        failure, report, calls, generated = self.exercise(
            packed_change=1, candidates=("ggx-width-authored",))
        self.assertIn("Native replay is not exact", failure)
        self.assertEqual(report["ggx_scene_scoring"], "blocked")
        self.assertEqual(generated, 0)

    def test_candidate_cannot_enable_ggx_in_native_mode_or_repeat(self):
        for arguments in (
            ["--candidate", "ggx-width-authored"],
            ["--mode", "compare", "--candidate", "ggx-width", "--candidate", "ggx-width"],
        ):
            argv = ["replay", "unused", "unused", "--vertex", "unused", "--fragment", "unused",
                    "--runner", "unused", *arguments]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(replay, "prepare") as prepare, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                replay.main()
            self.assertEqual(error.exception.code, 2)
            prepare.assert_not_called()


if __name__ == "__main__":
    unittest.main()
