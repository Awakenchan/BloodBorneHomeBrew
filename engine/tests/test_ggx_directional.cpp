// SPDX-License-Identifier: GPL-3.0-or-later
// Offline native/GGX replay. All game-derived shaders and inputs are external files.
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
#define CHECK(call) do { const VkResult result = (call); if (result != VK_SUCCESS) { \
    std::fprintf(stderr, "%s: Vulkan error %d\n", #call, int(result)); std::exit(1); } } while (false)

std::vector<uint32_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file), "Cannot open replay input");
    const auto bytes = file.tellg();
    Require(bytes > 0 && bytes % 4 == 0 && bytes <= 64 * 1024 * 1024, "Invalid replay input size");
    std::vector<uint32_t> data(size_t(bytes) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), bytes);
    Require(bool(file), "Incomplete replay input read");
    return data;
}

struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void* data{};
};
struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};
struct ImageSpec {
    uint32_t width{}, height{};
    VkFormat format{};
    VkComponentMapping mapping{};
};

struct Replay {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    PFN_vkCmdSetVertexInputEXT set_vertex_input{};
    PFN_vkCmdPushDescriptorSetKHR push_descriptors{};
    std::array<VkDescriptorBufferInfo, 12> buffer_info{};
    std::array<VkDescriptorImageInfo, 12> image_info{};
    std::array<VkWriteDescriptorSet, 12> writes{};
    std::array<VkSampler, 2> samplers{};
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    std::array<Buffer, 4> constants{};
    std::array<uint32_t, 4> uniform{};
    std::array<VkDeviceSize, 4> constant_sizes{};
    std::array<Buffer, 2> vertices{};
    std::array<uint32_t, 2> vertex_strides{}, vertex_offsets{}, vertex_formats{};
    std::array<Image, 5> textures{};
    std::array<Image, 2> targets{};
    Image depth{};
    Buffer indices{}, readback{}, unused_clip_planes{};
    uint32_t width{}, height{}, count{}, first_instance{}, instances{};
    int32_t base_vertex{};
    uint32_t indexed{}, index_bytes{}, topology{};
    uint32_t depth_test{}, depth_function{}, depth_write{}, depth_format{};
    uint32_t front_face{}, cull_mode{}, discard{}, restart{}, depth_bias{};
    uint32_t negative_one_to_one{}, depth_clip{}, depth_clamp{}, provoking_last{};
    uint32_t sample_shading{}, alpha_to_coverage{}, alpha_to_one{};
    float line_width{}, minimum_sample_shading{};
    std::array<VkPipelineColorBlendAttachmentState, 2> native_blends{};
    VkRect2D scissor{};
    std::array<uint32_t, 32> push{};
    VkViewport viewport{};
    VkDeviceSize plane_bytes{};
    bool floating{};
    float timestamp_period{};
    uint32_t timestamp_bits{};
    VkPhysicalDeviceProperties gpu_properties{};

    uint32_t Memory(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        Require(false, "Required replay memory type unavailable");
        return 0;
    }

    Buffer MakeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage) {
        Buffer buffer;
        VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes, .usage = usage};
        CHECK(vkCreateBuffer(device, &info, nullptr, &buffer.handle));
        VkMemoryRequirements requirement;
        vkGetBufferMemoryRequirements(device, buffer.handle, &requirement);
        VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirement.size,
            .memoryTypeIndex = Memory(requirement.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory));
        CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));
        CHECK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.data));
        std::memset(buffer.data, 0, size_t(bytes));
        buffers.push_back(buffer);
        return buffer;
    }

    Image MakeImage(const ImageSpec& spec, VkImageUsageFlags usage) {
        Image image;
        image.aspect = spec.format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
            : spec.format == VK_FORMAT_D32_SFLOAT_S8_UINT
                ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = spec.format,
            .extent = {spec.width, spec.height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
        CHECK(vkCreateImage(device, &info, nullptr, &image.handle));
        VkMemoryRequirements requirement;
        vkGetImageMemoryRequirements(device, image.handle, &requirement);
        VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirement.size,
            .memoryTypeIndex = Memory(requirement.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(device, &allocation, nullptr, &image.memory));
        CHECK(vkBindImageMemory(device, image.handle, image.memory, 0));
        VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image.handle, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = spec.format,
            .components = spec.mapping, .subresourceRange = {image.aspect, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(device, &view, nullptr, &image.view));
        images.push_back(image);
        return image;
    }

    VkCommandBuffer Begin() const {
        VkCommandBuffer command;
        VkCommandBufferAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(device, &allocation, &command));
        VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command, &begin));
        return command;
    }

    void End(VkCommandBuffer command) const {
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &command);
    }

    void Barrier(VkCommandBuffer command, Image image, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags source, VkAccessFlags destination,
                 VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage) const {
        VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = source, .dstAccessMask = destination, .oldLayout = from, .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image.handle,
            .subresourceRange = {image.aspect, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, source_stage, destination_stage,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void Upload(Image image, const std::vector<uint32_t>& data, uint32_t w, uint32_t h,
                VkImageLayout final_layout, VkAccessFlags final_access,
                VkPipelineStageFlags final_stage) {
        Require(data.size() == size_t(w) * h, "Replay image size mismatch");
        const auto staging = MakeBuffer(data.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(staging.data, data.data(), data.size() * 4);
        auto command = Begin();
        Barrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        const auto copy_aspect = image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                    ? VK_IMAGE_ASPECT_DEPTH_BIT : image.aspect;
        VkBufferImageCopy region{.imageSubresource = {copy_aspect, 0, 0, 1},
            .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(command, staging.handle, image.handle,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout,
                VK_ACCESS_TRANSFER_WRITE_BIT, final_access, VK_PIPELINE_STAGE_TRANSFER_BIT, final_stage);
        End(command);
    }

    Replay(const std::filesystem::path& fixture, const std::filesystem::path& vertex_shader,
           const std::filesystem::path& fragment_shader, bool float_mode) : floating(float_mode) {
        std::ifstream config(fixture / "replay.txt");
        uint32_t version;
        config >> version >> width >> height >> count >> base_vertex >> first_instance
               >> instances >> topology >> indexed >> index_bytes;
        config >> viewport.x >> viewport.y >> viewport.width >> viewport.height
               >> viewport.minDepth >> viewport.maxDepth;
        for (auto& value : uniform) config >> value;
        config >> depth_test >> depth_function >> depth_write >> depth_format;
        config >> front_face >> cull_mode >> discard >> restart >> depth_bias >> line_width;
        config >> scissor.offset.x >> scissor.offset.y >> scissor.extent.width >> scissor.extent.height;
        config >> negative_one_to_one >> depth_clip >> depth_clamp >> provoking_last;
        uint32_t samples;
        config >> samples >> sample_shading >> minimum_sample_shading >> alpha_to_coverage >> alpha_to_one;
        for (auto& blend : native_blends) {
            uint32_t enable, source, destination, operation, alpha_source, alpha_destination, alpha_operation, mask;
            config >> enable >> source >> destination >> operation >> alpha_source >>
                alpha_destination >> alpha_operation >> mask;
            Require(bool(config) && enable == 1 && source == VK_BLEND_FACTOR_SRC_ALPHA &&
                        destination == VK_BLEND_FACTOR_ONE && operation == VK_BLEND_OP_ADD &&
                        alpha_source == VK_BLEND_FACTOR_ONE && alpha_destination == VK_BLEND_FACTOR_ZERO &&
                        alpha_operation == VK_BLEND_OP_ADD && mask == 15,
                    "Unsupported captured native lighting blend");
            blend = {.blendEnable = !floating, .srcColorBlendFactor = VkBlendFactor(source),
                .dstColorBlendFactor = VkBlendFactor(destination), .colorBlendOp = VkBlendOp(operation),
                .srcAlphaBlendFactor = VkBlendFactor(alpha_source),
                .dstAlphaBlendFactor = VkBlendFactor(alpha_destination),
                .alphaBlendOp = VkBlendOp(alpha_operation), .colorWriteMask = mask};
        }
        for (uint32_t i = 0; i < vertices.size(); ++i)
            config >> vertex_strides[i] >> vertex_offsets[i] >> vertex_formats[i];
        Require(bool(config) && version == 5 && depth_test <= 1 && depth_function <= 7 &&
                    (!depth_test || depth_format == VK_FORMAT_D32_SFLOAT ||
                                   depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
                    front_face <= 1 && cull_mode <= 3 && discard == 0 && restart == 0 &&
                    depth_bias == 0 && line_width == 1 && negative_one_to_one <= 1 &&
                    depth_clip <= 1 && depth_clamp <= 1 && provoking_last <= 1 &&
                    samples == VK_SAMPLE_COUNT_1_BIT && sample_shading == 0 &&
                    minimum_sample_shading == 0 && alpha_to_coverage == 0 && alpha_to_one == 0 &&
                    scissor.offset.x >= 0 && scissor.offset.y >= 0 &&
                    scissor.extent.width <= width && scissor.extent.height <= height &&
                    depth_write == 0 && width && height && width <= 4096 && height <= 4096
                    && count >= 3 && count <= 6 && instances == 1 && first_instance == 0
                    && base_vertex == 0 && (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                        topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),
                "Unsupported replay geometry or config");
        plane_bytes = VkDeviceSize(width) * height * (floating ? 16 : 4);
        const auto push_words = Read(fixture / "push-data.bin");
        Require(push_words.size() == push.size(), "Invalid captured push constants");
        std::memcpy(push.data(), push_words.data(), sizeof(push));
        VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "ggx-directional-test", .apiVersion = VK_API_VERSION_1_3};
        VkInstanceCreateInfo create{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &application};
        CHECK(vkCreateInstance(&create, nullptr, &instance));
        uint32_t device_count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &device_count, nullptr));
        Require(device_count != 0, "No Vulkan replay device");
        std::vector<VkPhysicalDevice> devices(device_count);
        CHECK(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()));
        gpu = devices.front();
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                gpu = candidate;
                break;
            }
        }
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
        gpu_properties = properties;
        timestamp_period = properties.limits.timestampPeriod;
        std::printf("GPU: %s; %ux%u %s replay\n", properties.deviceName, width, height,
                    floating ? "floating-point unblended" : "native packed additive");
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        uint32_t family_count = 0, family = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
        while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            ++family;
        Require(family < family_count, "No Vulkan graphics queue");
        timestamp_bits = families[family].timestampValidBits;
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
        VkPhysicalDeviceProvokingVertexFeaturesEXT provoking{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
            .pNext = &barycentric};
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input_dynamic{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
            .pNext = &provoking};
        VkPhysicalDeviceDepthClipEnableFeaturesEXT clip_enable{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
            .pNext = &vertex_input_dynamic};
        VkPhysicalDeviceDepthClipControlFeaturesEXT clip_control{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,
            .pNext = &clip_enable};
        VkPhysicalDeviceRobustness2FeaturesEXT robustness{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
            .pNext = &clip_control};
        VkPhysicalDeviceVulkan13Features available13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &robustness};
        VkPhysicalDeviceVulkan12Features available12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &available13};
        VkPhysicalDeviceVulkan11Features available11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &available12};
        VkPhysicalDeviceFeatures2 available{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &available11};
        vkGetPhysicalDeviceFeatures2(gpu, &available);
        Require(available13.dynamicRendering && barycentric.fragmentShaderBarycentric &&
                    available12.uniformBufferStandardLayout && available12.shaderInt8 &&
                    available12.storageBuffer8BitAccess && available11.storageBuffer16BitAccess &&
                    available.features.shaderInt16 && available.features.shaderInt64 &&
                    robustness.robustBufferAccess2 && robustness.robustImageAccess2 &&
                    robustness.nullDescriptor && available11.shaderDrawParameters &&
                    available.features.sampleRateShading && vertex_input_dynamic.vertexInputDynamicState &&
                    clip_enable.depthClipEnable && clip_control.depthClipControl &&
                    (!depth_clamp || available.features.depthClamp) &&
                    (!provoking_last || provoking.provokingVertexLast),
                "Native shader replay features unavailable");
        VkPhysicalDeviceVulkan13Features enabled13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &robustness, .robustImageAccess = available13.robustImageAccess,
            .dynamicRendering = VK_TRUE};
        VkPhysicalDeviceVulkan12Features enabled12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &enabled13,
            .storageBuffer8BitAccess = VK_TRUE, .shaderFloat16 = available12.shaderFloat16,
            .shaderInt8 = VK_TRUE, .scalarBlockLayout = available12.scalarBlockLayout,
            .uniformBufferStandardLayout = VK_TRUE};
        VkPhysicalDeviceVulkan11Features enabled11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &enabled12,
            .storageBuffer16BitAccess = VK_TRUE, .shaderDrawParameters = VK_TRUE};
        VkPhysicalDeviceFeatures features{};
        features.robustBufferAccess = available.features.robustBufferAccess;
        features.independentBlend = available.features.independentBlend;
        features.sampleRateShading = VK_TRUE;
        features.depthClamp = depth_clamp;
        features.shaderImageGatherExtended = available.features.shaderImageGatherExtended;
        features.shaderInt16 = VK_TRUE;
        features.shaderInt64 = VK_TRUE;
        features.samplerAnisotropy = available.features.samplerAnisotropy;
        const float priority = 1;
        VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const std::array extensions{VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
                                    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
                                    VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
                                    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
                                    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                    VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
                                    VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &enabled11, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
            .enabledExtensionCount = uint32_t(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
            .pEnabledFeatures = &features};
        CHECK(vkCreateDevice(gpu, &device_info, nullptr, &device));
        set_vertex_input = reinterpret_cast<PFN_vkCmdSetVertexInputEXT>(
            vkGetDeviceProcAddr(device, "vkCmdSetVertexInputEXT"));
        Require(set_vertex_input != nullptr, "Dynamic native vertex input entry point unavailable");
        push_descriptors = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        Require(push_descriptors != nullptr, "Native push descriptor entry point unavailable");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = family};
        CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
        for (uint32_t i = 0; i < constants.size(); ++i) {
            const auto data = Read(fixture / ("buffer-" + std::to_string(i) + ".bin"));
            Require(data.size() <= 16384, "Captured constant buffer exceeds supported range");
            Require(uniform[i] <= 1, "Invalid replay descriptor type");
            const VkDeviceSize bytes = uniform[i] ? 65536 : data.size() * 4;
            constant_sizes[i] = bytes;
            constants[i] = MakeBuffer(bytes, uniform[i] ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            std::memcpy(constants[i].data, data.data(), data.size() * 4);
        }
        unused_clip_planes = MakeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        for (uint32_t i = 0; i < vertices.size(); ++i) {
            const auto data = Read(fixture / ("vertex-" + std::to_string(i) + ".bin"));
            const auto components = vertex_formats[i] == VK_FORMAT_R32G32_SFLOAT ? 2u
                : vertex_formats[i] == VK_FORMAT_R32G32B32_SFLOAT ? 3u
                : vertex_formats[i] == VK_FORMAT_R32G32B32A32_SFLOAT ? 4u : 0u;
            Require(components && vertex_strides[i] >= vertex_offsets[i] + components * 4 &&
                        data.size() * 4 >= size_t(count - 1) * vertex_strides[i] +
                            vertex_offsets[i] + components * 4,
                    "Unsupported or incomplete captured vertex stream");
            vertices[i] = MakeBuffer(data.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            std::memcpy(vertices[i].data, data.data(), data.size() * 4);
        }
        if (indexed) {
            Require(index_bytes == 2 || index_bytes == 4, "Unsupported index type");
            const auto data = Read(fixture / "indices.bin");
            Require(data.size() * 4 >= count * index_bytes, "Incomplete captured indices");
            indices = MakeBuffer(data.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            std::memcpy(indices.data, data.data(), data.size() * 4);
        }
        for (uint32_t i = 0; i < textures.size(); ++i) {
            uint32_t format, r, g, b, a;
            ImageSpec spec;
            config >> spec.width >> spec.height >> format >> r >> g >> b >> a;
            Require(bool(config) && spec.width && spec.height && spec.width <= 4096 &&
                        spec.height <= 4096 && (format == VK_FORMAT_R8G8B8A8_UNORM ||
                                               format == VK_FORMAT_R32_SFLOAT),
                    "Unsupported sampled image layout");
            spec.format = VkFormat(format);
            spec.mapping = {VkComponentSwizzle(r), VkComponentSwizzle(g),
                            VkComponentSwizzle(b), VkComponentSwizzle(a)};
            textures[i] = MakeImage(spec, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Upload(textures[i], Read(fixture / ("texture-" + std::to_string(i) + ".bin")),
                   spec.width, spec.height, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        for (auto& sampler : samplers) {
            uint32_t min_filter, mag_filter, mip_filter, u, v, w, anisotropy, compare, compare_op, border;
            VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            config >> min_filter >> mag_filter >> mip_filter >> u >> v >> w >>
                info.mipLodBias >> info.minLod >> info.maxLod >> info.maxAnisotropy >>
                anisotropy >> compare >> compare_op >> border;
            Require(bool(config) && min_filter <= 1 && mag_filter <= 1 && mip_filter <= 1 &&
                        u <= 2 && v <= 2 && w <= 2 && anisotropy <= 1 && compare == 0 &&
                        compare_op <= 7 && border <= 5 &&
                        info.maxAnisotropy <= properties.limits.maxSamplerAnisotropy &&
                        (info.maxAnisotropy <= 1 || features.samplerAnisotropy),
                    "Unsupported captured sampler");
            info.minFilter = VkFilter(min_filter);
            info.magFilter = VkFilter(mag_filter);
            info.mipmapMode = VkSamplerMipmapMode(mip_filter);
            info.addressModeU = VkSamplerAddressMode(u);
            info.addressModeV = VkSamplerAddressMode(v);
            info.addressModeW = VkSamplerAddressMode(w);
            info.anisotropyEnable = anisotropy;
            info.compareEnable = compare;
            info.compareOp = VkCompareOp(compare_op);
            info.borderColor = VkBorderColor(border);
            CHECK(vkCreateSampler(device, &info, nullptr, &sampler));
        }
        const VkFormat format = floating ? VK_FORMAT_R32G32B32A32_SFLOAT
                                         : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        for (uint32_t i = 0; i < targets.size(); ++i) {
            targets[i] = MakeImage({width, height, format, {}},
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (!floating)
                Upload(targets[i], Read(fixture / ("before-" + std::to_string(i) + ".bin")),
                       width, height, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        if (depth_test) {
            depth = MakeImage({width, height, VkFormat(depth_format), {}},
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Upload(depth, Read(fixture / "before-depth.bin"), width, height,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        }
        readback = MakeBuffer(plane_bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::array<VkDescriptorSetLayoutBinding, 12> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            const auto type = i == 11 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                : i <= 2 || i == 10 ? (uniform[i == 10 ? 3 : i]
                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                : i <= 7 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[i] = {i, type, 1, i >= 10 ? VK_SHADER_STAGE_VERTEX_BIT
                                             : VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo set_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data()};
        CHECK(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout));
        VkPushConstantRange push_range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
        VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
        CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            const bool is_buffer = i <= 2 || i >= 10;
            if (is_buffer) {
                const auto buffer = i == 11 ? unused_clip_planes : constants[i == 10 ? 3 : i];
                buffer_info[i] = {buffer.handle, 0, i == 11 ? 16 : constant_sizes[i == 10 ? 3 : i]};
            } else if (i <= 7) {
                image_info[i] = {VK_NULL_HANDLE, textures[i - 3].view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            } else {
                image_info[i].sampler = samplers[i - 8];
            }
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
                .pImageInfo = is_buffer ? nullptr : &image_info[i],
                .pBufferInfo = is_buffer ? &buffer_info[i] : nullptr};
        }
        pipeline = MakePipeline(vertex_shader, fragment_shader);
    }

    VkPipeline MakePipeline(const std::filesystem::path& vertex_shader,
                            const std::filesystem::path& fragment_shader) const {
        std::array<VkShaderModule, 2> modules;
        std::array<VkPipelineShaderStageCreateInfo, 2> stages;
        for (uint32_t i = 0; i < modules.size(); ++i) {
            const auto data = Read(i ? fragment_shader : vertex_shader);
            Require(data.size() >= 5 && data[0] == 0x07230203, "Invalid replay shader");
            VkShaderModuleCreateInfo module_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = data.size() * 4, .pCode = data.data()};
            CHECK(vkCreateShaderModule(device, &module_info, nullptr, &modules[i]));
            stages[i] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
                .module = modules[i], .pName = "main"};
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VkPrimitiveTopology(topology)};
        const VkPipelineViewportDepthClipControlCreateInfoEXT viewport_clip{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
            .negativeOneToOne = negative_one_to_one};
        VkPipelineViewportStateCreateInfo view{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = &viewport_clip,
            .viewportCount = 0, .scissorCount = 0};
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
                                        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                                        VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
        const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = uint32_t(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data()};
        const VkPipelineRasterizationDepthClipStateCreateInfoEXT depth_clip_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
            .depthClipEnable = depth_clip};
        const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
            .pNext = &depth_clip_state,
            .provokingVertexMode = provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                                 : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT};
        VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = &provoking_state,
            .depthClampEnable = depth_clamp, .rasterizerDiscardEnable = discard,
            .cullMode = cull_mode, .frontFace = VkFrontFace(front_face), .lineWidth = line_width};
        VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .sampleShadingEnable = sample_shading,
            .minSampleShading = minimum_sample_shading, .alphaToCoverageEnable = alpha_to_coverage,
            .alphaToOneEnable = alpha_to_one};
        VkPipelineDepthStencilStateCreateInfo depth_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = depth_test, .depthWriteEnable = depth_write,
            .depthCompareOp = VkCompareOp(depth_function)};
        VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 2, .pAttachments = native_blends.data()};
        const VkFormat format = floating ? VK_FORMAT_R32G32B32A32_SFLOAT
                                         : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        const std::array<VkFormat, 2> formats{format, format};
        VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 2, .pColorAttachmentFormats = formats.data(),
            .depthAttachmentFormat = depth_test ? VkFormat(depth_format) : VK_FORMAT_UNDEFINED,
            .stencilAttachmentFormat = depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT
                                          ? VkFormat(depth_format) : VK_FORMAT_UNDEFINED};
        VkGraphicsPipelineCreateInfo pipeline_info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering, .stageCount = 2, .pStages = stages.data(),
            .pInputAssemblyState = &assembly,
            .pViewportState = &view, .pRasterizationState = &raster,
            .pMultisampleState = &multisample, .pDepthStencilState = &depth_state,
            .pColorBlendState = &blend, .pDynamicState = &dynamic, .layout = layout};
        VkPipeline created{};
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created));
        for (const auto module : modules) vkDestroyShaderModule(device, module, nullptr);
        return created;
    }

    void Render(VkCommandBuffer command, VkPipeline selected,
                VkQueryPool queries = VK_NULL_HANDLE, uint32_t query = 0) const {
        std::array<VkRenderingAttachmentInfo, 2> attachments{};
        for (uint32_t i = 0; i < targets.size(); ++i) {
            if (floating)
                Barrier(command, targets[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            attachments[i] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = targets[i].view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = floating ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {}};
        }
        const VkRenderingAttachmentInfo depth_attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depth.view, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        auto stencil_attachment = depth_attachment;
        stencil_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        stencil_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {width, height}}, .layerCount = 1,
            .colorAttachmentCount = 2, .pColorAttachments = attachments.data(),
            .pDepthAttachment = depth_test ? &depth_attachment : nullptr,
            .pStencilAttachment = depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT
                                      ? &stencil_attachment : nullptr};
        if (queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, query);
        vkCmdBeginRendering(command, &rendering);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, selected);
        vkCmdSetViewportWithCount(command, 1, &viewport);
        vkCmdSetScissorWithCount(command, 1, &scissor);
        std::array<VkVertexInputBindingDescription2EXT, 2> vertex_bindings{};
        std::array<VkVertexInputAttributeDescription2EXT, 2> attributes{};
        for (uint32_t i = 0; i < vertices.size(); ++i) {
            vertex_bindings[i] = {
                .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
                .binding = i, .stride = vertex_strides[i], .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                .divisor = 1};
            attributes[i] = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = i, .binding = i, .format = VkFormat(vertex_formats[i]),
                .offset = vertex_offsets[i]};
        }
        set_vertex_input(command, 2, vertex_bindings.data(), 2, attributes.data());
        push_descriptors(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                         uint32_t(writes.size()), writes.data());
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(push), push.data());
        const std::array<VkBuffer, 2> vertex_handles{vertices[0].handle, vertices[1].handle};
        const std::array<VkDeviceSize, 2> offsets{};
        vkCmdBindVertexBuffers(command, 0, 2, vertex_handles.data(), offsets.data());
        if (indexed) {
            vkCmdBindIndexBuffer(command, indices.handle, 0,
                                 index_bytes == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, count, instances, 0, base_vertex, first_instance);
        } else {
            vkCmdDraw(command, count, instances, uint32_t(base_vertex), first_instance);
        }
        vkCmdEndRendering(command);
        if (queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, query + 1);
    }

    void Draw(const std::filesystem::path& output) const {
        Require(!std::filesystem::exists(output), "Refusing to overwrite replay output");
        auto command = Begin();
        Render(command, pipeline);
        for (uint32_t i = 0; i < targets.size(); ++i) {
            Barrier(command, targets[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkBufferImageCopy copy{.bufferOffset = plane_bytes * i,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {width, height, 1}};
            vkCmdCopyImageToBuffer(command, targets[i].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.handle, 1, &copy);
        }
        VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        End(command);
        std::ofstream file(output, std::ios::binary);
        file.write(static_cast<const char*>(readback.data), std::streamsize(plane_bytes * 2));
        file.close();
        Require(bool(file), "Replay output write failed");
    }

    void Benchmark(const std::filesystem::path& fixture,
                   const std::filesystem::path& vertex_shader,
                   const std::vector<std::filesystem::path>& candidates,
                   const std::filesystem::path& output, uint32_t warmup, uint32_t samples) {
        Require(!floating && timestamp_bits > 0 && timestamp_bits <= 64 &&
                    timestamp_period > 0 && gpu_properties.limits.timestampComputeAndGraphics,
                "Packed graphics GPU timestamps unavailable");
        Require(!std::filesystem::exists(output), "Refusing to overwrite benchmark output");
        std::vector<VkPipeline> pipelines{pipeline};
        for (const auto& candidate : candidates)
            pipelines.push_back(MakePipeline(vertex_shader, candidate));
        std::array<Buffer, 2> initial;
        for (uint32_t i = 0; i < initial.size(); ++i) {
            const auto data = Read(fixture / ("before-" + std::to_string(i) + ".bin"));
            initial[i] = MakeBuffer(plane_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            Require(data.size() * 4 == plane_bytes, "Invalid benchmark target snapshot");
            std::memcpy(initial[i].data, data.data(), size_t(plane_bytes));
        }
        const auto restore = [&](VkCommandBuffer command) {
            for (uint32_t i = 0; i < targets.size(); ++i) {
                Barrier(command, targets[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .imageExtent = {width, height, 1}};
                vkCmdCopyBufferToImage(command, initial[i].handle, targets[i].handle,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                Barrier(command, targets[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        };
        const uint32_t modes = uint32_t(pipelines.size());
        const uint32_t query_count = 2 * samples * modes;
        VkQueryPool queries{};
        const VkQueryPoolCreateInfo query_info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = query_count};
        CHECK(vkCreateQueryPool(device, &query_info, nullptr, &queries));
        auto command = Begin();
        vkCmdResetQueryPool(command, queries, 0, query_count);
        for (uint32_t round = 0; round < warmup + samples; ++round) {
            for (uint32_t slot = 0; slot < modes; ++slot) {
                const uint32_t mode = (round + slot) % modes;
                restore(command);
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     0, 0, nullptr, 0, nullptr, 0, nullptr);
                const bool measured = round >= warmup;
                const uint32_t query = measured ? 2 * ((round - warmup) * modes + mode) : 0;
                Render(command, pipelines[mode], measured ? queries : VK_NULL_HANDLE, query);
            }
        }
        // Reset outside the timed intervals so a subsequent native readback remains exact.
        restore(command);
        End(command);
        std::vector<uint64_t> stamps(query_count);
        CHECK(vkGetQueryPoolResults(device, queries, 0, query_count,
            stamps.size() * sizeof(uint64_t), stamps.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        const uint64_t mask = timestamp_bits == 64 ? UINT64_MAX : (uint64_t(1) << timestamp_bits) - 1;
        std::ofstream file(output);
        file << std::setprecision(17) << "{\"gpu\": " << std::quoted(gpu_properties.deviceName)
             << ", \"vendor_id\": " << gpu_properties.vendorID
             << ", \"device_id\": " << gpu_properties.deviceID
             << ", \"driver_version\": " << gpu_properties.driverVersion
             << ", \"width\": " << width << ", \"height\": " << height
             << ", \"timestamp_period_ns\": " << timestamp_period
             << ", \"timestamp_valid_bits\": " << timestamp_bits
             << ", \"warmup_rounds\": " << warmup << ", \"sample_rounds\": " << samples
             << ", \"samples_ticks\": [";
        for (uint32_t round = 0; round < samples; ++round) {
            file << (round ? ",[" : "[");
            for (uint32_t mode = 0; mode < modes; ++mode) {
                const uint32_t query = 2 * (round * modes + mode);
                const auto ticks = (stamps[query + 1] - stamps[query]) & mask;
                Require(ticks > 0 && ticks < mask / 2, "Invalid or ambiguous GPU timestamp interval");
                file << (mode ? "," : "") << ticks;
            }
            file << "]";
        }
        file << "]}\n";
        file.close();
        Require(bool(file), "Benchmark output write failed");
        vkDestroyQueryPool(device, queries, nullptr);
        for (size_t i = 1; i < pipelines.size(); ++i)
            vkDestroyPipeline(device, pipelines[i], nullptr);
    }

    ~Replay() {
        vkDeviceWaitIdle(device);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        for (auto sampler : samplers) vkDestroySampler(device, sampler, nullptr);
        for (auto image : images) {
            vkDestroyImageView(device, image.view, nullptr);
            vkDestroyImage(device, image.handle, nullptr);
            vkFreeMemory(device, image.memory, nullptr);
        }
        for (auto buffer : buffers) {
            vkUnmapMemory(device, buffer.memory);
            vkDestroyBuffer(device, buffer.handle, nullptr);
            vkFreeMemory(device, buffer.memory, nullptr);
        }
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }
};
} // namespace

int main(int argc, char** argv) {
    const bool benchmark = argc >= 9 && std::strcmp(argv[5], "--benchmark") == 0;
    Require(argc == 5 || (argc == 6 && std::strcmp(argv[5], "--float") == 0) || benchmark,
            "Usage: ggx-directional-test fixture vertex.spv fragment.spv output [--float | "
            "--benchmark warmup rounds candidate.spv ...]");
    Require(!std::filesystem::exists(argv[4]), "Refusing to overwrite a replay input or prior output");
    uint32_t warmup = 0, samples = 0;
    if (benchmark) {
        const auto number = [](const char* text) {
            Require(*text >= '0' && *text <= '9', "Invalid benchmark count");
            char* end{};
            const auto value = std::strtoul(text, &end, 10);
            Require(*end == '\0' && value <= 4096, "Invalid benchmark count");
            return uint32_t(value);
        };
        warmup = number(argv[6]);
        samples = number(argv[7]);
        Require(warmup >= 16 && samples >= 32 && argc <= 11,
                "Benchmark requires 16+ warmup rounds, 32+ samples and at most three candidates");
        Require(!std::filesystem::exists(std::string(argv[4]) + ".native-packed.raw"),
                "Refusing to overwrite benchmark verification output");
    }
    Replay replay(argv[1], argv[2], argv[3], argc == 6);
    if (benchmark) {
        std::vector<std::filesystem::path> candidates;
        for (int i = 8; i < argc; ++i) candidates.emplace_back(argv[i]);
        replay.Benchmark(argv[1], argv[2], candidates, argv[4], warmup, samples);
        replay.Draw(std::string(argv[4]) + ".native-packed.raw");
    } else {
        replay.Draw(argv[4]);
    }
}
