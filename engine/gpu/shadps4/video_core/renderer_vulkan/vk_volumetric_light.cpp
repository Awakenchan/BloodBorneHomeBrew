// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_volumetric_light.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/volumetric_light_integrate_comp.h"
#include "video_core/host_shaders/volumetric_light_repair_comp.h"
#include "video_core/host_shaders/volumetric_light_tint_comp.h"
#include "video_core/host_shaders/volumetric_light_composite_comp.h"
#include "video_core/host_shaders/volumetric_light_celestial_comp.h"
#include "video_core/host_shaders/point_volumetric_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
namespace {
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
struct Params {
    u32 offsets, steps;
    float reach, density, strength, anisotropy, bias, padding;
};
static_assert(sizeof(Params) == 32);

float Parameter(const char* name, float fallback, float low, float high) {
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end || !std::isfinite(value) || value < low || value > high) {
        std::printf("Volumetric lighting: invalid %s; using %.3f\n", name, fallback);
        return fallback;
    }
    return value;
}

void Before(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}

void After(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 b{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b});
}
} // namespace

VolumetricLight::VolumetricLight(const Instance& instance_, Scheduler& scheduler_,
                               VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                               SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_} {
    enabled = Parameter("BB_VOLUMETRIC_LIGHT", 1.0f, 0.0f, 1.0f) > 0.5f;
    if (!enabled) return;
    strength = Parameter("BB_VOLUMETRIC_STRENGTH", strength, 0.0f, 1.0f);
    reach = Parameter("BB_VOLUMETRIC_RANGE", reach, 1.0f, 100.0f);
    density = Parameter("BB_VOLUMETRIC_DENSITY", density, 0.001f, 0.2f);
    anisotropy = Parameter("BB_VOLUMETRIC_ANISOTROPY", anisotropy, 0.0f, 0.8f);
    steps = u32(Parameter("BB_VOLUMETRIC_STEPS", float(steps), 8.0f, 96.0f));
    direction_state = std::make_unique<VideoCore::Buffer>(
        instance, 0, 112, VideoCore::MemoryType::DeviceLocal, "Volumetric same-frame direction");
    const auto device = instance.GetDevice();
    sampler = Check<"create volumetric sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    }));
    CreatePass(integrate, {vk::DescriptorType::eStorageBuffer,
                          vk::DescriptorType::eStorageBuffer,
                          vk::DescriptorType::eCombinedImageSampler,
                          vk::DescriptorType::eCombinedImageSampler,
                          vk::DescriptorType::eStorageImage,
                          vk::DescriptorType::eStorageImage,
                          vk::DescriptorType::eStorageBuffer},
               VOLUMETRIC_LIGHT_INTEGRATE_COMP, sizeof(Params));
    CreatePass(repair, {vk::DescriptorType::eStorageBuffer,
                       vk::DescriptorType::eStorageBuffer,
                       vk::DescriptorType::eCombinedImageSampler,
                       vk::DescriptorType::eCombinedImageSampler,
                       vk::DescriptorType::eStorageImage,
                       vk::DescriptorType::eStorageImage,
                       vk::DescriptorType::eStorageBuffer,
                       vk::DescriptorType::eStorageImage},
               VOLUMETRIC_LIGHT_REPAIR_COMP, sizeof(Params));
    CreatePass(tint, {vk::DescriptorType::eStorageBuffer,
                     vk::DescriptorType::eCombinedImageSampler,
                     vk::DescriptorType::eStorageImage},
               VOLUMETRIC_LIGHT_TINT_COMP, 2 * sizeof(u32));
    CreatePass(composite, {vk::DescriptorType::eCombinedImageSampler,
                          vk::DescriptorType::eCombinedImageSampler,
                          vk::DescriptorType::eStorageImage,
                          vk::DescriptorType::eStorageBuffer,
                          vk::DescriptorType::eCombinedImageSampler,
                          vk::DescriptorType::eCombinedImageSampler},
               VOLUMETRIC_LIGHT_COMPOSITE_COMP, 2 * sizeof(u32));
    CreatePass(point, {vk::DescriptorType::eStorageBuffer,
                       vk::DescriptorType::eStorageBuffer,
                       vk::DescriptorType::eCombinedImageSampler,
                       vk::DescriptorType::eStorageImage,
                       vk::DescriptorType::eStorageBuffer,
                       vk::DescriptorType::eStorageBuffer},
               POINT_VOLUMETRIC_COMP, 16 * sizeof(u32));
    CreatePass(celestial, {vk::DescriptorType::eStorageBuffer, vk::DescriptorType::eStorageBuffer,
                          vk::DescriptorType::eStorageBuffer, vk::DescriptorType::eStorageBuffer,
                          vk::DescriptorType::eStorageBuffer, vk::DescriptorType::eStorageBuffer},
               VOLUMETRIC_LIGHT_CELESTIAL_COMP, 8 * sizeof(u32));
}

VolumetricLight::~VolumetricLight() = default;

bool VolumetricLight::Enabled() const {
    return enabled && !failed &&
           (BbSettings::Get().effects[BbSettings::EffectVolumetricLight].load(
                std::memory_order_relaxed) ||
            BbSettings::Get().graphics_point_fog_intensity.load(std::memory_order_relaxed) > 0.0f);
}

void VolumetricLight::CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                                 std::span<const u32> code, u32 push_size) {
    const auto device = instance.GetDevice();
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (auto type : types) {
        bindings.push_back({u32(bindings.size()), type, 1, vk::ShaderStageFlagBits::eCompute});
    }
    pass.set = Check<"create volumetric descriptor layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, push_size};
    pass.layout = Check<"create volumetric pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*pass.set,
        .pushConstantRangeCount = push_size ? 1u : 0u,
        .pPushConstantRanges = push_size ? &range : nullptr,
    }));
    const auto module = CompileSPV(code, device);
    ASSERT(module);
    pass.pipeline = Check<"create volumetric pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module, .pName = "main"},
                .layout = *pass.layout}));
    device.destroyShaderModule(module);
}

void VolumetricLight::EnsureImage(u32 w, u32 h) {
    if (fog && depth_width == w && depth_height == h) return;
    if (fog) {
        scheduler.DeferOperation(
            [image = std::move(fog), view = std::move(fog_view),
             repair_image = std::move(repair_fog), repair_image_view = std::move(repair_view),
             point_image = std::move(point_fog), point_image_view = std::move(point_view),
             z = std::move(depth_snapshot), zv = std::move(depth_view)]() mutable {
                view.reset();
                image.Destroy();
                repair_image_view.reset();
                repair_image.Destroy();
                point_image_view.reset();
                point_image.Destroy();
                zv.reset();
                z.Destroy();
            });
    }
    depth_width = w;
    depth_height = h;
    width = (w + 1) / 2;
    height = (h + 1) / 2;
    const auto device = instance.GetDevice();
    fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    fog_view = Check<"create volumetric image view">(device.createImageViewUnique({
        .image = vk::Image(fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    repair_fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    repair_fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    repair_view = Check<"create volumetric repair view">(device.createImageViewUnique({
        .image = vk::Image(repair_fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    point_fog = VideoCore::UniqueImage(device, instance.GetAllocator());
    point_fog.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    point_view = Check<"create point volumetric view">(device.createImageViewUnique({
        .image = vk::Image(point_fog), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat, .subresourceRange = ColorRange,
    }));
    depth_snapshot = VideoCore::UniqueImage(device, instance.GetAllocator());
    depth_snapshot.Create({
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32Sfloat,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    depth_view = Check<"create volumetric depth snapshot view">(device.createImageViewUnique({
        .image = vk::Image(depth_snapshot), .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32Sfloat, .subresourceRange = ColorRange,
    }));
    SetObjectName(device, vk::Image(fog), "Directional volumetric scattering");
    SetObjectName(device, vk::Image(depth_snapshot), "Volumetric opaque depth snapshot");
    SetObjectName(device, vk::Image(repair_fog), "Volumetric thin-surface repair");
    std::printf("Volumetric lighting: %ux%u, %u steps, range %.1f m, strength %.3f\n",
                width, height, steps, reach, strength);
}

void VolumetricLight::Integrate(VideoCore::ImageId shadow_target,
                                const vk::DescriptorImageInfo& depth,
                                const ContactShadow::Inputs& in) {
    Reset();
    if (!Enabled() || !shadow_target || !depth.imageView || !in.shadow_map ||
        !in.constants || !in.uniforms) return;
    if (in.constants_range < (in.constants_offset_word & 255u) + 336u ||
        in.uniforms_range < ((in.constants_offset_word >> 8) & 255u) + 296u) {
        failed = true;
        std::printf("Volumetric lighting: unexpected shadow constant ranges; off\n");
        return;
    }
    auto& target = texture_cache.GetImage(shadow_target);
    u32 w = target.info.size.width, h = target.info.size.height;
    if (scene_targets.Reduced() && scene_targets.EligibleScene(target)) {
        w = scene_targets.Size().width;
        h = scene_targets.Size().height;
    }
    scheduler.EndRendering();
    EnsureImage(w, h);
    const float intensity = BbSettings::Get().graphics_volumetric_intensity.load(
        std::memory_order_relaxed);
    const float live_strength = strength *
        std::clamp(std::isfinite(intensity) ? intensity : 1.0f, 0.0f, 3.0f) *
        float(BbSettings::Get().effects[BbSettings::EffectVolumetricLight].load(
            std::memory_order_relaxed));
    const Params params{in.constants_offset_word, steps, reach, density, live_strength,
                        anisotropy, 0.02f, 0.0f};
    const vk::DescriptorImageInfo di{*sampler, depth.imageView, depth.imageLayout};
    const vk::DescriptorImageInfo si{*sampler, in.shadow_map, in.shadow_layout};
    const vk::DescriptorImageInfo out{{}, *fog_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo repaired{{}, *repair_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo zout{{}, *depth_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo state{direction_state->Handle(), 0, 112};
    scheduler.Record([image = vk::Image(point_fog)](vk::CommandBuffer cmd) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = ColorRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{std::array<float, 4>{}}, ColorRange);
    });
    for (u32 pass_index = 0; pass_index < 2; ++pass_index) {
        const bool repairing = pass_index == 1;
        const auto& pass = repairing ? repair : integrate;
        scheduler.RecordCrumb(
        {.name = repairing ? "Volumetric thin-surface repair" : "Directional volumetric integration"},
        [params, di, si, out, repaired, zout, state, in, repairing,
         image = repairing ? vk::Image(repair_fog) : vk::Image(fog),
         zimage = vk::Image(depth_snapshot),
         w = repairing ? depth_width : width, h = repairing ? depth_height : height,
         pipeline = *pass.pipeline, layout = *pass.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = Compute, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image, .subresourceRange = ColorRange,
            };
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            if (!repairing) {
                auto depth_barrier = barrier;
                depth_barrier.image = zimage;
                cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &depth_barrier});
            }
            const vk::DescriptorBufferInfo cb{in.constants, in.constants_offset, in.constants_range};
            const vk::DescriptorBufferInfo ub{in.uniforms, in.uniforms_offset, in.uniforms_range};
            const std::array<vk::WriteDescriptorSet, 8> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &cb},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &ub},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &si},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = repairing ? &repaired : &out},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &zout},
                {.dstBinding = 6, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &state},
                {.dstBinding = 7, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                    std::span{writes.data(), repairing ? 8u : 7u});
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
    }
    ready = true;
}

void VolumetricLight::Tint(const vk::DescriptorBufferInfo& lighting, u32 offset_bytes,
                           const vk::DescriptorImageInfo& material) {
    if (!ready || tinted || !Enabled()) return;
    constexpr u32 RecordBytes = 72 * sizeof(float);
    if (!lighting.buffer || !material.imageView || lighting.range < offset_bytes + RecordBytes) {
        failed = true;
        std::printf("Volumetric lighting: unexpected directional lighting table; off\n");
        return;
    }
    const std::array<u32, 2> params{
        offset_bytes, u32(std::min<vk::DeviceSize>((lighting.range - offset_bytes) / RecordBytes, 256))};
    scheduler.EndRendering();
    const vk::DescriptorImageInfo mi{*sampler, material.imageView, material.imageLayout};
    for (u32 pass_index = 0; pass_index < 2; ++pass_index) {
    const bool repairing = pass_index == 1;
    const vk::DescriptorImageInfo out{
        {}, repairing ? *repair_view : *fog_view, vk::ImageLayout::eGeneral};
    scheduler.RecordCrumb(
        {.name = "Resolved directional volumetric tint"},
        [params, lighting, mi, out, w = repairing ? depth_width : width,
         h = repairing ? depth_height : height,
         pipeline = *tint.pipeline, layout = *tint.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const std::array<vk::WriteDescriptorSet, 3> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lighting},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &mi},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                              params.data());
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
    }
    tinted = true;
    if (!tint_reported) {
        tint_reported = true;
        std::printf("Volumetric lighting: resolved primary diffuse RGB, %u region records\n",
                    params[1]);
    }
}

void VolumetricLight::PointLights(const PointShadow::Inputs& in, u32 count,
                                const PointRegion& region) {
    if (!ready || !Enabled()) return;
    const float intensity = BbSettings::Get().graphics_point_fog_intensity.load(
        std::memory_order_relaxed);
    if (!(intensity > 0.0f) || !std::isfinite(intensity)) return;
    const u32 constants_offset = in.offsets & 255u;
    const u32 lights_offset = (in.offsets >> 8) & 255u;
    if (!in.constants || !in.lights || count == 0 || count > 12 ||
        in.constants_range < constants_offset + 64u ||
        in.lights_range < lights_offset + count * 64u) {
        failed = true;
        std::printf("Point volumetric lighting: unexpected point-light buffer layout; off\n");
        return;
    }
    struct PointParams {
        u32 offsets, count;
        float intensity;
        u32 vertex_offset, vertex_stride, index_offset, index_size;
        s32 first_vertex;
        std::array<float, 4> viewport;
        std::array<s32, 4> scissor;
    };
    static_assert(sizeof(PointParams) == 64);
    const PointParams params{in.offsets, count, std::clamp(intensity, 0.0f, 3.0f),
        region.vertex_offset, region.vertex_stride, region.index_offset, region.index_size,
        region.first_vertex, region.viewport,
        {region.scissor.offset.x, region.scissor.offset.y,
         region.scissor.offset.x + s32(region.scissor.extent.width),
         region.scissor.offset.y + s32(region.scissor.extent.height)}};
    scheduler.EndRendering();
    const vk::DescriptorBufferInfo constants{in.constants, in.constants_offset, in.constants_range};
    const vk::DescriptorBufferInfo lights{in.lights, in.lights_offset, in.lights_range};
    const vk::DescriptorImageInfo depth{*sampler, *depth_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{{}, *point_view, vk::ImageLayout::eGeneral};
    scheduler.RecordCrumb(
        {.name = "Unshadowed point volumetric integration"},
        [params, constants, lights, depth, out, vertices = region.vertices, indices = region.indices,
         w = depth_width, h = depth_height,
         pipeline = *point.pipeline, layout = *point.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const std::array<vk::WriteDescriptorSet, 6> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &constants},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lights},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &depth},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &vertices},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &indices},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
}

void VolumetricLight::Scene(VideoCore::ImageId color) {
    if (!ready || !Enabled() || !color) return;
    scene = color;
    scene_uid = texture_cache.GetImage(color).image_uid;
}

void VolumetricLight::Celestial(const CelestialInputs& in) {
    if (!ready || !Enabled()) return;
    if (++celestial_candidates > 4) return;
    // Bind aligned ranges; the shader adds the exact source's byte adjustment.
    const u64 alignment = instance.StorageMinAlignment();
    auto aligned = [alignment](vk::DescriptorBufferInfo info) {
        const auto adjust = info.offset % alignment;
        info.offset -= adjust;
        info.range += adjust;
        return info;
    };
    if (!in.positions.buffer || !in.normals.buffer || !in.model.buffer ||
        !in.transforms.buffer || !in.scene.buffer || in.vertex_stride % 4 ||
        in.normal_stride % 4 || in.positions.offset % 4 || in.normals.offset % 4 ||
        in.model_offset % 4 || in.transform_offset % 4 || in.scene_offset % 4 ||
        in.model.range < in.model_offset + 84u ||
        in.scene.range < in.scene_offset + 864u ||
        in.transforms.range < in.transform_offset + 48u) {
        celestial_candidates = 5;
        if (!fallback_reported) {
            std::printf("Volumetric celestial: unsupported draw buffers; using shadow direction\n");
            fallback_reported = true;
        }
        return;
    }
    const std::array<vk::DescriptorBufferInfo, 6> buffers{
        aligned(in.positions), in.model, in.transforms, in.scene, aligned(in.normals),
        vk::DescriptorBufferInfo{direction_state->Handle(), 0, 112}};
    const std::array<u32, 8> params{
        u32(in.positions.offset - buffers[0].offset) / 4, in.vertex_stride / 4,
        u32(in.normals.offset - buffers[4].offset) / 4, in.normal_stride / 4, in.normal_word,
        in.model_offset / 4, in.transform_offset / 4, in.scene_offset / 4};
    scheduler.EndRendering();
    scheduler.RecordCrumb(
        {.name = "Verified celestial volumetric phase direction"},
        [buffers, params, pipeline = *celestial.pipeline,
         layout = *celestial.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            std::array<vk::WriteDescriptorSet, 6> writes{};
            for (u32 i = 0; i < writes.size(); ++i) {
                writes[i] = {.dstBinding = i, .descriptorCount = 1,
                             .descriptorType = vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &buffers[i]};
            }
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                              params.data());
            cmd.dispatch(1, 1, 1);
            After(cmd);
        });
    if (!celestial_reported) {
        celestial_reported = true;
        std::printf("Volumetric celestial: GPU daytime sky-dome fingerprint validation enabled; "
                    "unmatched, invalid or ambiguous geometry keeps shadow phase\n");
    }
}

void VolumetricLight::Composite() {
    const bool has_result = ready;
    const auto color_id = scene;
    const auto uid = scene_uid;
    const bool has_tint = tinted;
    const u32 candidates = celestial_candidates;
    Reset();
    if (!has_result || !Enabled() || !color_id) return;
    if (!has_tint) {
        failed = true;
        std::printf("Volumetric lighting: directional tint draw missing; off\n");
        return;
    }
    auto* color_ptr = texture_cache.TryGetImage(color_id, uid);
    if (!color_ptr) {
        failed = true;
        std::printf("Volumetric lighting: HDR scene expired before composite; off\n");
        return;
    }
    auto& color = *color_ptr;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        failed = true;
        std::printf("Volumetric lighting: scene color is not RGBA16F; off\n");
        return;
    }
    scheduler.EndRendering();
    u32 w = color.info.size.width, h = color.info.size.height;
    vk::ImageView target_view{};
    if (scene_targets.Reduced() && scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo info;
        info.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(color_id, info, Compute,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        if (!(proxy.usage & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("Volumetric lighting: scene proxy lacks storage support; off\n");
            return;
        }
        w = scene_targets.Size().width;
        h = scene_targets.Size().height;
        target_view = proxy.view;
    } else {
        if (!(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("Volumetric lighting: scene lacks storage support; off\n");
            return;
        }
        const auto device = instance.GetDevice();
        target_view = Check<"create volumetric scene view">(device.createImageView({
            .image = color.GetImage(), .viewType = vk::ImageViewType::e2D,
            .format = color.info.pixel_format, .subresourceRange = ColorRange,
        }));
        scheduler.DeferOperation([device, target_view] { device.destroyImageView(target_view); });
        runtime.Transit(&color, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
    }
    if (w != depth_width || h != depth_height) {
        failed = true;
        std::printf("Volumetric lighting: scene and shadow viewport sizes differ; off\n");
        return;
    }
    const vk::DescriptorImageInfo fi{*sampler, *fog_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo di{*sampler, *depth_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{{}, target_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo repaired{*sampler, *repair_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo points{*sampler, *point_view, vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo state{direction_state->Handle(), 0, 112};
    struct CompositeParams { float anisotropy; u32 allow_celestial; };
    const CompositeParams params{anisotropy, candidates <= 4 ? 1u : 0u};
    if ((candidates == 0 || candidates > 4) && !fallback_reported) {
        fallback_reported = true;
        std::printf("Volumetric celestial: absent or excessive candidate draws; using shadow direction\n");
    }
    if (!composite_reported) {
        composite_reported = true;
        std::printf("Volumetric lighting: late sky/scene composite into %ux%u HDR scene\n", w, h);
    }
    scheduler.RecordCrumb(
        {.name = "Directional volumetric composite"},
        [fi, di, out, repaired, points, state, params, w, h, pipeline = *composite.pipeline,
         layout = *composite.layout](vk::CommandBuffer cmd) {
            Before(cmd);
            const std::array<vk::WriteDescriptorSet, 6> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &fi},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &di},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &state},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &repaired},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &points},
            }};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            After(cmd);
        });
}
} // namespace Vulkan
