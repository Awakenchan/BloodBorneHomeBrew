// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/host_shaders/point_volumetric_comp.h"

namespace {
#define CHECK(call) do { if ((call) != VK_SUCCESS) { std::fprintf(stderr, "%s failed\n", #call); std::abort(); } } while (false)
constexpr unsigned W = 19, H = 11;
struct Push {
    unsigned offsets = 16 | (32 << 8), count = 1;
    float intensity = 0.2f;
    unsigned vertex_offset = 16, vertex_stride = 32, index_offset = 2, index_size = 2;
    int first_vertex = 1;
    std::array<float, 4> viewport{W / 2.0f, -H / 2.0f, W / 2.0f, H / 2.0f};
    std::array<int, 4> scissor{0, 0, W, H};
};
static_assert(sizeof(Push) == 64);
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* data{}; unsigned size{}; };
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct Test {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkDescriptorSetLayout set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkSampler sampler{};
    PFN_vkCmdPushDescriptorSetKHR descriptors{};
    std::vector<Buffer> buffers;
    std::vector<Image> images;

    unsigned Memory(unsigned bits, VkMemoryPropertyFlags flags) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    }
    Buffer MakeBuffer(unsigned size) {
        Buffer b{.size = size};
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(device, &info, nullptr, &b.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = Memory(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
        CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.data));
        std::memset(b.data, 0, size);
        buffers.push_back(b);
        return b;
    }
    Image MakeImage(VkFormat format) {
        Image i;
        VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {W, H, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateImage(device, &info, nullptr, &i.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = Memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &i.memory));
        CHECK(vkBindImageMemory(device, i.image, i.memory, 0));
        VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = i.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(device, &view, nullptr, &i.view));
        images.push_back(i);
        return i;
    }
    Test() {
        VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "point-volumetric-test", .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        unsigned count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        assert(count);
        gpu = devices[0];
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        unsigned families_count = 0, family = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(families_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, families.data());
        while (family < families_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        assert(family < families_count);
        const float priority = 1;
        VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const char* extension = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
            .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
        CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        descriptors = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        assert(descriptors);
        VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        const std::array<VkDescriptorType, 6> types{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (unsigned i = 0; i < bindings.size(); ++i)
            bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = unsigned(bindings.size()), .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &set));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
        VkShaderModule module;
        VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(POINT_VOLUMETRIC_COMP), .pCode = POINT_VOLUMETRIC_COMP};
        CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
        VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
        CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &pipeline));
        vkDestroyShaderModule(device, module, nullptr);
        VkSamplerCreateInfo sm{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        CHECK(vkCreateSampler(device, &sm, nullptr, &sampler));
    }
    void Barrier(VkImage image, VkImageLayout old_layout = VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
    ~Test() {
        vkDeviceWaitIdle(device);
        for (auto i : images) {
            vkDestroyImageView(device, i.view, nullptr);
            vkDestroyImage(device, i.image, nullptr);
            vkFreeMemory(device, i.memory, nullptr);
        }
        for (auto b : buffers) {
            vkUnmapMemory(device, b.memory);
            vkDestroyBuffer(device, b.buffer, nullptr);
            vkFreeMemory(device, b.memory, nullptr);
        }
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
}

int main() {
    Test t;
    const auto cb = t.MakeBuffer(256), lights = t.MakeBuffer(1024), vertices = t.MakeBuffer(1024);
    const auto indices = t.MakeBuffer(64), result = t.MakeBuffer(W * H * 8);
    const auto depth = t.MakeImage(VK_FORMAT_R32_SFLOAT), fog = t.MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT);
    auto* constants = static_cast<float*>(cb.data) + 4;
    for (unsigned i = 0; i < 4; ++i) constants[i * 5] = 1;

    auto* light = static_cast<float*>(lights.data) + 8;
    light[2] = 1; light[3] = 0.01f; light[4] = 1; light[5] = 0.5f; light[6] = 0.25f;
    Push push;
    std::array<float, 4> quad{0, 0, W, H};
    const auto upload_quad = [&] {
        const std::array<std::array<float, 2>, 4> points{{
            {quad[0], quad[1]}, {quad[2], quad[1]}, {quad[0], quad[3]}, {quad[2], quad[3]}}};
        for (unsigned i = 0; i < 4; ++i) {
            auto* p = reinterpret_cast<float*>(static_cast<unsigned char*>(vertices.data) +
                push.vertex_offset + (i + push.first_vertex) * push.vertex_stride);
            p[0] = (points[i][0] - push.viewport[2]) / push.viewport[0];
            p[1] = (points[i][1] - push.viewport[3]) / push.viewport[1];
            p[2] = 0; p[3] = 1;
            auto* at = static_cast<unsigned char*>(indices.data) + push.index_offset + i * push.index_size;
            if (push.index_size == 2) { const uint16_t index = i; std::memcpy(at, &index, 2); }
            if (push.index_size == 4) std::memcpy(at, &i, 4);
        }
    };
    bool initialized = false;
    const auto run = [&](unsigned tiles, bool quad_tiles = false) {
        CHECK(vkResetCommandBuffer(t.cmd, 0));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(t.cmd, &begin));
        t.Barrier(depth.image, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
        t.Barrier(fog.image, initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue scene{{0.5f, 0, 0, 0}}, zero{};
        vkCmdClearColorImage(t.cmd, depth.image, VK_IMAGE_LAYOUT_GENERAL, &scene, 1, &range);
        vkCmdClearColorImage(t.cmd, fog.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        t.Barrier(depth.image); t.Barrier(fog.image);
        std::array<VkDescriptorBufferInfo, 4> buffers{{
            {cb.buffer, 0, cb.size}, {lights.buffer, 0, lights.size},
            {vertices.buffer, 0, vertices.size}, {indices.buffer, 0, indices.size}}};
        const VkDescriptorImageInfo d{t.sampler, depth.view, VK_IMAGE_LAYOUT_GENERAL},
            o{{}, fog.view, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (unsigned i = 0; i < 6; ++i) {
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                .descriptorCount = 1, .descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                    i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
            if (i == 2 || i == 3) writes[i].pImageInfo = i == 2 ? &d : &o;
            else writes[i].pBufferInfo = &buffers[i < 2 ? i : i - 2];
        }
        vkCmdBindPipeline(t.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.pipeline);
        t.descriptors(t.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t.layout, 0, 6, writes.data());
        for (unsigned tile = 0; tile < tiles; ++tile) {
            auto p = push;
            if (quad_tiles) {
                std::vector<unsigned char> data(vertices.size);
                std::memcpy(data.data(), vertices.data, vertices.size);
                const float lo = float(tile * W / tiles), hi = float((tile + 1) * W / tiles);
                for (unsigned i = 0; i < 4; ++i) {
                    const float x = ((i & 1 ? hi : lo) - p.viewport[2]) / p.viewport[0];
                    std::memcpy(data.data() + p.vertex_offset +
                        (i + p.first_vertex) * p.vertex_stride, &x, sizeof x);
                }
                vkCmdUpdateBuffer(t.cmd, vertices.buffer, 0, vertices.size, data.data());
                VkMemoryBarrier updated{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
                vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &updated, 0, nullptr, 0, nullptr);
            } else if (tiles > 1) {
                p.scissor[0] = tile * W / tiles;
                p.scissor[2] = (tile + 1) * W / tiles;
            }
            vkCmdPushConstants(t.cmd, t.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(t.cmd, (W + 7) / 8, (H + 7) / 8, 1);
            t.Barrier(fog.image);
            VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
            vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &read, 0, nullptr, 0, nullptr);
        }
        VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
        vkCmdCopyImageToBuffer(t.cmd, fog.image, VK_IMAGE_LAYOUT_GENERAL, result.buffer, 1, &copy);
        VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(t.cmd));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &t.cmd};
        CHECK(vkQueueSubmit(t.queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(t.queue));
        initialized = true;
        const auto* pixels = static_cast<const uint16_t*>(result.data);
        return std::vector<uint16_t>(pixels, pixels + W * H * 4);
    };
    upload_quad();
    const auto full = run(1);
    assert(std::ranges::any_of(full, [](auto v) { return v != 0; }));
    for (unsigned tiles : {2, 3, 7, 19}) {
        upload_quad();
        assert(run(tiles) == full);
        assert(run(tiles, true) == full);
    }
    push.index_size = 4; push.index_offset = 4;
    upload_quad();
    assert(run(1) == full);
    push.index_size = 0;
    upload_quad();
    assert(run(1) == full);
    push.scissor = {3, 2, 17, 9};
    quad = {5, 1, 14, 8};
    upload_quad();
    const auto clipped = run(1);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x)
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned at = (y * W + x) * 4 + c;
                assert(clipped[at] == (x >= 5 && x < 14 && y >= 2 && y < 8 ? full[at] : 0));
            }
    push.intensity = 0;
    const auto off = run(1);
    assert(std::ranges::all_of(off, [](auto v) { return v == 0; }));
    std::puts("Point volumetrics: tiled/fullscreen output identical; quad/scissor coverage, "
              "16/32-bit indices, offsets, non-indexed quads and zero intensity passed");
}
