"""Offline GPU material/angular controls, requiring an already exact native scene replay.

The atlases replace scene inputs deliberately, never shaders or game caches.
Their float64 BRDF oracle is a numerical control, not the packed scene baseline.
PNG previews show this directional draw only, not final composited game frames.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dump_view import hdr_to_png
from ggx_directional_patch import check_original, generate
from ggx_directional_replay import captured_modules, compare_float_outputs, replay


MODES = ("native", "ggx-width", "ggx-peak")
LABELS = ("Native", "GGX width", "GGX peak")
CALIBRATION_MODES = ("native", "ggx-width", "ggx-width-authored")
CALIBRATION_LABELS = ("Native", "GGX width", "GGX native-anchored")
SWATCHES = ((0, 0, 0), (51, 51, 51), (128, 128, 128), (230, 230, 230), (180, 95, 60))
MATERIAL_ROWS = (
    ("smoothness 0", 0, 50, 0), ("smoothness 64", 64, 50, 0),
    ("smoothness 128", 128, 50, 0), ("smoothness 192", 192, 50, 0),
    ("smoothness 255", 255, 50, 0), ("255 / narrow light", 255, 1e6, 0),
    ("128 / diffuse wrap", 128, 50, 64),
)
CALIBRATION_MATERIAL_ROWS = tuple(
    (f"{smoothness} / {name}", smoothness, width, 0)
    for name, width in (("width 50", 50), ("broad", 2), ("narrow", 1e6))
    for smoothness in (0, 64, 128, 192, 255)
) + (("128 / diffuse wrap", 128, 50, 64),)
ANGULAR_ROWS = (
    ("first lobe / L35", 0, 35, 50), ("second lobe / L35", 1, 35, 50),
    ("third lobe / L35", 2, 35, 50), ("fourth lobe / L35", 3, 35, 50),
    ("first / grazing L160", 0, 160, 50), ("third / grazing L160", 2, 160, 50),
    ("fourth / grazing L160", 3, 160, 50), ("third / rear L120", 2, 120, 50),
    ("fourth / rear L120", 3, 120, 50), ("opposing L180", 0, 180, 50),
    ("zero specular light", None, 35, 50), ("disabled extra lobes", None, 35, 50),
    ("first / broad light", 0, 35, 2),
)
CALIBRATION_ANGULAR_ROWS = ANGULAR_ROWS + tuple(
    (f"L160 s{smoothness} {name}/{slot + 1}", slot, 160, width, smoothness)
    for slot in (0, 2, 3)
    for name, width in (("w50", 50), ("broad", 2), ("narrow", 1e6))
    for smoothness in (0, 64, 128, 192, 255)
)
ORACLE_ATOL = 1e-6
ORACLE_RTOL = 0.002


def read_float(path, width, height):
    if path.stat().st_size != 2 * width * height * 4 * 4:
        raise ValueError(f"Wrong floating-point output size: {path}")
    return np.fromfile(path, dtype="<f4").reshape(2, height, width, 4)


def verify_scene(fixture):
    manifest = json.loads((fixture / "inputs.json").read_text())
    report = json.loads((fixture / "report.json").read_text())
    if (report.get("native_packed_mismatched_pixels") != [0, 0] or
            report.get("ggx_scene_scoring") != "verified"):
        raise ValueError("An exact native scene and both verified GGX comparisons are required")
    capture = Path(manifest["capture"])
    records = [line.split() for line in (capture / "metadata.txt").read_text().splitlines()]
    pipeline, _ = captured_modules(
        capture, records, fixture / "native.vert.spv", fixture / "native.frag.spv")
    check_original((fixture / "native.frag.spv").read_bytes())
    if hashlib.sha256(pipeline.read_bytes()).hexdigest() != manifest["pipeline_sha256"]:
        raise ValueError("Native pipeline evidence changed after scene verification")
    for name, digest in manifest["source_sha256"].items():
        if hashlib.sha256((capture / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Native capture source changed after scene verification: {name}")
    width, height = manifest["width"], manifest["height"]
    expected = b"".join((capture / f"native-{i}.bin").read_bytes() for i in (0, 1))
    if (len(expected) != 2 * width * height * 4 or
            (fixture / "native-packed.raw").read_bytes() != expected):
        raise ValueError("Packed native scene no longer matches the actual draw exactly")
    scene = {mode: read_float(fixture / f"{mode}-float.raw", width, height) for mode in MODES}
    for mode in MODES[1:]:
        compare_float_outputs(scene["native"], scene[mode])
    config = (fixture / "replay.txt").read_text().splitlines()
    if (len(config) != 20 or config[0].split() !=
            ["5", str(width), str(height), "4", "0", "0", "1", "4", "0", "2"] or
            config[2] != "0 1 1 1" or any((fixture / f"buffer-{i}.bin").stat().st_size != size
                                         for i, size in ((0, 64800), (1, 208), (2, 864), (3, 16))) or
            any((fixture / f"vertex-{i}.bin").stat().st_size != 96 for i in (0, 1))):
        raise ValueError("Unsupported exact directional control fixture layout")
    push = np.fromfile(fixture / "push-data.bin", dtype="<u4")
    if push.size != 32 or np.any(push[4:]):
        raise ValueError("Controlled fixture requires the captured zero descriptor offsets")
    return config, scene, manifest


def light_direction(degrees):
    if degrees == 180:
        return np.array([0, 0, -1], dtype="<f4")
    angle = np.radians(degrees)
    return np.array([np.sin(angle), 0, np.cos(angle)], dtype="<f4")


def lighting_profile(table, tag, slot, degrees, width):
    profile = table[tag]
    profile[:4, 2] = 1
    profile[:4, 3] = width
    profile[17] = 1
    profile[slot, :3] = light_direction(degrees)
    profile[4 + slot, :3] = 0.2
    profile[8 + slot, :3] = 1
    if slot >= 2:
        profile.view("<u4")[16, slot - 2] = 1


def encoded_normals(normals):
    return np.clip(np.rint((normals + 1) * 127.5), 0, 255).astype("u1")


def material_atlas(tile=96, rows=MATERIAL_ROWS):
    height, width = len(rows) * tile, len(SWATCHES) * tile
    textures = [np.full((height, width, 4), 255, dtype="u1") for _ in range(3)]
    table = np.zeros((225, 18, 4), dtype="<f4")
    axis = (np.arange(tile) + 0.5) * 2 / tile - 1
    x, y = np.meshgrid(axis, -axis)
    inside = x * x + y * y < 1
    normals = np.stack((x, y, np.sqrt(np.maximum(0, 1 - x * x - y * y))), axis=-1)
    normals[~inside] = (0, 0, -1)
    normal = encoded_normals(normals)
    for row, (_, smoothness, light_width, wrap) in enumerate(rows):
        lighting_profile(table, row, 0, 35, light_width)
        for column, color in enumerate(SWATCHES):
            region = np.s_[row * tile:(row + 1) * tile, column * tile:(column + 1) * tile]
            textures[0][region] = (*color, row)
            textures[1][region][..., :3] = normal
            textures[2][region] = (smoothness, wrap, 0, 0)
    return textures, table


def angular_atlas(width=384, stripe=16, rows=ANGULAR_ROWS):
    height = len(rows) * stripe
    textures = [np.full((height, width, 4), 255, dtype="u1") for _ in range(3)]
    table = np.zeros((225, 18, 4), dtype="<f4")
    angle = np.radians(np.linspace(-110, 110, width))
    normals = encoded_normals(np.stack((np.sin(angle), np.zeros(width), np.cos(angle)), axis=-1))
    for row, data in enumerate(rows):
        _, slot, degrees, light_width = data[:4]
        smoothness = data[4] if len(data) == 5 else 128
        region = np.s_[row * stripe:(row + 1) * stripe]
        textures[0][region] = (128, 128, 128, row)
        textures[1][region][..., :3] = normals
        textures[2][region] = (smoothness, 0, 0, 0)
        lighting_profile(table, row, slot if slot is not None else 0, degrees, light_width)
        if row == 10:
            for i in range(4):
                lighting_profile(table, row, i, degrees, light_width)
            table[row, 8:12, :3] = 0
        elif row == 11:
            for i in (2, 3):
                lighting_profile(table, row, i, degrees, light_width)
            table[row, 8:10, :3] = 0
            table[row].view("<u4")[16, :2] = 0
    return textures, table


def reference_specular(textures, table, mode):
    normal = textures[1][..., :3].astype(np.float64) / 255 * 2 - 1
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    f0 = (textures[0][..., :3].astype(np.float64) / 255) ** 2
    profiles = table[textures[0][..., 3]].astype(np.float64)
    flags = table.astype("<f4", copy=False).view("<u4")[textures[0][..., 3], 16, :2]
    exponent = np.exp2(13 * textures[2][..., 0].astype(np.float64) / 255)
    nv = normal[..., 2]
    result = np.zeros_like(f0)
    for slot in range(4):
        light = profiles[..., slot, :3]
        half = light.copy()
        half[..., 2] += 1
        length = np.linalg.norm(half, axis=-1, keepdims=True)
        half = np.divide(half, length, out=np.zeros_like(half), where=length != 0)
        nl = np.sum(normal * light, axis=-1)
        nh = np.clip(np.sum(normal * half, axis=-1), 0, 1)
        n = np.maximum(1, exponent * profiles[..., slot, 3] /
                       (exponent * 0.01000213623046875 + profiles[..., slot, 3]))
        fresnel = f0 + (1 - f0) * (1 - np.clip(half[..., 2:3], 0, 1)) ** 5
        if mode == "native":
            lobe = (n + 2) / 8 * nh ** n
            if slot < 2:
                lobe *= np.clip(nl * 16, 0, 1)
        else:
            if mode in ("ggx-width", "ggx-width-authored"):
                c2 = np.exp2(-2 / n)
                a2 = (1 - c2) / (np.sqrt(2) - c2)
            elif mode == "ggx-peak":
                a2 = 2 / (n + 2)
            else:
                raise ValueError("Unknown control BRDF")
            denominator = (1 - nh * nh) + a2 * nh * nh
            distribution = a2 / denominator ** 2
            v, l = np.minimum(nv, 1), np.minimum(nl, 1)
            visibility = 0.5 / np.maximum(
                l * np.sqrt((1 - a2) * v * v + a2) +
                v * np.sqrt((1 - a2) * l * l + a2), 1e-20)
            lobe = np.where((nv > 0) & (nl > 0) & (nh > 0), distribution * visibility * l, 0)
            if mode == "ggx-width-authored":
                c = np.clip(half[..., 2], 0, 1)
                native_reference = (n + 2) / 8
                if slot < 2:
                    native_reference *= np.minimum(16 * c, 1)
                ggx_reference = 1 / (4 * a2 * np.sqrt((1 - a2) * c * c + a2))
                lobe *= native_reference / ggx_reference
        if slot >= 2:
            lobe = np.where(flags[..., slot - 2] > 0, lobe, 0)
        result += lobe[..., None] * fresnel * profiles[..., 8 + slot, :3]
    return result


def calibration_sweep():
    widths = np.array([2, 50, 1e6], dtype=np.float64)
    smoothness = np.arange(256, dtype=np.float64)
    base = np.exp2(13 * smoothness[:, None] / 255)
    n = np.maximum(1, base * widths / (base * 0.01000213623046875 + widths))
    c2 = np.exp2(-2 / n)
    a2 = (1 - c2) / (np.sqrt(2) - c2)
    half_height = a2 ** 2 / ((1 - c2) + a2 * c2) ** 2
    half_height_error = float(np.abs(half_height - 0.5).max())
    angles = np.array([0, 30, 60, 75, 80, 85, 89, 89.99])
    c = np.cos(np.radians(angles))
    reference_root = np.sqrt((1 - a2[..., None]) * c * c + a2[..., None])
    ggx_reference = 1 / (4 * a2[..., None] * reference_root)
    errors = []
    for primary in (False, True):
        native_reference = (n[..., None] + 2) / 8 * (np.minimum(16 * c, 1) if primary else 1)
        gain = native_reference / ggx_reference
        errors.append(np.abs(ggx_reference * gain / native_reference - 1).max())
    if not np.isfinite(a2).all() or max(errors) > 1e-12 or half_height_error > 1e-11:
        raise ValueError("Analytic native-reference normalization failed across the material/light-width sweep")
    rows = []
    for byte in (0, 64, 128, 192, 255):
        for j, width in enumerate(widths):
            exponent, alpha2 = n[byte, j], a2[byte, j]
            native_half = float(np.degrees(np.arccos(np.exp2(-1 / exponent))))
            angular = []
            for degrees in (0, 60, 80, 89):
                cosine = np.cos(np.radians(degrees))
                gate = min(16 * cosine, 1)
                native_peak = (exponent + 2) / 8 * gate
                raw_peak = 1 / (4 * alpha2 * np.sqrt((1 - alpha2) * cosine * cosine + alpha2))
                target = 1 / (4 * native_peak)
                if target > 1:
                    peak_alpha2, peak_half = None, None
                else:
                    low, high = 0.0, 1.0
                    for _ in range(64):
                        mid = (low + high) / 2
                        if mid * np.sqrt((1 - mid) * cosine * cosine + mid) < target:
                            low = mid
                        else:
                            high = mid
                    peak_alpha2 = (low + high) / 2
                    half_cos2 = (1 - np.sqrt(2) * peak_alpha2) / (1 - peak_alpha2)
                    peak_half = (float(np.degrees(np.arccos(np.sqrt(half_cos2))))
                                 if half_cos2 >= 0 else None)
                angular.append({
                    "mirrored_incidence_degrees": degrees,
                    "unscaled_ggx_width_peak_ratio_to_native": float(raw_peak / native_peak),
                    "authored_reference_gain": float(native_peak / raw_peak),
                    "roughness_only_peak_match_alpha_squared": peak_alpha2,
                    "roughness_only_peak_match_ndf_half_angle_degrees": peak_half,
                })
            rows.append({"smoothness_byte": byte, "native_light_width": float(width),
                         "effective_exponent": float(exponent), "width_alpha_squared": float(alpha2),
                         "native_and_width_ggx_ndf_half_angle_degrees": native_half,
                         "angles": angular})
    return {
        "smoothness_byte_count": 256, "native_light_widths": widths.tolist(),
        "mirrored_incidence_angles_degrees": angles.tolist(), "primary_and_optional_lobes_checked": True,
        "maximum_reference_peak_relative_error": float(max(errors)), "representative_rows": rows,
        "maximum_ndf_half_height_error": half_height_error,
        "limitations": [
            "Matching the N=H reference does not guarantee the maximum over all normals.",
            "Distribution half-width is preserved; full Smith/cosine response can shift peak and width.",
            "Angle-dependent authored normalization is not an energy-conservation or full PBR claim.",
            "No clipping of output brightness and no change to the native light-width formula.",
        ],
    }


def write_fixture(source, destination, config, textures, table):
    destination.mkdir()
    height, width = textures[0].shape[:2]
    config = config.copy()
    config[0] = f"5 {width} {height} 4 0 0 1 4 0 2"
    config[1] = f"0 {height} {width} {-height} 0 1"
    config[5] = f"0 0 {width} {height}"
    for i in range(5):
        config[13 + i] = " ".join((str(width), str(height), *config[13 + i].split()[2:]))
    (destination / "replay.txt").write_text("\n".join(config) + "\n", encoding="ascii")
    for name in ("native.vert.spv", "vertex-0.bin", "vertex-1.bin", "push-data.bin", "buffer-3.bin"):
        shutil.copyfile(source / name, destination / name)
    table.tofile(destination / "buffer-0.bin")
    camera = np.zeros(52, dtype="<f4")
    camera[11], camera[15] = -1, 1
    camera[[32, 37, 42]] = 1
    camera.tofile(destination / "buffer-1.bin")
    constants = np.zeros(216, dtype="<f4")
    constants[76:79] = 1
    constants.tofile(destination / "buffer-2.bin")
    for i, texture in enumerate(textures):
        texture.tofile(destination / f"texture-{i}.bin")
    np.ones((height, width), dtype="<f4").tofile(destination / "texture-3.bin")
    np.full((height, width, 4), 255, dtype="u1").tofile(destination / "texture-4.bin")
    np.ones((height, width), dtype="<f4").tofile(destination / "before-depth.bin")


def check_control(textures, table, results):
    summary = {}
    for mode, result in results.items():
        if not np.isfinite(result).all() or np.any(result[..., 3] != 1):
            raise ValueError(f"{mode}: nonfinite controls or incomplete fullscreen coverage")
        expected = reference_specular(textures, table, mode)
        error = np.abs(result[1, ..., :3].astype(np.float64) - expected)
        tolerance = ORACLE_ATOL + ORACLE_RTOL * np.abs(expected)
        if np.any(error > tolerance):
            pixel = np.unravel_index(np.argmax(error / tolerance), error.shape)
            raise ValueError(f"{mode}: GPU BRDF disagrees with independent oracle at {pixel}: "
                             f"actual {result[1][pixel]}, expected {expected[pixel]}")
        summary[mode] = {"finite": True, "full_coverage": True,
                         "oracle_maximum_tolerance_fraction": float(np.max(error / tolerance))}
        if mode != "native":
            summary[mode].update(compare_float_outputs(results["native"], result))
    return summary


def comparison_sheet(path, panels, row_labels, tile, title, subtitle, gain, swatch_labels=None,
                     labels=LABELS):
    left, header = 190, 100
    panel_height, panel_width = panels[0].shape[:2]
    image = Image.new("RGB", (left + 3 * (panel_width + 12), header + panel_height + 32), "#171a20")
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=16)
    draw.text((12, 10), title, fill="white", font=ImageFont.load_default(size=20))
    draw.text((12, 38), subtitle, fill="#bec8d4", font=font)
    for i, (label, panel) in enumerate(zip(labels, panels)):
        x = left + i * (panel_width + 12)
        draw.text((x, 64), label, fill="white", font=font)
        image.paste(Image.fromarray(hdr_to_png(panel * gain)), (x, header))
        if swatch_labels:
            small = ImageFont.load_default(size=12)
            for j, label in enumerate(swatch_labels):
                draw.text((x + j * tile + 4, header + panel_height + 8), label, fill="white", font=small)
    for row, label in enumerate(row_labels):
        draw.text((8, header + row * tile + tile // 2 - 8), label, fill="#bec8d4", font=font)
    with path.open("xb") as file:
        image.save(file, format="PNG")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("verified_fixture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--calibrate", action="store_true",
                        help="Compare width-matched GGX with opt-in native-reference normalization")
    args = parser.parse_args()
    config, scene, manifest = verify_scene(args.verified_fixture)
    modes = CALIBRATION_MODES if args.calibrate else MODES
    labels = CALIBRATION_LABELS if args.calibrate else LABELS
    material_rows = CALIBRATION_MATERIAL_ROWS if args.calibrate else MATERIAL_ROWS
    angular_rows = CALIBRATION_ANGULAR_ROWS if args.calibrate else ANGULAR_ROWS
    args.output.mkdir(exist_ok=False)
    shaders = args.output / "shaders"
    shaders.mkdir()
    for mode in modes:
        generate(args.verified_fixture / "native.frag.spv", shaders / f"{mode}.frag.spv", mode)
        source_name = "native.frag.spv" if mode == "native" else f"{mode}.frag.spv"
        if mode in MODES and (shaders / f"{mode}.frag.spv").read_bytes() != (
                args.verified_fixture / source_name).read_bytes():
            raise ValueError("Scene candidates differ from the current generator; rerun exact scene comparison")
    report = {"status": "in_progress", "scope": "offline directional controls; not final game appearance",
              "scene_fixture": str(args.verified_fixture.resolve()),
              "shader_sha256": {mode: hashlib.sha256((shaders / f"{mode}.frag.spv").read_bytes()).hexdigest()
                                for mode in modes},
              "compiled_module_sha256": manifest["compiled_module_sha256"],
              "synthetic_inputs": True, "native_scene_packed_mismatches": [0, 0],
              "oracle_absolute_tolerance": ORACLE_ATOL, "oracle_relative_tolerance": ORACLE_RTOL,
              "oracle_tolerance_is_not_a_scene_baseline_tolerance": True,
              "display": {"mapping": "Reinhard then sRGB, same gain for all candidates",
                          "material_gain": 1, "angle_gain": 1, "scene_specular_gain": 128,
                          "scene_direct_lighting_gain": 2},
              "controls": {}}
    report_path = args.output / "report.json"
    report_path.write_text(json.dumps(report, indent=2))
    if args.calibrate:
        report["calibration"] = calibration_sweep()
        report["calibration"]["mode"] = "ggx-width-authored"
        scene_output = args.output / "ggx-width-authored-scene-float.raw"
        replay(args.runner, args.verified_fixture, shaders / "ggx-width-authored.frag.spv", scene_output, True)
        scene["ggx-width-authored"] = read_float(scene_output, manifest["width"], manifest["height"])
        report["calibration"]["scene"] = compare_float_outputs(scene["native"], scene["ggx-width-authored"])
        native_mean = report["calibration"]["scene"]["native_specular_mean"]
        if native_mean <= 0:
            raise ValueError("Calibration scene must have positive native direct-specular signal")
        report["calibration"]["scene_mean_specular_ratio_to_native"] = (
            report["calibration"]["scene"]["ggx_specular_mean"] / native_mean)
        report_path.write_text(json.dumps(report, indent=2))
    rendered = {}
    for name in ("materials", "angles"):
        textures, table = (material_atlas(rows=material_rows) if name == "materials" else
                           angular_atlas(width=512 if args.calibrate else 384, rows=angular_rows))
        destination = args.output / name
        write_fixture(args.verified_fixture, destination, config, textures, table)
        height, width = textures[0].shape[:2]
        results = {}
        for mode in modes:
            output = destination / f"{mode}-float.raw"
            replay(args.runner, destination, shaders / f"{mode}.frag.spv", output, True)
            results[mode] = read_float(output, width, height)
        rendered[name] = results
        report["controls"][name] = check_control(textures, table, results)
        report["controls"][name]["output_sha256"] = {
            mode: hashlib.sha256((destination / f"{mode}-float.raw").read_bytes()).hexdigest()
            for mode in modes}
        row_height = 96 if name == "materials" else 16
        rows = material_rows if name == "materials" else angular_rows
        report["controls"][name]["row_metrics"] = [
            {"label": row[0], "models": {
                mode: {"specular_peak": float(result[1, i * row_height:(i + 1) * row_height, :, :3].max()),
                       "specular_mean": float(result[1, i * row_height:(i + 1) * row_height, :, :3].mean())}
                for mode, result in results.items()}}
            for i, row in enumerate(rows)]
        report["controls"][name]["input_sha256"] = {
            file.name: hashlib.sha256(file.read_bytes()).hexdigest()
            for file in destination.iterdir() if file.suffix == ".bin" or file.name == "replay.txt"}
        if name == "materials":
            plain_row = next(i for i, row in enumerate(material_rows) if row[1:] == (128, 50, 0))
            wrapped_row = next(i for i, row in enumerate(material_rows) if row[1:] == (128, 50, 64))
            plain, wrapped = (np.s_[i * 96:(i + 1) * 96] for i in (plain_row, wrapped_row))
            for mode, result in results.items():
                if (not np.array_equal(result[1, plain].view("<u4"), result[1, wrapped].view("<u4")) or
                        np.array_equal(result[0, plain].view("<u4"), result[0, wrapped].view("<u4"))):
                    raise ValueError(f"{mode}: wrapped diffuse control did not isolate diffuse behavior")
                if not np.any(result[1, :, :96, :3]):
                    raise ValueError(f"{mode}: zero F0 incorrectly suppressed Schlick grazing reflection")
            report["controls"][name]["wrapped_diffuse_changes_without_specular_changes"] = True
            report["controls"][name]["zero_f0_retains_schlick_reflection"] = True
            report["controls"][name]["specular_rgb_bytes"] = [list(swatch) for swatch in SWATCHES]
        else:
            for row in (10, 11):
                region = np.s_[row * 16:(row + 1) * 16]
                for mode, result in results.items():
                    if np.any(result[1][region][..., :3]) or not np.any(result[0][region][..., :3]):
                        raise ValueError(f"{mode}: zero-specular or disabled-lobe control failed")
            for row in range(4):
                for mode in modes[1:]:
                    compare_float_outputs(results["native"][:, row * 16:(row + 1) * 16],
                                          results[mode][:, row * 16:(row + 1) * 16])
            normal = textures[1][..., :3].astype(np.float64) / 255 * 2 - 1
            normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
            backfacing = normal[..., 2] <= 0
            for mode in modes[1:]:
                if np.any(results[mode][1, ..., :3][backfacing]):
                    raise ValueError(f"{mode}: back-facing view must have exactly zero specular")
                for row, data in enumerate(angular_rows):
                    slot = data[1]
                    if slot is not None:
                        region = np.s_[row * 16:(row + 1) * 16]
                        rear = np.sum(normal[region] * table[row, slot, :3], axis=-1) <= 0
                        if np.any(results[mode][1][region][..., :3][rear]):
                            raise ValueError(f"{mode}: back-facing light must have exactly zero specular")
            report["controls"][name]["rows"] = [list(row) for row in angular_rows]
            report["controls"][name]["zero_specular_and_disabled_lobes_exactly_dark"] = True
            report["controls"][name]["all_four_specular_sites_exercised"] = True
            report["controls"][name]["backfacing_view"] = {
                mode: float(result[1, ..., :3][backfacing].max())
                for mode, result in results.items()}
            report["controls"][name]["ggx_backfacing_lights_exactly_dark"] = True
            native_peak = float(results["native"][1, 4 * 16:5 * 16, :, :3].max())
            report["controls"][name]["mirrored_grazing_peak_ratio_to_native"] = {
                mode: float(results[mode][1, 4 * 16:5 * 16, :, :3].max()) / native_peak
                for mode in modes[1:]}
        report_path.write_text(json.dumps(report, indent=2))
    if args.calibrate:
        for name in ("materials", "angles"):
            ratios = [
                {"label": row["label"],
                 "ratio": row["models"]["ggx-width-authored"]["specular_peak"] /
                          row["models"]["native"]["specular_peak"]}
                for row in report["controls"][name]["row_metrics"]
                if row["models"]["native"]["specular_peak"] > 0]
            report["calibration"][f"{name}_sampled_peak_ratio_range"] = {
                "minimum": min(ratios, key=lambda row: row["ratio"]),
                "maximum": max(ratios, key=lambda row: row["ratio"])}
    comparison_sheet(
        args.output / "material-controls.png",
        [rendered["materials"][mode][1, ..., :3] for mode in modes],
        [row[0] for row in material_rows], 96, "Synthetic material controls - direct specular only",
        "Encoded specular RGB swatches; native 8-bit normals. Same gain 1 / Reinhard / sRGB.", 1,
        ("zero F0", "low grey", "mid grey", "high grey", "tinted"), labels=labels)
    angle_panels = [np.repeat(rendered["angles"][mode][1, ..., :3], 2, axis=0) for mode in modes]
    comparison_sheet(
        args.output / "angular-controls.png", angle_panels, [row[0] for row in angular_rows], 32,
        "Synthetic angular controls - all four directional specular lobes",
        "View +Z; normal sweeps -110 to +110 degrees left to right. Same gain 1; not a game frame.",
        1, labels=labels)
    if args.calibrate:
        selected = list(range(13))
        selected.extend(next(i for i, row in enumerate(angular_rows)
                             if len(row) == 5 and row[1:] == values)
                        for values in ((0, 160, 50, 0), (3, 160, 50, 0), (0, 160, 1e6, 255)))
        summary_panels = [
            np.concatenate([panel[i * 32:(i + 1) * 32] for i in selected], axis=0)
            for panel in angle_panels]
        comparison_sheet(
            args.output / "angular-calibration-summary.png", summary_panels,
            [angular_rows[i][0] for i in selected], 32,
            "Native-anchored calibration - representative angles and rough-grazing limit",
            "Excerpt from 58 GPU rows. Authored response, not pure GGX. Same display gain 1.",
            1, labels=labels)
    scene_panels = [np.concatenate((scene[mode][1, ..., :3] * 128,
                                   (scene[mode][0, ..., :3] + scene[mode][1, ..., :3]) * 2), axis=0)
                    for mode in modes]
    scene_panels = [np.asarray(Image.fromarray(hdr_to_png(panel)).resize(
        (640, 720), Image.Resampling.LANCZOS)) for panel in scene_panels]
    # Panels are already display encoded; do not tone-map them a second time.
    preview = Image.new("RGB", (1920, 814), "#171a20")
    draw = ImageDraw.Draw(preview)
    font = ImageFont.load_default(size=16)
    draw.text((12, 8), "Captured directional draw only - NOT final composited game appearance",
              fill="white", font=ImageFont.load_default(size=20))
    draw.text((12, 36), "Top: specular gain 128. Bottom: this draw's diffuse + specular gain 2. "
              "Same Reinhard/sRGB display transform across models.", fill="#bec8d4", font=font)
    for i, panel in enumerate(scene_panels):
        draw.text((i * 640 + 12, 66), labels[i], fill="white", font=font)
        preview.paste(Image.fromarray(panel), (i * 640, 94))
    with (args.output / "directional-draw-preview.png").open("xb") as file:
        preview.save(file, format="PNG")
    verify_scene(args.verified_fixture)
    report["status"] = "verified"
    report["controls"]["materials"]["rows"] = [list(row) for row in material_rows]
    report["performance_measured"] = False
    report["production_model_selected"] = False
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
