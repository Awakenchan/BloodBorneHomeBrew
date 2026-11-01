"""Prepare and verify a captured native directional draw before an opt-in GGX A/B.

Requires an explicit BB_SPECULAR_CAPTURE=1 capture, local native shader binaries,
SPIR-V tools and ggx-directional-test. All outputs belong in an ignored directory.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import numpy as np

from ggx_directional_patch import GGX_MODES, check_original, generate, tool


def fields(line):
    return dict(zip(line[2::2], line[3::2]))


def captured_modules(capture, records, vertex, fragment):
    keys = {}
    for line in records:
        if line[0] == "shader_key":
            if len(line) != 3 or not re.fullmatch(r"[0-9a-f]{1,16}", line[2]):
                raise ValueError("Invalid captured shader key")
            stage = int(line[1])
            if stage in keys:
                raise ValueError("Duplicate captured shader stage")
            keys[stage] = line[2]
    if set(keys) != {0, 3}:
        raise ValueError("Expected the exact native fragment and vertex stage keys")
    hashes = {}
    for stage, supplied in ((0, fragment), (3, vertex)):
        snapshot = capture.parent / f"compiled-{keys[stage]}.spv"
        if not snapshot.is_file():
            raise ValueError("Actual compiled module is missing; cache aliases are not a replay reference")
        original = snapshot.read_bytes()
        if supplied.read_bytes() != original:
            raise ValueError(f"Stage {stage} shader differs from the actual compiled module; "
                             "rejecting stale cache alias")
        hashes[stage] = hashlib.sha256(original).hexdigest()
    pipeline_keys = [line[1:] for line in records if line[0] == "pipeline_key"]
    if pipeline_keys:
        if (len(pipeline_keys) != 1 or len(pipeline_keys[0]) != 1 or
                not re.fullmatch(r"[0-9a-f]{1,16}", pipeline_keys[0][0])):
            raise ValueError("Invalid captured pipeline key")
        pipeline = capture.parent / f"pipeline-{pipeline_keys[0][0]}.txt"
    else:
        pipeline = capture.parent / f"pipeline-{keys[0]}-{keys[3]}.txt"
    if not pipeline.is_file():
        raise ValueError("Actual native pipeline state is missing; recapture rather than assume defaults")
    return pipeline, hashes


def prepare(capture, output, vertex, fragment):
    if not (capture / "complete.txt").is_file():
        raise ValueError("Incomplete native capture; no scene inputs may be fabricated")
    records = [line.split() for line in (capture / "metadata.txt").read_text().splitlines()]
    pipeline_source, module_hashes = captured_modules(capture, records, vertex, fragment)
    check_original(fragment.read_bytes())
    pipeline_records = [line.split() for line in pipeline_source.read_text().splitlines()]
    pipeline = {line[0]: line[1:] for line in pipeline_records if line[0] != "blend"}
    if (pipeline.get("version") != ["1"] or pipeline.get("push_descriptors") != ["1"] or
            pipeline.get("dynamic_vertex_input") != ["1"] or pipeline.get("subgroup_size") != ["0"]):
        raise ValueError("Unsupported captured pipeline interface or subgroup requirement")
    blends = {int(line[1]): line[2:] for line in pipeline_records if line[0] == "blend"}
    if (set(blends) != {0, 1} or
            any(blend != ["1", "6", "1", "0", "1", "0", "0", "15"]
                for blend in blends.values())):
        raise ValueError("Unsupported captured native lighting blend")
    if (pipeline.get("multisample") != ["1", "0", "0", "0", "0"] or
            len(pipeline.get("clip", [])) != 3 or
            any(value not in ("0", "1") for value in pipeline["clip"]) or
            pipeline.get("provoking_last") not in (["0"], ["1"])):
        raise ValueError("Unsupported or incomplete captured multisample or clipping state")
    scalar = {line[0]: line[1:] for line in records if line[0] in
              ("indices", "index_bytes", "base_vertex", "first_instance", "instances",
               "host_topology", "indexed", "depth_test", "stencil_test", "viewport",
               "vs", "ps", "specular_capture", "depth_func", "depth_write", "depth_bounds",
               "depth_format", "host_viewport", "host_scissor", "host_raster", "host_depth",
               "host_color_masks", "host_render_area")}
    if scalar.get("vs") != ["8747a367"] or scalar.get("ps") != ["d92266b9"]:
        raise ValueError("Unexpected native shader pair")
    if (len(scalar.get("host_depth", [])) != 5 or scalar["host_depth"][1] != "0" or
            scalar["host_depth"][3:] != ["0", "0"]):
        raise ValueError("Stencil, depth-write or depth-bounds behavior is outside this prototype")
    depth_format = int(scalar["depth_format"][0])
    if scalar["host_depth"][0] == "1" and depth_format not in (126, 130):
        raise ValueError("Unsupported actual native depth format")
    if scalar["host_depth"][0] == "0":
        depth_format = 0
    targets = {int(line[1]): fields(line) for line in records if line[0] == "target"}
    width, height = int(targets[0]["width"]), int(targets[0]["height"])
    if targets[0] != targets[1] or int(targets[0]["format"]) != 122:
        raise ValueError("Expected matching native packed lighting targets")
    if (scalar.get("host_render_area") != [str(width), str(height)] or
            len(scalar.get("host_viewport", [])) != 6 or
            len(scalar.get("host_scissor", [])) != 4 or
            len(scalar.get("host_raster", [])) != 6 or
            scalar["host_raster"][0] not in ("0", "1") or
            scalar["host_raster"][1] not in ("0", "1", "2", "3") or
            scalar["host_raster"][2:5] != ["0", "0", "0"] or
            float(scalar["host_raster"][5]) != 1 or
            scalar.get("host_color_masks") != ["15", "15", "0", "0", "0", "0", "0", "0"]):
        raise ValueError("Unsupported or incomplete actual native raster state")
    output.mkdir(exist_ok=False)
    constants = {int(line[1]): fields(line) for line in records if line[0] == "constant"}
    types = {int(line[1]): fields(line) for line in records if line[0] == "buffer_type"}
    selected = []
    for stage, local in ((0, 0), (0, 1), (0, 2), (3, 0)):
        matches = [binding for binding, data in constants.items()
                   if int(data["stage"]) == stage and int(data["local"]) == local]
        if len(matches) != 1:
            raise ValueError("Expected one complete native buffer per replay binding")
        binding = matches[0]
        if int(types[binding]["special"]):
            raise ValueError("Unexpected special buffer in native lighting inputs")
        selected.append(binding)
    if selected[:3] != [0, 1, 2]:
        raise ValueError("Fragment descriptor relinking has not been verified")
    for i, binding in enumerate(selected):
        source = capture / f"constant-{binding}.bin"
        if source.stat().st_size != int(constants[binding]["bytes"]):
            raise ValueError("Captured buffer size mismatch")
        shutil.copyfile(source, output / f"buffer-{i}.bin")
    push = (capture / "push-data.bin").read_bytes()
    if len(push) != 128:
        raise ValueError("Unexpected push-data size")
    (output / "push-data.bin").write_bytes(push)
    attributes = {int(line[1]): fields(line) for line in records if line[0] == "attribute"}
    streams = {int(line[1]): fields(line) for line in records if line[0] == "vertex"}
    vertex_configuration = []
    for location in (0, 1):
        attribute = attributes[location]
        stream = streams[int(attribute["binding"])]
        stride, offset = int(stream["stride"]), int(attribute["offset"])
        components = {103: 2, 106: 3, 109: 4}.get(int(attribute["format"]))
        if components is None or stride < offset + components * 4:
            raise ValueError("Unsupported vertex attribute conversion")
        source = (capture / f"vertex-{attribute['binding']}.bin").read_bytes()
        if len(source) != int(stream["bytes"]) or len(source) % stride:
            raise ValueError("Incomplete captured vertex stream")
        (output / f"vertex-{location}.bin").write_bytes(source)
        vertex_configuration.append(" ".join(map(str, (stride, offset, int(attribute["format"])))))
    if scalar["indexed"] == ["1"]:
        shutil.copyfile(capture / "indices.bin", output / "indices.bin")
    fragment_reflection = json.loads(subprocess.check_output(
        [tool("spirv-cross"), str(fragment), "--reflect"], text=True))
    vertex_reflection = json.loads(subprocess.check_output(
        [tool("spirv-cross"), str(vertex), "--reflect"], text=True))
    fragment_uniforms = {item["binding"] for item in fragment_reflection.get("ubos", [])}
    vertex_uniforms = vertex_reflection.get("ubos", [])
    configuration = [
        "5 " + " ".join(map(str, (width, height, *[int(scalar[key][0]) for key in
            ("indices", "base_vertex", "first_instance", "instances", "host_topology",
             "indexed", "index_bytes")]))),
        " ".join(scalar["host_viewport"]),
        " ".join(map(str, (*[int(i in fragment_uniforms) for i in range(3)],
                          int(bool(vertex_uniforms))))),
        " ".join((scalar["host_depth"][0], scalar["host_depth"][2],
                  scalar["host_depth"][1], str(depth_format))),
        " ".join(scalar["host_raster"]),
        " ".join(scalar["host_scissor"]),
        " ".join(pipeline["clip"]),
        " ".join(pipeline["provoking_last"]),
        " ".join(pipeline["multisample"]),
        *[" ".join(blends[i]) for i in (0, 1)],
        *vertex_configuration,
    ]
    if scalar["host_depth"][0] == "1":
        depth_source = capture / "before-depth.bin"
        if depth_source.stat().st_size != width * height * 4:
            raise ValueError("Incomplete native depth plane")
        shutil.copyfile(depth_source, output / "before-depth.bin")
    images = {int(line[1]): fields(line) for line in records if line[0] == "gpu_image"}
    mips = {int(line[1]): fields(line) for line in records if line[0] == "gpu_mip"}
    for i in range(5):
        image, mip = images[i], mips[i]
        if image["base_mip"] != "0" or image["mip_count"] != "1" or mip["level"] != "0":
            raise ValueError("Only captured single-mip native views are supported")
        # Mapping has four values rather than the metadata's usual key/value pair.
        line = next(line for line in records if line[:2] == ["gpu_image", str(i)])
        mapping = line[line.index("mapping") + 1:]
        configuration.append(" ".join((mip["width"], mip["height"], image["format"], *mapping)))
        source = capture / f"gpu-texture-{i}.bin"
        if source.stat().st_size != int(mip["bytes"]):
            raise ValueError("Incomplete sampled image")
        shutil.copyfile(source, output / f"texture-{i}.bin")
    for i in (0, 1):
        line = next(line for line in records if line[:4] == ["sampler_host", "stage", "0", "local"]
                    and line[4] == str(i))
        sampler = dict(zip(line[5::2], line[6::2]))
        configuration.append(" ".join(sampler[key] for key in
            ("min", "mag", "mip", "u", "v", "w", "bias", "min_lod", "max_lod", "aniso",
             "aniso_enable", "compare_enable", "compare_op", "border")))
        shutil.copyfile(capture / f"before-{i}.bin", output / f"before-{i}.bin")
    (output / "replay.txt").write_text("\n".join(configuration) + "\n", encoding="ascii")
    vertex_bindings = {item["binding"] for kind in ("ubos", "ssbos")
                       for item in vertex_reflection.get(kind, [])}
    vertex_uniform_bindings = {item["binding"] for item in vertex_uniforms}
    vertex_storage_bindings = {item["binding"] for item in vertex_reflection.get("ssbos", [])}
    if (vertex_bindings != {10, 11} or vertex_uniform_bindings not in (set(), {10}) or
            11 not in vertex_storage_bindings or 10 in vertex_uniform_bindings & vertex_storage_bindings):
        raise ValueError("Unsupported actual native vertex descriptor layout; do not relink shaders")
    shutil.copyfile(vertex, output / "native.vert.spv")
    shutil.copyfile(pipeline_source, output / "native-pipeline.txt")
    generate(fragment, output / "native.frag.spv")
    manifest = {
        "capture": str(capture.resolve()), "width": width, "height": height,
        "fragment_sha256": hashlib.sha256(fragment.read_bytes()).hexdigest(),
        "vertex_sha256": hashlib.sha256(vertex.read_bytes()).hexdigest(),
        "vertex_changes": "none: byte-exact actual compiled vertex module",
        "compiled_module_sha256": {str(stage): value for stage, value in module_hashes.items()},
        "pipeline_sha256": hashlib.sha256(pipeline_source.read_bytes()).hexdigest(),
        "depth_format": depth_format,
        "source_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in capture.iterdir() if path.is_file()},
    }
    (output / "inputs.json").write_text(json.dumps(manifest, indent=2))
    return width, height


def replay(runner, fixture, fragment, output, floating=False):
    command = [str(runner), str(fixture), str(fixture / "native.vert.spv"),
               str(fragment), str(output)]
    if floating:
        command.append("--float")
    subprocess.run(command, check=True)


def compare_float_outputs(baseline, result, require_specular_change=True):
    if (baseline.shape != result.shape or baseline.ndim != 4 or
            baseline.shape[0] != 2 or baseline.shape[-1] != 4 or
            baseline.dtype != np.dtype("<f4") or result.dtype != np.dtype("<f4")):
        raise ValueError("Unexpected native/GGX floating-point output shape or type")
    if not np.isfinite(baseline).all() or not np.isfinite(result).all() or not np.array_equal(
            result[0].view("<u4"), baseline[0].view("<u4")):
        raise ValueError("GGX must be finite and preserve native diffuse exactly")
    if not np.array_equal(result[1, ..., 3].view("<u4"),
                          baseline[1, ..., 3].view("<u4")):
        raise ValueError("GGX must preserve native specular blend alpha exactly")
    delta = result[1, ..., :3] - baseline[1, ..., :3]
    if require_specular_change and not np.any(delta):
        raise ValueError("Captured scene does not exercise the GGX specular change")
    return {
        "finite": True, "diffuse_bit_exact": True, "specular_alpha_bit_exact": True,
        "specular_changed_pixels": int(np.any(delta != 0, axis=2).sum()),
        "native_specular_mean": float(baseline[1, ..., :3].mean()),
        "ggx_specular_mean": float(result[1, ..., :3].mean()),
        "maximum_specular_absolute_change": float(np.abs(delta).max()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--vertex", type=Path, required=True)
    parser.add_argument("--fragment", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--mode", choices=("native", "compare"), default="native")
    parser.add_argument("--candidate", choices=GGX_MODES, action="append",
                        help="Opt-in comparison candidate; repeat for several, default width and peak")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if args.candidate and args.mode != "compare":
        parser.error("--candidate requires --mode compare; native mode never enables GGX")
    if args.candidate and len(set(args.candidate)) != len(args.candidate):
        parser.error("Duplicate --candidate would overwrite an experiment")
    width, height = prepare(args.capture, args.output, args.vertex, args.fragment)
    if args.prepare_only:
        print("Complete captured inputs prepared; native image equivalence not yet evaluated")
        return
    native = args.output / "native.frag.spv"
    packed = args.output / "native-packed.raw"
    replay(args.runner, args.output, native, packed)
    actual = np.fromfile(packed, dtype="<u4").reshape(2, height, width)
    expected = np.stack([np.fromfile(args.capture / f"native-{i}.bin", dtype="<u4")
                         .reshape(height, width) for i in (0, 1)])
    mismatch = np.count_nonzero(actual != expected, axis=(1, 2)).tolist()
    report = {"native_packed_mismatched_pixels": mismatch,
              "native_baseline_exact": not any(mismatch), "mode": args.mode,
              "ggx_scene_scoring": "blocked" if any(mismatch) or args.mode == "compare"
                                   else "not_requested"}
    report_path = args.output / "report.json"
    report_path.write_text(json.dumps(report, indent=2))
    if any(mismatch):
        raise ValueError("Native replay is not exact; GGX scene scoring is blocked")
    replay(args.runner, args.output, native, args.output / "native-float.raw", True)
    baseline = np.fromfile(args.output / "native-float.raw", dtype="<f4").reshape(2, height, width, 4)
    report["native_float_finite"] = bool(np.isfinite(baseline).all())
    report_path.write_text(json.dumps(report, indent=2))
    if not report["native_float_finite"]:
        raise ValueError("Nonfinite native reference; invalid scene samples need investigation")
    if args.mode == "compare":
        report["ggx"] = {}
        for mode in args.candidate or ("ggx-width", "ggx-peak"):
            shader = args.output / f"{mode}.frag.spv"
            generate(args.fragment, shader, mode)
            replay(args.runner, args.output, shader, args.output / f"{mode}-float.raw", True)
            result = np.fromfile(args.output / f"{mode}-float.raw", dtype="<f4").reshape(2, height, width, 4)
            report["ggx"][mode] = compare_float_outputs(baseline, result)
        report["ggx_scene_scoring"] = "verified"
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
