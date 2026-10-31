// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/types.h"
#include <array>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class CameraMotion;
class Instance;
class Runtime;
class SceneTargets;
class Scheduler;

class PointShadow {
public:
    PointShadow(const Instance& instance, Scheduler& scheduler,
                VideoCore::TextureCache& texture_cache, Runtime& runtime,
                SceneTargets& scene_targets, CameraMotion& camera_motion);
    ~PointShadow();

    struct Inputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        vk::Buffer lights;
        vk::DeviceSize lights_offset = 0;
        vk::DeviceSize lights_range = 0;
        u32 offsets = 0;
        u32 count = 0;
        vk::Rect2D tile{};
    };

    void OnLightDraw(VideoCore::ImageId color0, VideoCore::ImageId color1, const Inputs& inputs);

    void Flush();

private:
    bool Ready();

    struct OwnImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    bool EnsureImage(OwnImage& own, u32 width, u32 height);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    CameraMotion& camera_motion;

    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    OwnImage snapshot[2];
    std::array<VideoCore::ImageId, 2> pending{};
    std::array<u64, 2> pending_uid{};
    Inputs pending_inputs;
    u32 frame_index = 0;
    float length_m = 2.5f;
    float thickness_m = 0.6f;
    float strength = 0.85f;
    float fade_start_m = 18.0f;
    float fade_end_m = 30.0f;
    float bias_m = 0.03f;
    u32 steps = 16;
    bool tried = false;
    std::array<s32, 4> pending_rect{};
    bool failed = false;
};

}
