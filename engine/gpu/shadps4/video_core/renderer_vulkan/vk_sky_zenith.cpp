// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_sky_zenith.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include "video_core/host_shaders/sky_zenith_validate_comp.h"
#include "video_core/host_shaders/sky_zenith_horizontal_comp.h"
#include "video_core/host_shaders/sky_zenith_vertical_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
namespace {
constexpr vk::ImageSubresourceRange AllMips{vk::ImageAspectFlagBits::eColor, 0, 12, 0, 1};
void ComputeBarrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}
}

SkyZenith::SkyZenith(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* setting = std::getenv("BB_SKY_ZENITH_FIX");
    enabled = setting && setting[0] == '1' && setting[1] == '\0';
    if (!enabled) return;
    const char* isolation = std::getenv("BB_SKY_ZENITH_ISOLATE");
    const bool isolate = isolation && isolation[0] == '1' && isolation[1] == '\0';
    mode = isolate ? 2u : 1u;
    if (const char* file = std::getenv("BB_SKY_ZENITH_MODE_FILE")) mode_file = file;
    const auto device = instance.GetDevice();
    sampler = Check<"create sky zenith sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge, .maxLod = 11.0f}));
    const std::array<std::span<const u32>, 3> codes{
        SKY_ZENITH_VALIDATE_COMP, SKY_ZENITH_HORIZONTAL_COMP, SKY_ZENITH_VERTICAL_COMP};
    const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
    for (u32 i = 0; i < passes.size(); ++i) {
        auto& pass = passes[i];
        pass.set = Check<"create sky zenith descriptors">(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 16};
        pass.layout = Check<"create sky zenith layout">(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*pass.set,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        const auto module = CompileSPV(codes[i], device);
        pass.pipeline = Check<"create sky zenith pipeline">(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                .layout = *pass.layout}));
        device.destroyShaderModule(module);
    }
    validation = std::make_unique<VideoCore::Buffer>(
        instance, 0, 4, VideoCore::MemoryType::DeviceLocal, "Dream panorama validation");
    horizontal = VideoCore::UniqueImage(device, instance.GetAllocator());
    result = VideoCore::UniqueImage(device, instance.GetAllocator());
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D, .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {2048, 512, 1}, .mipLevels = 12, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined};
    horizontal.Create(info);
    result.Create(info);
    for (u32 mip = 0; mip < 12; ++mip) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1};
        horizontal_views[mip] = Check<"create sky zenith horizontal view">(
            device.createImageViewUnique({.image = vk::Image(horizontal),
                .viewType = vk::ImageViewType::e2D, .format = info.format, .subresourceRange = range}));
        result_views[mip] = Check<"create sky zenith result view">(
            device.createImageViewUnique({.image = vk::Image(result),
                .viewType = vk::ImageViewType::e2D, .format = info.format, .subresourceRange = range}));
    }
    result_view = Check<"create sky zenith sampled view">(device.createImageViewUnique({
        .image = vk::Image(result), .viewType = vk::ImageViewType::e2D,
        .format = info.format, .subresourceRange = AllMips}));
}
SkyZenith::~SkyZenith() = default;

vk::ImageView SkyZenith::Filter(vk::DescriptorImageInfo source,
                              vk::DescriptorBufferInfo geometry, u32 count) {
    if (!enabled || !source.imageView || !geometry.buffer) return {};
    if (!mode_file.empty() && polls++ % 30 == 0) {
        if (FILE* file = std::fopen(mode_file.c_str(), "r")) {
            u32 next;
            float next_strength = 0.4f;
            char extra;
            char text[128]{};
            const bool read = std::fgets(text, sizeof(text), file) != nullptr;
            const int fields = read ? std::sscanf(text, "%u %f %c", &next, &next_strength, &extra) : 0;
            const bool valid_fields = fields == 2 ||
                (fields == 1 && std::sscanf(text, "%u %c", &next, &extra) == 1);
            if (valid_fields && next <= 3 && std::isfinite(next_strength) &&
                next_strength >= 0.0f && next_strength <= 1.0f) {
                if (next != mode || next_strength != strength)
                    std::printf("Sky zenith diagnostic mode %u, strength %.2f\n", next, next_strength);
                mode = next;
                strength = next_strength;
            } else {
                std::printf("Sky zenith: invalid diagnostic mode; retaining mode %u\n", mode);
            }
            std::fclose(file);
        } else if (errno != ENOENT) {
            std::printf("Sky zenith: diagnostic mode file read failed (%d)\n", errno);
        }
    }
    if (mode == 0) return {};
    source.sampler = *sampler;
    scheduler.EndRendering();
    if (!initialized) {
        initialized = true;
        scheduler.Record([h = vk::Image(horizontal), r = vk::Image(result)](vk::CommandBuffer cmd) {
            std::array<vk::ImageMemoryBarrier2, 2> barriers{};
            for (u32 i = 0; i < 2; ++i)
                barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = i ? r : h, .subresourceRange = AllMips};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers.data()});
        });
    }
    for (u32 step = 0; step < 25; ++step) {
        const u32 pass_index = step == 0 ? 0 : (step % 2 ? 1 : 2);
        const u32 mip = step == 0 ? 0 : (step - 1) / 2;
        const auto& pass = passes[pass_index];
        const std::array<vk::DescriptorImageInfo, 3> images{source,
            vk::DescriptorImageInfo{{}, *horizontal_views[mip], vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{{}, *result_views[mip], vk::ImageLayout::eGeneral}};
        const std::array<vk::DescriptorBufferInfo, 2> buffers{
            geometry, vk::DescriptorBufferInfo{validation->Handle(), 0, 4}};
        struct Params { u32 mip, count, mode; float strength; };
        const Params params{mip, count, mode == 2 ? 1u : mode == 3 ? 3u : 0u, strength};
        scheduler.Record([images, buffers, params, pass_index,
                          pipe = *pass.pipeline, layout = *pass.layout](vk::CommandBuffer cmd) {
            ComputeBarrier(cmd);
            std::array<vk::WriteDescriptorSet, 5> writes{};
            for (u32 i = 0; i < 5; ++i) {
                writes[i] = {.dstBinding = i, .descriptorCount = 1,
                    .descriptorType = i == 0 ? vk::DescriptorType::eCombinedImageSampler :
                                      i < 3 ? vk::DescriptorType::eStorageImage :
                                              vk::DescriptorType::eStorageBuffer,
                    .pImageInfo = i < 3 ? &images[i] : nullptr,
                    .pBufferInfo = i >= 3 ? &buffers[i - 3] : nullptr};
            }
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipe);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            const u32 width = std::max(1u, 2048u >> params.mip);
            const u32 height = std::max(1u, 512u >> params.mip);
            cmd.dispatch(pass_index == 0 ? 1u : (width + 7) / 8,
                         pass_index == 0 ? 1u : (height + 7) / 8, 1);
        });
    }
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    return *result_view;
}

u32 SkyZenith::CaptureVerification() {
    VideoCore::Buffer readback(instance, 0, 4, VideoCore::MemoryType::HostCached,
                               "Sky zenith capture verification");
    scheduler.Record([src = validation->Handle(), dst = readback.Handle()](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmd.copyBuffer(src, dst, vk::BufferCopy{0, 0, 4});
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    scheduler.Finish();
    readback.Invalidate(0, 4);
    u32 verified;
    std::memcpy(&verified, readback.mapped_data.data(), sizeof(verified));
    return verified;
}
}
