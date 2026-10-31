// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_point_shadow.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/point_shadow_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_effect_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
namespace {
struct Push {
    u32 buf_off;
    u32 frame_index;
    std::array<float, 2> size;
    float length_m;
    float thickness_m;
    float strength;
    float fade_start_m;
    float fade_end_m;
    u32 light_count;
    u32 steps;
    float bias_m;
    std::array<s32, 4> rect;
};
static_assert(sizeof(Push) == 64);

constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto TargetFormat = vk::Format::eB10G11R11UfloatPack32;

}

PointShadow::PointShadow(const Instance& instance_, Scheduler& scheduler_,
                         VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                         SceneTargets& scene_targets_, CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_}, camera_motion{camera_motion_} {
    fade_start_m = fade_end_m * 0.6f;
}

bool PointShadow::Ready() {
    if (tried) {
        return !failed;
    }
    tried = true;
    using enum vk::DescriptorType;
    const bool made =
        instance.GetPhysicalDevice().getFeatures().shaderStorageImageExtendedFormats &&
        EffectPass::StorageFormat(instance, TargetFormat) &&
        EffectPass::CreateSampler(instance, sampler, vk::Filter::eNearest) &&
        EffectPass::Create(instance, set_layout, layout, pipeline,
                           {eStorageBuffer, eCombinedImageSampler, eStorageBuffer, eStorageImage,
                            eStorageImage, eStorageImage, eStorageImage},
                           sizeof(Push), POINT_SHADOW_COMP, "Point shadows", true);
    if (!made) {
        failed = true;
        std::printf("Point shadows: off - this GPU cannot run the pass\n");
    }
    return made;
}

PointShadow::~PointShadow() = default;

bool PointShadow::EnsureImage(OwnImage& own, u32 width, u32 height) {
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
        .format = TargetFormat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, vk::Image(own.image), "Point shadow snapshot");
    own.view = Check<"create point shadow snapshot view">(device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = TargetFormat,
        .subresourceRange = ColorRange,
    }));
    own.width = width;
    own.height = height;
    return true;
}

void PointShadow::OnLightDraw(VideoCore::ImageId color0, VideoCore::ImageId color1,
                              const Inputs& inputs) {
    if (failed || !color0 || !color1 || !inputs.constants || !inputs.lights ||
        !BbSettings::Get().effects[BbSettings::EffectPointShadows].load(
            std::memory_order_relaxed) ||
        !Ready()) {
        return;
    }
    const auto depth_id = camera_motion.Depth();
    if (!depth_id) {
        return;
    }
    const bool reduced = scene_targets.Reduced() &&
                         scene_targets.Eligible(texture_cache.GetImage(depth_id));
    const std::array ids{color0, color1};
    std::array<VideoCore::Image*, 2> targets{};
    for (int i = 0; i < 2; ++i) {
        targets[i] = &texture_cache.GetImage(ids[i]);
        const auto& info = targets[i]->info;
        if (info.pixel_format != TargetFormat ||
            (!reduced && !(targets[i]->usage_flags & vk::ImageUsageFlagBits::eStorage)) ||
            (!reduced && !(targets[i]->usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) ||
            (reduced && !scene_targets.Eligible(*targets[i])) ||
            info.size.width != targets[0]->info.size.width ||
            info.size.height != targets[0]->info.size.height) {
            failed = true;
            std::printf("Point shadows: the light targets are not matching B10G11R11 storage "
                        "images; off\n");
            return;
        }
    }
    if (inputs.constants_range < (inputs.offsets & 255u) + 64u ||
        inputs.lights_range < ((inputs.offsets >> 8) & 255u) + inputs.count * 64u) {
        return;
    }
    u32 width = targets[0]->info.size.width, height = targets[0]->info.size.height;
    if (reduced) {
        const auto size = scene_targets.ProxySize(*targets[0]);
        width = size.width;
        height = size.height;
    }
    const s32 x0 = std::clamp(inputs.tile.offset.x, 0, s32(width));
    const s32 y0 = std::clamp(inputs.tile.offset.y, 0, s32(height));
    const s32 x1 = std::clamp(inputs.tile.offset.x + s32(inputs.tile.extent.width), x0, s32(width));
    const s32 y1 = std::clamp(inputs.tile.offset.y + s32(inputs.tile.extent.height), y0, s32(height));
    if (x0 == x1 || y0 == y1) {
        return;
    }
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("Point shadows: %ux%u, length %.2f m, thickness %.2f m, strength %.2f, "
                    "%u steps, first tile %dx%d\n",
                    width, height, length_m, thickness_m, strength, steps, x1 - x0, y1 - y0);
    }

    if ((!snapshot[0].image || snapshot[0].width != width || snapshot[0].height != height) &&
        !EffectPass::HasRoom(instance, 2ull * 4 * width * height)) {
        failed = true;
        std::printf("Point shadows: off - not enough video memory\n");
        return;
    }
    scheduler.EndRendering();
    for (auto& own : snapshot) {
        EnsureImage(own, width, height);
    }
    VideoCore::ImageViewInfo target_info;
    target_info.format = TargetFormat;
    std::array<vk::Image, 2> src{};
    vk::ImageLayout src_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (reduced) {
        src_layout = vk::ImageLayout::eGeneral;
        for (int i = 0; i < 2; ++i) {
            const auto proxy = scene_targets.Read(ids[i], target_info, vk::PipelineStageFlagBits2::eCopy,
                                                  vk::AccessFlagBits2::eTransferRead);
            if (!(proxy.usage & vk::ImageUsageFlagBits::eStorage) ||
                !(proxy.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
                failed = true;
                scene_targets.Attachment(ids[0], target_info);
                scene_targets.Attachment(ids[1], target_info);
                std::printf("Point shadows: the reduced light targets cannot be used; off\n");
                return;
            }
            src[i] = proxy.image;
        }
    } else {
        for (auto* target : targets) {
            runtime.Transit(target, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        }
        runtime.FlushBarriers();
        src = {targets[0]->GetImage(), targets[1]->GetImage()};
    }
    const std::array<vk::Image, 2> dst{vk::Image(snapshot[0].image),
                                       vk::Image(snapshot[1].image)};
    EffectPass::Mark(5, "point shadow snapshots");
    scheduler.RecordCrumb({.name = "Point shadows snapshot"},
                          [src, dst, x0, y0, x1, y1, src_layout](vk::CommandBuffer cmdbuf) {
        std::array<vk::ImageMemoryBarrier2, 2> to_dst;
        for (int i = 0; i < 2; ++i) {
            to_dst[i] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst[i],
                .subresourceRange = ColorRange,
            };
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_dst.data()});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffset = {x0, y0, 0},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffset = {x0, y0, 0},
            .extent = {u32(x1 - x0), u32(y1 - y0), 1},
        };
        for (int i = 0; i < 2; ++i) {
            cmdbuf.copyImage(src[i], src_layout, dst[i],
                             vk::ImageLayout::eTransferDstOptimal, region);
        }
        std::array<vk::ImageMemoryBarrier2, 2> to_general;
        for (int i = 0; i < 2; ++i) {
            to_general[i] = to_dst[i];
            to_general[i].srcStageMask = vk::PipelineStageFlagBits2::eCopy;
            to_general[i].srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            to_general[i].dstStageMask = Compute;
            to_general[i].dstAccessMask = vk::AccessFlagBits2::eShaderRead;
            to_general[i].oldLayout = vk::ImageLayout::eTransferDstOptimal;
            to_general[i].newLayout = vk::ImageLayout::eGeneral;
        }
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = to_general.data()});
    });
    if (reduced) {
        scene_targets.Attachment(ids[0], target_info);
        scene_targets.Attachment(ids[1], target_info);
    } else {
        for (auto* target : targets) {
            runtime.Transit(target, vk::ImageLayout::eColorAttachmentOptimal,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite |
                                vk::AccessFlagBits2::eColorAttachmentRead);
        }
        runtime.FlushBarriers();
    }

    pending[0] = color0;
    pending[1] = color1;
    pending_uid = {targets[0]->image_uid, targets[1]->image_uid};
    pending_inputs = inputs;
    pending_rect = {x0, y0, x1, y1};
}

void PointShadow::Flush() {
    if (!pending[0]) {
        return;
    }
    const auto ids = pending;
    const auto uids = pending_uid;
    pending[0] = pending[1] = {};
    const auto depth_id = camera_motion.Depth();
    if (!depth_id) {
        return;
    }
    auto* depth_ptr = texture_cache.TryGetImage(depth_id, 0);
    auto* target0 = texture_cache.TryGetImage(ids[0], uids[0]);
    auto* target1 = texture_cache.TryGetImage(ids[1], uids[1]);
    if (!depth_ptr || !target0 || !target1) {
        return;
    }
    auto& depth = *depth_ptr;
    std::array<VideoCore::Image*, 2> targets{target0, target1};
    const bool reduced = scene_targets.Reduced() && scene_targets.Eligible(depth) &&
                         scene_targets.Eligible(*targets[0]);
    u32 width = targets[0]->info.size.width, height = targets[0]->info.size.height;
    if (reduced) {
        const auto size = scene_targets.ProxySize(*targets[0]);
        width = size.width;
        height = size.height;
    }
    if (snapshot[0].width != width || snapshot[0].height != height) {
        return;
    }

    scheduler.EndRendering();
    const auto device = instance.GetDevice();
    std::array<vk::ImageView, 2> target_views;
    vk::ImageView depth_view{};
    if (reduced) {
        VideoCore::ImageViewInfo target_info, depth_info;
        target_info.format = TargetFormat;
        depth_info.format = depth.info.pixel_format;
        for (int i = 0; i < 2; ++i) {
            target_views[i] = scene_targets
                                  .Read(ids[i], target_info, Compute,
                                        vk::AccessFlagBits2::eShaderRead |
                                            vk::AccessFlagBits2::eShaderWrite)
                                  .view;
        }
        depth_view = scene_targets.Read(depth_id, depth_info).view;
    } else {
        for (int i = 0; i < 2; ++i) {
            target_views[i] = Check<"create point shadow target view">(device.createImageView({
                .image = targets[i]->GetImage(),
                .viewType = vk::ImageViewType::e2D,
                .format = TargetFormat,
                .subresourceRange = ColorRange,
            }));
        }
        depth_view = Check<"create point shadow depth view">(device.createImageView({
            .image = depth.GetImage(),
            .viewType = vk::ImageViewType::e2D,
            .format = depth.info.pixel_format,
            .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
        }));
        scheduler.DeferOperation([device, target_views, depth_view] {
            device.destroyImageView(target_views[0]);
            device.destroyImageView(target_views[1]);
            device.destroyImageView(depth_view);
        });
        for (auto* target : targets) {
            runtime.Transit(target, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        }
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }

    const Push push{
        .buf_off = pending_inputs.offsets,
        .frame_index = frame_index++,
        .size = {float(width), float(height)},
        .length_m = length_m,
        .thickness_m = thickness_m,
        .strength = strength,
        .fade_start_m = fade_start_m,
        .fade_end_m = fade_end_m,
        .light_count = pending_inputs.count,
        .steps = steps,
        .bias_m = bias_m,
        .rect = pending_rect,
    };
    EffectPass::Mark(6, "point shadows");
    scheduler.RecordCrumb(
        {.name = "Point shadows"},
        [push, target_views, depth_view, sampler = *sampler,
         snap0 = *snapshot[0].view, snap1 = *snapshot[1].view, in = pending_inputs,
         pipeline = *pipeline, layout = *layout](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = Compute,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                 vk::AccessFlagBits2::eShaderWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            const vk::DescriptorBufferInfo constants{in.constants, in.constants_offset,
                                                     in.constants_range};
            const vk::DescriptorBufferInfo lights{in.lights, in.lights_offset, in.lights_range};
            const vk::DescriptorImageInfo depth_info{sampler, depth_view,
                                                     vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo cur0{{}, target_views[0], vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo old0{{}, snap0, vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo cur1{{}, target_views[1], vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo old1{{}, snap1, vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 7> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &constants},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                 .pImageInfo = &depth_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lights},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cur0},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &old0},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &cur1},
                {.dstBinding = 6, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &old1},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push),
                                 &push);
            const u32 gx0 = u32(push.rect[0]) / 8, gy0 = u32(push.rect[1]) / 8;
            const u32 gx1 = (u32(push.rect[2]) + 7) / 8, gy1 = (u32(push.rect[3]) + 7) / 8;
            cmdbuf.dispatchBase(gx0, gy0, 0, gx1 - gx0, gy1 - gy0, 1);
            const vk::MemoryBarrier2 after{
                .srcStageMask = Compute,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        });
}

}
