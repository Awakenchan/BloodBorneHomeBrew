// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include "video_core/host_shaders/native_motion_blur_test_comp.h"

using u32 = uint32_t;

int main() {
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app{.pApplicationName = "motion-blur-shader-test",
        .apiVersion = VK_API_VERSION_1_3};
    auto instance = vk::createInstanceUnique({.pApplicationInfo = &app},nullptr,d).value;
    d.init(*instance);
    auto gpus = instance->enumeratePhysicalDevices(d).value;
    assert(!gpus.empty());
    const auto physical = gpus[0];
    auto families = physical.getQueueFamilyProperties(d);
    u32 family = 0;
    while (family < families.size() &&
           !(families[family].queueFlags & vk::QueueFlagBits::eCompute)) ++family;
    assert(family < families.size());
    const float priority = 1;
    const vk::DeviceQueueCreateInfo qi{.queueFamilyIndex = family, .queueCount = 1,
        .pQueuePriorities = &priority};
    const char* extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    const vk::PhysicalDeviceVulkan13Features features{.synchronization2 = true};
    auto owned_device = physical.createDeviceUnique({.pNext = &features, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qi, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &extension},nullptr,d).value;
    const auto device = *owned_device;
    d.init(device);
    const auto queue = device.getQueue(family,0,d);
    auto pool = device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = family},nullptr,d).value;
    const auto command = device.allocateCommandBuffers({.commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1},d).value[0];
    const auto memory_types = physical.getMemoryProperties(d);
    const auto memory_type = [&](u32 mask, vk::MemoryPropertyFlags flags) {
        for (u32 i = 0; i < memory_types.memoryTypeCount; ++i)
            if ((mask & (1u << i)) &&
                (memory_types.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    };
    constexpr u32 W = 129, H = 17, N = W * H, Bytes = N * 16;
    using Pixel = std::array<float, 4>;
    std::array<vk::UniqueDeviceMemory, 3> image_memory;
    std::array<vk::UniqueImage, 3> images;
    std::array<vk::UniqueImageView, 3> views;
    for (u32 i = 0; i < 3; ++i) {
        images[i] = device.createImageUnique({.imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat, .extent = {W,H,1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst},nullptr,d).value;
        const auto requirements = device.getImageMemoryRequirements(*images[i],d);
        image_memory[i] = device.allocateMemoryUnique({.allocationSize = requirements.size,
            .memoryTypeIndex = memory_type(requirements.memoryTypeBits,
                                           vk::MemoryPropertyFlagBits::eDeviceLocal)},nullptr,d).value;
        assert(device.bindImageMemory(*images[i],*image_memory[i],0,d) == vk::Result::eSuccess);
        views[i] = device.createImageViewUnique({.image = *images[i],
            .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR32G32B32A32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}}, nullptr,d).value;
    }
    const std::array<vk::DescriptorSetLayoutBinding,4> bindings{{
        {0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute},
        {1,vk::DescriptorType::eCombinedImageSampler,1,vk::ShaderStageFlagBits::eCompute},
        {2,vk::DescriptorType::eCombinedImageSampler,1,vk::ShaderStageFlagBits::eCompute},
        {3,vk::DescriptorType::eStorageImage,1,vk::ShaderStageFlagBits::eCompute}}};
    auto descriptors = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data()},nullptr,d).value;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,4};
    auto layout = device.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*descriptors, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push},nullptr,d).value;
    auto module = device.createShaderModuleUnique({.codeSize = sizeof(NATIVE_MOTION_BLUR_TEST_COMP),
        .pCode = NATIVE_MOTION_BLUR_TEST_COMP},nullptr,d).value;
    auto pipeline = device.createComputePipelineUnique({},{
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
        .layout = *layout},nullptr,d).value;
    auto sampler = device.createSamplerUnique({.magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear, .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge},nullptr,d).value;
    std::array<vk::UniqueDeviceMemory, 2> buffer_memory;
    std::array<vk::UniqueBuffer, 2> buffers;
    std::array<void*, 2> mapped;
    for (u32 i = 0; i < 2; ++i) {
        buffers[i] = device.createBufferUnique({
            .size = i == 0 ? Bytes * 2 : 1024 * sizeof(float),
            .usage = i == 0 ? vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst :
                             vk::BufferUsageFlagBits::eStorageBuffer},nullptr,d).value;
        const auto r = device.getBufferMemoryRequirements(*buffers[i],d);
        buffer_memory[i] = device.allocateMemoryUnique({.allocationSize = r.size,
            .memoryTypeIndex = memory_type(r.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)},nullptr,d).value;
        assert(device.bindBufferMemory(*buffers[i],*buffer_memory[i],0,d) == vk::Result::eSuccess);
        mapped[i] = device.mapMemory(*buffer_memory[i],0,VK_WHOLE_SIZE,{},d).value;
    }
    const auto staging = *buffers[0], constants = *buffers[1];
    auto* cb = static_cast<float*>(mapped[1]);
    std::fill_n(cb, 1024, 0.0f);
    cb[744] = cb[745] = 2; cb[746] = cb[747] = -1;
    cb[748] = 8.0f/W; cb[750] = -4.0f/W;
    cb[754] = 0.001f; cb[756] = 0.15f; cb[757] = -0.01f;
    cb[760] = cb[761] = 2; cb[762] = cb[763] = -1;
    std::fill_n(cb + 636, 4, 1.0f);
    std::vector<Pixel> color(N, {4,2,1,0.5f}), velocity(N, {1,0.5f,0,0});
    bool initialized = false;
    const auto barrier = [&](vk::CommandBuffer cmd, vk::PipelineStageFlags2 src,
                             vk::AccessFlags2 access, vk::PipelineStageFlags2 dst,
                             vk::AccessFlags2 dst_access) {
        const vk::MemoryBarrier2 b{.srcStageMask = src, .srcAccessMask = access,
            .dstStageMask = dst, .dstAccessMask = dst_access};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &b},d);
    };
    const auto run = [&](u32 quality) {
        std::memcpy(mapped[0],color.data(),Bytes);
        std::memcpy(static_cast<char*>(mapped[0]) + Bytes,velocity.data(),Bytes);
        auto cmd = command;
        assert(cmd.reset({},d) == vk::Result::eSuccess);
        const vk::CommandBufferBeginInfo begin{};
        assert(cmd.begin(begin,d) == vk::Result::eSuccess);
        if (!initialized) {
            std::array<vk::ImageMemoryBarrier2,3> transitions{};
            for (u32 i = 0; i < 3; ++i) transitions[i] = {
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eGeneral,
                .image = *images[i],
                .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 3,
                                 .pImageMemoryBarriers = transitions.data()},d);
            initialized = true;
        }
        barrier(cmd,vk::PipelineStageFlagBits2::eAllCommands,vk::AccessFlagBits2::eMemoryRead |
            vk::AccessFlagBits2::eMemoryWrite,vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferWrite);
        for (u32 i = 0; i < 2; ++i) {
            const vk::BufferImageCopy copy{.bufferOffset = i * Bytes,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
                .imageExtent = {W,H,1}};
            cmd.copyBufferToImage(staging,*images[i],vk::ImageLayout::eGeneral,copy,d);
        }
        barrier(cmd,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderRead);
        const vk::DescriptorBufferInfo buffer{constants,0,1024 * sizeof(float)};
        std::array<vk::DescriptorImageInfo,3> infos;
        std::array<vk::WriteDescriptorSet,4> writes{};
        writes[0] = {.dstBinding = 0, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffer};
        for (u32 i = 0; i < 3; ++i) {
            infos[i] = {.sampler = i < 2 ? *sampler : vk::Sampler{}, .imageView = *views[i],
                .imageLayout = vk::ImageLayout::eGeneral};
            writes[i+1] = {.dstBinding = i+1, .descriptorCount = 1,
                .descriptorType = bindings[i+1].descriptorType, .pImageInfo = &infos[i]};
        }
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline,d);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes,d);
        cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,4,&quality,d);
        cmd.dispatch((W+7)/8,(H+7)/8,1,d);
        barrier(cmd,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        const vk::BufferImageCopy copy{.imageSubresource = {vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageExtent = {W,H,1}};
        cmd.copyImageToBuffer(*images[2],vk::ImageLayout::eGeneral,staging,copy,d);
        barrier(cmd,vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead);
        assert(cmd.end(d) == vk::Result::eSuccess);
        const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
        assert(queue.submit(submit,{},d) == vk::Result::eSuccess);
        assert(queue.waitIdle(d) == vk::Result::eSuccess);
        std::vector<Pixel> out(N);
        std::memcpy(out.data(),mapped[0],Bytes);
        for (const auto& pixel : out) for (float v : pixel) assert(std::isfinite(v));
        return out;
    };
    auto out = run(1);
    for (const auto& p : out) for (u32 c = 0; c < 4; ++c)
        assert(std::abs(p[c] - color[0][c]) < 0.001f);

    for (float gain : {13.44f, 1.0f}) {
        std::fill_n(cb + 636,4,gain);
        out = run(1);
        for (const auto& p : out) for (u32 c = 0; c < 4; ++c)
            assert(std::abs(p[c] - color[0][c] * gain) < 0.001f);
    }
    for (auto& v : velocity) v = {0.5f,0.5f,0,0};
    assert(run(1) == color);
    for (auto& v : velocity) v = {1,0.5f,0,0};

    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x)
        color[y*W+x] = {x == W/2 ? 8.0f : 0.0f,0,0,0.5f};
    const auto stock = run(0);
    const auto high = run(1);
    double stock_error = 0, high_error = 0;
    for (u32 x = 16; x < W-16; ++x) {
        const float expected = std::abs(int(x) - int(W/2)) <= 8 ? 8.0f/17 : 0;
        stock_error += std::abs(stock[(H/2)*W+x][0] - expected);
        high_error += std::abs(high[(H/2)*W+x][0] - expected);
        assert(std::abs(high[(H/2)*W+x][3] - 0.5f) < 0.001f);
    }
    assert(high_error < stock_error * 0.05);

    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x) {
        color[y*W+x] = x < W/2 ? Pixel{8,0,0,0.1f} : Pixel{0,4,0,0.9f};
        velocity[y*W+x] = x < W/2 ? Pixel{0,0.5f,0,0} : Pixel{1,0.5f,0,0};
    }
    out = run(1);
    for (u32 x = W/2+1; x < W/2+8; ++x) {
        assert(std::abs(out[(H/2)*W+x][0]) < 0.001f);
        assert(std::abs(out[(H/2)*W+x][1] - 4) < 0.001f);
    }

    cb[748] = 1.0f/W; cb[750] = -0.5f/W;
    assert(run(1) == run(0));
    std::printf("PASS: native motion blur HDR/gains, zero/short motion, depth/velocity rejection, "
                "odd extents and frame edges; long-trail error stock=%g high=%g\n",
                stock_error,high_error);
    for (const auto& memory : buffer_memory) device.unmapMemory(*memory,d);
}
