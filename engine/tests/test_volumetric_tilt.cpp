// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "video_core/host_shaders/volumetric_tilt_test_comp.h"

#define CHECK(call) do { if ((call) != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed\n", #call); std::abort(); } } while (false)

int main() {
    VkInstance instance;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "volumetric-tilt-test", .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ci, nullptr, &instance));
    unsigned count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    assert(count > 0);
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    const auto gpu = devices[0];
    unsigned families_count = 0, family = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(families_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, families.data());
    while (family < families_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    assert(family < families_count);
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
        .size = 8 * 64, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bi, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    unsigned memory_type = 0;
    const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (memory_type < properties.memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << memory_type)) ||
            (properties.memoryTypes[memory_type].propertyFlags & flags) != flags)) ++memory_type;
    assert(memory_type < properties.memoryTypeCount);
    const VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = memory_type};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &ai, nullptr, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    void* mapped;
    CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo si{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &set_layout));
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    const VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
    const VkDescriptorSetAllocateInfo sai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &sai, &set));
    const VkDescriptorBufferInfo buffer_info{buffer, 0, bi.size};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer_info};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    const VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
    const VkShaderModuleCreateInfo mi{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(VOLUMETRIC_TILT_TEST_COMP), .pCode = VOLUMETRIC_TILT_TEST_COMP};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
    const VkComputePipelineCreateInfo pc{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, {}, 1, &pc, nullptr, &pipeline));
    const VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
    const VkCommandBufferAllocateInfo cai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
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
    const auto* values = static_cast<const float*>(mapped);
    const auto near = [](float a, float b) { assert(std::isfinite(a) && std::abs(a - b) < 1e-5f); };
    const std::array<float, 8> elevations{45, 30, 25, 20, 15, 25, 0, 90};
    for (unsigned i = 0; i < 8; ++i) {
        const auto* v = values + i * 16;
        const float elevation = elevations[i] * 3.14159265358979323846f / 180;
        near(v[0], std::cos(elevation)); near(v[1], std::sin(elevation)); near(v[2], 0);
        const float offset = (45 - elevations[i]) * 3.14159265358979323846f / 180;
        near(v[4], 0.25f + 0.4f * std::tan(offset));
        near(v[5], 0.25f); near(v[8], 0.25f); near(v[9], 0.25f);
        near(v[12], i >= 6 ? 0 : 1);
        near(v[13], 0); near(v[14], 1); near(v[15], 0);
    }
    vkUnmapMemory(device, memory);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorPool(device, descriptors, nullptr);
    vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("PASS: volumetric tilt 0/15/20/25/30 degrees, horizon clamp, camera covariance, "
              "blocker-plane anchoring and cascade boundaries");
}
