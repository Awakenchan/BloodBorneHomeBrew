// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_sky_seam.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/sky_seam_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
SkySeam::SkySeam(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* setting = std::getenv("BB_SKY_SEAM_FIX");
    enabled = !(setting && setting[0] == '0' && setting[1] == '\0');
    if (!enabled) return;
    if (!instance.IsVertexInputDynamicState()) {
        enabled = false;
        std::printf("Sky seam: dynamic vertex input unavailable; original UVs retained\n");
        return;
    }
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute};
    set = Check<"create sky seam descriptors">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, 20};
    layout = Check<"create sky seam layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &*set,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range}));
    const auto module = CompileSPV(SKY_SEAM_COMP, device);
    pipeline = Check<"create sky seam pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
            .layout = *layout}));
    device.destroyShaderModule(module);
    output = std::make_unique<VideoCore::Buffer>(
        instance, 0, 4098 * 4, VideoCore::MemoryType::DeviceLocal, "Verified Dream sky UVs");
}
SkySeam::~SkySeam() = default;
vk::DescriptorBufferInfo SkySeam::VerificationBuffer() const {
    return {output->Handle(), 0, (capture_count + 2) * 4};
}

vk::Buffer SkySeam::Correct(vk::DescriptorBufferInfo positions, vk::DescriptorBufferInfo uv,
                          u32 position_stride, u32 uv_stride, u32 count) {
    if (!enabled) return {};
    if (count < 485 || count > 4096 || position_stride < 12 || uv_stride < 4 ||
        positions.range < 484 * u64(position_stride) + 12 ||
        uv.range < (count - 1) * u64(uv_stride) + 4) {
        std::printf("Sky seam: invalid vertex range; original UVs retained\n");
        return {};
    }
    const u64 alignment = instance.StorageMinAlignment();
    const u32 po = u32(positions.offset % alignment);
    const u32 uo = u32(uv.offset % alignment);
    if (po % 4 || uo % 4 || position_stride % 4 || uv_stride % 4) {
        std::printf("Sky seam: unaligned vertex streams; original UVs retained\n");
        return {};
    }
    positions.offset -= po;
    positions.range += po;
    uv.offset -= uo;
    uv.range += uo;
    const std::array<u32, 5> params{po / 4, uo / 4, position_stride / 4, uv_stride / 4, count};
    const std::array<vk::DescriptorBufferInfo, 3> buffers{
        positions, uv, vk::DescriptorBufferInfo{output->Handle(), 0, (count + 2) * 4}};
    capture_count = count;
    scheduler.EndRendering();
    scheduler.Record(
        [buffers, params, pipe = *pipeline, pl = *layout](vk::CommandBuffer cmd) {
            // Also orders reuse of the owned UV stream after the previous vertex fetch.
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            std::array<vk::WriteDescriptorSet, 3> writes{};
            for (u32 i = 0; i < writes.size(); ++i)
                writes[i] = {.dstBinding = i, .descriptorCount = 1,
                             .descriptorType = vk::DescriptorType::eStorageBuffer,
                             .pBufferInfo = &buffers[i]};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipe);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pl, 0, writes);
            cmd.pushConstants(pl, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params.data());
            cmd.dispatch(1, 1, 1);
            const vk::MemoryBarrier2 after{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput,
                .dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        });
    if (!reported) {
        reported = true;
        std::printf("Sky seam: GPU geometry/UV validation active; nonmatching streams retain original UVs\n");
    }
    return output->Handle();
}

u32 SkySeam::CaptureVerification() {
    VideoCore::Buffer readback(
        instance, 0, 8, VideoCore::MemoryType::HostCached,
        "Sky seam capture verification");
    scheduler.Record([src = output->Handle(), dst = readback.Handle(), count = capture_count]
                     (vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        const vk::BufferCopy copy{count * 4, 0, 8};
        cmd.copyBuffer(src, dst, copy);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    // Explicit frame captures already synchronize; normal rendering never reads this back.
    scheduler.Finish();
    readback.Invalidate(0, readback.SizeBytes());
    u32 verified;
    std::memcpy(&verified, readback.mapped_data.data(), sizeof verified);
    u32 hash;
    std::memcpy(&hash, readback.mapped_data.data() + 4, sizeof hash);
    std::printf("Sky seam capture: verified %u, geometry/UV hash %08x\n", verified, hash);
    return verified;
}
}
