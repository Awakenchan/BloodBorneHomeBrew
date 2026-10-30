// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_contact_shadow.h"
#include "video_core/renderer_vulkan/vk_point_shadow.h"

namespace Vulkan {

class SceneTargets;

class VolumetricLight {
public:
    VolumetricLight(const Instance& instance, Scheduler& scheduler,
                    VideoCore::TextureCache& texture_cache, Runtime& runtime,
                    SceneTargets& scene_targets);
    ~VolumetricLight();

    void Reset() { ready = tinted = false; scene = {}; celestial_candidates = 0; }
    void Integrate(VideoCore::ImageId shadow_target, const vk::DescriptorImageInfo& depth,
                   const ContactShadow::Inputs& inputs);
    void Tint(const vk::DescriptorBufferInfo& lighting, u32 offset_bytes,
              const vk::DescriptorImageInfo& material);
    void Scene(VideoCore::ImageId color);
    struct CelestialInputs {
        vk::DescriptorBufferInfo positions, normals, model, transforms, scene;
        u32 vertex_stride, normal_stride, normal_word;
        u32 model_offset, transform_offset, scene_offset;
    };
    void Celestial(const CelestialInputs& inputs);
    void PointLights(const PointShadow::Inputs& inputs, u32 count);
    void Composite();

private:
    struct Pass {
        vk::UniqueDescriptorSetLayout set;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    void CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                    std::span<const u32> code, u32 push_size);
    void EnsureImage(u32 width, u32 height);
    bool Enabled() const;

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    Pass integrate, repair, tint, composite, celestial, point;
    std::unique_ptr<VideoCore::Buffer> direction_state;
    vk::UniqueSampler sampler;
    VideoCore::UniqueImage fog;
    vk::UniqueImageView fog_view;
    VideoCore::UniqueImage repair_fog;
    vk::UniqueImageView repair_view;
    VideoCore::UniqueImage point_fog;
    vk::UniqueImageView point_view;
    VideoCore::UniqueImage depth_snapshot;
    vk::UniqueImageView depth_view;
    VideoCore::ImageId scene{};
    u64 scene_uid = 0;
    u32 depth_width = 0, depth_height = 0;
    u32 width = 0, height = 0;
    float strength = 0.08f;
    float reach = 40.0f;
    float density = 0.025f;
    float anisotropy = 0.4f;
    u32 steps = 32;
    bool ready = false;
    bool tinted = false;
    bool enabled = true;
    bool failed = false;
    bool composite_reported = false;
    bool tint_reported = false;
    u32 celestial_candidates = 0;
    bool celestial_reported = false;
    bool fallback_reported = false;
};

} // namespace Vulkan
