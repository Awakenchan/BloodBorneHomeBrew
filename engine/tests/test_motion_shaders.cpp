// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <setjmp.h>
#include <sirit/sirit.h>
#include <unordered_map>
#include <unordered_set>
#include "common/hash.h"
#include "common/serdes.h"
#include "bbport_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_pom_patch.h"
#include "video_core/renderer_vulkan/vk_ggx_patch.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern "C" {
void runtime_blocked_report(void) {}
[[noreturn]] void runtime_thread_exit(void*) { std::abort(); }
int sys_pad_type(void) { return 0; }
int sys_pad_button(uint32_t, int) { return 0; }
}

static void CheckMetadata() {
    using namespace Shader;
    const auto load = [](u8 motion, u32 version, HwStage stage = HwStage::Vertex,
                         u64 pgm_hash = 0x123456) {
        Info original{};
        original.hw_stage = stage;
        original.sw_stage = stage == HwStage::Vertex ? SwStage::Vertex : SwStage::Fragment;
        original.pgm_hash = pgm_hash;
        StageSpecialization spec{};
        spec.info = &original;
        spec.runtime_info.Initialize(original.hw_stage, original.sw_stage);
        if (stage == HwStage::Vertex) spec.runtime_info.hw.vs.motion_vectors = motion;
        const auto hash = HashCombine(original.pgm_hash, u64{2});
        Serialization::Archive emitted{Vulkan::SerializeShaderMeta(original, spec, hash, 2)};
        Serialization::Reader emitted_reader{emitted};
        u32 meta_version{}, emitted_version{};
        emitted_reader.Read(meta_version);
        emitted_reader.Read(emitted_version);
        const u32 expected = stage == HwStage::Fragment &&
                                     pgm_hash == Vulkan::Ggx::DirectionalHash &&
                                     BbSettings::Get().startup_directional_ggx ? 10
                                 : stage != HwStage::Vertex || !motion ? 7
                                 : (motion & MotionVectors::VertexCapture) ? 9 : 8;
        assert(emitted_version == expected);
        Serialization::Archive output;
        Serialization::Writer writer{output};
        writer.Write(meta_version);
        writer.Write(version);
        writer.Write(hash);
        writer.Write(size_t{2});
        spec.Serialize(output);
        original.Serialize(output);
        Serialization::Archive input{output.TakeOff()};
        Info restored{};
        StageSpecialization restored_spec{};
        restored_spec.info = &restored;
        std::optional<Gcn::FetchShaderData> fetch;
        size_t index{};
        const bool loaded = Vulkan::LoadShaderMeta(input, restored, fetch, restored_spec, index, hash);
        if (loaded) {
            assert(index == 2 && restored.pgm_hash == original.pgm_hash);
            assert(restored_spec.runtime_info == spec.runtime_info);
            assert(restored_spec.info == &restored);
        }
        return loaded;
    };
    assert(!load(true, 7));
    assert(load(true, 8));
    assert(load(false, 7));
    assert(!load(false, 8));
    assert(!load(true, 99));
    assert(load(MotionVectors::VertexCapture, 9));
    assert(load(MotionVectors::VertexBoth, 9));
    assert(!load(MotionVectors::VertexCapture, 8));
    assert(!load(false, 9));
    // Upstream caches native fragments; effects are reapplied to submitted modules on warmup.
    for (const auto& shader : Vulkan::Pom::kShaders) {
        assert(load(0, 7, HwStage::Fragment, shader.hash));
        assert(!load(0, 9, HwStage::Fragment, shader.hash));
    }
    assert(load(0, 7, HwStage::Fragment, Vulkan::Ggx::DirectionalHash));
    assert(!load(0, 9, HwStage::Fragment, Vulkan::Ggx::DirectionalHash));
    assert(!load(0, 10, HwStage::Fragment, Vulkan::Ggx::DirectionalHash));
    BbSettings::Get().startup_directional_ggx = true;
    assert(!load(0, 7, HwStage::Fragment, Vulkan::Ggx::DirectionalHash));
    assert(load(0, 10, HwStage::Fragment, Vulkan::Ggx::DirectionalHash));
    assert(load(0, 7, HwStage::Fragment));
    assert(!load(0, 10, HwStage::Fragment));
    assert(load(MotionVectors::VertexCapture, 9));
    for (const auto& shader : Vulkan::Pom::kShaders) {
        assert(load(0, 7, HwStage::Fragment, shader.hash));
    }
    BbSettings::Get().startup_directional_ggx = false;
}

static void CheckDirectionalModule(const char* path) {
    using namespace Vulkan;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file && file.tellg() >= 20 && file.tellg() % 4 == 0);
    std::vector<u32> native(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(native.data()), std::streamsize(native.size() * 4));
    assert(file);
    const auto original = native;
    auto expected = native;
    assert(Ggx::Patch(Ggx::DirectionalHash, true, true, expected) == Ggx::Result::Applied);
    auto& settings = BbSettings::Get();
    settings.graphics_directional_ggx = true;
    assert(DirectionalGgxModule(Ggx::DirectionalHash, true, native).empty());
    settings.startup_directional_ggx = true;
    assert(DirectionalGgxModule(Ggx::DirectionalHash, true, native) == expected);
    settings.graphics_directional_ggx = false;
    assert(DirectionalGgxModule(Ggx::DirectionalHash, true, native) == expected);
    assert(DirectionalGgxModule(Ggx::DirectionalHash, false, native).empty());
    assert(DirectionalGgxModule(0, true, native).empty());
    auto unsupported = native;
    unsupported.back() ^= 1;
    const auto unsupported_original = unsupported;
    assert(DirectionalGgxModule(Ggx::DirectionalHash, true, unsupported).empty());
    assert(native == original && unsupported == unsupported_original);
    settings.startup_directional_ggx = false;
    assert(DirectionalGgxModule(Ggx::DirectionalHash, true, native).empty());
}

static void CheckMotionInterface(const std::vector<u32>& code, bool vertex_motion) {
    using namespace Shader;
    std::unordered_map<u32, u32> sets, bindings, storage;
    std::unordered_set<u32> int64_types;
    unsigned atomic_stores = 0;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        assert(count && count <= code.size() - i);
        const auto op = static_cast<spv::Op>(code[i] & 0xffff);
        if (op == spv::Op::OpTypeInt && code[i + 2] == 64) {
            int64_types.insert(code[i + 1]);
        }
        if (op == spv::Op::OpDecorate && count == 4) {
            if (code[i + 2] == u32(spv::Decoration::DescriptorSet)) {
                sets[code[i + 1]] = code[i + 3];
            }
            if (code[i + 2] == u32(spv::Decoration::Binding)) {
                bindings[code[i + 1]] = code[i + 3];
            }
        }
        if (op == spv::Op::OpVariable) {
            storage[code[i + 2]] = code[i + 3];
        }
        if (op == spv::Op::OpConstant && int64_types.contains(code[i + 1])) {
            assert(count == 5 && code[i + 4] == 0 && code[i + 3] <= 32);
        }
        atomic_stores += op == spv::Op::OpAtomicExchange;
        i += count;
    }
    unsigned motion_descriptors = 0;
    for (const auto& [id, set] : sets) {
        if (set == MotionVectors::DescriptorSet) {
            ++motion_descriptors;
            assert(bindings.at(id) == MotionVectors::AddressBinding);
            assert(storage.at(id) == u32(spv::StorageClass::Uniform));
        }
    }
    assert(motion_descriptors == (vertex_motion ? 1u : 0u));
    assert(atomic_stores == (vertex_motion ? 4u : 0u));
    static_assert(sizeof(PushData) == 128);
}

int main(int argc, char** argv) {
    using namespace Shader;
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    CheckMetadata();
    if (argc == 4) {
        CheckDirectionalModule(argv[2]);
        CheckDirectionalModule(argv[3]);
    }
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    for (bool vertex : {true, false}) {
        for (u8 mode : {u8(0), Shader::MotionVectors::VertexOutputs, Shader::MotionVectors::VertexCapture,
                        Shader::MotionVectors::VertexBoth}) {
            if (!vertex && mode >= Shader::MotionVectors::VertexCapture) {
                continue;
            }
            const bool motion = mode == Shader::MotionVectors::VertexOutputs;
            Info info{};
            info.hw_stage = vertex ? HwStage::Vertex : HwStage::Fragment;
            info.sw_stage = vertex ? SwStage::Vertex : SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            if (vertex) {
                runtime.hw.vs.motion_vectors = mode;
            } else {
                runtime.hw.fs.motion_vectors = motion;
                runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
            }
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            IR::IREmitter ir(block);
            const auto output = vertex ? IR::Attribute::Position0 : IR::Attribute::RenderTarget0;
            for (u32 component = 0; component < 4; ++component) {
                info.stores.Set(output, component);
                if (!vertex && component < 2) {
                    info.loads.Set(IR::Attribute::FragCoord, component);
                    ir.SetAttribute(output, ir.GetAttribute(IR::Attribute::FragCoord, component), component);
                } else {
                    ir.SetAttribute(output, ir.Imm32(component == 3 ? 1.0f : 0.0f), component);
                }
            }
            ir.Epilogue();
            IR::Program program(info);
            program.blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block},
                                           .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            Profile profile{};
            profile.supported_spirv = 0x00010600;
            profile.support_int64 = true;
            Backend::Bindings bindings{};
            const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
            if (mode < Shader::MotionVectors::VertexCapture) {
                CheckMotionInterface(code, vertex && motion);
            }
            const char* kind = mode == Shader::MotionVectors::VertexBoth ? "-both.spv"
                               : mode == Shader::MotionVectors::VertexCapture ? "-capture.spv"
                                                                            : motion ? "-motion.spv" : "-plain.spv";
            const auto path = dir / (std::string(vertex ? "vertex" : "fragment") +
                                      kind);
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            if (!out) { return 1; }
        }
    }
}
