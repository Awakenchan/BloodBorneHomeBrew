// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>
#include "video_core/host_shaders/sky_zenith_validate_comp.h"
#include "video_core/host_shaders/sky_zenith_horizontal_comp.h"
#include "video_core/host_shaders/sky_zenith_vertical_comp.h"

#define CHECK(call) do { if ((call) != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed\n", #call); std::abort(); } } while (false)

namespace {
constexpr unsigned W = 2048, H = 512, InputBytes = W * H * 4, OutputBytes = W * H * 8;
constexpr unsigned GeometryOffset = InputBytes + OutputBytes;
constexpr unsigned ValidationOffset = GeometryOffset + 8192;
float Half(unsigned value) {
    const unsigned exponent = (value >> 10) & 31u, mantissa = value & 1023u;
    assert(!(value & 32768u) && exponent < 31);
    return exponent ? std::ldexp(1.0f + float(mantissa) / 1024.0f, int(exponent) - 15) :
                      std::ldexp(float(mantissa), -24);
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    std::vector<unsigned char> source(InputBytes);
    std::ifstream file(argv[1], std::ios::binary);
    file.read(reinterpret_cast<char*>(source.data()), source.size());
    assert(file.gcount() == InputBytes);
    VkInstance instance;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "sky-zenith-test", .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ci, nullptr, &instance));
    unsigned device_count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &device_count, nullptr));
    assert(device_count);
    std::vector<VkPhysicalDevice> devices(device_count);
    CHECK(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()));
    const auto gpu = devices[0];
    unsigned family_count = 0, family = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    assert(family < family_count);
    const float priority = 1;
    const VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    VkDevice device;
    CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    const auto allocate = [&](VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
        unsigned type = 0;
        while (type < properties.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
               (properties.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
        assert(type < properties.memoryTypeCount);
        const VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = type};
        VkDeviceMemory memory;
        CHECK(vkAllocateMemory(device, &ai, nullptr, &memory));
        return memory;
    };
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ValidationOffset + 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bi, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const auto memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    void* mapped;
    CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    std::array<VkImage, 3> images{};
    std::array<VkImageView, 3> views{};
    std::array<VkDeviceMemory, 3> image_memory{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (unsigned i = 0; i < 3; ++i) {
        const auto format = i ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_SRGB;
        const VkImageCreateInfo ii{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {W, H, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = i ? VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) :
                         VkImageUsageFlags(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        CHECK(vkCreateImage(device, &ii, nullptr, &images[i]));
        vkGetImageMemoryRequirements(device, images[i], &req);
        image_memory[i] = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkBindImageMemory(device, images[i], image_memory[i], 0));
        const VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format, .subresourceRange = range};
        CHECK(vkCreateImageView(device, &vi, nullptr, &views[i]));
    }
    VkSampler sampler;
    const VkSamplerCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST};
    CHECK(vkCreateSampler(device, &sci, nullptr, &sampler));
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (unsigned i = 0; i < 5; ++i) bindings[i] =
        {i, i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
            i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5, .pBindings = bindings.data()};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &set_layout));
    const std::array<VkDescriptorPoolSize, 3> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
    const VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = pool_sizes.data()};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &pool));
    const VkDescriptorSetAllocateInfo sai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &sai, &set));
    const std::array<VkDescriptorImageInfo, 3> image_infos{{
        {sampler, views[0], VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, views[2], VK_IMAGE_LAYOUT_GENERAL}}};
    const std::array<VkDescriptorBufferInfo, 2> buffer_infos{{
        {buffer, GeometryOffset, 1826 * 4}, {buffer, ValidationOffset, 4}}};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (unsigned i = 0; i < 5; ++i) writes[i] =
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
         .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
         .pImageInfo = i < 3 ? &image_infos[i] : nullptr,
         .pBufferInfo = i >= 3 ? &buffer_infos[i - 3] : nullptr};
    vkUpdateDescriptorSets(device, 5, writes.data(), 0, nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
    std::array<VkPipeline, 3> pipelines{};
    const std::array<std::span<const unsigned>, 3> codes{
        SKY_ZENITH_VALIDATE_COMP, SKY_ZENITH_HORIZONTAL_COMP, SKY_ZENITH_VERTICAL_COMP};
    for (unsigned i = 0; i < 3; ++i) {
        const VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = codes[i].size_bytes(), .pCode = codes[i].data()};
        VkShaderModule module;
        CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
        const VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
            .layout = layout};
        CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &pipelines[i]));
        vkDestroyShaderModule(device, module, nullptr);
    }
    const VkCommandPoolCreateInfo cpi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool commands;
    CHECK(vkCreateCommandPool(device, &cpi, nullptr, &commands));
    const VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    const VkBufferImageCopy upload{.bufferOffset = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    auto download = upload;
    download.bufferOffset = InputBytes;
    std::vector<unsigned short> baseline(W * H * 4);
    for (unsigned test = 0; test < 8; ++test) {
        std::memcpy(mapped, source.data(), source.size());
        if (test == 2) static_cast<unsigned char*>(mapped)[0] = 255;
        auto* geometry = reinterpret_cast<unsigned*>(static_cast<char*>(mapped) + GeometryOffset);
        geometry[1824] = test == 0 || test == 5 || test == 7 ? 0 : 1;
        CHECK(vkResetCommandBuffer(cmd, 0));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        for (unsigned i = 0; i < 3; ++i) {
            const VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = test ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0u,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = test ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[i], .subresourceRange = range};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        vkCmdCopyBufferToImage(cmd, buffer, images[0], VK_IMAGE_LAYOUT_GENERAL, 1, &upload);
        struct Params { unsigned mip, count, mode; float strength; };
        const Params params{0, 1824, test >= 6 ? 3u : test >= 4 ? 1u : 0u, 0.4f};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &params);
        for (unsigned i = 0; i < 3; ++i) {
            const VkMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
            vkCmdDispatch(cmd, i == 0 ? 1 : W / 8, i == 0 ? 1 : H / 8, 1);
        }
        const VkMemoryBarrier before{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        vkCmdCopyImageToBuffer(cmd, images[2], VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &download);
        const VkMemoryBarrier after{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(queue));
        const bool matched = test == 1 || test == 3 || test == 4 || test == 6;
        const auto verified = *reinterpret_cast<unsigned*>(
            static_cast<char*>(mapped) + ValidationOffset);
        assert(verified == unsigned(matched));
        const auto* output = reinterpret_cast<const unsigned short*>(
            static_cast<const char*>(mapped) + InputBytes);
        if (test == 0) std::copy_n(output, baseline.size(), baseline.data());
        double variation_before = 0, variation_after = 0;
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
            const unsigned at = (y * W + x) * 4, next = (y * W + (x + 1) % W) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                const float original = Half(baseline[at + c]), value = Half(output[at + c]);
                assert(std::isfinite(value));
                if (test == 4) {
                    const std::array<float, 3> flat{0.03f, 0.05f, 0.08f};
                    assert(std::abs(value - flat[c]) < 0.00005f);
                    continue;
                }
                if (!matched || y >= (test == 6 ? 224u : 96u)) {
                    // Compare the actual GPU sRGB decode, not a CPU approximation of it.
                    if (test == 2 && at + c == 0) assert(value == 1.0f);
                    else assert(output[at + c] == baseline[at + c]);
                }
                if (matched && y < 32) {
                    variation_before += std::abs(original - Half(baseline[next + c]));
                    variation_after += std::abs(value - Half(output[next + c]));
                    assert(std::abs(value - original) < 0.01f);
                }
            }
            assert(std::abs(Half(output[at + 3]) - float(source[at + 3]) / 255.0f) < 0.0004f);
        }
        if (test == 6) {
            std::array<std::array<double, 3>, H> row_mean{};
            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    row_mean[y][c] += Half(baseline[(y * W + x) * 4 + c]) / double(W);
            double previous_amount = 0.4;
            for (unsigned y = 0; y <= 224; ++y) {
                const double t = std::clamp((double(y) + 0.5) / 224, 0.0, 1.0);
                const double amount = 0.4 * (1 - t * t * t * (t * (t * 6 - 15) + 10));
                assert(amount <= previous_amount && previous_amount - amount < 0.009);
                previous_amount = amount;
                std::array<double, 3> smooth{};
                double weights = 0;
                for (int i = -32; i <= 32; ++i) {
                    const double weight = std::exp(-double(i * i) / 288);
                    const auto& sample = row_mean[std::clamp(int(y) + i, 0, int(H) - 1)];
                    for (unsigned c = 0; c < 3; ++c) smooth[c] += sample[c] * weight;
                    weights += weight;
                }
                for (unsigned x = 0; x < W; x += 127) for (unsigned c = 0; c < 3; ++c) {
                    const unsigned at = (y * W + x) * 4 + c;
                    const double original = Half(baseline[at]);
                    const double expected = original + amount * (smooth[c] / weights - original);
                    // The reference and the two image stores each round to half precision.
                    if (std::abs(Half(output[at]) - expected) >= 0.0015 * std::max(expected, 0.01)) {
                        std::fprintf(stderr, "Transition mismatch %u,%u channel %u: %.8f expected %.8f\n",
                                     x, y, c, Half(output[at]), expected);
                        std::abort();
                    }
                }
            }
            assert(previous_amount == 0);
        }
        if (matched && test != 4) {
            std::printf("Zenith horizontal variation ratio %.4f\n", variation_after / variation_before);
            assert(variation_after < variation_before * 0.95);
            if (test == 6) assert(variation_after > variation_before * 0.55);
            const auto horizontal_reference = [&](int x, int y) {
                std::array<double, 3> sum{}, center{};
                for (unsigned c = 0; c < 3; ++c)
                    center[c] = Half(baseline[(y * W + x) * 4 + c]);
                double weights = 0;
                if (test == 6) {
                    for (unsigned i = 0; i < W; ++i) for (unsigned c = 0; c < 3; ++c)
                        sum[c] += Half(baseline[(unsigned(y) * W + i) * 4 + c]);
                    for (auto& value : sum) value /= W;
                    return sum;
                }
                for (int i = -64; i <= 64; ++i) {
                    const unsigned sample = (unsigned(y) * W + unsigned((x + i + int(W)) % int(W))) * 4;
                    double distance = 0;
                    for (unsigned c = 0; c < 3; ++c)
                        distance += std::pow(Half(baseline[sample + c]) - center[c], 2);
                    const double weight = std::exp(-double(i * i) / (2 * 24 * 24) -
                                                   distance / (2 * 0.004 * 0.004));
                    for (unsigned c = 0; c < 3; ++c) sum[c] += Half(baseline[sample + c]) * weight;
                    weights += weight;
                }
                for (auto& value : sum) value /= weights;
                return sum;
            };
            for (const int x : {0, int(W) - 1}) {
                const auto center = horizontal_reference(x, 0);
                std::array<double, 3> sum{};
                double weights = 0;
                const int radius = test == 6 ? 32 : 8;
                const double sigma = test == 6 ? 12 : 3;
                for (int i = -radius; i <= radius; ++i) {
                    const auto sample = horizontal_reference(x, std::max(0, i));
                    double distance = 0;
                    for (unsigned c = 0; c < 3; ++c) distance += std::pow(sample[c] - center[c], 2);
                    const double weight = std::exp(-double(i * i) / (2 * sigma * sigma) -
                        (test == 6 ? 0 : distance / (2 * 0.004 * 0.004)));
                    for (unsigned c = 0; c < 3; ++c) sum[c] += sample[c] * weight;
                    weights += weight;
                }
                for (unsigned c = 0; c < 3; ++c)
                    if (test != 6)
                        assert(std::abs(Half(output[x * 4 + c]) - sum[c] / weights) < 0.00001);
            }
        }
    }
    vkDestroyCommandPool(device, commands, nullptr);
    for (auto pipeline : pipelines) vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    vkDestroySampler(device, sampler, nullptr);
    for (unsigned i = 0; i < 3; ++i) {
        vkDestroyImageView(device, views[i], nullptr);
        vkDestroyImage(device, images[i], nullptr);
        vkFreeMemory(device, image_memory[i], nullptr);
    }
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("PASS: zenith smoothing, preserved lower panorama/alpha, geometry/texture refusal, reuse");
}
