"""Offline-only GGX experiment for one exact translated directional-light module.

Native mode (the default) copies the original bytes. The opt-in GGX modes replace
only four specular lobes: native Fresnel, diffuse, visibility weights, material
decoding and descriptors remain intact. ggx-width-authored additionally applies
an analytic native-reference gain, not an energy-conserving/PBR conversion.
No generated game-derived module should be committed. The renderer independently
emits the native-anchored transformation through vk_ggx_patch.h when opted in.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


ORIGINAL_WORDS = 7981
ORIGINAL_FNV = 0x4C359939
ORIGINAL_SHA256 = "a68e602feaf7b6d57b67521cc35c48bce800abf5448b5f671b484b4104b9bfc2"
ORIGINALS = {
    ORIGINAL_SHA256: (ORIGINAL_WORDS, ORIGINAL_FNV, "legacy"),
    "c20b4cfbcc7e5182135e8a6d5e9f20dd0e92bf1b47d58f27fbd822f09ed4c592":
        (9664, 0xF9B58C68, "current"),
}
GGX_MODES = ("ggx-width", "ggx-peak", "ggx-width-authored")


def check_original(data):
    digest = hashlib.sha256(data).hexdigest()
    known = ORIGINALS.get(digest)
    if not known or len(data) != known[0] * 4:
        raise ValueError("Unrecognized directional shader size")
    words = struct.unpack(f"<{known[0]}I", data)
    checksum = 2166136261
    for word in words:
        checksum = ((checksum ^ word) * 16777619) & 0xFFFFFFFF
    if words[0] != 0x07230203 or checksum != known[1]:
        raise ValueError("Unrecognized directional shader; original bytes required")
    return known[2]


def tool(name):
    folder = os.environ.get("SPIRV_BIN")
    if folder:
        return str(Path(folder) / (name + (".exe" if os.name == "nt" else "")))
    result = shutil.which(name)
    if not result:
        raise FileNotFoundError(f"{name} not found; set SPIRV_BIN to the SPIR-V tools folder")
    return result


def run_tool(name, *args):
    subprocess.run([tool(name), *map(str, args)], check=True)


def ggx_function(mode):
    # Pi-scaled D * correlated Smith visibility * NoL; native Schlick follows it.
    authored = mode == "ggx-width-authored"
    mapping = (
        "%ggx_t = OpFDiv %f32_id %ggx_minus_two %ggx_n\n"
        "%ggx_c2 = OpExtInst %f32_id %329 Exp2 %ggx_t\n"
        "%ggx_num = OpFSub %f32_id %f32_id_1 %ggx_c2\n"
        "%ggx_map_den = OpFSub %f32_id %ggx_sqrt_two %ggx_c2\n"
        "%ggx_a2 = OpFDiv %f32_id %ggx_num %ggx_map_den\n"
        if mode in ("ggx-width", "ggx-width-authored") else
        "%ggx_map_den = OpFAdd %f32_id %ggx_n %f32_id_2\n"
        "%ggx_a2 = OpFDiv %f32_id %f32_id_2 %ggx_map_den\n"
    )
    parameters = (
        "%ggx_view_half = OpFunctionParameter %f32_id\n"
        "%ggx_primary = OpFunctionParameter %f32_id\n"
        if authored else ""
    )
    # Reference normalization at N=H; not an output clamp or energy-conservation claim.
    normalization = (
        "%ggx_c = OpExtInst %f32_id %329 FClamp %ggx_view_half %f32_id_0 %f32_id_1\n"
        "%ggx_c_squared = OpFMul %f32_id %ggx_c %ggx_c\n"
        "%ggx_reference_part = OpFMul %f32_id %ggx_one_a2 %ggx_c_squared\n"
        "%ggx_reference_sum = OpFAdd %f32_id %ggx_reference_part %ggx_a2\n"
        "%ggx_reference_root = OpExtInst %f32_id %329 Sqrt %ggx_reference_sum\n"
        "%ggx_reference_n = OpFAdd %f32_id %ggx_n %f32_id_2\n"
        "%ggx_reference_width = OpFMul %f32_id %ggx_reference_n %ggx_a2\n"
        "%ggx_reference_product = OpFMul %f32_id %ggx_reference_width %ggx_reference_root\n"
        "%ggx_reference_scale = OpFMul %f32_id %ggx_reference_product %f32_id_0_5\n"
        "%ggx_reference_gate_wide = OpFMul %f32_id %ggx_c %f32_id_16\n"
        "%ggx_reference_gate_primary = OpExtInst %f32_id %329 FMin %ggx_reference_gate_wide %f32_id_1\n"
        "%ggx_is_primary = OpFOrdGreaterThan %bool_id %ggx_primary %f32_id_0\n"
        "%ggx_reference_gate = OpSelect %f32_id %ggx_is_primary %ggx_reference_gate_primary %f32_id_1\n"
        "%ggx_reference_gain = OpFMul %f32_id %ggx_reference_scale %ggx_reference_gate\n"
        "%ggx_lit = OpFMul %f32_id %ggx_raw_lit %ggx_reference_gain\n"
        if authored else ""
    )
    return (
        "%ggx_eval = OpFunction %f32_id None %ggx_function_type\n"
        "%ggx_exponent = OpFunctionParameter %f32_id\n"
        "%ggx_view = OpFunctionParameter %f32_id\n"
        "%ggx_light = OpFunctionParameter %f32_id\n"
        "%ggx_half = OpFunctionParameter %f32_id\n"
        + parameters +
        "%ggx_entry = OpLabel\n"
        "%ggx_front_v = OpFOrdGreaterThan %bool_id %ggx_view %f32_id_0\n"
        "%ggx_front_l = OpFOrdGreaterThan %bool_id %ggx_light %f32_id_0\n"
        "%ggx_front_h = OpFOrdGreaterThan %bool_id %ggx_half %f32_id_0\n"
        "%ggx_front_vl = OpLogicalAnd %bool_id %ggx_front_v %ggx_front_l\n"
        "%ggx_front = OpLogicalAnd %bool_id %ggx_front_vl %ggx_front_h\n"
        "OpSelectionMerge %ggx_merge None\n"
        "OpBranchConditional %ggx_front %ggx_visible %ggx_dark\n"
        "%ggx_dark = OpLabel\n"
        "OpBranch %ggx_merge\n"
        "%ggx_visible = OpLabel\n"
        "%ggx_n = OpExtInst %f32_id %329 FClamp %ggx_exponent %f32_id_1 %ggx_max_n\n"
        "%ggx_v = OpExtInst %f32_id %329 FMin %ggx_view %f32_id_1\n"
        "%ggx_l = OpExtInst %f32_id %329 FMin %ggx_light %f32_id_1\n"
        "%ggx_h = OpExtInst %f32_id %329 FMin %ggx_half %f32_id_1\n"
        + mapping +
        "%ggx_h2 = OpFMul %f32_id %ggx_h %ggx_h\n"
        "%ggx_one_h2 = OpFSub %f32_id %f32_id_1 %ggx_h2\n"
        "%ggx_ah2 = OpFMul %f32_id %ggx_a2 %ggx_h2\n"
        "%ggx_d_den = OpFAdd %f32_id %ggx_one_h2 %ggx_ah2\n"
        "%ggx_d_den2 = OpFMul %f32_id %ggx_d_den %ggx_d_den\n"
        "%ggx_d = OpFDiv %f32_id %ggx_a2 %ggx_d_den2\n"
        "%ggx_one_a2 = OpFSub %f32_id %f32_id_1 %ggx_a2\n"
        "%ggx_v2 = OpFMul %f32_id %ggx_v %ggx_v\n"
        "%ggx_l2 = OpFMul %f32_id %ggx_l %ggx_l\n"
        "%ggx_v_part = OpFMul %f32_id %ggx_one_a2 %ggx_v2\n"
        "%ggx_l_part = OpFMul %f32_id %ggx_one_a2 %ggx_l2\n"
        "%ggx_v_sum = OpFAdd %f32_id %ggx_v_part %ggx_a2\n"
        "%ggx_l_sum = OpFAdd %f32_id %ggx_l_part %ggx_a2\n"
        "%ggx_v_root = OpExtInst %f32_id %329 Sqrt %ggx_v_sum\n"
        "%ggx_l_root = OpExtInst %f32_id %329 Sqrt %ggx_l_sum\n"
        "%ggx_lambda_v = OpFMul %f32_id %ggx_l %ggx_v_root\n"
        "%ggx_lambda_l = OpFMul %f32_id %ggx_v %ggx_l_root\n"
        "%ggx_vis_den = OpFAdd %f32_id %ggx_lambda_v %ggx_lambda_l\n"
        "%ggx_vis_safe = OpExtInst %f32_id %329 FMax %ggx_vis_den %ggx_min_den\n"
        "%ggx_vis = OpFDiv %f32_id %f32_id_0_5 %ggx_vis_safe\n"
        "%ggx_brdf = OpFMul %f32_id %ggx_d %ggx_vis\n"
        + (f"%{'ggx_raw_lit' if authored else 'ggx_lit'} = OpFMul %f32_id %ggx_brdf %ggx_l\n")
        + normalization +
        "OpBranch %ggx_merge\n"
        "%ggx_merge = OpLabel\n"
        "%ggx_result = OpPhi %f32_id %f32_id_0 %ggx_dark %ggx_lit %ggx_visible\n"
        "OpReturnValue %ggx_result\n"
        "OpFunctionEnd\n"
    )


def dot_instructions(label, left, right):
    lines = []
    for i, (a, b) in enumerate(zip(left, right)):
        lines.append(f"%{label}_{i} = OpFMul %f32_id %{a} %{b}")
    lines.append(f"%{label}_yz = OpFAdd %f32_id %{label}_1 %{label}_2")
    lines.append(f"%{label} = OpFAdd %f32_id %{label}_0 %{label}_yz")
    return lines


def patch_assembly(text, mode, variant="legacy"):
    if mode not in GGX_MODES:
        raise ValueError("Expected an explicit offline GGX calibration mode")
    replacements = {}
    current = variant == "current"
    offset = 14 if current else 0
    for original_result in (957, 959, 989, 991, 1206, 1392):
        result = original_result + offset
        replacements[result] = [f"%{result} = OpCopyObject %f32_id %f32_id_1"]
    normal = tuple(value + offset for value in (463, 468, 473))
    view = tuple(value + offset for value in (700, 705, 710))
    sites = (
        (964, 952, 788, (512, 513, 514)),
        (965, 953, 794, (536, 537, 538)),
        (1179, 1176, 1170, (1066, 1067, 1068)),
        (1365, 1362, 1356, (1252, 1253, 1254)),
    )
    if current:
        sites = tuple((result + offset, exponent + offset, half_dot + offset,
                       tuple(value + offset for value in light))
                      for result, exponent, half_dot, light in sites)
    half_vectors = ((741, 746, 751), (772, 777, 782), (1154, 1159, 1164), (1340, 1345, 1350))
    for index, (result, exponent, half_dot, light) in enumerate(sites):
        body = dot_instructions(f"ggx_l_{result}", normal, light)
        if result == sites[0][0]:
            body = dot_instructions("ggx_v_dot", normal, view) + body
        arguments = f"%{exponent} %ggx_v_dot %ggx_l_{result} %{half_dot}"
        if mode == "ggx-width-authored":
            half_vector = tuple(value + offset for value in half_vectors[index])
            body.extend(dot_instructions(f"ggx_vh_{result}", view, half_vector))
            arguments += f" %ggx_vh_{result} %f32_id_{1 if index < 2 else 0}"
        body.append(f"%{result} = OpFunctionCall %f32_id %ggx_eval {arguments}")
        replacements[result] = body
    lines = text.splitlines()
    seen = set()
    rewritten = []
    for line in lines:
        match = re.match(r"\s*%(\d+) = ", line)
        result = int(match[1]) if match else None
        if result in replacements:
            if result in seen:
                raise ValueError("Duplicate specular replacement site")
            seen.add(result)
            rewritten.extend(replacements[result])
        else:
            rewritten.append(line)
    if seen != replacements.keys():
        raise ValueError("Expected specular sites not found; no output written")
    first_function = next(i for i, line in enumerate(rewritten) if "= OpFunction " in line)
    declarations = [
        "%ggx_function_type = OpTypeFunction %f32_id %f32_id %f32_id %f32_id %f32_id"
        + (" %f32_id %f32_id" if mode == "ggx-width-authored" else ""),
        "%ggx_minus_two = OpConstant %f32_id -2",
        "%ggx_sqrt_two = OpConstant %f32_id 1.4142135623730951",
        "%ggx_max_n = OpConstant %f32_id 8192",
        "%ggx_min_den = OpConstant %f32_id 1e-20",
    ]
    rewritten[first_function:first_function] = declarations
    function = ggx_function(mode).replace("%329", "%141") if current else ggx_function(mode)
    return "\n".join(rewritten) + "\n" + function


def generate(source, output, mode="native"):
    source, output = Path(source), Path(output)
    if output.exists() or output.resolve() == source.resolve():
        raise FileExistsError("Refusing to overwrite a shader or prior experiment")
    original = source.read_bytes()
    variant = check_original(original)
    if mode == "native":
        with output.open("xb") as file:
            file.write(original)
        return
    with tempfile.TemporaryDirectory(prefix="bb-ggx-") as temporary:
        directory = Path(temporary)
        assembly, patched = directory / "native.spvasm", directory / "patched.spvasm"
        binary = directory / "patched.spv"
        run_tool("spirv-dis", source, "-o", assembly)
        patched.write_text(patch_assembly(assembly.read_text(), mode, variant), encoding="ascii")
        run_tool("spirv-as", "--target-env", "spv1.6", patched, "-o", binary)
        run_tool("spirv-val", "--target-env", "vulkan1.3",
                 "--uniform-buffer-standard-layout", binary)
        with output.open("xb") as file:
            file.write(binary.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mode", choices=("native", *GGX_MODES), default="native")
    args = parser.parse_args()
    generate(args.original, args.output, args.mode)
    print(f"Offline {args.mode} module written; renderer and cache unchanged")


if __name__ == "__main__":
    main()
