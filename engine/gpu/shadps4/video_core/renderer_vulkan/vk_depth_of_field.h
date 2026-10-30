// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <span>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class SceneTargets;

class DepthOfField {
public:
    DepthOfField(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime,
                 SceneTargets& scene_targets);
    ~DepthOfField();

    void Reset();
    void Camera(const vk::DescriptorBufferInfo& constants, u32 offset);
    void Encode(VideoCore::ImageId source, const vk::DescriptorImageInfo& depth);
    void Kernel(VAddr target, const vk::DescriptorBufferInfo& pixel, u32 pixel_offset,
                const vk::DescriptorBufferInfo& vertex, u32 vertex_offset);
    void Composite(VideoCore::ImageId target, VAddr sharp_address, VAddr blur_address,
                   const vk::DescriptorImageInfo& sharp,
                   const vk::DescriptorBufferInfo& constants, u32 offset);

private:
    enum Stage : u32 {
        CaptureCamera, Snapshot, CaptureKernel, Prepare, Gather, Resolve,
        Focus, NearTiles, Near, Count
    };
    struct Image {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };
    struct Target {
        vk::ImageView view;
        u32 width, height;
    };
    struct Params {
        u32 constants_word, vertex_word, width, height;
        float intensity = 1.0f;
        float delta_seconds = 0.0f;
        u32 reset_focus = 1;
    };
    static_assert(sizeof(Params) == 28);
    bool Enabled() const;
    void Fallback(const char* reason);
    Target Access(VideoCore::ImageId id, bool write);
    void EnsureImages(u32 width, u32 height);
    void Dispatch(Stage stage, const Params& params, u32 width, u32 height,
                  std::span<const vk::WriteDescriptorSet> writes);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;
    vk::UniqueDescriptorSetLayout set;
    vk::UniquePipelineLayout layout;
    std::array<vk::UniquePipeline, Count> pipelines;
    vk::UniqueSampler sampler;
    std::unique_ptr<VideoCore::Buffer> state;
    Image depth, prepared, half_depth, blurred, near_tiles;
    u32 near_tile_size = 0;
    bool focus_reset = true;
    std::chrono::steady_clock::time_point last_focus{};
    VideoCore::ImageId source{};
    u64 source_uid = 0;
    VAddr source_address = 0, blur_target = 0;
    u32 width = 0, height = 0;
    bool enabled = true, camera_ready = false, encoded = false, kernel_ready = false;
    bool fallback_reported = false;
};
} // namespace Vulkan
