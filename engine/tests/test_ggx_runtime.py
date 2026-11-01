"""Local runtime-emitter equivalence regression; requires external verified replay fixtures."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "re"))
from ggx_directional_patch import check_original, run_tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--patch-runner", type=Path, required=True)
    parser.add_argument("--gpu-runner", type=Path, required=True)
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--scene", type=Path, action="append", required=True)
    parser.add_argument("--controls", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if check_original(args.legacy.read_bytes()) != "legacy" or check_original(
            args.current.read_bytes()) != "current":
        raise ValueError("Expected the two exact native shader layouts")
    args.output.mkdir(parents=True, exist_ok=False)
    subprocess.run([str(args.patch_runner), str(args.legacy), str(args.current),
                    str(args.output / "shaders")], check=True)
    for variant in ("legacy", "current"):
        run_tool("spirv-val", "--target-env", "vulkan1.3", "--uniform-buffer-standard-layout",
                 args.output / "shaders" / f"{variant}.spv")
    report = {"status": "in_progress", "checks": {}}
    for index, scene in enumerate(args.scene):
        evidence = json.loads((scene / "report.json").read_text())
        if evidence["native_packed_mismatched_pixels"] != [0, 0] or evidence[
                "ggx_scene_scoring"] != "verified":
            raise ValueError("Verified native and authored scene required")
        variant = check_original((scene / "native.frag.spv").read_bytes())
        for name, shader, floating in (
                ("native-packed", scene / "native.frag.spv", False),
                ("native-float", scene / "native.frag.spv", True),
                ("ggx-width-authored-float", args.output / "shaders" / f"{variant}.spv", True)):
            actual = args.output / f"scene-{index}-{name}.raw"
            subprocess.run([str(args.gpu_runner), str(scene), str(scene / "native.vert.spv"),
                            str(shader), str(actual)] + (["--float"] if floating else []), check=True)
            if actual.read_bytes() != (scene / f"{name}.raw").read_bytes():
                raise ValueError(f"Runtime/native regression in scene {index}: {name}")
            if floating and not np.isfinite(np.fromfile(actual, dtype="<f4")).all():
                raise ValueError("Nonfinite runtime scene")
        report["checks"][f"scene-{index}"] = "native packed/float and authored runtime byte-exact"
    controls_report = json.loads((args.controls / "report.json").read_text())
    if controls_report["status"] != "verified":
        raise ValueError("Verified calibration controls required")
    for control in ("materials", "angles"):
        fixture = args.controls / control
        variant = check_original((args.controls / "shaders" / "native.frag.spv").read_bytes())
        actual = args.output / f"{control}-float.raw"
        subprocess.run([str(args.gpu_runner), str(fixture), str(fixture / "native.vert.spv"),
                        str(args.output / "shaders" / f"{variant}.spv"), str(actual), "--float"],
                       check=True)
        if actual.read_bytes() != (fixture / "ggx-width-authored-float.raw").read_bytes():
            raise ValueError(f"Runtime authored regression in {control} controls")
        report["checks"][control] = "authored runtime byte-exact"
    report["sha256"] = {
        str(path.relative_to(args.output)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in args.output.rglob("*") if path.is_file()
    }
    report["status"] = "verified"
    (args.output / "report.json").write_text(json.dumps(report, indent=2))
    print("Runtime GGX: native unchanged; all authored GPU scenes and controls byte-exact")


if __name__ == "__main__":
    main()
