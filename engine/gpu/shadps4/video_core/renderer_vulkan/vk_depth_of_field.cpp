// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_depth_of_field.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/depth_of_field_camera_comp.h"
#include "video_core/host_shaders/depth_of_field_snapshot_comp.h"
#include "video_core/host_shaders/depth_of_field_kernel_comp.h"
#include "video_core/host_shaders/depth_of_field_prepare_comp.h"
#include "video_core/host_shaders/depth_of_field_gather_comp.h"
#include "video_core/host_shaders/depth_of_field_composite_comp.h"
#include "video_core/host_shaders/depth_of_field_focus_comp.h"
#include "video_core/host_shaders/depth_of_field_near_tiles_comp.h"
#include "video_core/host_shaders/depth_of_field_near_comp.h"
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
constexpr auto Sampled = vk::DescriptorType::eCombinedImageSampler;
constexpr auto Storage = vk::DescriptorType::eStorageImage;
constexpr auto Buffer = vk::DescriptorType::eStorageBuffer;

bool HasWords(const vk::DescriptorBufferInfo& info, u32 offset, u32 count) {
    return info.buffer && offset % 4 == 0 && info.range >= u64(offset) + 4ull * count;
}
vk::WriteDescriptorSet Write(u32 binding, const vk::DescriptorBufferInfo& info) {
    return {.dstBinding = binding, .descriptorCount = 1, .descriptorType = Buffer,
            .pBufferInfo = &info};
}
vk::WriteDescriptorSet Write(u32 binding, vk::DescriptorType type,
                              const vk::DescriptorImageInfo& info) {
    return {.dstBinding = binding, .descriptorCount = 1, .descriptorType = type,
            .pImageInfo = &info};
}
void Barrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}
} // namespace

DepthOfField::DepthOfField(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                           SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_} {
    const char* env = std::getenv("BB_MODERN_DOF");
    enabled = env && std::string_view{env} == "1";
    if (!enabled) return;
    const auto device = instance.GetDevice();
    const std::array types{Buffer, Buffer, Buffer, Sampled, Sampled, Sampled, Sampled,
                           Sampled, Storage, Storage, Storage, Storage, Storage, Storage, Sampled};
    std::array<vk::DescriptorSetLayoutBinding, types.size()> bindings;
    for (u32 i = 0; i < types.size(); ++i)
        bindings[i] = {i, types[i], 1, vk::ShaderStageFlagBits::eCompute};
    set = Check<"create DOF descriptors">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params)};
    layout = Check<"create DOF pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*set,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range}));
    const std::array<std::span<const u32>, Count> codes{
        DEPTH_OF_FIELD_CAMERA_COMP, DEPTH_OF_FIELD_SNAPSHOT_COMP, DEPTH_OF_FIELD_KERNEL_COMP,
        DEPTH_OF_FIELD_PREPARE_COMP, DEPTH_OF_FIELD_GATHER_COMP, DEPTH_OF_FIELD_COMPOSITE_COMP,
        DEPTH_OF_FIELD_FOCUS_COMP, DEPTH_OF_FIELD_NEAR_TILES_COMP, DEPTH_OF_FIELD_NEAR_COMP};
    for (u32 i = 0; i < Count; ++i) {
        const auto module = CompileSPV(codes[i], device);
        ASSERT(module);
        pipelines[i] = Check<"create DOF pipeline">(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module, .pName = "main"}, .layout = *layout}));
        device.destroyShaderModule(module);
    }
    sampler = Check<"create DOF sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
    state = std::make_unique<VideoCore::Buffer>(
        instance, 0, 48, VideoCore::MemoryType::DeviceLocal, "DOF camera, kernel and autofocus");
}

DepthOfField::~DepthOfField() = default;

bool DepthOfField::Enabled() const {
    return enabled && BbSettings::Get().effects[BbSettings::EffectDof].load(
                          std::memory_order_relaxed);
}

void DepthOfField::Reset() {
    camera_ready = encoded = kernel_ready = false;
    source = {};
    source_address = blur_target = 0;
}

void DepthOfField::Fallback(const char* reason) {
    if (fallback_reported) return;
    fallback_reported = true;
    std::printf("Modern DOF: %s; retaining stock DOF for this view\n", reason);
}

DepthOfField::Target DepthOfField::Access(VideoCore::ImageId id, bool write) {
    auto& image = texture_cache.GetImage(id);
    if (image.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        image.info.resources.levels != 1 || image.info.resources.layers != 1) {
        Fallback("unsupported HDR target layout");
        return {};
    }
    const auto access = vk::AccessFlagBits2::eShaderRead |
                        (write ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone);
    if (scene_targets.Reduced() && scene_targets.EligibleScene(image)) {
        VideoCore::ImageViewInfo info;
        info.format = image.info.pixel_format;
        const auto proxy = scene_targets.Read(id, info, Compute, access);
        if (write && !(proxy.usage & vk::ImageUsageFlagBits::eStorage)) {
            Fallback("HDR proxy lacks storage support");
            return {};
        }
        return {proxy.view, scene_targets.Size().width, scene_targets.Size().height};
    }
    if (write && !(image.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        Fallback("HDR target lacks storage support");
        return {};
    }
    const auto device = instance.GetDevice();
    const auto view = Check<"create DOF HDR view">(device.createImageView({
        .image = image.GetImage(), .viewType = vk::ImageViewType::e2D,
        .format = image.info.pixel_format, .subresourceRange = ColorRange}));
    scheduler.DeferOperation([device, view] { device.destroyImageView(view); });
    runtime.Transit(&image, vk::ImageLayout::eGeneral, Compute, access);
    runtime.FlushBarriers();
    return {view, image.info.size.width, image.info.size.height};
}

void DepthOfField::EnsureImages(u32 w, u32 h) {
    if (width == w && height == h) return;
    for (Image* image : {&depth, &prepared, &half_depth, &blurred}) {
        if (image->image) {
            scheduler.DeferOperation(
                [old = std::move(image->image), view = std::move(image->view)]() mutable {
                    view.reset();
                    old.Destroy();
                });
        }
    }
    width = w;
    height = h;
    focus_reset = true;
    const auto device = instance.GetDevice();
    const auto create = [&](Image& image, u32 x, u32 y, vk::Format format) {
        image.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.image.Create({
            .imageType = vk::ImageType::e2D, .format = format, .extent = {x, y, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined});
        image.view = Check<"create DOF working view">(device.createImageViewUnique({
            .image = vk::Image(image.image), .viewType = vk::ImageViewType::e2D,
            .format = format, .subresourceRange = ColorRange}));
        scheduler.Record([target = vk::Image(image.image)](vk::CommandBuffer cmd) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = Compute, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target, .subresourceRange = ColorRange};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
    };
    create(depth, w, h, vk::Format::eR32Sfloat);
    create(prepared, (w + 1) / 2, (h + 1) / 2, vk::Format::eR16G16B16A16Sfloat);
    create(half_depth, (w + 1) / 2, (h + 1) / 2, vk::Format::eR32Sfloat);
    create(blurred, (w + 1) / 2, (h + 1) / 2, vk::Format::eR16G16B16A16Sfloat);
}

void DepthOfField::Dispatch(Stage stage, const Params& params, u32 w, u32 h,
                            std::span<const vk::WriteDescriptorSet> writes) {
    // Descriptor pointers are borrowed by the caller; record copies the pointed-to data.
    std::vector<vk::DescriptorImageInfo> images(writes.size());
    std::vector<vk::DescriptorBufferInfo> buffers(writes.size());
    std::vector<vk::WriteDescriptorSet> owned(writes.begin(), writes.end());
    for (size_t i = 0; i < writes.size(); ++i) {
        if (writes[i].pImageInfo) {
            images[i] = *writes[i].pImageInfo;
            owned[i].pImageInfo = &images[i];
        } else {
            buffers[i] = *writes[i].pBufferInfo;
            owned[i].pBufferInfo = &buffers[i];
        }
    }
    scheduler.Record(
        [params, w, h, images = std::move(images), buffers = std::move(buffers),
         writes = std::move(owned), pipeline = *pipelines[stage],
         pipeline_layout = *layout](vk::CommandBuffer cmd) {
            Barrier(cmd);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            auto descriptors = writes;
            for (size_t i = 0; i < descriptors.size(); ++i) {
                if (descriptors[i].pImageInfo)
                    descriptors[i].pImageInfo = &images[i];
                else
                    descriptors[i].pBufferInfo = &buffers[i];
            }
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, descriptors);
            cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                              0, sizeof(params), &params);
            cmd.dispatch(w, h, 1);
            Barrier(cmd);
        });
}

void DepthOfField::Camera(const vk::DescriptorBufferInfo& constants, u32 offset) {
    camera_ready = false;
    if (!Enabled()) return;
    if (!HasWords(constants, offset, 16)) {
        Fallback("unrecognized camera constants");
        return;
    }
    scheduler.EndRendering();
    const vk::DescriptorBufferInfo gpu_state{state->Handle(), 0, 48};
    const std::array writes{Write(0, constants), Write(2, gpu_state)};
    Dispatch(CaptureCamera, {offset / 4, 0, 0, 0}, 1, 1, writes);
    camera_ready = true;
}

void DepthOfField::Encode(VideoCore::ImageId id, const vk::DescriptorImageInfo& input_depth) {
    encoded = kernel_ready = false;
    if (!Enabled() || !camera_ready || !id || !input_depth.imageView) return;
    scheduler.EndRendering();
    const auto target = Access(id, false);
    if (!target.view || !target.width || !target.height) return;
    EnsureImages(target.width, target.height);
    const vk::DescriptorBufferInfo gpu_state{state->Handle(), 0, 48};
    const vk::DescriptorImageInfo di{*sampler, input_depth.imageView, input_depth.imageLayout};
    const vk::DescriptorImageInfo out{{}, *depth.view, vk::ImageLayout::eGeneral};
    const std::array writes{Write(2, gpu_state), Write(4, Sampled, di), Write(8, Storage, out)};
    Dispatch(Snapshot, {0, 0, width, height}, (width + 7) / 8, (height + 7) / 8, writes);
    auto& image = texture_cache.GetImage(id);
    source = id;
    source_uid = image.image_uid;
    source_address = image.info.guest_address;
    encoded = true;
}

void DepthOfField::Kernel(VAddr target, const vk::DescriptorBufferInfo& pixel, u32 pixel_offset,
                          const vk::DescriptorBufferInfo& vertex, u32 vertex_offset) {
    kernel_ready = false;
    if (!Enabled() || !encoded) return;
    if (!target || !HasWords(pixel, pixel_offset, 643) ||
        !HasWords(vertex, vertex_offset, 162)) {
        Fallback("unrecognized stock blur constants");
        return;
    }
    scheduler.EndRendering();
    const vk::DescriptorBufferInfo gpu_state{state->Handle(), 0, 48};
    const std::array writes{Write(0, pixel), Write(1, vertex), Write(2, gpu_state)};
    Dispatch(CaptureKernel, {pixel_offset / 4, vertex_offset / 4, width, height}, 1, 1, writes);
    blur_target = target;
    kernel_ready = true;
}

void DepthOfField::Composite(VideoCore::ImageId target_id, VAddr sharp_address, VAddr blur_address,
                             const vk::DescriptorImageInfo& sharp,
                             const vk::DescriptorBufferInfo& constants, u32 offset) {
    const bool ready = encoded && kernel_ready;
    encoded = kernel_ready = false;
    if (!Enabled() || !ready || !target_id) return;
    const auto* source_image = texture_cache.TryGetImage(source, source_uid);
    if (!source_image || sharp_address != source_address ||
        blur_address != blur_target || target_id == source || !sharp.imageView ||
        (source_image && texture_cache.GetImage(target_id).GetImage() == source_image->GetImage()) ||
        !HasWords(constants, offset, 78)) {
        Fallback("unrecognized DOF resource chain");
        return;
    }
    scheduler.EndRendering();
    const auto target = Access(target_id, true);
    if (!target.view) return;
    if (target.width != width || target.height != height) {
        Fallback("DOF source and output extents differ");
        return;
    }
    const vk::DescriptorBufferInfo gpu_state{state->Handle(), 0, 48};
    const vk::DescriptorImageInfo si{*sampler, sharp.imageView, sharp.imageLayout};
    const vk::DescriptorImageInfo di{*sampler, *depth.view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo pi{*sampler, *prepared.view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo hi{*sampler, *half_depth.view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo bi{*sampler, *blurred.view, vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out{{}, target.view, vk::ImageLayout::eGeneral};
    const float intensity = BbSettings::Get().graphics_dof_intensity.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - last_focus).count();
    const Params params{offset / 4, 0, width, height,
                        std::clamp(std::isfinite(intensity) ? intensity : 1.0f, 0.0f, 3.0f),
                        std::clamp(elapsed, 0.0f, 0.1f), u32(focus_reset || elapsed > 0.25f)};
    last_focus = now;
    focus_reset = false;
    const std::array focus_writes{Write(2, gpu_state), Write(4, Sampled, di)};
    Dispatch(Focus, params, 1, 1, focus_writes);
    const u32 half_w = (width + 1) / 2, half_h = (height + 1) / 2;
    const std::array prep_writes{Write(0, constants), Write(2, gpu_state),
        Write(3, Sampled, si), Write(4, Sampled, di), Write(9, Storage, pi),
        Write(10, Storage, hi)};
    Dispatch(Prepare, params, (half_w + 7) / 8, (half_h + 7) / 8, prep_writes);
    const std::array gather_writes{Write(0, constants), Write(2, gpu_state),
        Write(5, Sampled, pi), Write(6, Sampled, hi), Write(11, Storage, bi)};
    Dispatch(Gather, params, (half_w + 7) / 8, (half_h + 7) / 8, gather_writes);
    const std::array resolve_writes{Write(0, constants), Write(2, gpu_state),
        Write(3, Sampled, si), Write(4, Sampled, di), Write(6, Sampled, hi),
        Write(7, Sampled, bi), Write(12, Storage, out)};
    Dispatch(Resolve, params, (width + 7) / 8, (height + 7) / 8, resolve_writes);
    if (params.intensity > 0.0f) {
        const u32 tile_size = u32(std::ceil((10.0f / 1080.0f) * height * params.intensity)) + 1;
        const u32 tiles_w = (width + tile_size - 1) / tile_size;
        const u32 tiles_h = (height + tile_size - 1) / tile_size;
        if (!near_tiles.image || near_tile_size != tile_size ||
            near_tiles.image.image_ci.extent.width != tiles_w ||
            near_tiles.image.image_ci.extent.height != tiles_h) {
            if (near_tiles.image) {
                scheduler.DeferOperation(
                    [old = std::move(near_tiles.image), view = std::move(near_tiles.view)]() mutable {
                        view.reset();
                        old.Destroy();
                    });
            }
            near_tile_size = tile_size;
            const auto device = instance.GetDevice();
            near_tiles.image = VideoCore::UniqueImage(device, instance.GetAllocator());
            near_tiles.image.Create({
                .imageType = vk::ImageType::e2D, .format = vk::Format::eR32Sfloat,
                .extent = {tiles_w, tiles_h, 1}, .mipLevels = 1, .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined});
            near_tiles.view = Check<"create near DOF tiles">(device.createImageViewUnique({
                .image = vk::Image(near_tiles.image), .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR32Sfloat, .subresourceRange = ColorRange}));
            scheduler.Record([image = vk::Image(near_tiles.image)](vk::CommandBuffer cmd) {
                const vk::ImageMemoryBarrier2 b{
                    .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                    .dstStageMask = Compute, .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image, .subresourceRange = ColorRange};
                cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
            });
        }
        const vk::DescriptorImageInfo tiles{*sampler, *near_tiles.view, vk::ImageLayout::eGeneral};
        const std::array tile_writes{Write(2, gpu_state), Write(4, Sampled, di),
                                    Write(13, Storage, tiles)};
        Dispatch(NearTiles, params, tiles_w, tiles_h, tile_writes);
        const std::array near_writes{Write(2, gpu_state), Write(3, Sampled, si),
            Write(4, Sampled, di), Write(12, Storage, out), Write(14, Sampled, tiles)};
        Dispatch(Near, params, (width + 7) / 8, (height + 7) / 8, near_writes);
    }
}
} // namespace Vulkan
