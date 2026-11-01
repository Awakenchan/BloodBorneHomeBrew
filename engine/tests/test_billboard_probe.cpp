// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/host_shaders/billboard_probe_fragment.h"
#include "video_core/host_shaders/billboard_probe_instanced.h"
#include "video_core/host_shaders/billboard_probe_vertex.h"
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
#include <vulkan/vulkan.h>

#define CHECK(call)                                                                                \
    do {                                                                                           \
        const VkResult result_ = (call);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            std::fprintf(stderr, "%s failed: %d\n", #call, int(result_));                          \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

namespace {
constexpr unsigned Size = 128, Pixels = Size * Size;
constexpr VkDeviceSize ImageBytes = Pixels * 16;
struct Record {
    std::array<std::array<float, 4>, 4> attributes;
};
struct Params {
    int base_vertex;
    unsigned first_index, first_instance;
};
std::vector<uint32_t> ReadWords(const char *path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 20 || file.tellg() % 4 != 0) {
        std::fprintf(stderr, "Invalid local SPIR-V fixture: %s\n", path);
        std::exit(1);
    }
    std::vector<uint32_t> words(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(words.data()), std::streamsize(words.size() * 4));
    assert(file && words[0] == 0x07230203);
    return words;
}
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void *mapped{};
};
struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImageView view{};
};
struct Probe {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    unsigned family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkPipelineLayout layout{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkQueryPool timestamps{};
    float period = 0;
    unsigned timestamp_bits = 0;
    Buffer records, indices, constants, readback;
    std::array<Image, 4> images{};
    std::array<VkPipeline, 2> pipelines{};

    unsigned MemoryType(unsigned bits, VkMemoryPropertyFlags flags) const {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        std::fprintf(stderr, "Required Vulkan memory type unavailable\n");
        std::exit(1);
    }
    Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) const {
        Buffer result;
        const VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
        CHECK(vkCreateBuffer(device, &info, nullptr, &result.handle));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, result.handle, &req);
        const VkMemoryAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex =
                MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device, &alloc, nullptr, &result.memory));
        CHECK(vkBindBufferMemory(device, result.handle, result.memory, 0));
        CHECK(vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));
        std::memset(result.mapped, 0, size);
        return result;
    }
    VkPipeline Pipeline(bool instanced, std::span<const uint32_t> hull,
                        std::span<const uint32_t> domain) const {
        std::vector<VkShaderModule> modules;
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        const auto stage = [&](VkShaderStageFlagBits kind, std::span<const uint32_t> code) {
            VkShaderModule module;
            const VkShaderModuleCreateInfo info{.sType =
                                                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = code.size_bytes(),
                                                .pCode = code.data()};
            CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
            modules.push_back(module);
            stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              .stage = kind,
                              .module = module,
                              .pName = "main"});
        };
        stage(VK_SHADER_STAGE_VERTEX_BIT, instanced
                                              ? std::span<const uint32_t>{BILLBOARD_PROBE_INSTANCED}
                                              : std::span<const uint32_t>{BILLBOARD_PROBE_VERTEX});
        if (!instanced) {
            stage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, hull);
            stage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, domain);
        }
        stage(VK_SHADER_STAGE_FRAGMENT_BIT, BILLBOARD_PROBE_FRAGMENT);
        const VkPipelineVertexInputStateCreateInfo vertex{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = instanced ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                  : VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
        const VkPipelineTessellationStateCreateInfo tess{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
            .patchControlPoints = 1};
        const VkViewport viewport{0, 0, float(Size), float(Size), 0, 1};
        const VkRect2D scissor{{0, 0}, {Size, Size}};
        const VkPipelineViewportStateCreateInfo vp{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor};
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1};
        const VkPipelineMultisampleStateCreateInfo ms{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        std::array<VkPipelineColorBlendAttachmentState, 4> attachments{};
        for (auto &attachment : attachments)
            attachment.colorWriteMask = 15;
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 4,
            .pAttachments = attachments.data()};
        const std::array<VkFormat, 4> formats{
            VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
        const VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 4,
            .pColorAttachmentFormats = formats.data()};
        const VkGraphicsPipelineCreateInfo info{.sType =
                                                    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .pNext = &rendering,
                                                .stageCount = unsigned(stages.size()),
                                                .pStages = stages.data(),
                                                .pVertexInputState = &vertex,
                                                .pInputAssemblyState = &assembly,
                                                .pTessellationState = instanced ? nullptr : &tess,
                                                .pViewportState = &vp,
                                                .pRasterizationState = &raster,
                                                .pMultisampleState = &ms,
                                                .pColorBlendState = &blend,
                                                .layout = layout};
        VkPipeline pipeline;
        CHECK(vkCreateGraphicsPipelines(device, {}, 1, &info, nullptr, &pipeline));
        for (auto module : modules)
            vkDestroyShaderModule(device, module, nullptr);
        return pipeline;
    }
    Probe(std::span<const uint32_t> hull, std::span<const uint32_t> domain) {
        const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                    .pApplicationName = "billboard-probe",
                                    .apiVersion = VK_API_VERSION_1_3};
        const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app};
        CHECK(vkCreateInstance(&ci, nullptr, &instance));
        unsigned count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        assert(count);
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        gpu = devices[0];
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                gpu = candidate;
                break;
            }
        }
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
        std::printf("GPU: %s\n", properties.deviceName);
        period = properties.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            ++family;
        assert(family < count);
        timestamp_bits = families[family].timestampValidBits;
        VkPhysicalDeviceVulkan13Features v13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
        VkPhysicalDeviceVulkan11Features v11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                           .pNext = &v11};
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        assert(features.features.tessellationShader && v13.dynamicRendering);
        const float priority = 1;
        const VkDeviceQueueCreateInfo qi{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
        const VkDeviceCreateInfo di{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &features,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qi};
        CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        const VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        records = MakeBuffer(sizeof(Record) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        indices = MakeBuffer(128 * 4,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        constants = MakeBuffer(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        readback = MakeBuffer(ImageBytes * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const std::array<VkDescriptorSetLayoutBinding, 4> bindings{
            {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr},
             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr},
             {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr},
             {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr}}};
        const VkDescriptorSetLayoutCreateInfo si{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 4,
            .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &si, nullptr, &set_layout));
        const VkPushConstantRange range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
        const VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1,
                                            .pSetLayouts = &set_layout,
                                            .pushConstantRangeCount = 1,
                                            .pPushConstantRanges = &range};
        CHECK(vkCreatePipelineLayout(device, &li, nullptr, &layout));
        const std::array<VkDescriptorPoolSize, 2> sizes{
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}}};
        const VkDescriptorPoolCreateInfo dpi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .maxSets = 1,
                                             .poolSizeCount = 2,
                                             .pPoolSizes = sizes.data()};
        CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
        const VkDescriptorSetAllocateInfo sai{.sType =
                                                  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = descriptors,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &set_layout};
        CHECK(vkAllocateDescriptorSets(device, &sai, &set));
        const std::array<VkDescriptorBufferInfo, 4> infos{
            {{constants.handle, 0, 65536},
             {constants.handle, 0, 65536},
             {records.handle, 0, sizeof(Record) * 128},
             {indices.handle, 0, 128 * 4}}};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (unsigned i = 0; i < 4; ++i)
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = set,
                         .dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = bindings[i].descriptorType,
                         .pBufferInfo = &infos[i]};
        vkUpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
        for (auto &image : images) {
            const VkImageCreateInfo ii{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                       .imageType = VK_IMAGE_TYPE_2D,
                                       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                       .extent = {Size, Size, 1},
                                       .mipLevels = 1,
                                       .arrayLayers = 1,
                                       .samples = VK_SAMPLE_COUNT_1_BIT,
                                       .tiling = VK_IMAGE_TILING_OPTIMAL,
                                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            CHECK(vkCreateImage(device, &ii, nullptr, &image.handle));
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device, image.handle, &req);
            const VkMemoryAllocateInfo ai{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = req.size,
                .memoryTypeIndex =
                    MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            CHECK(vkAllocateMemory(device, &ai, nullptr, &image.memory));
            CHECK(vkBindImageMemory(device, image.handle, image.memory, 0));
            const VkImageViewCreateInfo vi{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = ii.format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            CHECK(vkCreateImageView(device, &vi, nullptr, &image.view));
        }
        pipelines[0] = Pipeline(false, hull, domain);
        pipelines[1] = Pipeline(true, hull, domain);
        if (timestamp_bits) {
            const VkQueryPoolCreateInfo ti{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                           .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                           .queryCount = 2};
            CHECK(vkCreateQueryPool(device, &ti, nullptr, &timestamps));
        }
    }
    std::vector<float> Draw(bool instanced, Params params, unsigned count, unsigned repeats = 1,
                            double *milliseconds = nullptr) {
        VkCommandBuffer cmd;
        const VkCommandBufferAllocateInfo ai{.sType =
                                                 VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
        const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &bi));
        std::array<VkImageMemoryBarrier, 4> barriers{};
        for (unsigned i = 0; i < 4; ++i)
            barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                           .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                           .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .image = images[i].handle,
                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 4, barriers.data());
        if (milliseconds) {
            assert(timestamps);
            vkCmdResetQueryPool(cmd, timestamps, 0, 2);
        }
        std::array<VkRenderingAttachmentInfo, 4> attachments{};
        for (unsigned i = 0; i < 4; ++i)
            attachments[i] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                              .imageView = images[i].view,
                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                              .clearValue = {.color = {{0, 0, 0, 0}}}};
        const VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea = {{0, 0}, {Size, Size}},
                                 .layerCount = 1,
                                 .colorAttachmentCount = 4,
                                 .pColorAttachments = attachments.data()};
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[instanced]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                nullptr);
        std::array<uint32_t, 32> push{};
        std::memcpy(push.data(), &params, sizeof(params));
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128, push.data());
        vkCmdBindIndexBuffer(cmd, indices.handle, 0, VK_INDEX_TYPE_UINT32);
        if (milliseconds)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
        for (unsigned i = 0; i < repeats; ++i) {
            if (instanced)
                vkCmdDraw(cmd, 4, count, 0, params.first_instance);
            else
                vkCmdDrawIndexed(cmd, count, 1, params.first_index, params.base_vertex, 0);
        }
        if (milliseconds)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
        vkCmdEndRendering(cmd);
        for (auto &barrier : barriers) {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4,
                             barriers.data());
        for (unsigned i = 0; i < 4; ++i) {
            const VkBufferImageCopy copy{.bufferOffset = ImageBytes * i,
                                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                         .imageExtent = {Size, Size, 1}};
            vkCmdCopyImageToBuffer(cmd, images[i].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.handle, 1, &copy);
        }
        const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &host, 0, nullptr, 0, nullptr);
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .commandBufferCount = 1,
                                  .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(queue, 1, &submit, {}));
        CHECK(vkQueueWaitIdle(queue));
        if (milliseconds) {
            std::array<uint64_t, 2> times{};
            CHECK(vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(times), times.data(), 8,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            const uint64_t mask =
                timestamp_bits == 64 ? UINT64_MAX : (uint64_t(1) << timestamp_bits) - 1;
            *milliseconds = double((times[1] - times[0]) & mask) * period * 1e-6 / repeats;
        }
        std::vector<float> result(Pixels * 16);
        std::memcpy(result.data(), readback.mapped, ImageBytes * 4);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return result;
    }
    ~Probe() {
        for (auto pipeline : pipelines)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (timestamps)
            vkDestroyQueryPool(device, timestamps, nullptr);
        for (auto image : images) {
            vkDestroyImageView(device, image.view, nullptr);
            vkDestroyImage(device, image.handle, nullptr);
            vkFreeMemory(device, image.memory, nullptr);
        }
        for (auto buffer : {records, indices, constants, readback}) {
            vkUnmapMemory(device, buffer.memory);
            vkDestroyBuffer(device, buffer.handle, nullptr);
            vkFreeMemory(device, buffer.memory, nullptr);
        }
        vkDestroyDescriptorPool(device, descriptors, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(
            stderr,
            "Usage: billboard-probe-test native-hull.spv native-domain.spv [--benchmark]\n");
        return 1;
    }
    const bool benchmark = argc == 4 && std::strcmp(argv[3], "--benchmark") == 0;
    assert(argc == 3 || benchmark);
    const auto hull = ReadWords(argv[1]), domain = ReadWords(argv[2]);
    Probe probe(hull, domain);
    auto *records = static_cast<Record *>(probe.records.mapped);
    auto *indices = static_cast<uint32_t *>(probe.indices.mapped);
    for (unsigned i = 0; i < 128; ++i) {
        const float x = -0.9f + float(i % 8) * 0.25f, y = -0.85f + float(i / 8 % 8) * 0.24f;
        records[i].attributes = {{{float(i) / 128, 0.3f, 0.8f, 0.6f},
                                  {0.12f, float(i % 7), 0.25f, 0.75f},
                                  {x, y, 0.4f, 1},
                                  {0.15f, 0.12f, 0.37f, 0}}};
        indices[i] = (i * 17) % 100;
    }
    // Exercise clipping, negative extents, overlapping order and non-unit clip W.
    records[0].attributes[2] = {-0.8f, 0, 0.5f, 0.5f};
    records[1].attributes[2] = {0, 0, 2, 1};
    records[2].attributes[2] = {0, 0, -0.2f, 1};
    records[3].attributes[2] = {0, 0, 0.5f, -1};
    records[4].attributes[3][0] = -0.15f;
    records[5].attributes[3][1] = 0;
    records[6].attributes[2] = {0, 0, 0.3f, 1};
    records[7].attributes[2] = records[6].attributes[2];
    records[7].attributes[3] = records[6].attributes[3];
    indices[90] = 6;
    indices[91] = 7;
    indices[92] = 6;
    for (Params params : {Params{0, 0, 0}, {7, 3, 9}, {-1, 1, 5}}) {
        for (unsigned count : {1u, 17u, 63u}) {
            const auto native = probe.Draw(false, params, count);
            const auto instanced = probe.Draw(true, params, count);
            unsigned covered = 0;
            float maximum = 0;
            for (unsigned i = 0; i < native.size(); ++i) {
                assert(std::isfinite(native[i]) && std::isfinite(instanced[i]));
                const float delta = std::abs(native[i] - instanced[i]);
                maximum = std::max(maximum, delta);
                if (delta > 2e-5f * std::max(1.0f, std::abs(native[i]))) {
                    std::fprintf(stderr,
                                 "Mismatch base=%d first=%u instances=%u count=%u "
                                 "component=%u native=%g instanced=%g\n",
                                 params.base_vertex, params.first_index, params.first_instance,
                                 count, i, native[i], instanced[i]);
                    return 1;
                }
            }
            for (unsigned i = 0; i < Pixels; ++i)
                covered += native[Pixels * 12 + i * 4 + 3] != 0;
            assert(covered || (params.base_vertex == 0 && count == 1));
            std::printf("Equivalent: base=%d first=%u firstInstance=%u records=%u "
                        "covered=%u maxDelta=%g\n",
                        params.base_vertex, params.first_index, params.first_instance, count,
                        covered, maximum);
        }
    }
    const auto ordered_native = probe.Draw(false, {0, 90, 19}, 3);
    const auto ordered_instanced = probe.Draw(true, {0, 90, 19}, 3);
    assert(ordered_native == ordered_instanced);
    const unsigned center = (Size / 2 * Size + Size / 2) * 4;
    assert(ordered_native[center] == records[6].attributes[0][0]);
    std::puts("Equivalent overlapping/repeated record order with nonzero firstInstance.");
    if (benchmark) {
        if (!probe.timestamps) {
            std::fprintf(stderr, "GPU timestamps unavailable\n");
            return 1;
        }
        std::array<std::vector<double>, 2> times;
        for (unsigned sample = 0; sample < 12; ++sample) {
            for (unsigned order = 0; order < 2; ++order) {
                const unsigned variant = (sample + order) % 2;
                double ms = 0;
                probe.Draw(variant, {0, 0, 0}, 63, 128, &ms);
                if (sample >= 2)
                    times[variant].push_back(ms);
            }
        }
        for (unsigned i = 0; i < 2; ++i) {
            std::sort(times[i].begin(), times[i].end());
            std::printf("Synthetic 63-record draw, %s median GPU ms: %.6f (range %.6f..%.6f)\n",
                        i ? "instanced" : "native tessellation", times[i][times[i].size() / 2],
                        times[i].front(), times[i].back());
        }
        std::puts(
            "Synthetic four-MRT workload, not Bloodborne frame time; do not infer FPS savings.");
    }
}
