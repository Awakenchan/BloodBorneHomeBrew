// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: screen-space contact shadows and PCSS (contact_shadow.comp). After the game's shadow
// resolve draw and its blur chain have written their result (shadow term in RGBA8 alpha),
// one compute dispatch replaces the game's 4-tap PCF with a tighter atlas-safe PCF, or
// NVIDIA's percentage-closer soft shadows (the "effect_pcss" setting), and marches the scene
// depth toward the sun, darkening the shadow where small occluders are missed by the shadow map
// (the "effect_contact_shadows" setting).
// BB_CONTACT_SHADOWS=0 / BB_PCSS=0 remove either from the build of the pass; BB_CONTACT_LENGTH
// (metres), BB_CONTACT_THICKNESS (metres), BB_CONTACT_STRENGTH (0..1), BB_CONTACT_FLIP=-1 (sun
// direction) and BB_CONTACT_DEBUG=1 (show the contact term alone) tune the contact shadows;
// BB_PCSS_LIGHT (penumbra per metre of blocker distance), BB_PCSS_MAX (metres), BB_PCSS_MIN
// (metres) and BB_PCSS_BIAS (metres) tune PCSS.

#pragma once

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
class Scheduler;

class ContactShadow {
public:
    ContactShadow(const Instance& instance, Scheduler& scheduler,
                  VideoCore::TextureCache& texture_cache, Runtime& runtime,
                  CameraMotion& camera_motion);
    ~ContactShadow();

    /// The shadow resolve draw's constants (its first storage buffer and the dword offset in it),
    /// its uniform buffer (the distance fade) and the shadow map it samples.
    struct Inputs {
        vk::Buffer constants;
        vk::DeviceSize constants_offset = 0;
        vk::DeviceSize constants_range = 0;
        vk::Buffer uniforms;
        vk::DeviceSize uniforms_offset = 0;
        vk::DeviceSize uniforms_range = 0;
        vk::ImageView shadow_map;
        vk::ImageLayout shadow_layout = vk::ImageLayout::eUndefined;
        u32 constants_offset_word = 0;
        u32 frame_index = 0;
    };

    /// The shadow resolve draws into `target`; the dispatch runs before the next draw.
    void OnShadowDraw(VideoCore::ImageId target, const Inputs& inputs);
    void OnBlurDraw(VideoCore::ImageId target, VAddr source);
    /// Wait through native ping-pong blurs; resolve alpha on their final target.
    void Flush(bool native_blur = false);
    bool Enabled() const {
        return (directional_enabled || contact_enabled || pcss_enabled) && !failed;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;

    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    VideoCore::ImageId pending;
    Inputs pending_inputs;
    bool use_contact = false;
    bool use_pcss = false;
    bool use_filter = false;
    float length_m = 0.6f;
    float thickness_m = 0.35f;
    float strength = 0.75f;
    float flip = 1.0f;
    u32 debug = 0;
    float light_tan = 0.02f;
    float max_penumbra_m = 0.3f;
    float min_penumbra_m = 0.0f;
    float bias_m = 0.04f;
    bool contact_enabled = true;
    bool pcss_enabled = true;
    bool directional_enabled = true;
    bool failed = false;
};

} // namespace Vulkan
