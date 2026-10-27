// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: screen-space indirect lighting (ssgi.comp): Bloodborne's lit opaque scene color gets
// one bounce of light from the screen's other surfaces, with a second bounce through last
// frame's result. It runs once a frame, when the transparents start (the opaque scene is
// finished and the upscaler has not yet copied it for its reactive mask), as three compute
// dispatches recorded on the recording threads: view-space normals from the depth, the gather at
// half size, and the bilateral upsample added to the scene color. BB_SSGI=0 turns it off.

#pragma once

#include <array>
#include <span>
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

class ScreenSpaceGI {
public:
    ScreenSpaceGI(const Instance& instance, Scheduler& scheduler,
                  VideoCore::TextureCache& texture_cache, Runtime& runtime,
                  SceneTargets& scene_targets, CameraMotion& camera_motion);
    ~ScreenSpaceGI();

    /// The opaque scene is complete: adds the indirect light to `color` (RGBA16F scene color)
    /// using `depth` (the scene depth). `jitter`: the viewport shift of this frame's scene,
    /// pixels. Once a frame.
    void OnOpaqueScene(VideoCore::ImageId color, VideoCore::ImageId depth,
                       std::array<float, 2> jitter);
    /// A G-buffer draw: `albedo` is its sRGB base-color target (colour 2), drawn with `depth`.
    void OnGBufferDraw(VideoCore::ImageId depth, VideoCore::ImageId albedo);
    /// The ambient occlusion pass drew into `ao` (R channel); the GI is attenuated by it.
    void OnAoDraw(VideoCore::ImageId ao);
    /// The display pass: the next frame may run it again.
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
    /// True when the image was (re)created.
    bool EnsureImage(OwnImage& image, u32 width, u32 height, vk::Format format, const char* name);
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    CameraMotion& camera_motion;

    Pass prep, gather, denoise, composite;
    OwnImage geo;                  ///< RGBA32F, scene size: view normal, view depth (<= 0: sky)
    std::array<OwnImage, 2> light; ///< RGBA16F, half size: indirect light, view depth; ping-pong
    std::array<OwnImage, 2> filtered; ///< RGBA16F, half size: the denoiser's iterations
    std::array<float, 12> prev_view{};  ///< last frame's camera and viewport shift
    std::array<float, 2> prev_proj{};
    std::array<float, 2> prev_jitter{};
    bool prev_camera_valid = false;
    u32 frame = 0;
    bool done_this_frame = false;
    bool history_valid = false;
    bool failed = false;
    VideoCore::ImageId gbuffer_depth, gbuffer_albedo; ///< last G-buffer draw this frame
    VideoCore::ImageId ao_image;                      ///< last AO draw this frame
    std::array<ViewEntry, 4> view_cache{};
    u64 view_uses = 0;
};

} // namespace Vulkan
