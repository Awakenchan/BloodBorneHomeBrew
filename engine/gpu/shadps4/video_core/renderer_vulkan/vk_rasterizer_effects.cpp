// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bbport_settings.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_effect_shaders.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Vulkan {

namespace {
constexpr u64 ShadowResolveHash = 0xf55643de;
constexpr u64 ShadowBlurHash = 0x11ffdfb5;
constexpr u64 PointLightsFullHash = 0x9f9ef267;
constexpr u64 HdrPostHash = 0xccbf44a6;
constexpr u64 DirectionalTableHash = 0xd92266b9;
constexpr u64 SkyDomeHash = 0xdda49e89;
constexpr u64 SkyDomeVertexHash = 0xb28f5a56;
constexpr u64 SceneTargetHash = 0x32e47788;
constexpr u64 PointGridVertexHash = 0x8747a367;

constexpr u32 PointLightCount(u64 hash) {
    switch (hash) {
    case 0x370174f7:
    case 0x7cc2c044:
        return 1;
    case 0x186d69d5:
    case 0x600f86b0:
        return 2;
    case 0xd22cbfd1:
        return 3;
    case 0x3105494c:
        return 4;
    case 0xd75fe2ff:
        return 5;
    case 0x07d07cf3:
        return 6;
    case 0xf2531727:
        return 7;
    case 0xc6edb19a:
        return 8;
    case 0xb59b4c4e:
        return 9;
    case 0x505ebdbe:
        return 10;
    case 0x22b357f3:
        return 11;
    case PointLightsFullHash:
        return 12;
    default:
        return 0;
    }
}

u32 PackedOffsets(const Shader::PushData& push) {
    static_assert(offsetof(Shader::PushData, buf_offsets) == 80);
    u32 word = 0;
    std::memcpy(&word, push.buf_offsets.data(), sizeof(word));
    return word;
}

u64 FragmentHash(const GraphicsPipeline* pipeline) {
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    return ps ? ps->pgm_hash : 0;
}

bool AnyWanted() {
    const auto& s = BbSettings::Get();
    for (const int effect : {BbSettings::EffectGi, BbSettings::EffectContactShadows,
                             BbSettings::EffectPcss, BbSettings::EffectVolumetricLight,
                             BbSettings::EffectPointFog, BbSettings::EffectPointShadows}) {
        if (s.effects[effect].load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return EffectShaders::GtaoActive() || EffectShaders::SsrActive();
}
}

void Rasterizer::EffectsOnFrameEnd() {
    effects_wanted = AnyWanted();
    ssgi->OnFrameEnd();
    ssr_temporal->OnFrameEnd();
}

void Rasterizer::EffectsBeforeTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const u64 hash = FragmentHash(pipeline);
    if (hash != EffectShaders::SsrHash) {
        ssr_temporal->Flush(upscaler->Jitter());
    }
    contact_shadow->Flush(hash == ShadowBlurHash);
    point_shadow->Flush();
}

void Rasterizer::EffectsAfterTargets(const GraphicsPipeline* pipeline) {
    if (!effects_wanted) {
        return;
    }
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask) >= 5 && db_desc.first) {
        volumetric_light->Reset();
        ssgi->OnGBufferDraw(db_desc.first, cb_descs[2].first, cb_descs[0].first);
    }
    if (cb_descs[0].first && EffectShaders::GtaoActive() &&
        FragmentHash(pipeline) == EffectShaders::SsaoHash) {
        ssgi->OnAoDraw(cb_descs[0].first);
    }
}

void Rasterizer::EffectsBlendedScene() {
    if (effects_wanted) {
        ssgi->OnOpaqueScene(cb_descs[0].first, db_desc.first, upscaler->Jitter());
    }
}

void Rasterizer::EffectsBeforeUpscaler(u64 compute_hash) {
    if (effects_wanted && compute_hash == upscaler->TriggerHash()) {
        volumetric_light->Composite(upscaler->SceneColor(), "the upscaler");
    }
}

void Rasterizer::EffectsBeforeResources(const GraphicsPipeline* pipeline) {
    if (effects_wanted && FragmentHash(pipeline) == HdrPostHash) {
        volumetric_light->Composite();
    }
}

vk::Rect2D Rasterizer::EffectTile(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                   bool is_indexed, u32 index_offset, u32 first_vertex) const {
    const vk::Rect2D scissor = DrawScissor(0);
    const auto& fetch = pipeline->GetFetchShader();
    const auto& v = vertex_binds;
    const auto position =
        std::ranges::find(v.attributes, 0u, &vk::VertexInputAttributeDescription2EXT::location);
    if (!fetch || position == v.attributes.end() || position->binding >= v.num_buffers) {
        return scissor;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    VAddr base = 0;
    u32 stride = 0;
    for (const auto& attribute : fetch->attributes) {
        if (attribute.semantic == 0) {
            const auto sharp = attribute.GetSharp(vs);
            base = sharp.base_address;
            stride = sharp.GetStride();
        }
    }
    if (!base || stride < 16) {
        return scissor;
    }
    const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
    const VAddr indices =
        is_indexed ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
    if (is_indexed && (!memory->IsValidGpuMapping(indices, 0) ||
                       memory->ClampRangeSize(indices, 4 * index_size) != 4 * index_size)) {
        return scissor;
    }
    const auto& vp = regs.viewports[0];
    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
    for (u32 i = 0; i < 4; ++i) {
        u32 index = i;
        if (is_indexed) {
            index = index_size == 2
                        ? reinterpret_cast<const u16*>(indices)[i]
                        : reinterpret_cast<const u32*>(indices)[i];
        }
        const VAddr at = base + u64(s64(index) + s64(s32(first_vertex))) * stride + position->offset;
        if (!memory->IsValidGpuMapping(at, 0) || memory->ClampRangeSize(at, 16) != 16) {
            return scissor;
        }
        const auto* p = reinterpret_cast<const float*>(at);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[3]) ||
            std::abs(p[3]) < 1e-8f) {
            return scissor;
        }
        const float x = p[0] / p[3] * (vp.xscale * target_scale[0]) +
                        (vp.xoffset * target_scale[0] + target_offset[0]);
        const float y = p[1] / p[3] * (vp.yscale * target_scale[1]) +
                        (vp.yoffset * target_scale[1] + target_offset[1]);
        lo_x = std::min(lo_x, x);
        lo_y = std::min(lo_y, y);
        hi_x = std::max(hi_x, x);
        hi_y = std::max(hi_y, y);
    }
    const s32 x0 = std::max(s32(std::floor(lo_x)), scissor.offset.x);
    const s32 y0 = std::max(s32(std::floor(lo_y)), scissor.offset.y);
    const s32 x1 = std::min(s32(std::ceil(hi_x)), scissor.offset.x + s32(scissor.extent.width));
    const s32 y1 = std::min(s32(std::ceil(hi_y)), scissor.offset.y + s32(scissor.extent.height));
    if (x1 <= x0 || y1 <= y0) {
        return scissor;
    }
    return {.offset = {x0, y0}, .extent = {u32(x1 - x0), u32(y1 - y0)}};
}

void Rasterizer::EffectsBeforeBind(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline,
                                   bool is_indexed, u32 index_offset, u32 first_vertex) {
    if (!pipeline->GetGraphicsKey().mrt_mask) {
        return;
    }
    const u64 hash = FragmentHash(pipeline);
    const u32 tile_lights = PointLightCount(hash);
    if (tile_lights != 0) {
        effect_tile = EffectTile(regs, pipeline, is_indexed, index_offset, first_vertex);
    }
    static_assert(offsetof(Shader::PushData, motion_param) == 120);
    static_assert(offsetof(Shader::PushData, scene_size) == 124);
    if (hash == EffectShaders::SsaoHash) {
        if (EffectShaders::GtaoActive()) {
            const float k = std::clamp(
                BbSettings::Get().graphics_ao_strength.load(std::memory_order_relaxed), 0.0f, 1.0f);
            push_data.motion_param = std::bit_cast<u32>(k);
        }
    } else if (hash == EffectShaders::SsrHash) {
        if (EffectShaders::SsrActive() && cb_descs[0].first) {
            push_data.motion_param = ssr_frame;
            SsrTemporal::TraceInputs inputs{.frame_index = ssr_frame};
            if (!buffer_infos.empty() && buffer_infos[0].buffer && image_infos.size() == 6) {
                inputs.constants = buffer_infos[0].buffer;
                inputs.constants_offset = buffer_infos[0].offset;
                inputs.constants_range = buffer_infos[0].range;
                inputs.constants_dword_offset = PackedOffsets(push_data);
                inputs.normals_view = image_infos[1].imageView;
                inputs.normals_layout = image_infos[1].imageLayout;
                inputs.color_view = image_infos[2].imageView;
                inputs.color_layout = image_infos[2].imageLayout;
            }
            ssr_temporal->OnSsrDraw(cb_descs[0].first, inputs);
            ++ssr_frame;
        }
    } else if (hash == ShadowBlurHash) {
        if (cb_descs[0].first) {
            const auto& blur = pipeline->GetStage(Shader::SwStage::Fragment);
            contact_shadow->OnBlurDraw(
                cb_descs[0].first, blur.images.empty() ? 0 : blur.images[0].GetSharp(blur).Address());
        }
    } else if (hash == ShadowResolveHash) {
        if (cb_descs[0].first && !buffer_infos.empty() && buffer_infos[0].buffer) {
            const bool has_ubo = buffer_infos.size() > 1 && buffer_infos[1].buffer;
            const bool has_map = image_infos.size() > 1 && image_infos[1].imageView;
            contact_shadow->OnShadowDraw(
                cb_descs[0].first,
                {.constants = buffer_infos[0].buffer,
                 .constants_offset = buffer_infos[0].offset,
                 .constants_range = buffer_infos[0].range,
                 .uniforms = has_ubo ? buffer_infos[1].buffer : vk::Buffer{},
                 .uniforms_offset = has_ubo ? buffer_infos[1].offset : 0,
                 .uniforms_range = has_ubo ? buffer_infos[1].range : 0,
                 .shadow_map = has_map ? image_infos[1].imageView : vk::ImageView{},
                 .shadow_layout = has_map ? image_infos[1].imageLayout : vk::ImageLayout::eUndefined,
                 .constants_offset_word = PackedOffsets(push_data),
                 .frame_index = shadow_frame++});
        }
    } else if (tile_lights != 0) {
        if ((pipeline->GetGraphicsKey().mrt_mask & 3) == 3 && cb_descs[0].first &&
            cb_descs[1].first && buffer_infos.size() >= 2 && buffer_infos[0].buffer &&
            buffer_infos[1].buffer) {
            point_shadow->OnLightDraw(cb_descs[0].first, cb_descs[1].first,
                                      {.constants = buffer_infos[0].buffer,
                                       .constants_offset = buffer_infos[0].offset,
                                       .constants_range = buffer_infos[0].range,
                                       .lights = buffer_infos[1].buffer,
                                       .lights_offset = buffer_infos[1].offset,
                                       .lights_range = buffer_infos[1].range,
                                       .offsets = PackedOffsets(push_data),
                                       .count = tile_lights,
                                       .tile = effect_tile});
        }
    } else if (hash == EffectShaders::SsrCompositeHash && EffectShaders::SsrCompositeActive()) {
        const float intensity =
            BbSettings::Get().graphics_ssr_intensity.load(std::memory_order_relaxed);
        push_data.motion_param =
            std::bit_cast<u32>(std::clamp(std::isfinite(intensity) ? intensity : 1.0f, 0.0f, 3.0f));
        if (const auto full = ssr_temporal->FullResult(); full && image_infos.size() == 12) {
            image_infos[6].imageView = full;
            image_infos[6].imageLayout = vk::ImageLayout::eGeneral;
        }
    }
}

void Rasterizer::EffectsAfterDraw(const AmdGpu::Regs& regs, const GraphicsPipeline* pipeline, bool is_indexed,
                                  u32 num_indices, u32 num_instances, u32 first_vertex) {
    if (!effects_wanted) {
        return;
    }
    const auto* ps_info = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    if (!ps_info) {
        return;
    }
    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const u64 hash = ps_info->pgm_hash;
    if (hash == ShadowResolveHash && cb_descs[0].first && buffer_infos.size() >= 2 &&
        image_infos.size() >= 2) {
        volumetric_light->Integrate(cb_descs[0].first, image_infos[0],
                                    {.constants = buffer_infos[0].buffer,
                                     .constants_offset = buffer_infos[0].offset,
                                     .constants_range = buffer_infos[0].range,
                                     .uniforms = buffer_infos[1].buffer,
                                     .uniforms_offset = buffer_infos[1].offset,
                                     .uniforms_range = buffer_infos[1].range,
                                     .shadow_map = image_infos[1].imageView,
                                     .shadow_layout = image_infos[1].imageLayout,
                                     .constants_offset_word = PackedOffsets(push_data)});
    } else if (hash == DirectionalTableHash && !buffer_infos.empty() && !image_infos.empty()) {
        volumetric_light->Tint(buffer_infos[0], push_data.buf_offsets[0] & 255u, image_infos[0]);
    } else if (const u32 count = PointLightCount(hash);
               count != 0 && (pipeline->GetGraphicsKey().mrt_mask & 3) == 3 && cb_descs[0].first &&
               cb_descs[1].first && buffer_infos.size() >= 2) {
        if (vs_info.pgm_hash != PointGridVertexHash || ps_info->buffers.size() < 2 ||
            ps_info->buffers[0].IsSpecial() || ps_info->buffers[1].IsSpecial()) {
            return;
        }
        const VAddr constants = ps_info->buffers[0].GetSharp(*ps_info).base_address;
        const VAddr lights = ps_info->buffers[1].GetSharp(*ps_info).base_address;
        const u64 light_bytes = u64(count) * 64;
        if (memory->IsValidGpuMapping(constants, 0) && memory->ClampRangeSize(constants, 64) == 64 &&
            memory->IsValidGpuMapping(lights, 0) &&
            memory->ClampRangeSize(lights, light_bytes) == light_bytes) {
            volumetric_light->AddPointLights(reinterpret_cast<const u32*>(constants),
                                             reinterpret_cast<const u32*>(lights), count);
        }
    } else if (hash == SkyDomeHash && vs_info.pgm_hash == SkyDomeVertexHash && is_indexed &&
               num_indices == 1824 && num_instances == 1 && first_vertex == 0) {
        const auto& v = vertex_binds;
        const auto position =
            std::ranges::find(v.attributes, 0u, &vk::VertexInputAttributeDescription2EXT::location);
        const auto normal =
            std::ranges::find(v.attributes, 1u, &vk::VertexInputAttributeDescription2EXT::location);
        u32 first_buffer = 0;
        for (const auto* stage : pipeline->GetStages()) {
            if (stage == &vs_info) {
                break;
            }
            if (stage) {
                first_buffer += u32(stage->buffers.size());
            }
        }
        if (position == v.attributes.end() || normal == v.attributes.end() ||
            position->format != vk::Format::eR32G32B32Sfloat || vs_info.buffers.size() < 3 ||
            vs_info.buffers[0].IsSpecial() || vs_info.buffers[1].IsSpecial() ||
            vs_info.buffers[2].IsSpecial() || first_buffer + 3 > buffer_infos.size()) {
            return;
        }
        const auto pi = position->binding;
        const auto ni = normal->binding;
        const bool packed = normal->format == vk::Format::eR8G8B8A8Uint ||
                            normal->format == vk::Format::eR8G8B8A8Sint;
        const bool wide = normal->format == vk::Format::eR32G32B32A32Uint ||
                          normal->format == vk::Format::eR32G32B32A32Sint;
        const u64 position_bytes = 392 * v.host_strides[pi] + 12;
        const u64 normal_bytes = 392 * v.host_strides[ni] + (packed ? 4 : 16);
        if ((packed || wide) && pi < v.num_buffers && ni < v.num_buffers &&
            v.host_strides[pi] >= 12 && v.host_strides[ni] >= (packed ? 4u : 16u) &&
            v.host_sizes[pi] >= position_bytes && v.host_sizes[ni] >= normal_bytes) {
            volumetric_light->Celestial({
                .positions = {v.host_buffers[pi], v.host_offsets[pi], position_bytes},
                .normals = {v.host_buffers[ni], v.host_offsets[ni], normal_bytes},
                .model = buffer_infos[first_buffer],
                .transforms = buffer_infos[first_buffer + 1],
                .scene = buffer_infos[first_buffer + 2],
                .vertex_stride = u32(v.host_strides[pi]),
                .normal_stride = u32(v.host_strides[ni]),
                .normal_word = packed ? 0u : 3u,
                .model_offset = push_data.buf_offsets[first_buffer],
                .transform_offset = push_data.buf_offsets[first_buffer + 1],
                .scene_offset = push_data.buf_offsets[first_buffer + 2],
            });
        }
    } else if (hash == SceneTargetHash && cb_descs[0].first) {
        volumetric_light->Scene(cb_descs[0].first);
    }
}

}
