// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: screen-space contact shadows (vk_contact_shadow.h).

#include "video_core/renderer_vulkan/vk_contact_shadow.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#include "bbport_settings.h"
#include "common/assert.h"
#include "video_core/host_shaders/contact_shadow_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct Push {
    u32 buf_off;
    u32 frame_index;
    std::array<float, 2> size;
    float length_m;
    float thickness_m;
    float strength;
    float flip;
    u32 debug;
    u32 contact_on;
    u32 pcss_on;
    float light_tan;
    float max_penumbra_m;
    float min_penumbra_m;
    float bias_m;
};
static_assert(sizeof(Push) == 60);

constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;

float EnvFloat(const char* name, float fallback) {
    const char* env = std::getenv(name);
    return env && env[0] ? static_cast<float>(std::atof(env)) : fallback;
}

} // namespace

ContactShadow::ContactShadow(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                             CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_} {
    contact_enabled = EnvFloat("BB_CONTACT_SHADOWS", 1.0f) > 0.5f;
    pcss_enabled = EnvFloat("BB_PCSS", 1.0f) > 0.5f;
    if (!contact_enabled && !pcss_enabled) {
        return;
    }
    length_m = EnvFloat("BB_CONTACT_LENGTH", length_m);
    thickness_m = EnvFloat("BB_CONTACT_THICKNESS", thickness_m);
    strength = EnvFloat("BB_CONTACT_STRENGTH", strength);
    flip = EnvFloat("BB_CONTACT_FLIP", 1.0f) < 0.0f ? -1.0f : 1.0f;
    debug = EnvFloat("BB_CONTACT_DEBUG", 0.0f) > 0.5f ? 1u : 0u;
    light_tan = EnvFloat("BB_PCSS_LIGHT", light_tan);
    max_penumbra_m = EnvFloat("BB_PCSS_MAX", max_penumbra_m);
    min_penumbra_m = EnvFloat("BB_PCSS_MIN", min_penumbra_m);
    bias_m = EnvFloat("BB_PCSS_BIAS", bias_m);

    const auto device = instance.GetDevice();
    sampler = Check<"create contact shadow sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 0.0f,
    }));
    const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    set_layout = Check<"create contact shadow descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(Push)};
    layout = Check<"create contact shadow pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(CONTACT_SHADOW_COMP, device);
    ASSERT(module);
    SetObjectName(device, module, "contact_shadow.comp");
    pipeline = Check<"create contact shadow pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *layout,
            }));
    device.destroyShaderModule(module);
}

ContactShadow::~ContactShadow() = default;

void ContactShadow::OnShadowDraw(VideoCore::ImageId target, const Inputs& inputs) {
    if (!Enabled() || !target || !inputs.constants) {
        return;
    }
    const auto& settings = BbSettings::Get();
    use_contact = contact_enabled &&
                  settings.effects[BbSettings::EffectContactShadows].load(std::memory_order_relaxed);
    use_pcss = pcss_enabled && inputs.uniforms && inputs.shadow_map &&
               settings.effects[BbSettings::EffectPcss].load(std::memory_order_relaxed);
    if (!use_contact && !use_pcss) {
        return;
    }
    pending = target;
    pending_inputs = inputs;
}

void ContactShadow::Flush() {
    if (!pending) {
        return;
    }
    const auto target_id = pending;
    pending = {};
    const auto depth_id = camera_motion.Depth();
    if (!depth_id) {
        return;
    }
    auto& target = texture_cache.GetImage(target_id);
    auto& depth = texture_cache.GetImage(depth_id);
    if (target.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
        !(target.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        failed = true;
        std::printf("Contact shadows: the shadow target is not an RGBA8 storage image; off\n");
        return;
    }
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("Contact shadows: %ux%u, length %.2f m, thickness %.2f m, strength %.2f\n",
                    target.info.size.width, target.info.size.height, length_m, thickness_m,
                    strength);
    }

    scheduler.EndRendering();
    const auto device = instance.GetDevice();
    const auto target_view = Check<"create contact shadow target view">(device.createImageView({
        .image = target.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const auto depth_view = Check<"create contact shadow depth view">(device.createImageView({
        .image = depth.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = depth.info.pixel_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, target_view, depth_view] {
        device.destroyImageView(target_view);
        device.destroyImageView(depth_view);
    });
    runtime.Transit(&target, vk::ImageLayout::eGeneral, Compute,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();

    const u32 width = target.info.size.width, height = target.info.size.height;
    const Push push{
        .buf_off = pending_inputs.constants_offset_word,
        .frame_index = pending_inputs.frame_index,
        .size = {float(width), float(height)},
        .length_m = length_m,
        .thickness_m = thickness_m,
        .strength = strength,
        .flip = flip,
        .debug = debug,
        .contact_on = use_contact ? 1u : 0u,
        .pcss_on = use_pcss ? 1u : 0u,
        .light_tan = light_tan,
        .max_penumbra_m = max_penumbra_m,
        .min_penumbra_m = min_penumbra_m,
        .bias_m = bias_m,
    };
    const auto in = pending_inputs;
    const bool pcss_on = use_pcss;
    scheduler.RecordCrumb(
        {.name = "Contact shadows"},
        [push, width, height, target_view, depth_view, sampler = *sampler, in, pcss_on,
         constants = pending_inputs.constants, constants_offset = pending_inputs.constants_offset,
         constants_range = pending_inputs.constants_range, pipeline = *pipeline,
         layout = *layout](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = Compute,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                 vk::AccessFlagBits2::eShaderWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            const vk::DescriptorBufferInfo buffer_info{constants, constants_offset,
                                                       constants_range};
            const vk::DescriptorImageInfo depth_info{sampler, depth_view,
                                                     vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo target_info{{}, target_view, vk::ImageLayout::eGeneral};
            const vk::DescriptorBufferInfo ubo_info =
                pcss_on ? vk::DescriptorBufferInfo{in.uniforms, in.uniforms_offset,
                                                   in.uniforms_range}
                        : buffer_info;
            const vk::DescriptorImageInfo map_info =
                pcss_on ? vk::DescriptorImageInfo{sampler, in.shadow_map, in.shadow_layout}
                        : depth_info;
            const std::array<vk::WriteDescriptorSet, 5> writes{{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &buffer_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                 .pImageInfo = &depth_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &target_info},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                 .pImageInfo = &map_info},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &ubo_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push),
                                 &push);
            cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
            const vk::MemoryBarrier2 after{
                .srcStageMask = Compute,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                 vk::AccessFlagBits2::eMemoryWrite};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        });
}

} // namespace Vulkan
