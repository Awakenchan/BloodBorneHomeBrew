// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: temporal accumulation of the screen-space reflections (vk_ssr_temporal.h).

#include "video_core/renderer_vulkan/vk_ssr_temporal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "video_core/host_shaders/ssr_temporal_copy_comp.h"
#include "video_core/host_shaders/ssr_temporal_resolve_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct Params {
    std::array<float, 4> proj;
    std::array<float, 12> reproject;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> prev_jitter;
    std::array<float, 2> prev_proj;
    u32 history;
    float blend;
};
static_assert(sizeof(Params) == 104);

std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        }
        r[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    return r;
}

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto AllCommands = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto SampledImage = vk::DescriptorType::eSampledImage;
constexpr auto StorageImage = vk::DescriptorType::eStorageImage;
constexpr auto Format = vk::Format::eR16G16B16A16Sfloat;

float EnvFloat(const char* name, float fallback) {
    const char* env = std::getenv(name);
    return env && env[0] ? static_cast<float>(std::atof(env)) : fallback;
}

void MemoryBarrier(vk::CommandBuffer cmdbuf, vk::PipelineStageFlags2 src_stage,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                   vk::AccessFlags2 dst_access) {
    const vk::MemoryBarrier2 barrier{.srcStageMask = src_stage,
                                     .srcAccessMask = src_access,
                                     .dstStageMask = dst_stage,
                                     .dstAccessMask = dst_access};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

vk::ImageMemoryBarrier2 ToGeneral(vk::Image image) {
    return {
        .srcStageMask = AllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

vk::DescriptorImageInfo Info(vk::ImageView view) {
    return {.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
}

vk::WriteDescriptorSet Write(u32 binding, vk::DescriptorType type,
                             const vk::DescriptorImageInfo* image) {
    return {.dstBinding = binding, .descriptorCount = 1, .descriptorType = type,
            .pImageInfo = image};
}

} // namespace

SsrTemporal::SsrTemporal(const Instance& instance_, Scheduler& scheduler_,
                         VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                         SceneTargets& scene_targets_, CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_}, camera_motion{camera_motion_} {
    enabled = EnvFloat("BB_SSR_TEMPORAL", 1.0f) > 0.5f;
    if (!enabled) {
        return;
    }
    CreatePass(resolve, {SampledImage, SampledImage, SampledImage, StorageImage},
               SSR_TEMPORAL_RESOLVE_COMP, "ssr_temporal.comp (resolve)");
    CreatePass(copy, {SampledImage, StorageImage}, SSR_TEMPORAL_COPY_COMP,
               "ssr_temporal.comp (copy)");
}

SsrTemporal::~SsrTemporal() {
    const auto device = instance.GetDevice();
    for (const auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

void SsrTemporal::CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                             std::span<const u32> code, const char* name) {
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
    u32 count = 0;
    for (const vk::DescriptorType type : types) {
        bindings[count] = {.binding = count,
                           .descriptorType = type,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        ++count;
    }
    pass.set_layout = Check<"create SSR temporal descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = count,
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(Params)};
    pass.layout = Check<"create SSR temporal pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*pass.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(code, device);
    ASSERT(module);
    SetObjectName(device, module, name);
    pass.pipeline = Check<"create SSR temporal pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pass.layout,
            }));
    device.destroyShaderModule(module);
}

bool SsrTemporal::EnsureImage(OwnImage& own, u32 width, u32 height, const char* name) {
    if (own.image && own.width == width && own.height == height) {
        return false;
    }
    if (own.image) {
        scheduler.DeferOperation(
            [image = std::move(own.image), view = std::move(own.view)]() mutable {
                view.reset();
                image.Destroy();
            });
    }
    const auto device = instance.GetDevice();
    own.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    own.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = Format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, vk::Image(own.image), "SSR temporal {}", name);
    own.view = Check<"create SSR temporal image view">(device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = Format,
        .subresourceRange = ColorRange,
    }));
    own.width = width;
    own.height = height;
    return true;
}

vk::ImageView SsrTemporal::CachedView(const VideoCore::Image& image, vk::Format format,
                                      vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check<"create SSR temporal view">(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

void SsrTemporal::OnSsrDraw(VideoCore::ImageId target) {
    if (!enabled || failed || done_this_frame) {
        return;
    }
    pending = target;
}

void SsrTemporal::OnFrameEnd() {
    done_this_frame = false;
    pending = {};
}

void SsrTemporal::Flush(std::array<float, 2> jitter) {
    if (!pending) {
        return;
    }
    const auto target_id = pending;
    pending = {};
    const auto depth_id = camera_motion.Depth();
    const auto proj = camera_motion.PassProjection();
    const auto camera = camera_motion.PassMatrices();
    if (!depth_id || proj[0] == 0.0f || proj[1] == 0.0f) {
        return;
    }
    auto& target = texture_cache.GetImage(target_id);
    auto& depth = texture_cache.GetImage(depth_id);
    if (target.info.pixel_format != Format) {
        failed = true;
        std::printf("SSR temporal: the SSR target is not RGBA16F; off\n");
        return;
    }
    if (!(target.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        failed = true;
        std::printf("SSR temporal: the SSR target is not a storage image; off\n");
        return;
    }
    if (scene_targets.Reduced() && scene_targets.EligibleScene(depth)) {
        failed = true;
        std::printf("SSR temporal: reduced scene targets are not supported; off\n");
        return;
    }
    done_this_frame = true;

    u32 width = depth.info.size.width, height = depth.info.size.height;
    if (const auto render = camera_motion.RenderSize(); render[0] && render[1]) {
        width = std::min(width, render[0]);
        height = std::min(height, render[1]);
    }
    const u32 half_width = target.info.size.width, half_height = target.info.size.height;

    scheduler.EndRendering();
    const auto target_view = CachedView(target, Format, vk::ImageAspectFlagBits::eColor);
    const auto depth_view =
        CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
    runtime.Transit(&target, vk::ImageLayout::eGeneral, Compute,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();

    bool created = false;
    for (auto& image : accum) {
        created |= EnsureImage(image, half_width, half_height, "accum");
    }
    if (created) {
        history_valid = false;
    }
    static const float blend = EnvFloat("BB_SSR_TEMPORAL_BLEND", 0.9f);
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("SSR temporal: %ux%u target, %ux%u scene, blend %.2f\n", half_width,
                    half_height, width, height, blend);
    }

    const bool use_previous = history_valid && prev_camera_valid && camera.valid;
    constexpr std::array<float, 12> Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const Params params{
        .proj = proj,
        .reproject = use_previous ? Multiply(prev_view, camera.inv_view) : Identity,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .prev_jitter = use_previous ? prev_jitter : jitter,
        .prev_proj = use_previous ? prev_proj : std::array<float, 2>{proj[0], proj[1]},
        .history = use_previous ? 1u : 0u,
        .blend = blend,
    };
    const u32 current = frame & 1;
    scheduler.RecordCrumb(
        {.name = "SSR temporal"},
        [params, half_width, half_height, target_view, depth_view,
         cur_image = vk::Image(accum[current].image), cur_view = *accum[current].view,
         old_image = vk::Image(accum[current ^ 1].image), old_view = *accum[current ^ 1].view,
         init_previous = !history_valid, resolve_pipeline = *resolve.pipeline,
         resolve_layout = *resolve.layout, copy_pipeline = *copy.pipeline,
         copy_layout = *copy.layout](vk::CommandBuffer cmdbuf) {
            MemoryBarrier(cmdbuf, AllCommands, vk::AccessFlagBits2::eMemoryWrite, Compute,
                          vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            std::array<vk::ImageMemoryBarrier2, 2> barriers{ToGeneral(cur_image)};
            u32 barrier_count = 1;
            if (init_previous) {
                barriers[barrier_count++] = ToGeneral(old_image);
            }
            cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = barrier_count, .pImageMemoryBarriers = barriers.data()});
            const u32 groups_x = (half_width + 7) / 8, groups_y = (half_height + 7) / 8;
            const auto run = [&](vk::Pipeline pipeline, vk::PipelineLayout layout,
                                 std::span<const vk::WriteDescriptorSet> writes) {
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params),
                                     &params);
                cmdbuf.dispatch(groups_x, groups_y, 1);
            };
            {
                const std::array infos{Info(target_view), Info(depth_view), Info(old_view),
                                       Info(cur_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, SampledImage, &infos[1]),
                                        Write(2, SampledImage, &infos[2]),
                                        Write(3, StorageImage, &infos[3])};
                run(resolve_pipeline, resolve_layout, writes);
            }
            MemoryBarrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite, Compute,
                          vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
            {
                const std::array infos{Info(cur_view), Info(target_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, StorageImage, &infos[1])};
                run(copy_pipeline, copy_layout, writes);
            }
            MemoryBarrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite, AllCommands,
                          vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        });
    history_valid = true;
    prev_view = camera.view;
    prev_proj = {proj[0], proj[1]};
    prev_jitter = jitter;
    prev_camera_valid = camera.valid;
    ++frame;
}

} // namespace Vulkan
