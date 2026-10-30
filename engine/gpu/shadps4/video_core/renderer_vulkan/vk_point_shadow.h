// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: screen-space shadows for point lights (point_shadow.comp). The game's point-light draws
// (pixel shader 9f9ef267, 11 lights each) add their lighting into two B10G11R11 targets with
// additive full-screen quads. Before such a draw the targets are copied; after it a compute pass
// scales the light the draw added (target - copy) by a screen-space visibility toward each light
// (the "effect_point_shadows" setting).
// BB_POINT_SHADOWS=0 removes the pass; BB_POINT_LENGTH (metres of march), BB_POINT_THICKNESS
// (metres), BB_POINT_STRENGTH (0..1), BB_POINT_STEPS, BB_POINT_BIAS (metres), BB_POINT_FADE
// (distance in metres where it has faded out) and BB_POINT_DEBUG=1 (show the visibility) tune it.

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
class Scheduler;

class PointShadow {
public:
    PointShadow(const Instance& instance, Scheduler& scheduler,
                VideoCore::TextureCache& texture_cache, Runtime& runtime,
                CameraMotion& camera_motion);
    ~PointShadow();

    /// A point-light draw's constants (slot 0: matrix and count) and lights (slot 1), with the
    /// byte offsets inside them packed one per byte.
    struct Inputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        vk::Buffer lights;
        vk::DeviceSize lights_offset = 0;
        vk::DeviceSize lights_range = 0;
        u32 offsets = 0;
    };

    /// A light draw into `color0` and `color1` is about to start: copies them.
    void OnLightDraw(VideoCore::ImageId color0, VideoCore::ImageId color1, const Inputs& inputs);
    /// Any other draw is about to start: runs the pass of a pending light draw.
    void Flush();
    bool Enabled() const {
        return enabled && !failed;
    }

private:
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
    u32 debug = 0;
    bool enabled = true;
    bool failed = false;
};

} // namespace Vulkan
