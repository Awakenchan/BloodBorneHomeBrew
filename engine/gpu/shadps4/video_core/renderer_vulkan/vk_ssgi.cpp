// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: screen-space indirect lighting (vk_ssgi.h, ssgi.comp).

#include "video_core/renderer_vulkan/vk_ssgi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "bbport_settings.h"
#include "video_core/host_shaders/ssgi_composite_comp.h"
#include "video_core/host_shaders/ssgi_denoise_comp.h"
#include "video_core/host_shaders/ssgi_gather_comp.h"
#include "video_core/host_shaders/ssgi_prep_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

/// Push constants of ssgi.comp.
struct Params {
    std::array<float, 4> proj;
    std::array<float, 12> reproject;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> prev_jitter;
    std::array<float, 2> prev_proj;
    u32 frame;
    u32 history;
    float strength;
    float radius;
    float feedback;
    float ao_amount;
    float albedo_on;
    float step_size;
};
static_assert(sizeof(Params) == 128);

/// a * b for 3x4 affine matrices (rows).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        }
        r[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    return r;
}

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto AllCommands = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr auto SampledImage = vk::DescriptorType::eSampledImage;
constexpr auto StorageImage = vk::DescriptorType::eStorageImage;

float EnvFloat(const char* name, float fallback) {
    const char* env = std::getenv(name);
    return env && env[0] ? static_cast<float>(std::atof(env)) : fallback;
}

/// Everything before (the game's passes and transitions) finished and visible to the compute
/// shaders.
void BeforeCompute(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = AllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

/// One dispatch's writes visible to the next.
void BetweenPasses(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

/// The compute shaders' writes visible to what follows; later writes to what they read wait.
void AfterCompute(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = Compute,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = AllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

/// A port image about to be rewritten whole (or never used yet): old contents go.
vk::ImageMemoryBarrier2 ToGeneral(vk::Image image) {
    return {
        .srcStageMask = AllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = Compute,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

vk::DescriptorImageInfo Info(vk::ImageView view) {
    return {.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
}

vk::WriteDescriptorSet Write(u32 binding, vk::DescriptorType type,
                             const vk::DescriptorImageInfo* image) {
    return {.dstBinding = binding, .descriptorCount = 1, .descriptorType = type,
            .pImageInfo = image};
}

} // namespace

ScreenSpaceGI::ScreenSpaceGI(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                             SceneTargets& scene_targets_, CameraMotion& camera_motion_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_}, camera_motion{camera_motion_} {
    if (!EmulatorSettings.IsSsgiEnabled()) {
        return;
    }
    CreatePass(prep, {SampledImage, StorageImage, SampledImage}, SSGI_PREP_COMP,
               "ssgi.comp (prep)");
    CreatePass(gather, {SampledImage, SampledImage, SampledImage, StorageImage, SampledImage},
               SSGI_GATHER_COMP, "ssgi.comp (gather)");
    CreatePass(denoise, {SampledImage, SampledImage, StorageImage}, SSGI_DENOISE_COMP,
               "ssgi.comp (denoise)");
    CreatePass(composite, {SampledImage, SampledImage, StorageImage, SampledImage, SampledImage},
               SSGI_COMPOSITE_COMP, "ssgi.comp (composite)");
}

ScreenSpaceGI::~ScreenSpaceGI() {
    const auto device = instance.GetDevice();
    for (const auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

void ScreenSpaceGI::CreatePass(Pass& pass, std::initializer_list<vk::DescriptorType> types,
                               std::span<const u32> code, const char* name) {
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
    u32 count = 0;
    for (const vk::DescriptorType type : types) {
        bindings[count] = {.binding = count,
                           .descriptorType = type,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        ++count;
    }
    pass.set_layout = Check<"create SSGI descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = count,
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(Params)};
    pass.layout = Check<"create SSGI pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*pass.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto module = CompileSPV(code, device);
    ASSERT(module);
    SetObjectName(device, module, name);
    pass.pipeline = Check<"create SSGI pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pass.layout,
            }));
    device.destroyShaderModule(module);
}

bool ScreenSpaceGI::EnsureImage(OwnImage& own, u32 width, u32 height, vk::Format format,
                                const char* name) {
    if (own.image && own.width == width && own.height == height) {
        return false;
    }
    if (own.image) {
        // Recorded passes may still use the old size's image.
        scheduler.DeferOperation(
            [image = std::move(own.image), view = std::move(own.view)]() mutable {
                view.reset();
                image.Destroy();
            });
    }
    const auto device = instance.GetDevice();
    own.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    own.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, vk::Image(own.image), "SSGI {}", name);
    own.view = Check<"create SSGI image view">(device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
    own.width = width;
    own.height = height;
    return true;
}

vk::ImageView ScreenSpaceGI::CachedView(const VideoCore::Image& image, vk::Format format,
                                        vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check<"create SSGI view">(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

void ScreenSpaceGI::OnGBufferDraw(VideoCore::ImageId depth, VideoCore::ImageId albedo,
                                  VideoCore::ImageId normal) {
    gbuffer_depth = depth;
    gbuffer_albedo = albedo;
    gbuffer_normal = normal;
}

void ScreenSpaceGI::OnAoDraw(VideoCore::ImageId ao) {
    static bool seen = false;
    if (!seen) {
        seen = true;
        std::printf("SSGI: AO pass seen\n");
    }
    ao_image = ao;
}

void ScreenSpaceGI::OnFrameEnd() {
    done_this_frame = false;
    ao_image = {};
    gbuffer_depth = {};
    gbuffer_albedo = {};
    gbuffer_normal = {};
}

void ScreenSpaceGI::OnOpaqueScene(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                                  std::array<float, 2> jitter) {
    if (done_this_frame || failed || !prep.pipeline || !color_id || !depth_id ||
        !BbSettings::Get().effects[BbSettings::EffectGi].load(std::memory_order_relaxed)) {
        return;
    }
    const auto proj = camera_motion.PassProjection();
    if (proj[0] == 0.0f || proj[1] == 0.0f) {
        return;
    }
    done_this_frame = true;
    auto& color = texture_cache.GetImage(color_id);
    auto& depth = texture_cache.GetImage(depth_id);
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        failed = true;
        std::printf("SSGI: the scene color is not RGBA16F; indirect lighting is off\n");
        return;
    }

    const bool reduced = scene_targets.Reduced() && scene_targets.EligibleScene(color);
    u32 width = color.info.size.width, height = color.info.size.height;
    if (reduced) {
        width = scene_targets.Size().width;
        height = scene_targets.Size().height;
    } else if (const auto render = camera_motion.RenderSize(); render[0] && render[1]) {
        width = std::min(width, render[0]);
        height = std::min(height, render[1]);
    }

    vk::ImageView color_view{}, depth_view{}, albedo_view{};
    static const bool want_albedo = EnvFloat("BB_SSGI_ALBEDO", 1.0f) > 0.5f;
    VideoCore::Image* albedo = nullptr;
    if (want_albedo && gbuffer_albedo && gbuffer_depth == depth_id) {
        albedo = &texture_cache.GetImage(gbuffer_albedo);
        if (albedo->info.pixel_format != vk::Format::eR8G8B8A8Srgb) {
            albedo = nullptr;
        }
    }
    if (reduced) {
        VideoCore::ImageViewInfo color_info, depth_info;
        color_info.format = color.info.pixel_format;
        depth_info.format = depth.info.pixel_format;
        const auto c = scene_targets.Read(color_id, color_info, Compute,
                                          vk::AccessFlagBits2::eShaderRead |
                                              vk::AccessFlagBits2::eShaderWrite);
        if (!(c.usage & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("SSGI: the reduced scene color is not a storage image; off\n");
            return;
        }
        color_view = c.view;
        depth_view = scene_targets.Read(depth_id, depth_info).view;
        if (albedo && scene_targets.EligibleScene(*albedo)) {
            VideoCore::ImageViewInfo albedo_info;
            albedo_info.format = albedo->info.pixel_format;
            albedo_view = scene_targets.Read(gbuffer_albedo, albedo_info).view;
        }
    } else {
        if (!(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            failed = true;
            std::printf("SSGI: the scene color is not a storage image; off\n");
            return;
        }
        color_view = CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        depth_view = CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
        scheduler.EndRendering();
        runtime.Transit(&color, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, Compute,
                        vk::AccessFlagBits2::eShaderRead);
        if (albedo && albedo->info.size.width >= width && albedo->info.size.height >= height) {
            albedo_view = CachedView(*albedo, albedo->info.pixel_format,
                                     vk::ImageAspectFlagBits::eColor);
            runtime.Transit(albedo, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead);
        }
        runtime.FlushBarriers();
    }
    scheduler.EndRendering();
    vk::ImageView ao_view{};
    static const float ao_amount = EnvFloat("BB_SSGI_AO", 1.0f);
    if (ao_amount > 0.0f && ao_image) {
        auto& ao = texture_cache.GetImage(ao_image);
        if (reduced && scene_targets.EligibleScene(ao)) {
            VideoCore::ImageViewInfo ao_info;
            ao_info.format = ao.info.pixel_format;
            ao_view = scene_targets.Read(ao_image, ao_info).view;
        } else {
            ao_view = CachedView(ao, ao.info.pixel_format, vk::ImageAspectFlagBits::eColor);
            runtime.Transit(&ao, vk::ImageLayout::eGeneral, Compute,
                            vk::AccessFlagBits2::eShaderRead);
            runtime.FlushBarriers();
        }
    }
    const bool has_ao = ao_view != vk::ImageView{};
    static bool ao_printed = false;
    if (has_ao && !ao_printed) {
        ao_printed = true;
        std::printf("SSGI: AO texture bound (%ux%u)\n",
                    texture_cache.GetImage(ao_image).info.size.width,
                    texture_cache.GetImage(ao_image).info.size.height);
    }
    const bool has_albedo = albedo_view != vk::ImageView{};

    // The G-buffer's world normals (with normal-map detail) refine the depth-derived ones.
    vk::ImageView normal_view{};
    static const float normal_amount = EnvFloat("BB_SSGI_NORMALS", 0.6f);
    if (normal_amount > 0.0f && gbuffer_normal && gbuffer_depth == depth_id) {
        auto& normal = texture_cache.GetImage(gbuffer_normal);
        if (normal.info.pixel_format == vk::Format::eR8G8B8A8Unorm) {
            if (reduced) {
                if (scene_targets.EligibleScene(normal)) {
                    VideoCore::ImageViewInfo normal_info;
                    normal_info.format = normal.info.pixel_format;
                    normal_view = scene_targets.Read(gbuffer_normal, normal_info).view;
                }
            } else if (normal.info.size.width >= width && normal.info.size.height >= height) {
                normal_view = CachedView(normal, normal.info.pixel_format,
                                         vk::ImageAspectFlagBits::eColor);
                runtime.Transit(&normal, vk::ImageLayout::eGeneral, Compute,
                                vk::AccessFlagBits2::eShaderRead);
                runtime.FlushBarriers();
            }
        }
    }
    const bool has_normal = normal_view != vk::ImageView{};
    const u32 half_width = (width + 1) / 2, half_height = (height + 1) / 2;
    bool created = EnsureImage(geo, width, height, vk::Format::eR32G32B32A32Sfloat, "geometry");
    for (auto& image : light) {
        created |= EnsureImage(image, half_width, half_height, vk::Format::eR16G16B16A16Sfloat,
                               "light");
    }
    for (auto& image : filtered) {
        EnsureImage(image, half_width, half_height, vk::Format::eR16G16B16A16Sfloat, "filtered");
    }
    if (created) {
        history_valid = false;
    }

    static const float strength = EnvFloat("BB_SSGI_STRENGTH", want_albedo ? 8.0f : 4.0f);
    static const float radius = EnvFloat("BB_SSGI_RADIUS", 3.0f);
    static const float feedback = EnvFloat("BB_SSGI_BOUNCE", 0.8f);
    static const bool reproject_on = EnvFloat("BB_SSGI_REPROJECT", 1.0f) > 0.5f;
    static const bool denoise_on = EnvFloat("BB_SSGI_DENOISE", 1.0f) > 0.5f;
    const bool debug = EmulatorSettings.IsSsgiDebug();
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("SSGI: %ux%u (%s), strength %.2f, radius %.1f, bounce %.2f, albedo %s, "
                    "reprojection %s, denoise %s, AO %s (%.2f), normals %s (%.2f)\n",
                    width, height, reduced ? "reduced scene" : "native scene", strength, radius,
                    feedback, has_albedo ? "G-buffer" : "approximated",
                    reproject_on ? "on" : "off", denoise_on ? "on" : "off",
                    has_ao ? "bound" : "not found", ao_amount,
                    has_normal ? "G-buffer" : "from depth", normal_amount);
    }

    // Last frame's light is read where this frame's surfaces were: previous view * inverse view.
    const auto camera = camera_motion.PassMatrices();
    const bool use_previous = reproject_on && prev_camera_valid && camera.valid;
    constexpr std::array<float, 12> Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Params params{
        .proj = proj,
        .reproject = use_previous ? Multiply(prev_view, camera.inv_view) : Identity,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .prev_jitter = use_previous ? prev_jitter : jitter,
        .prev_proj = use_previous ? prev_proj : std::array<float, 2>{proj[0], proj[1]},
        .frame = frame,
        .history = (history_valid && (use_previous || !reproject_on) ? 1u : 0u) |
                   (debug ? 2u : 0u),
        .strength = strength,
        .radius = radius,
        .feedback = feedback,
        .ao_amount = has_ao ? ao_amount : 0.0f,
        .albedo_on = has_albedo ? 1.0f : 0.0f,
        .step_size = 1.0f,
    };
    const u32 current = frame & 1;
    scheduler.RecordCrumb(
        {.name = "SSGI"},
        [params, init_previous = !history_valid, width, height, half_width,
         half_height, color_view, depth_view, albedo_view = has_albedo ? albedo_view : *geo.view,
         ao_view = has_ao ? ao_view : *geo.view,
         normal_view = has_normal ? normal_view : *geo.view,
         normal_rows = has_normal && camera.valid ? camera.view : Identity,
         normal_amount = has_normal && camera.valid ? normal_amount : 0.0f,
         geo_image = vk::Image(geo.image), geo_view = *geo.view,
         cur_image = vk::Image(light[current].image), cur_view = *light[current].view,
         old_image = vk::Image(light[current ^ 1].image), old_view = *light[current ^ 1].view,
         filtered_image = std::array{vk::Image(filtered[0].image), vk::Image(filtered[1].image)},
         filtered_view = std::array{*filtered[0].view, *filtered[1].view},
         prep_pipeline = *prep.pipeline, prep_layout = *prep.layout,
         gather_pipeline = *gather.pipeline, gather_layout = *gather.layout,
         denoise_pipeline = *denoise.pipeline, denoise_layout = *denoise.layout,
         composite_pipeline = *composite.pipeline,
         composite_layout = *composite.layout](vk::CommandBuffer cmdbuf) mutable {
            BeforeCompute(cmdbuf);
            std::array<vk::ImageMemoryBarrier2, 5> barriers{
                ToGeneral(geo_image), ToGeneral(cur_image), ToGeneral(filtered_image[0]),
                ToGeneral(filtered_image[1])};
            u32 barrier_count = 4;
            if (init_previous) {
                barriers[barrier_count++] = ToGeneral(old_image);
            }
            cmdbuf.pipelineBarrier2(
                {.imageMemoryBarrierCount = barrier_count, .pImageMemoryBarriers = barriers.data()});

            const auto run = [&](vk::Pipeline pipeline, vk::PipelineLayout layout,
                                 std::span<const vk::WriteDescriptorSet> writes, u32 groups_x,
                                 u32 groups_y) {
                cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params),
                                     &params);
                cmdbuf.dispatch(groups_x, groups_y, 1);
            };
            const u32 full_x = (width + 7) / 8, full_y = (height + 7) / 8;
            const u32 half_x = (half_width + 7) / 8, half_y = (half_height + 7) / 8;
            {
                const std::array infos{Info(depth_view), Info(geo_view), Info(normal_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, StorageImage, &infos[1]),
                                        Write(2, SampledImage, &infos[2])};
                // PREP has no use for the reprojection rows: they carry the world-to-view
                // rotation, and step_size the share of the G-buffer normal.
                const auto reproject = params.reproject;
                std::memcpy(params.reproject.data(), normal_rows.data(), sizeof(normal_rows));
                params.step_size = normal_amount;
                run(prep_pipeline, prep_layout, writes, full_x, full_y);
                params.reproject = reproject;
                params.step_size = 1.0f;
            }
            BetweenPasses(cmdbuf);
            {
                const std::array infos{Info(geo_view), Info(color_view), Info(old_view),
                                       Info(cur_view), Info(albedo_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, SampledImage, &infos[1]),
                                        Write(2, SampledImage, &infos[2]),
                                        Write(3, StorageImage, &infos[3]),
                                        Write(4, SampledImage, &infos[4])};
                run(gather_pipeline, gather_layout, writes, half_x, half_y);
            }
            BetweenPasses(cmdbuf);
            vk::ImageView result_view = cur_view;
            if (denoise_on) {
                // The accumulated light stays unfiltered as history; two wavelet iterations
                // (taps 1 and 2 texels apart) make the copy that is composited.
                for (u32 i = 0; i < 2; ++i) {
                    params.step_size = float(1u << i);
                    const std::array infos{Info(geo_view), Info(i == 0 ? cur_view : filtered_view[0]),
                                           Info(filtered_view[i])};
                    const std::array writes{Write(0, SampledImage, &infos[0]),
                                            Write(1, SampledImage, &infos[1]),
                                            Write(2, StorageImage, &infos[2])};
                    run(denoise_pipeline, denoise_layout, writes, half_x, half_y);
                    BetweenPasses(cmdbuf);
                }
                result_view = filtered_view[1];
            }
            {
                const std::array infos{Info(geo_view), Info(result_view), Info(color_view),
                                       Info(albedo_view), Info(ao_view)};
                const std::array writes{Write(0, SampledImage, &infos[0]),
                                        Write(1, SampledImage, &infos[1]),
                                        Write(2, StorageImage, &infos[2]),
                                        Write(3, SampledImage, &infos[3]),
                                        Write(4, SampledImage, &infos[4])};
                run(composite_pipeline, composite_layout, writes, full_x, full_y);
            }
            AfterCompute(cmdbuf);
        });
    history_valid = true;
    prev_view = camera.view;
    prev_proj = {proj[0], proj[1]};
    prev_jitter = jitter;
    prev_camera_valid = camera.valid;
    ++frame;
}

} // namespace Vulkan
