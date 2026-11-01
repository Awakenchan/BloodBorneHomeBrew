// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include "video_core/host_shaders/sky_seam_comp.h"

#define CHECK(call) do { if ((call) != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed\n", #call); std::abort(); } } while (false)

int main(int argc, char** argv) {
    assert(argc == 2);
    std::array<unsigned, 485 * 6> source{};
    std::ifstream file(argv[1], std::ios::binary);
    file.read(reinterpret_cast<char*>(source.data()), sizeof(source));
    assert(file.gcount() == sizeof(source));
    VkInstance instance;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "sky-seam-test", .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ci, nullptr, &instance));
    unsigned count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    assert(count);
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
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
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bi, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    unsigned type = 0;
    const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < properties.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
           (properties.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
    assert(type < properties.memoryTypeCount);
    const VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &ai, nullptr, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    void* mapped;
    CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (unsigned i = 0; i < 3; ++i) bindings[i] =
        {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings.data()};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &set_layout));
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    const VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
    const VkDescriptorSetAllocateInfo sai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &sai, &set));
    const std::array<VkDescriptorBufferInfo, 3> infos{
        VkDescriptorBufferInfo{buffer, 0, 49152}, {buffer, 0, 49152}, {buffer, 49152, 1826 * 4}};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (unsigned i = 0; i < 3; ++i) writes[i] =
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &infos[i]};
    vkUpdateDescriptorSets(device, 3, writes.data(), 0, nullptr);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    const VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
    const VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(SKY_SEAM_COMP), .pCode = SKY_SEAM_COMP};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
    const VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = layout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &pipeline));
    const VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
    const VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    for (unsigned test = 0; test < 6; ++test) {
        const unsigned vertices = test >= 4 ? 1824 : 485;
        std::memset(mapped, 0, 65536);
        auto* input = static_cast<unsigned*>(mapped) + 16;
        std::memcpy(input, source.data(), sizeof(source));
        for (unsigned i = 485; i < vertices; ++i)
            input[i * 6 + 5] = (1023u << 16) | (i % 2 ? 20u : 4076u);
        if (test == 1 || test == 5) input[0] ^= 1;
        if (test == 2) input[5] ^= 1;
        CHECK(vkResetCommandBuffer(cmd, 0));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const std::array<unsigned, 5> params{16, 21, 6, 6, vertices};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, params.data());
        vkCmdDispatch(cmd, 1, 1, 1);
        const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(queue));
        const auto* output = static_cast<const unsigned*>(mapped) + 49152 / 4;
        const bool matched = test == 0 || test == 3 || test == 4;
        assert(output[vertices] == unsigned(matched));
        for (unsigned i = 0; i < vertices; ++i) {
            const unsigned uv = input[i * 6 + 5];
            const int u = static_cast<short>(uv & 65535);
            const int corrected = int(std::round(float(u - 20) * (4096.0f / 4056.0f)));
            const unsigned expected = matched ? (uv & 0xffff0000u) |
                (unsigned(corrected) & 65535u) : uv;
            assert(output[i] == expected);
        }
    }
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorPool(device, descriptors, nullptr);
    vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("PASS: GPU sky UV guard, exact seam mapping, unchanged V, mismatched geometry/UV fallback, reuse");
}
