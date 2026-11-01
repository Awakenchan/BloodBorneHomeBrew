// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <string>
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Scheduler;
class SkyZenith {
public:
    SkyZenith(const Instance& instance, Scheduler& scheduler);
    ~SkyZenith();
    bool Enabled() const { return enabled; }
    vk::ImageView Filter(vk::DescriptorImageInfo source,
                         vk::DescriptorBufferInfo geometry, u32 count);
    u32 CaptureVerification();
private:
    struct Pass {
        vk::UniqueDescriptorSetLayout set;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false, initialized = false;
    u32 mode = 0, polls = 0;
    float strength = 0.4f;
    std::string mode_file;
    std::array<Pass, 3> passes;
    vk::UniqueSampler sampler;
    VideoCore::UniqueImage horizontal, result;
    std::array<vk::UniqueImageView, 12> horizontal_views, result_views;
    vk::UniqueImageView result_view;
    std::unique_ptr<VideoCore::Buffer> validation;
};
}
