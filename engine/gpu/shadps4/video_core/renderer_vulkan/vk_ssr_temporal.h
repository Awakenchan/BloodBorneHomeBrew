// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: temporal accumulation of the screen-space reflections (ssr_temporal.comp). Once the
// SSR ray march (our ssr.frag) has drawn its half-size target, and before the next draw (the
// game's blur chain) reads it, two compute dispatches blend it with last frame's result and
// write the blend back into the target. BB_SSR_TEMPORAL=0 turns it off.

#pragma once

#include <array>
#include "common/types.h"
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

class SsrTemporal {
public:
    SsrTemporal(const Instance& instance, Scheduler& scheduler,
                VideoCore::TextureCache& texture_cache, Runtime& runtime,
                SceneTargets& scene_targets, CameraMotion& camera_motion);
    ~SsrTemporal();

    /// The inputs of the SSR draw the full-size trace repeats: its constants (ubo_0 and the
    /// dword offset in it), the G-buffer normals and the lit scene colour.
    struct TraceInputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        u32 constants_dword_offset = 0;
        vk::ImageView normals_view;
        vk::ImageView color_view;
        vk::ImageLayout normals_layout = vk::ImageLayout::eGeneral;
        vk::ImageLayout color_layout = vk::ImageLayout::eGeneral;
        u32 frame_index = 0;
    };

    /// The SSR ray march drew into `target`; it is blended before the next draw.
    void OnSsrDraw(VideoCore::ImageId target, const TraceInputs& inputs);
    /// The scene-size reflection of this frame's SSR draw (null: none; the composite then
    /// keeps the game's half-size target).
    vk::ImageView FullResult() const {
        return full_ready ? *accum_full[(frame - 1) & 1].view : vk::ImageView{};
    }
    /// Any other draw is about to start: runs the blend of a pending SSR draw.
    void Flush(std::array<float, 2> jitter);
    /// The display pass: the next frame may run again.
    void OnFrameEnd();

private:
    struct OwnImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    struct Pass {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };

    void CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                    std::span<const u32> code, const char* name);
    bool EnsureImage(OwnImage& image, u32 width, u32 height, const char* name);
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    CameraMotion& camera_motion;

    void CreateTracePass();

    Pass resolve, copy, trace;
    vk::UniqueSampler nearest_sampler, linear_sampler;
    std::array<OwnImage, 2> accum; ///< RGBA16F, target size: the blended result; ping-pong
    std::array<OwnImage, 2> accum_full; ///< RGBA16F, scene size: the same for the full-size trace
    OwnImage trace_image; ///< RGBA16F, scene size: this frame's trace
    TraceInputs trace_inputs;
    bool full_enabled = true;
    bool full_ready = false;
    bool full_history_valid = false;
    std::array<float, 12> prev_view{};
    std::array<float, 2> prev_proj{};
    std::array<float, 2> prev_jitter{};
    bool prev_camera_valid = false;
    bool history_valid = false;
    bool done_this_frame = false;
    bool failed = false;
    bool enabled = true;
    u32 frame = 0;
    VideoCore::ImageId pending;
    std::array<ViewEntry, 4> view_cache{};
    u64 view_uses = 0;
};

} // namespace Vulkan
