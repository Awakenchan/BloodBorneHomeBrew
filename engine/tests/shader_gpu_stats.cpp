// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

struct Entry {
    std::string name, stage, spv, partner;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<std::pair<uint32_t, VkFormat>> vertex_inputs, color_outputs;
};

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out(1);
    for (const char c : s) {
        if (c == sep) {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    if (out.size() == 1 && out[0].empty()) out.clear();
    return out;
}

std::vector<uint32_t> ReadSpv(const std::string& path, uint32_t nonce) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    if (words.size() > 5) {
        words[3] += nonce;
    }
    return words;
}

VkDescriptorType Kind(const std::string& k) {
    if (k == "ub") return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (k == "sb") return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (k == "si") return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if (k == "st") return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (k == "sa") return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (k == "ut") return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    if (k == "stb") return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

std::vector<Entry> ReadManifest(const char* path) {
    std::vector<Entry> entries;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = Split(line, '\t');
        if (f.size() < 7) continue;
        Entry e{f[0], f[1], f[2], f[3]};
        for (const auto& b : Split(f[4], ';')) {
            const auto p = Split(b, ':');
            if (p.size() != 3) continue;
            e.bindings.push_back({uint32_t(std::stoul(p[0])), Kind(p[1]), uint32_t(std::stoul(p[2])),
                                  VK_SHADER_STAGE_ALL, nullptr});
        }
        for (int list = 0; list < 2; ++list) {
            for (const auto& v : Split(f[5 + list], ';')) {
                const auto p = Split(v, ':');
                if (p.size() != 2) continue;
                (list ? e.color_outputs : e.vertex_inputs)
                    .push_back({uint32_t(std::stoul(p[0])), VkFormat(std::stoul(p[1]))});
            }
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        const VkResult r_ = (x);                                                                   \
        if (r_ != VK_SUCCESS) {                                                                    \
            std::fprintf(stderr, "%s failed: %d\n", #x, int(r_));                                  \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bb-shader-gpu-stats <manifest> <report.csv> [rounds] [threads]\n");
        return 2;
    }
    const auto entries = ReadManifest(argv[1]);
    const int rounds = argc >= 4 ? std::max(1, std::atoi(argv[3])) : 1;
    const int threads = argc >= 5 ? std::max(1, std::atoi(argv[4])) : 1;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bb-shader-gpu-stats";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (auto g : gpus) {
        vkGetPhysicalDeviceProperties(g, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            gpu = g;
            break;
        }
    }
    if (!gpu && !gpus.empty()) gpu = gpus[0];
    vkGetPhysicalDeviceProperties(gpu, &props);

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR exec{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, &bary};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &exec};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    features.features.robustBufferAccess = VK_FALSE;

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> offered(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, offered.data());
    std::vector<const char*> extensions;
    for (const char* wanted : {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
                               VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                               VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
                               VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME}) {
        for (const auto& o : offered) {
            if (std::strcmp(o.extensionName, wanted) == 0) extensions.push_back(wanted);
        }
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    VkDevice device;
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    const auto get_props = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
    const auto get_stats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));

    std::FILE* out = std::fopen(argv[2], "w");
    std::fprintf(out, "name,stage,round,create_ms,executable,statistic,value\n");
    const uint32_t nonce = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFF) + 1;
    size_t built = 0, failed = 0;
    double total_ms = 0, wall_ms = 0;
    std::mutex out_mutex;
    for (int round = 0; round < rounds; ++round) {
        const auto build = [&](const Entry& e) {
            const auto make_module = [&](const std::string& path) {
                const auto words = ReadSpv(path, nonce + uint32_t(round) * 0x10000);
                VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                smci.codeSize = words.size() * 4;
                smci.pCode = words.data();
                VkShaderModule module = VK_NULL_HANDLE;
                vkCreateShaderModule(device, &smci, nullptr, &module);
                return module;
            };
            VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = uint32_t(e.bindings.size());
            dslci.pBindings = e.bindings.data();
            VkDescriptorSetLayout set_layout;
            if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &set_layout) != VK_SUCCESS) {
                dslci.flags = 0;
                CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &set_layout));
            }
            VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0,
                                     std::min<uint32_t>(256, props.limits.maxPushConstantsSize)};
            VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            VkDescriptorSetLayoutBinding motion_binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
            VkDescriptorSetLayoutCreateInfo motion_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            motion_info.bindingCount = 1;
            motion_info.pBindings = &motion_binding;
            VkDescriptorSetLayout motion_layout;
            CHECK(vkCreateDescriptorSetLayout(device, &motion_info, nullptr, &motion_layout));
            const VkDescriptorSetLayout set_layouts[2] = {set_layout, motion_layout};
            plci.setLayoutCount = 2;
            plci.pSetLayouts = set_layouts;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &push;
            VkPipelineLayout layout;
            CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));

            const VkShaderModule module = make_module(e.spv);
            const VkShaderModule partner = e.partner == "-" ? VK_NULL_HANDLE : make_module(e.partner);
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkResult result = VK_ERROR_UNKNOWN;
            const auto start = std::chrono::steady_clock::now();
            if (e.stage == "comp") {
                VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                cpci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
                cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                              VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
                cpci.layout = layout;
                if (module) result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
            } else {
                const bool is_vs = e.stage == "vert";
                VkPipelineShaderStageCreateInfo stages[2] = {
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, is_vs ? module : partner, "main", nullptr},
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, is_vs ? partner : module, "main", nullptr}};
                std::vector<VkVertexInputAttributeDescription> attribs;
                for (const auto& [location, format] : e.vertex_inputs) {
                    attribs.push_back({location, 0, format, 0});
                }
                VkVertexInputBindingDescription vbind{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
                VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                vi.vertexBindingDescriptionCount = attribs.empty() ? 0 : 1;
                vi.pVertexBindingDescriptions = &vbind;
                vi.vertexAttributeDescriptionCount = uint32_t(attribs.size());
                vi.pVertexAttributeDescriptions = attribs.data();
                VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
                ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
                vp.viewportCount = 1;
                vp.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                rs.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
                ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
                ds.depthTestEnable = VK_TRUE;
                ds.depthWriteEnable = VK_TRUE;
                ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                uint32_t max_location = 0;
                for (const auto& [location, format] : e.color_outputs) {
                    max_location = std::max(max_location, location + 1);
                }
                std::vector<VkFormat> formats(max_location, VK_FORMAT_UNDEFINED);
                for (const auto& [location, format] : e.color_outputs) {
                    formats[location] = format;
                }
                std::vector<VkPipelineColorBlendAttachmentState> blend(max_location);
                for (auto& b : blend) {
                    b.colorWriteMask = 0xF;
                }
                VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
                cb.attachmentCount = max_location;
                cb.pAttachments = blend.data();
                const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
                dyn.dynamicStateCount = 2;
                dyn.pDynamicStates = dyn_states;
                VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
                rendering.colorAttachmentCount = max_location;
                rendering.pColorAttachmentFormats = formats.data();
                rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
                rendering.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
                VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
                gpci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
                gpci.stageCount = 2;
                gpci.pStages = stages;
                gpci.pVertexInputState = &vi;
                gpci.pInputAssemblyState = &ia;
                gpci.pViewportState = &vp;
                gpci.pRasterizationState = &rs;
                gpci.pMultisampleState = &ms;
                gpci.pDepthStencilState = &ds;
                gpci.pColorBlendState = &cb;
                gpci.pDynamicState = &dyn;
                gpci.layout = layout;
                if (module && partner) result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
            }
            const double ms_taken =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::scoped_lock lock{out_mutex};
            if (result != VK_SUCCESS) {
                std::fprintf(out, "%s,%s,%d,,,failed,%d\n", e.name.c_str(), e.stage.c_str(), round, int(result));
                ++failed;
            } else {
                ++built;
                total_ms += ms_taken;
                VkPipelineInfoKHR pinfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
                pinfo.pipeline = pipeline;
                uint32_t n = 0;
                if (get_props) get_props(device, &pinfo, &n, nullptr);
                std::vector<VkPipelineExecutablePropertiesKHR> execs(
                    n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
                if (get_props) get_props(device, &pinfo, &n, execs.data());
                for (uint32_t i = 0; i < n; ++i) {
                    VkPipelineExecutableInfoKHR einfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
                    einfo.pipeline = pipeline;
                    einfo.executableIndex = i;
                    uint32_t sn = 0;
                    get_stats(device, &einfo, &sn, nullptr);
                    std::vector<VkPipelineExecutableStatisticKHR> stats(
                        sn, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
                    get_stats(device, &einfo, &sn, stats.data());
                    for (const auto& s : stats) {
                        double value = 0;
                        switch (s.format) {
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: value = s.value.b32; break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: value = double(s.value.i64); break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: value = double(s.value.u64); break;
                        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: value = s.value.f64; break;
                        default: break;
                        }
                        std::fprintf(out, "%s,%s,%d,%.3f,%s,%s,%g\n", e.name.c_str(), e.stage.c_str(), round,
                                     ms_taken, execs[i].name, s.name, value);
                    }
                }
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (module) vkDestroyShaderModule(device, module, nullptr);
            if (partner) vkDestroyShaderModule(device, partner, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyDescriptorSetLayout(device, motion_layout, nullptr);
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        };
        std::atomic<size_t> next{0};
        const auto worker = [&] {
            for (size_t k; (k = next.fetch_add(1)) < entries.size();) {
                build(entries[k]);
            }
        };
        const auto round_start = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) {
            pool.emplace_back(worker);
        }
        worker();
        for (auto& thread : pool) {
            thread.join();
        }
        wall_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - round_start).count();
    }
    std::fclose(out);
    std::printf("%s (driver %u.%u): %zu pipelines built in %.1f ms on %d threads (%.1f ms of driver "
                "time), %zu failed\n",
                props.deviceName, props.driverVersion >> 22, (props.driverVersion >> 14) & 0xFF, built,
                wall_ms, threads, total_ms, failed);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return failed ? 1 : 0;
}
