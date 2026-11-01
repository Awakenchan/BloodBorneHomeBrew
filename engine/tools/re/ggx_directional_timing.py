"""Opt-in paired GPU timestamps on an exact captured directional pass; never whole-game FPS."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

from ggx_directional_patch import GGX_MODES


MODES = ("native", *GGX_MODES)


def summarize(timing, warmup, samples, width, height):
    ticks = np.asarray(timing["samples_ticks"], dtype=np.float64)
    period = timing["timestamp_period_ns"]
    bits = timing["timestamp_valid_bits"]
    if (ticks.shape != (samples, len(MODES)) or not np.isfinite(ticks).all()
            or not np.equal(ticks, np.floor(ticks)).all() or np.any(ticks <= 0)
            or not isinstance(bits, int) or not 1 <= bits <= 64
            or np.any(ticks >= ((1 << bits) - 1) // 2)
            or not np.isfinite(period) or period <= 0
            or timing["warmup_rounds"] != warmup or timing["sample_rounds"] != samples
            or timing["width"] != width or timing["height"] != height):
        raise ValueError("Invalid GPU timestamp metadata, intervals or sample count")
    if np.any(ticks < 100):
        raise ValueError("Directional interval is too short for useful timestamp resolution")
    milliseconds = ticks * period / 1e6
    ratios = milliseconds / milliseconds[:, :1]
    result = {}
    for index, mode in enumerate(MODES):
        values = milliseconds[:, index]
        result[mode] = {
            "gpu_ms_median": float(np.median(values)),
            "gpu_ms_p10": float(np.percentile(values, 10)),
            "gpu_ms_p90": float(np.percentile(values, 90)),
            "gpu_ms_p90_to_p10_ratio": float(
                np.percentile(values, 90) / np.percentile(values, 10)),
            "paired_ratio_median": float(np.median(ratios[:, index])),
            "paired_ratio_p10": float(np.percentile(ratios[:, index], 10)),
            "paired_ratio_p90": float(np.percentile(ratios[:, index], 90)),
        }
    quarter = max(1, samples // 4)
    return {
        "modes": result,
        "native_last_to_first_quarter_median_ratio": float(
            np.median(milliseconds[-quarter:, 0]) / np.median(milliseconds[:quarter, 0])),
    }


def check_packed(path, expected):
    actual = np.fromfile(path, dtype="<u4")
    if actual.size != expected.size or not np.array_equal(actual.reshape(expected.shape), expected):
        raise ValueError("Native packed replay must remain exact before and after GPU timing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--vertex", type=Path, required=True)
    parser.add_argument("--fragment", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--warmup", type=int, default=256)
    parser.add_argument("--samples", type=int, default=1024)
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    if not 16 <= args.warmup <= 4096 or not 32 <= args.samples <= 4096 or not 2 <= args.trials <= 10:
        parser.error("Require 16..4096 warmup rounds, 32..4096 sample rounds and 2..10 trials")
    # Existing replay CLI enforces actual compiled modules and zero native packed mismatches.
    args.output.mkdir(parents=True, exist_ok=False)
    report_path = args.output / "report.json"
    report = {"status": "in_progress", "scope": "captured offline directional pass only",
              "modes": MODES, "warmup_rounds": args.warmup, "sample_rounds": args.samples,
              "trials": [],
              "method": "One device per trial; all pipelines compiled before warmup; "
                        "cyclic mode order within each paired round. TOP/BOTTOM timestamps "
                        "bracket dynamic rendering including attachment load/store. Captured "
                        "packed targets and additive blend for every mode; targets restored "
                        "before every draw outside the timed interval. No timed readback.",
              "limitations": "Serialized, warm-cache offline draws with intervening target "
                             "restores, not an isolated shader-ALU benchmark or live frame. "
                             "GPU clock, thermal and other-process load are uncontrolled. "
                             "Percentiles describe samples, not confidence intervals.",
              "whole_game_fps_measured": False, "production_model_selected": False}
    report["validation_layers_requested"] = os.environ.get("VK_INSTANCE_LAYERS", "")
    report["interpretation"] = (
        "Verified means timestamps and native gates passed, not that GPU load was isolated. "
        "Compare repeated median estimates alongside the full sample spreads; do not infer FPS.")
    report_path.write_text(json.dumps(report, indent=2))
    scene = args.output / "scene"
    subprocess.run([sys.executable, str(Path(__file__).with_name("ggx_directional_replay.py")),
                    str(args.capture), str(scene), "--vertex", str(args.vertex),
                    "--fragment", str(args.fragment), "--runner", str(args.runner),
                    "--mode", "compare",
                    *[arg for mode in GGX_MODES for arg in ("--candidate", mode)]], check=True)
    scene_report = json.loads((scene / "report.json").read_text())
    if (scene_report["native_packed_mismatched_pixels"] != [0, 0]
            or scene_report["ggx_scene_scoring"] != "verified"):
        raise ValueError("Exact native replay and candidate verification required before timing")
    inputs = json.loads((scene / "inputs.json").read_text())
    width, height = inputs["width"], inputs["height"]
    expected = np.stack([np.fromfile(args.capture / f"native-{i}.bin", dtype="<u4")
                         .reshape(height, width) for i in (0, 1)])
    check_packed(scene / "native-packed.raw", expected)
    report["native_packed_mismatches_before_timing"] = [0, 0]
    report["candidate_float_checks"] = scene_report["ggx"]
    for trial in range(args.trials):
        raw = args.output / f"trial-{trial}.json"
        subprocess.run([str(args.runner), str(scene), str(scene / "native.vert.spv"),
                        str(scene / "native.frag.spv"), str(raw), "--benchmark",
                        str(args.warmup), str(args.samples),
                        *[str(scene / f"{mode}.frag.spv") for mode in GGX_MODES]], check=True)
        timing = json.loads(raw.read_text())
        summary = summarize(timing, args.warmup, args.samples, width, height)
        check_packed(Path(str(raw) + ".native-packed.raw"), expected)
        summary["native_packed_mismatches_after_timing"] = [0, 0]
        identity = {key: timing[key] for key in ("gpu", "vendor_id", "device_id",
                    "driver_version", "timestamp_period_ns", "timestamp_valid_bits")}
        if trial and identity != report["device"]:
            raise ValueError("GPU identity or timestamp properties changed between trials")
        report["device"] = identity
        report["trials"].append(summary)
        report_path.write_text(json.dumps(report, indent=2))
    report["summary"] = {
        mode: {
            "trial_gpu_ms_medians": [trial["modes"][mode]["gpu_ms_median"]
                                     for trial in report["trials"]],
            "trial_paired_ratio_medians": [trial["modes"][mode]["paired_ratio_median"]
                                          for trial in report["trials"]],
        } for mode in MODES
    }
    report["sha256"] = {
        str(path.relative_to(args.output)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in args.output.rglob("*") if path.is_file() and path != report_path
    }
    report["runner_sha256"] = hashlib.sha256(args.runner.read_bytes()).hexdigest()
    for filename, digest in inputs["source_sha256"].items():
        if hashlib.sha256((args.capture / filename).read_bytes()).hexdigest() != digest:
            raise ValueError("Source capture changed during GPU timing")
    for stage, path in (("vertex", args.vertex), ("fragment", args.fragment)):
        if hashlib.sha256(path.read_bytes()).hexdigest() != inputs[f"{stage}_sha256"]:
            raise ValueError("Actual compiled module changed during GPU timing")
    report["source_capture_and_modules_unchanged"] = True
    report["status"] = "verified"
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
