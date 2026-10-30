// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "video_core/host_shaders/depth_of_field_camera_comp.h"
#include "video_core/host_shaders/depth_of_field_snapshot_comp.h"
#include "video_core/host_shaders/depth_of_field_kernel_comp.h"
#include "video_core/host_shaders/depth_of_field_prepare_comp.h"
#include "video_core/host_shaders/depth_of_field_gather_comp.h"
#include "video_core/host_shaders/depth_of_field_composite_comp.h"
#include "video_core/host_shaders/depth_of_field_focus_comp.h"
#include "video_core/host_shaders/depth_of_field_near_tiles_comp.h"
#include "video_core/host_shaders/depth_of_field_near_comp.h"

namespace {
#define CHECK(call) do { \
    const VkResult result = (call); \
    if (result != VK_SUCCESS) { \
        std::fprintf(stderr, "%s: Vulkan error %d\n", #call, int(result)); \
        std::exit(1); \
    } \
} while (false)

constexpr uint32_t W = 257, H = 145, HW = (W + 1) / 2, HH = (H + 1) / 2;
struct Params {
    uint32_t cb, vb, width, height;
    float intensity = 1.0f, delta_seconds = 1.0f / 60.0f;
    uint32_t reset_focus = 1;
};

uint16_t Half(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    uint32_t sign = (bits >> 16) & 0x8000;
    int exp = int((bits >> 23) & 255) - 112;
    uint32_t mantissa = bits & 0x7fffff;
    if (exp >= 31) return uint16_t(sign | 0x7c00 | (mantissa ? 0x200 : 0));
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);
        mantissa |= 0x800000;
        return uint16_t(sign | ((mantissa + (1u << (13 - exp))) >> (14 - exp)));
    }
    return uint16_t(sign | ((uint32_t(exp) << 10 | mantissa >> 13) + ((mantissa >> 12) & 1)));
}
float Float(uint16_t h) {
    int e = (h >> 10) & 31;
    float m = float(h & 1023), s = h & 0x8000 ? -1.0f : 1.0f;
    return s * (e == 0 ? std::ldexp(m, -24) : std::ldexp(1.0f + m / 1024, e - 15));
}

struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* data{}; VkDeviceSize size{}; };
struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    uint32_t width, height, bytes;
};
struct Pass { VkPipelineLayout layout{}; VkPipeline pipeline{}; VkDescriptorSet set{}; };

struct Test {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkSampler sampler{};
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    std::array<Pass, 9> passes{};
    const std::array<VkDescriptorType, 15> types{
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};

    uint32_t Memory(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        std::abort();
    }
    Test() {
        const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "depth-of-field-shader-test", .apiVersion = VK_API_VERSION_1_3};
        const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app};
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        uint32_t count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        uint32_t family = 0;
        for (auto gpu : devices) {
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
            std::vector<VkQueueFamilyProperties> families(n);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families.data());
            for (uint32_t i = 0; i < n; ++i) {
                if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    physical = gpu;
                    family = i;
                    break;
                }
            }
            if (physical) break;
        }
        assert(physical);
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::printf("DOF shader tests: %s\n", properties.deviceName);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        const float priority = 1;
        const VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const VkPhysicalDeviceFeatures features{.shaderStorageImageExtendedFormats = VK_TRUE};
        const VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .pEnabledFeatures = &features};
        CHECK(vkCreateDevice(physical, &di, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        const VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
        for (uint32_t i = 0; i < types.size(); ++i)
            bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo li{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout));
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 27}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 54},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 54}}};
        const VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 9, .poolSizeCount = uint32_t(sizes.size()), .pPoolSizes = sizes.data()};
        CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
        const VkSamplerCreateInfo si{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        CHECK(vkCreateSampler(device, &si, nullptr, &sampler));
        const std::array<std::span<const uint32_t>, 9> codes{
            DEPTH_OF_FIELD_CAMERA_COMP, DEPTH_OF_FIELD_SNAPSHOT_COMP, DEPTH_OF_FIELD_KERNEL_COMP,
            DEPTH_OF_FIELD_PREPARE_COMP, DEPTH_OF_FIELD_GATHER_COMP, DEPTH_OF_FIELD_COMPOSITE_COMP,
            DEPTH_OF_FIELD_FOCUS_COMP, DEPTH_OF_FIELD_NEAR_TILES_COMP, DEPTH_OF_FIELD_NEAR_COMP};
        for (size_t i = 0; i < passes.size(); ++i) {
            auto& pass = passes[i];
            const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
            const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1, .pSetLayouts = &set_layout,
                .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
            CHECK(vkCreatePipelineLayout(device, &layout, nullptr, &pass.layout));
            const VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = codes[i].size_bytes(), .pCode = codes[i].data()};
            VkShaderModule module;
            CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
            const VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
                .layout = pass.layout};
            CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pass.pipeline));
            vkDestroyShaderModule(device, module, nullptr);
            const VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
            CHECK(vkAllocateDescriptorSets(device, &alloc, &pass.set));
        }
    }
    ~Test() {
        CHECK(vkDeviceWaitIdle(device));
        for (const auto& p : passes) {
            vkDestroyPipeline(device, p.pipeline, nullptr);
            vkDestroyPipelineLayout(device, p.layout, nullptr);
        }
        for (const auto& image : images) {
            vkDestroyImageView(device, image.view, nullptr);
            vkDestroyImage(device, image.image, nullptr);
            vkFreeMemory(device, image.memory, nullptr);
        }
        for (const auto& buffer : buffers) {
            vkUnmapMemory(device, buffer.memory);
            vkDestroyBuffer(device, buffer.buffer, nullptr);
            vkFreeMemory(device, buffer.memory, nullptr);
        }
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyDescriptorPool(device, descriptors, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
    Buffer MakeBuffer(VkDeviceSize size) {
        Buffer buffer{.size = size};
        const VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(device, &ci, nullptr, &buffer.buffer));
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(device, buffer.buffer, &r);
        const VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = r.size, .memoryTypeIndex = Memory(r.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device, &ai, nullptr, &buffer.memory));
        CHECK(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0));
        CHECK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.data));
        std::memset(buffer.data, 0, size_t(size));
        buffers.push_back(buffer);
        return buffer;
    }
    Image MakeImage(uint32_t w, uint32_t h, bool color) {
        Image image{.width = w, .height = h, .bytes = color ? 8u : 4u};
        const auto format = color ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32_SFLOAT;
        const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateImage(device, &ci, nullptr, &image.image));
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(device, image.image, &r);
        const VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = r.size, .memoryTypeIndex = Memory(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &ai, nullptr, &image.memory));
        CHECK(vkBindImageMemory(device, image.image, image.memory, 0));
        const VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(device, &vi, nullptr, &image.view));
        images.push_back(image);
        return image;
    }
    void Begin() {
        CHECK(vkResetCommandBuffer(cmd, 0));
        const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
    }
    void Barrier() {
        const VkMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    void Submit() {
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
    }
    void Upload(const Buffer& staging, const Image& image, const void* data) {
        std::memcpy(staging.data, data, size_t(image.width) * image.height * image.bytes);
        Begin();
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1}};
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        Barrier();
        Submit();
    }
    void Bind(uint32_t stage, const std::array<Buffer, 3>& bs,
              const std::array<Image, 12>& im) {
        std::array<VkDescriptorBufferInfo, 3> bi;
        std::array<VkDescriptorImageInfo, 12> ii;
        std::array<VkWriteDescriptorSet, 15> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = passes[stage].set, .dstBinding = i, .descriptorCount = 1,
                .descriptorType = types[i]};
            if (i < 3) {
                bi[i] = {bs[i].buffer, 0, bs[i].size};
                writes[i].pBufferInfo = &bi[i];
            } else {
                ii[i - 3] = {types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? sampler : VK_NULL_HANDLE,
                             im[i - 3].view, VK_IMAGE_LAYOUT_GENERAL};
                writes[i].pImageInfo = &ii[i - 3];
            }
        }
        vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    }
    void Dispatch(uint32_t stage, Params params, uint32_t w, uint32_t h) {
        const auto& p = passes[stage];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &p.set, 0, nullptr);
        vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdDispatch(cmd, w, h, 1);
        Barrier();
    }
    std::vector<uint16_t> Download(const Buffer& staging, const Image& image) {
        Begin();
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1}};
        vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &copy);
        Barrier();
        Submit();
        std::vector<uint16_t> out(size_t(image.width) * image.height * image.bytes / 2);
        std::memcpy(out.data(), staging.data, out.size() * 2);
        return out;
    }
};
} // namespace

int main() {
    Test t;
    const auto cb = t.MakeBuffer(4112 + 16), vb = t.MakeBuffer(4112 + 16), state = t.MakeBuffer(48);
    const auto staging = t.MakeBuffer(W * H * 8);
    auto* constants = static_cast<float*>(cb.data);
    auto* vertex = static_cast<float*>(vb.data);
    auto* gpu = static_cast<float*>(state.data);
    // Nonzero offsets exercise the same alignment adjustment as live constant-ring bindings.
    constexpr uint32_t C = 3, V = 2;
    constants[C + 11] = 1;
    constants[C + 14] = -20;
    constants[C + 15] = 20.001f;
    constants[C + 76] = 1;
    for (uint32_t i = 0; i < 7; ++i) {
        const float angle = float(i - 1) * 1.04719755f;
        vertex[V + 136 + 4 * i] = i == 0 ? 0 : 3.0f * std::cos(angle) / W;
        vertex[V + 137 + 4 * i] = i == 0 ? 0 : 3.0f * std::sin(angle) / H;
        for (uint32_t channel = 0; channel < 4; ++channel)
            constants[C + 616 + 4 * i + channel] = 1.0f / 7;
    }
    const auto sharp = t.MakeImage(W, H, true), raw_depth = t.MakeImage(W, H, false);
    const auto depth = t.MakeImage(W, H, false), prep = t.MakeImage(HW, HH, true);
    const auto half_depth = t.MakeImage(HW, HH, false), blur = t.MakeImage(HW, HH, true);
    const auto output = t.MakeImage(W, H, true);
    const auto tiles = t.MakeImage(W, H, false);
    t.Begin();
    for (const auto& image : t.images) {
        const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(t.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    t.Submit();
    for (uint32_t i = 0; i < 9; ++i) {
        t.Bind(i, {cb, vb, state},
            {sharp, i == 1 ? raw_depth : depth, prep, half_depth, blur,
             depth, prep, half_depth, blur, output, tiles, tiles});
    }
    std::vector<uint16_t> source(W * H * 4), stock(source.size());
    std::vector<float> depths(W * H);
    const auto fill = [&](bool edge, float q) {
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                const uint32_t p = y * W + x;
                const bool foreground = edge && x < 24;
                source[4 * p] = Half(foreground ? 8 : 0);
                source[4 * p + 1] = Half(0);
                source[4 * p + 2] = Half(foreground ? 0 : 4);
                source[4 * p + 3] = Half(foreground ? 0 : q);
                for (uint32_t c = 0; c < 3; ++c) stock[4 * p + c] = source[4 * p + c];
                stock[4 * p + 3] = Half(0.125f + float(p % 7) * 0.125f);
                float z = foreground ? 2 : 100;
                depths[p] = (1.0f / z - constants[C + 15]) / constants[C + 14];
            }
        }
    };
    const auto run = [&](float intensity = 1.0f, bool reset = true, float dt = 1.0f / 60.0f,
                         bool enable_near = true) {
        t.Upload(staging, sharp, source.data());
        t.Upload(staging, output, stock.data());
        t.Upload(staging, raw_depth, depths.data());
        t.Begin();
        Params p{C, V, W, H, intensity, dt, uint32_t(reset)};
        t.Dispatch(0, p, 1, 1);
        t.Dispatch(1, p, (W + 7) / 8, (H + 7) / 8);
        t.Dispatch(2, p, 1, 1);
        t.Dispatch(6, p, 1, 1);
        t.Dispatch(3, p, (HW + 7) / 8, (HH + 7) / 8);
        t.Dispatch(4, p, (HW + 7) / 8, (HH + 7) / 8);
        t.Dispatch(5, p, (W + 7) / 8, (H + 7) / 8);
        if (intensity > 0 && enable_near) {
            uint32_t tile_size = uint32_t(std::ceil((10.0f / 1080) * H * intensity)) + 1;
            t.Dispatch(7, p, (W + tile_size - 1) / tile_size, (H + tile_size - 1) / tile_size);
            t.Dispatch(8, p, (W + 7) / 8, (H + 7) / 8);
        }
        t.Submit();
        return t.Download(staging, output);
    };
    fill(false, 2);
    auto out = run();
    assert(gpu[3] == 1 && gpu[7] == 1);
    const float expected = 6 * std::sqrt(3.0f / 7);
    assert(std::abs(gpu[0] * W - expected) < 0.001f);
    assert(std::abs(gpu[2] * H - expected) < 0.001f);
    for (uint32_t p = 0; p < W * H; ++p) {
        assert(out[4 * p + 3] == stock[4 * p + 3]);
        assert(Float(out[4 * p + 2]) == 4);
    }
    fill(true, 2);
    out = run();
    assert(out == stock); // Constant layers stay constant; no red foreground bleed into blue.
    for (uint32_t leaf_x : {24u, 25u}) {
        fill(false, 2);
        for (uint32_t y = 0; y < H; ++y) {
            const uint32_t leaf = y * W + leaf_x;
            source[4 * leaf] = Half(8);
            source[4 * leaf + 2] = Half(0);
            source[4 * leaf + 3] = Half(0);
            stock[4 * leaf] = Half(8);
            stock[4 * leaf + 2] = Half(0);
            depths[leaf] = (0.5f - constants[C + 15]) / constants[C + 14];
            // Make stock fallback distinguishable from a successful edge repair.
            stock[4 * (leaf + 1) + 2] = Half(0.25f);
        }
        out = run();
        for (uint32_t y = 0; y < H; ++y) {
            const uint32_t leaf = y * W + leaf_x;
            assert(out[4 * leaf] == Half(8) && out[4 * leaf + 2] == Half(0));
            assert(out[4 * (leaf + 1)] == Half(0));
            assert(Float(out[4 * (leaf + 1) + 2]) > 3.99f);
            assert(out[4 * (leaf + 1) + 3] == stock[4 * (leaf + 1) + 3]);
        }
    }
    for (float q : {0.0f, 0.75f, -2.0f}) {
        fill(false, q);
        for (uint32_t p = 0; p < W * H; ++p) stock[4 * p + 2] = Half(0.25f);
        assert(run() == stock); // Focused and unverified near paths are untouched.
    }
    fill(false, 2);
    for (uint32_t p = 0; p < W * H; ++p) stock[4 * p + 2] = Half(0.25f);
    constants[C + 616] = std::numeric_limits<float>::quiet_NaN();
    assert(run() == stock);
    assert(gpu[3] == 0);
    constants[C + 616] = 1.0f / 7;
    constants[C + 11] = 0;
    assert(run() == stock);
    constants[C + 11] = 1;
    constants[C + 76] = 0;
    assert(run() == stock);
    constants[C + 76] = 1;
    fill(false, 2);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t p = y * W + x;
            source[4 * p + 2] = Half(((x / 2 + y / 2) & 1) ? 4 : 0);
            stock[4 * p + 2] = source[4 * p + 2];
        }
    }
    out = run();
    double before = 0, after = 0;
    for (uint32_t p = 0; p < W * H; ++p) {
        before += std::abs(Float(stock[4 * p + 2]) - 2);
        after += std::abs(Float(out[4 * p + 2]) - 2);
        assert(out[4 * p + 3] == stock[4 * p + 3]);
    }
    assert(after < before * 0.5);
    const auto zero = run(0.0f);
    for (uint32_t p = 0; p < W * H; ++p) {
        for (uint32_t c = 0; c < 3; ++c)
            assert(zero[4 * p + c] == source[4 * p + c]);
        assert(zero[4 * p + 3] == stock[4 * p + 3]);
    }
    // Measure the impulse's spread, not just a changed output checksum.
    fill(false, 2);
    for (uint32_t p = 0; p < W * H; ++p)
        source[4 * p + 2] = stock[4 * p + 2] = Half(0);
    for (uint32_t y = 14; y < 18; ++y)
        for (uint32_t x = 30; x < 34; ++x)
            source[4 * (y * W + x) + 2] = stock[4 * (y * W + x) + 2] = Half(4);
    const auto moment = [&](float intensity) {
        const auto image = run(intensity);
        double mass = 0, second = 0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                const double value = Float(image[4 * (y * W + x) + 2]);
                mass += value;
                second += value * ((x - 31.5) * (x - 31.5) + (y - 15.5) * (y - 15.5));
            }
        }
        assert(mass > 0);
        return second / mass;
    };
    const double low = moment(0.5f), normal = moment(1.0f), high = moment(3.0f);
    assert(low < normal && normal < high);
    fill(false, -2);
    for (uint32_t p = 0; p < W * H; ++p) stock[4 * p + 2] = Half(0.0f);
    const auto reduced = run(0.5f);
    for (uint32_t p = 0; p < W * H; ++p)
        assert(reduced[4 * p + 2] == Half(2.0f));
    // Autofocus uses a robust upper-centre median, not a single pixel or the hunter below it.
    const auto set_distance = [&](float z) {
        std::fill(depths.begin(), depths.end(), (1.0f / z - constants[C + 15]) / constants[C + 14]);
    };
    fill(false, 0);
    set_distance(10);
    run();
    assert(gpu[9] == 1 && std::abs(1.0f / gpu[8] - 10) < 0.02f);
    depths[uint32_t(0.32f * H) * W + uint32_t(0.5f * W)] =
        (1.0f / 0.2f - constants[C + 15]) / constants[C + 14];
    run();
    assert(std::abs(1.0f / gpu[8] - 10) < 0.02f);
    for (uint32_t y = H / 2; y < H; ++y)
        for (uint32_t x = W / 3; x < 2 * W / 3; ++x)
            depths[y * W + x] = (2.0f - constants[C + 15]) / constants[C + 14];
    run();
    assert(std::abs(1.0f / gpu[8] - 10) < 0.02f);
    set_distance(4);
    run(1, false);
    assert(1.0f / gpu[8] > 4 && 1.0f / gpu[8] < 10);
    for (int i = 0; i < 90; ++i) run(1, false);
    assert(std::abs(1.0f / gpu[8] - 4) < 0.15f);
    set_distance(10);
    run();
    set_distance(4);
    for (int i = 0; i < 30; ++i) run(1, false, 1.0f / 30);
    const float thirty_hz = gpu[8];
    set_distance(10);
    run();
    set_distance(4);
    for (int i = 0; i < 60; ++i) run(1, false, 1.0f / 60);
    assert(std::abs(gpu[8] - thirty_hz) < 0.002f);
    std::fill(depths.begin(), depths.end(), 1.0f);
    run();
    assert(std::abs(1.0f / gpu[8] - 30) < 0.01f);
    // A close HDR checkerboard blurs despite a zero authored far curve.
    fill(false, 0);
    set_distance(10);
    for (uint32_t y = 8; y < 38; ++y) {
        for (uint32_t x = 20; x < 70; ++x) {
            const uint32_t p = y * W + x;
            source[4 * p] = stock[4 * p] = Half(((x + y) & 1) ? 8 : 0);
            source[4 * p + 2] = stock[4 * p + 2] = Half(0);
            depths[p] = (1.0f / 0.3f - constants[C + 15]) / constants[C + 14];
        }
    }
    out = run(3);
    before = after = 0;
    for (uint32_t y = 13; y < 33; ++y) {
        for (uint32_t x = 25; x < 65; ++x) {
            const uint32_t p = y * W + x;
            before += std::abs(Float(stock[4 * p]) - 4);
            after += std::abs(Float(out[4 * p]) - 4);
            assert(out[4 * p + 3] == stock[4 * p + 3]);
            assert(Float(out[4 * p + 2]) == 0);
        }
    }
    assert(after < before * 0.5);
    // A nearer sharp occluder cannot receive blur from the defocused patch behind it.
    for (uint32_t y = 8; y < 38; ++y) {
        const uint32_t p = y * W + 45;
        source[4 * p] = stock[4 * p] = Half(0);
        source[4 * p + 1] = stock[4 * p + 1] = Half(6);
        depths[p] = (1.0f / 0.15f - constants[C + 15]) / constants[C + 14];
    }
    out = run(3);
    for (uint32_t y = 13; y < 33; ++y) {
        const uint32_t p = y * W + 45;
        assert(Float(out[4 * p]) == 0);
        const float green = Float(out[4 * p + 1]);
        if (std::abs(green - 6) > 0.01f)
            std::fprintf(stderr, "Near occlusion: y=%u red=%g green=%g\n",
                         y, Float(out[4 * p]), green);
        assert(std::abs(green - 6) <= 0.01f);
        assert(out[4 * p + 3] == stock[4 * p + 3]);
    }
    // An in-focus surface remains unchanged when no near layer can cover it.
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 150; x < W; ++x) {
            const uint32_t p = y * W + x;
            assert(out[4 * p] == stock[4 * p]);
            assert(out[4 * p + 2] == stock[4 * p + 2]);
        }
    const auto near_zero = run(0);
    for (uint32_t p = 0; p < W * H; ++p) {
        for (uint32_t c = 0; c < 3; ++c) assert(near_zero[4 * p + c] == source[4 * p + c]);
        assert(near_zero[4 * p + 3] == stock[4 * p + 3]);
    }
    // Conservative tile dilation must not sharpen far bokeh outside foreground coverage.
    fill(false, 2);
    set_distance(10);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t p = y * W + x;
            source[4 * p + 2] = stock[4 * p + 2] = Half(((x + y) & 1) ? 4 : 0);
            if (x >= 20 && x < 70 && y >= 8 && y < 38) {
                source[4 * p] = stock[4 * p] = Half(8);
                source[4 * p + 2] = stock[4 * p + 2] = Half(0);
                source[4 * p + 3] = Half(0);
                depths[p] = (1.0f / 0.3f - constants[C + 15]) / constants[C + 14];
            }
        }
    }
    const auto far_only = run(3, true, 1.0f / 60, false);
    out = run(3);
    for (uint32_t y = 13; y < 33; ++y) {
        for (uint32_t x = 75; x < 85; ++x) {
            const uint32_t p = y * W + x;
            for (uint32_t c = 0; c < 4; ++c) assert(out[4 * p + c] == far_only[4 * p + c]);
        }
    }
    // Close-up head, shoulders and cloak stay sharp even at maximum intensity.
    fill(false, 0);
    set_distance(0.3f);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t p = y * W + x;
            source[4 * p] = stock[4 * p] = Half(((x + y) & 1) ? 8 : 0);
        }
    out = run(3);
    for (const auto& region : std::array<std::array<float, 4>, 3>{{
             {0.40f, 0.60f, 0.00f, 0.35f},
             {0.27f, 0.73f, 0.45f, 0.65f},
             {0.09f, 0.91f, 0.85f, 1.00f}}}) {
        for (uint32_t y = uint32_t(region[2] * H); y < uint32_t(region[3] * H); ++y)
            for (uint32_t x = uint32_t(region[0] * W); x < uint32_t(region[1] * W); ++x) {
                const uint32_t p = y * W + x;
                for (uint32_t c = 0; c < 4; ++c) assert(out[4 * p + c] == stock[4 * p + c]);
            }
    }
    before = after = 0;
    for (uint32_t y = 8; y < 28; ++y)
        for (uint32_t x = 8; x < 48; ++x) {
            const uint32_t p = y * W + x;
            before += std::abs(Float(stock[4 * p]) - 4);
            after += std::abs(Float(out[4 * p]) - 4);
        }
    assert(after < before * 0.5);
    constants[C + 11] = 0;
    assert(run() == stock);
    assert(gpu[9] == 0);
    std::printf("PASS: calibrated radius, odd extents, HDR/alpha, focused/near fallback, "
                "foreground rejection, subpixel foliage repair, invalid-layout fallback "
                "and bokeh filtering; autofocus median, time-based convergence, sky fallback, "
                "near-only blur, near occlusion, close-up subject protection and zero intensity\n");
}
