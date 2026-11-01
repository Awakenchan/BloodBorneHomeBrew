"""Timestamp reporting and native gate regressions, not a GPU performance measurement."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "re"))
import ggx_directional_timing as timing


class GgxDirectionalTimingTests(unittest.TestCase):
    def fixture(self):
        return {"samples_ticks": [[1000, 2000, 3000, 4000]] * 32,
                "timestamp_period_ns": 2, "timestamp_valid_bits": 64,
                "warmup_rounds": 16, "sample_rounds": 32, "width": 2, "height": 1}

    def test_units_and_paired_ratios(self):
        result = timing.summarize(self.fixture(), 16, 32, 2, 1)
        self.assertEqual(result["modes"]["native"]["gpu_ms_median"], 0.002)
        self.assertEqual(result["modes"]["native"]["gpu_ms_p90_to_p10_ratio"], 1)
        self.assertEqual(result["modes"]["ggx-width-authored"]["paired_ratio_median"], 4)
        self.assertEqual(result["native_last_to_first_quarter_median_ratio"], 1)

    def test_ratios_pair_with_the_same_round(self):
        fixture = self.fixture()
        fixture["samples_ticks"] = [[1000, 2000, 3000, 4000]] * 16 + [
            [4000, 8000, 12000, 16000]] * 16
        result = timing.summarize(fixture, 16, 32, 2, 1)
        self.assertEqual(result["modes"]["ggx-width"]["paired_ratio_median"], 2)
        self.assertEqual(result["native_last_to_first_quarter_median_ratio"], 4)

    def test_bad_timestamp_metadata_is_rejected(self):
        for key, value in (("timestamp_period_ns", 0), ("timestamp_period_ns", float("nan")),
                           ("timestamp_valid_bits", 0), ("timestamp_valid_bits", 65),
                           ("sample_rounds", 31), ("warmup_rounds", 0), ("width", 3),
                           ("samples_ticks", [[1000] * 3] * 32),
                           ("samples_ticks", [[0] * 4] * 32),
                           ("samples_ticks", [[float("inf")] * 4] * 32),
                           ("samples_ticks", [[1000.5] * 4] * 32),
                           ("samples_ticks", [[99] * 4] * 32)):
            with self.subTest(key=key, value=value):
                fixture = self.fixture()
                fixture[key] = value
                with self.assertRaises(ValueError):
                    timing.summarize(fixture, 16, 32, 2, 1)

    def test_native_packed_gate_is_byte_exact(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "native.raw"
            expected = np.arange(4, dtype="<u4").reshape(2, 1, 2)
            expected.tofile(path)
            timing.check_packed(path, expected)
            (expected + 1).tofile(path)
            with self.assertRaises(ValueError):
                timing.check_packed(path, expected)
            expected.ravel()[:3].tofile(path)
            with self.assertRaises(ValueError):
                timing.check_packed(path, expected)

    def test_failed_native_gate_never_launches_benchmark(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "timing"
            argv = ["timing", "capture", str(output), "--vertex", "vertex",
                    "--fragment", "fragment", "--runner", "runner"]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    timing.subprocess, "run", side_effect=RuntimeError("Native gate failed")) as run:
                with self.assertRaisesRegex(RuntimeError, "Native gate failed"):
                    timing.main()
            self.assertEqual(run.call_count, 1)
            self.assertNotIn("--benchmark", run.call_args.args[0])


if __name__ == "__main__":
    unittest.main()
