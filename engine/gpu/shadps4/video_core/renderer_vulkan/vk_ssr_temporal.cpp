// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_ssr_temporal.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "video_core/host_shaders/ssr_temporal_copy_comp.h"
#include "video_core/host_shaders/ssr_temporal_resolve_comp.h"
#include "video_core/host_shaders/ssr_trace_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_effect_pass.h"
#include "video_core/renderer_vulkan/vk_effect_shaders.h"
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

}

SsrTemporal::SsrTemporal(const Instance& instance_, Scheduler& scheduler_,
                         VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                         SceneTargets& scene_targets_, CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_}, camera_motion{camera_motion_} {
}

bool SsrTemporal::Ready() {
    if (tried) {
        return !failed;
    }
    tried = true;
    const bool made =
        instance.IsUniformBufferStandardLayoutSupported() &&
        EffectPass::StorageFormat(instance, Format) &&
        CreatePass(resolve, {SampledImage, SampledImage, SampledImage, StorageImage},
                   SSR_TEMPORAL_RESOLVE_COMP, "SSR resolve") &&
        CreatePass(copy, {SampledImage, StorageImage}, SSR_TEMPORAL_COPY_COMP, "SSR copy") &&
        CreateTracePass();
    if (!made) {
        failed = true;
        std::printf("SSR temporal: off - this GPU cannot run the passes\n");
    }
    return made;
}

SsrTemporal::~SsrTemporal() {
    const auto device = instance.GetDevice();
    for (const auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

bool SsrTemporal::CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                             std::span<const u32> code, const char* name) {
    return EffectPass::Create(instance, pass.set_layout, pass.layout, pass.pipeline, types,
                              sizeof(Params), code, name);
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

void SsrTemporal::OnSsrDraw(VideoCore::ImageId target, const TraceInputs& inputs) {
    if (failed || done_this_frame || !Ready()) {
        return;
    }
    pending = target;
    trace_inputs = inputs;
}

void SsrTemporal::OnFrameEnd() {
    done_this_frame = false;
    full_ready = false;
    pending = {};
}

bool SsrTemporal::CreateTracePass() {
    using enum vk::DescriptorType;
    return EffectPass::CreateSampler(instance, nearest_sampler, vk::Filter::eNearest) &&
           EffectPass::CreateSampler(instance, linear_sampler, vk::Filter::eLinear) &&
           EffectPass::Create(instance, trace.set_layout, trace.layout, trace.pipeline,
                              {eUniformBuffer, eCombinedImageSampler, eCombinedImageSampler,
                               eCombinedImageSampler, eStorageImage},
                              16, SSR_TRACE_COMP, "SSR trace");
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
    const bool reduced = scene_targets.Reduced() && scene_targets.Eligible(depth) &&
                         scene_targets.Eligible(target);
    if (!reduced && !(target.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        failed = true;
        std::printf("SSR temporal: the SSR target is not a storage image; off\n");
        return;
    }
    done_this_frame = true;

    u32 width = depth.info.size.width, height = depth.info.size.height;
    u32 half_width = target.info.size.width, half_height = target.info.size.height;
    if (reduced) {
        const auto scene_size = scene_targets.ProxySize(depth);
        const auto target_size = scene_targets.ProxySize(target);
        width = scene_size.width;
        height = scene_size.height;
        half_width = target_size.width;
        half_height = target_size.height;
    } else if (const auto render = camera_motion.RenderSize(); render[0] && render[1]) {
        width = std::min(width, render[0]);
        height = std::min(height, render[1]);
    }

    scheduler.EndRendering();
    vk::ImageView target_view{}, depth_view{};
    if (reduced) {
        VideoCore::ImageViewInfo target_info, depth_info;
        target_info.format = Format;
        depth_info.format = depth.info.pixel_format;
        const auto proxy = scene_targets.Read(target_id, target_info, Compute,
                                              vk::AccessFlagBits2::eShaderRead |
                                                  vk::AccessFlagBits2::eShaderWrite);
        if (!(proxy.usage & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("SSR temporal: the reduced SSR target is not a storage image; off\n");
            return;
        }
        target_view = proxy.view;
        depth_view = scene_targets.Read(depth_id, depth_info).view;
    } else {
        target_view = CachedView(target, Format, vk::ImageAspectFlagBits::eColor);
        depth_view = CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
        runtime.Transit(&target, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }

    if ((!accum[0].image || accum[0].width != half_width || accum[0].height != half_height) &&
        !EffectPass::HasRoom(instance, 3ull * 8 * half_width * half_height + 3ull * 8 * width * height)) {
        failed = true;
        std::printf("SSR temporal: off - not enough video memory\n");
        return;
    }
    bool created = false;
    for (auto& image : accum) {
        created |= EnsureImage(image, half_width, half_height, "accum");
    }
    if (created) {
        history_valid = false;
    }
    constexpr float blend = 0.9f;
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
    EffectPass::Mark(2, "SSR temporal");
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
    if (EffectShaders::SsrCompositeActive() && trace_inputs.constants && trace_inputs.normals_view &&
        trace_inputs.color_view) {
        const auto normals_view = trace_inputs.normals_view;
        const auto color_view = trace_inputs.color_view;
        const auto normals_layout = trace_inputs.normals_layout;
        const auto color_layout = trace_inputs.color_layout;
        bool full_created = EnsureImage(trace_image, width, height, "trace");
        for (auto& image : accum_full) {
            full_created |= EnsureImage(image, width, height, "full accum");
        }
        if (full_created) {
            full_history_valid = false;
        }
        static bool full_printed = false;
        if (!full_printed) {
            full_printed = true;
            std::printf("SSR trace: %ux%u full-size reflections\n", width, height);
        }
        const bool full_previous = full_history_valid && prev_camera_valid && camera.valid;
        Params full_params = params;
        full_params.reproject = full_previous ? Multiply(prev_view, camera.inv_view) : Identity;
        full_params.prev_jitter = full_previous ? prev_jitter : jitter;
        full_params.prev_proj = full_previous ? prev_proj : std::array<float, 2>{proj[0], proj[1]};
        full_params.history = full_previous ? 1u : 0u;
        const std::array<u32, 4> trace_push{
            trace_inputs.constants_dword_offset, trace_inputs.frame_index,
            std::bit_cast<u32>(float(width)), std::bit_cast<u32>(float(height))};
        EffectPass::Mark(3, "SSR full trace");
        scheduler.RecordCrumb(
            {.name = "SSR full trace"},
            [full_params, trace_push, width, height, constants = trace_inputs.constants,
             constants_offset = trace_inputs.constants_offset,
             constants_range = trace_inputs.constants_range, depth_view, normals_view, color_view,
             normals_layout, color_layout,
             nearest = *nearest_sampler, linear = *linear_sampler,
             trace_image_handle = vk::Image(trace_image.image), trace_view = *trace_image.view,
             cur_image = vk::Image(accum_full[current].image), cur_view = *accum_full[current].view,
             old_image = vk::Image(accum_full[current ^ 1].image),
             old_view = *accum_full[current ^ 1].view, init_previous = !full_history_valid,
             trace_pipeline = *trace.pipeline, trace_layout = *trace.layout,
             resolve_pipeline = *resolve.pipeline,
             resolve_layout = *resolve.layout](vk::CommandBuffer cmdbuf) {
                MemoryBarrier(cmdbuf, AllCommands, vk::AccessFlagBits2::eMemoryWrite, Compute,
                              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
                std::array<vk::ImageMemoryBarrier2, 3> barriers{ToGeneral(trace_image_handle),
                                                                ToGeneral(cur_image)};
                u32 barrier_count = 2;
                if (init_previous) {
                    barriers[barrier_count++] = ToGeneral(old_image);
                }
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = barrier_count,
                                         .pImageMemoryBarriers = barriers.data()});
                const u32 groups_x = (width + 7) / 8, groups_y = (height + 7) / 8;
                {
                    const vk::DescriptorBufferInfo constants_info{constants, constants_offset,
                                                                  constants_range};
                    const std::array infos{
                        vk::DescriptorImageInfo{nearest, depth_view, vk::ImageLayout::eGeneral},
                        vk::DescriptorImageInfo{nearest, normals_view, normals_layout},
                        vk::DescriptorImageInfo{linear, color_view, color_layout},
                        Info(trace_view)};
                    const std::array<vk::WriteDescriptorSet, 5> writes{{
                        {.dstBinding = 0, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eUniformBuffer,
                         .pBufferInfo = &constants_info},
                        {.dstBinding = 1, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                         .pImageInfo = &infos[0]},
                        {.dstBinding = 2, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                         .pImageInfo = &infos[1]},
                        {.dstBinding = 3, .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                         .pImageInfo = &infos[2]},
                        Write(4, StorageImage, &infos[3]),
                    }};
                    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, trace_pipeline);
                    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, trace_layout, 0,
                                                writes);
                    cmdbuf.pushConstants(trace_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                         sizeof(trace_push), trace_push.data());
                    cmdbuf.dispatch(groups_x, groups_y, 1);
                }
                MemoryBarrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite, Compute,
                              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
                {
                    const std::array infos{Info(trace_view), Info(depth_view), Info(old_view),
                                           Info(cur_view)};
                    const std::array writes{Write(0, SampledImage, &infos[0]),
                                            Write(1, SampledImage, &infos[1]),
                                            Write(2, SampledImage, &infos[2]),
                                            Write(3, StorageImage, &infos[3])};
                    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, resolve_pipeline);
                    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, resolve_layout, 0,
                                                writes);
                    cmdbuf.pushConstants(resolve_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                         sizeof(Params), &full_params);
                    cmdbuf.dispatch(groups_x, groups_y, 1);
                }
                MemoryBarrier(cmdbuf, Compute, vk::AccessFlagBits2::eShaderWrite, AllCommands,
                              vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
            });
        full_history_valid = true;
        full_ready = true;
    }
    history_valid = true;
    prev_view = camera.view;
    prev_proj = {proj[0], proj[1]};
    prev_jitter = jitter;
    prev_camera_valid = camera.valid;
    ++frame;
}

}
