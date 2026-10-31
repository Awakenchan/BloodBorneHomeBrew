# SPDX-License-Identifier: GPL-2.0-or-later
import os
import struct
import subprocess
import sys
import tempfile

src, out = sys.argv[1], sys.argv[2]
bindir = os.environ.get("SPIRV_BIN", "")

def tool(name):
    return os.path.join(bindir, name) if bindir else name

tmp = tempfile.mkdtemp()
asm_path = os.path.join(tmp, "orig.spvasm")
subprocess.check_call([tool("spirv-dis"), src, "-o", asm_path])
asm = open(asm_path).read().split("\n")

with open(src, "rb") as f:
    original = f.read()
words = struct.unpack("<%dI" % (len(original) // 4), original)
checksum = 2166136261
for w in words:
    checksum = ((checksum ^ w) * 16777619) & 0xFFFFFFFF

consts = """
%i32_0 = OpConstant %i32_id 0
%i32_1 = OpConstant %i32_id 1
%i32_2 = OpConstant %i32_id 2
%f32_0_02 = OpConstant %f32_id 0.02
%f32_0_0001 = OpConstant %f32_id 0.0001
%f32_0_05 = OpConstant %f32_id 0.05
%f32_lodk = OpConstant %f32_id 0.0
%f32_fade_k = OpConstant %f32_id 0.08
%f32_1e5 = OpConstant %f32_id 100000
%c_iv00 = OpConstantComposite %i32vec2_id %i32_0 %i32_0
%c_iv10 = OpConstantComposite %i32vec2_id %i32_1 %i32_0
%c_iv01 = OpConstantComposite %i32vec2_id %i32_0 %i32_1
%c_iv11 = OpConstantComposite %i32vec2_id %i32_1 %i32_1
%c_iv22 = OpConstantComposite %i32vec2_id %i32_2 %i32_2
%c_half2 = OpConstantComposite %f32vec2_id %f32_id_0_5 %f32_id_0_5
%c_one2 = OpConstantComposite %f32vec2_id %f32_id_1 %f32_id_1
""".strip().split("\n")

body = []
a = body.append
a("%150 = OpCompositeConstruct %f32vec2_id %93 %95")
a("%151 = OpLoad %60 %fs_img6")
a("%152 = OpLoad %71 %fs_samp3")
a("%img1l = OpLoad %60 %fs_img1")
a("%lodk = OpFMul %f32_id %140 %f32_lodk")
a("%sampled6 = OpSampledImage %63 %151 %152")
a("%ls_i = OpImageQuerySizeLod %i32vec2_id %151 %i32_0")
a("%ls = OpConvertSToF %f32vec2_id %ls_i")
a("%ls_m1 = OpISub %i32vec2_id %ls_i %c_iv11")
a("%fs_i = OpImageQuerySizeLod %i32vec2_id %img1l %i32_0")
a("%fs_f = OpConvertSToF %f32vec2_id %fs_i")
a("%fs_m1 = OpISub %i32vec2_id %fs_i %c_iv11")
a("%ratio = OpFDiv %f32vec2_id %fs_f %ls")
a("%p = OpFMul %f32vec2_id %150 %ls")
a("%pb = OpFSub %f32vec2_id %p %c_half2")
a("%fl = OpExtInst %f32vec2_id %139 Floor %pb")
a("%fr = OpFSub %f32vec2_id %pb %fl")
a("%omf = OpFSub %f32vec2_id %c_one2 %fr")
a("%bi = OpConvertFToS %i32vec2_id %fl")
a("%cf = OpFMul %f32vec2_id %150 %fs_f")
a("%ci = OpConvertFToS %i32vec2_id %cf")
a("%cc = OpExtInst %i32vec2_id %139 SClamp %ci %c_iv00 %fs_m1")
a("%dcv = OpImageFetch %f32vec4_id %img1l %cc Lod %i32_0")
a("%dc0 = OpCompositeExtract %f32_id %dcv 0")
a("%dc = OpExtInst %f32_id %139 FMin %dc0 %f32_1e5")
a("%dcs = OpFMul %f32_id %dc %f32_0_02")
a("%denom = OpFAdd %f32_id %dcs %f32_0_0001")
a("%fx = OpCompositeExtract %f32_id %fr 0")
a("%fy = OpCompositeExtract %f32_id %fr 1")
a("%gx = OpCompositeExtract %f32_id %omf 0")
a("%gy = OpCompositeExtract %f32_id %omf 1")
taps = [("00", "gx", "gy"), ("10", "fx", "gy"), ("01", "gx", "fy"), ("11", "fx", "fy")]
acc = None
wsum = None
for i, (off, wx, wy) in enumerate(taps):
    n = lambda s: "%%t%d_%s" % (i, s)
    a("%s = OpIAdd %%i32vec2_id %%bi %%c_iv%s" % (n("raw"), off))
    a("%s = OpExtInst %%i32vec2_id %%139 SClamp %s %%c_iv00 %%ls_m1" % (n("tc"), n("raw")))
    a("%s = OpConvertSToF %%f32vec2_id %s" % (n("tcf"), n("tc")))
    a("%s = OpFMul %%f32vec2_id %s %%ratio" % (n("fcf"), n("tcf")))
    a("%s = OpConvertFToS %%i32vec2_id %s" % (n("fc0"), n("fcf")))
    a("%s = OpExtInst %%i32vec2_id %%139 SClamp %s %%c_iv00 %%fs_m1" % (n("fc"), n("fc0")))
    a("%s = OpImageFetch %%f32vec4_id %%img1l %s Lod %%i32_0" % (n("dv"), n("fc")))
    a("%s = OpCompositeExtract %%f32_id %s 0" % (n("d0"), n("dv")))
    a("%s = OpExtInst %%f32_id %%139 FMin %s %%f32_1e5" % (n("d"), n("d0")))
    a("%s = OpFSub %%f32_id %s %%dc" % (n("df"), n("d")))
    a("%s = OpExtInst %%f32_id %%139 FAbs %s" % (n("ad"), n("df")))
    a("%s = OpFDiv %%f32_id %s %%denom" % (n("r"), n("ad")))
    a("%s = OpFMul %%f32_id %s %s" % (n("r2"), n("r"), n("r")))
    a("%s = OpFAdd %%f32_id %s %%f32_0_05" % (n("rd"), n("r2")))
    a("%s = OpFMul %%f32_id %%%s %%%s" % (n("bil"), wx, wy))
    a("%s = OpFDiv %%f32_id %s %s" % (n("w"), n("bil"), n("rd")))
    a("%s = OpFAdd %%f32vec2_id %s %%c_half2" % (n("tcc"), n("tcf")))
    a("%s = OpFDiv %%f32vec2_id %s %%ls" % (n("uv"), n("tcc")))
    a("%s = OpImageSampleExplicitLod %%f32vec4_id %%sampled6 %s Lod %%lodk" % (n("col"), n("uv")))
    a("%s = OpVectorTimesScalar %%f32vec4_id %s %s" % (n("cw"), n("col"), n("w")))
    if acc is None:
        acc, wsum = n("cw"), n("w")
    else:
        a("%s = OpFAdd %%f32vec4_id %s %s" % (n("acc"), acc, n("cw")))
        a("%s = OpFAdd %%f32_id %s %s" % (n("ws"), wsum, n("w")))
        acc, wsum = n("acc"), n("ws")
a("%%inv = OpFDiv %%f32_id %%f32_id_1 %s" % wsum)
a("%%pre = OpVectorTimesScalar %%f32vec4_id %s %%inv" % acc)

a("%lod_n = OpFMul %f32_id %140 %f32_fade_k")
a("%lod_c = OpExtInst %f32_id %139 FClamp %lod_n %f32_id_0 %f32_id_1")
a("%fade0 = OpFSub %f32_id %f32_id_1 %lod_c")
a("%fade = OpFMul %f32_id %fade0 %fade0")
a("%intensity_ptr = OpAccessChain %_ptr_PushConstant_u32_id %push_data %u32_id_11")
a("%intensity_bits = OpLoad %u32_id %intensity_ptr")
a("%intensity = OpBitcast %f32_id %intensity_bits")
a("%154 = OpVectorTimesScalar %f32vec4_id %pre %fade")

start = next(i for i, l in enumerate(asm) if l.strip().startswith("%150 = OpCompositeConstruct"))
end = next(i for i, l in enumerate(asm) if l.strip().startswith("%154 = OpImageSampleExplicitLod"))
asm[start:end + 1] = body

for result, delta, environment in ((480, 479, 146), (482, 481, 147), (484, 483, 148)):
    line = next(i for i, l in enumerate(asm) if l.strip().startswith("%%%d = OpFAdd" % result))
    asm[line:line + 1] = [
        "%%ssr_delta_%d = OpFMul %%f32_id %%%d %%intensity" % (result, delta),
        "%%%d = OpFAdd %%f32_id %%ssr_delta_%d %%%d" % (result, result, environment),
    ]

first_var = next(i for i, l in enumerate(asm) if "= OpVariable" in l)
asm[first_var:first_var] = consts

slope = next(i for i, l in enumerate(asm) if l.strip().startswith("%f32_id_13 = OpConstant %f32_id 13"))
asm[slope] = asm[slope].replace("%f32_id 13", "%f32_id 16")

patched_asm = os.path.join(tmp, "patched.spvasm")
patched_spv = os.path.join(tmp, "patched.spv")
open(patched_asm, "w").write("\n".join(asm))
subprocess.check_call([tool("spirv-as"), "--target-env", "spv1.6", patched_asm, "-o", patched_spv])
subprocess.check_call([tool("spirv-val"), "--target-env", "vulkan1.3",
                       "--uniform-buffer-standard-layout", patched_spv])

data = open(patched_spv, "rb").read()
out_words = struct.unpack("<%dI" % (len(data) // 4), data)
with open(out, "w") as f:
    f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
    f.write("// Generated by engine/tools/re/ssr_composite_patch.py: the SSR composite pass (ps\n")
    f.write("// 3fe9e964) with a depth-aware upsample of the SSR target. Do not edit.\n\n")
    f.write("#pragma once\n\n#include <array>\n#include \"common/types.h\"\n\n")
    f.write("namespace Vulkan {\n\n")
    f.write("/// Size and FNV-1a checksum of the original module this replaces.\n")
    f.write("constexpr size_t kSsrCompositeOriginalWords = %d;\n" % len(words))
    f.write("constexpr u32 kSsrCompositeOriginalChecksum = 0x%08x;\n\n" % checksum)
    f.write("constexpr std::array<u32, %d> kSsrCompositeSpv{\n" % len(out_words))
    for i in range(0, len(out_words), 8):
        f.write("    " + ", ".join("0x%08x" % w for w in out_words[i:i + 8]) + ",\n")
    f.write("};\n\n} // namespace Vulkan\n")
print("ok", len(words), "->", len(out_words), "words, checksum %08x" % checksum)
