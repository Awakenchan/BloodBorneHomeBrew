// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore { class Buffer; }
namespace Vulkan {
class Instance;
class Scheduler;
class SkySeam {
public:
    SkySeam(const Instance& instance, Scheduler& scheduler);
    ~SkySeam();
    bool Enabled() const { return enabled; }
    vk::Buffer Correct(vk::DescriptorBufferInfo positions, vk::DescriptorBufferInfo uv,
                       u32 position_stride, u32 uv_stride, u32 count);
    u32 CaptureVerification();
    vk::DescriptorBufferInfo VerificationBuffer() const;
private:
    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false, reported = false;
    u32 capture_count = 0;
    std::unique_ptr<VideoCore::Buffer> output;
    vk::UniqueDescriptorSetLayout set;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};
}
