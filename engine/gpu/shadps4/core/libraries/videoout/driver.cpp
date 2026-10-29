// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <time.h>
#include <cstring>
#ifndef _WIN32
#include <sys/resource.h>
#include <dirent.h>
#include <unistd.h>
#endif
#include "common/assert.h"
#include "bbport_platform.h"
#include "lantern_pacing.h"
#include "bbport_toggles.h"
#include "bbport_heap_sites.h"
#include "bbport_wait_trace.h"
#include "bbport_freeze.h"
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/kernel/equeue.h"

extern "C" void runtime_sleep_stats(uint64_t* calls, uint64_t* ns);
extern "C" void runtime_wait_report(double frames);
extern "C" void sys_rwlock_census(uint64_t* live, uint64_t* found, uint64_t* steps);
extern "C" void sys_sema_census(uint64_t* live, uint64_t* found, uint64_t* steps);
extern "C" void runtime_thread_census(uint64_t* made, uint64_t* finished, uint64_t* found,
                                      uint64_t* steps);
extern "C" uint64_t runtime_memory_regions(void);
extern "C" uint64_t runtime_heap_growths(void);
#include "video_core/page_manager.h"
#include "core/libraries/videoout/videoout_error.h"
#include "sdl_window.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/pipeline_compile_queue.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns, g_bb_compile_ns_total;
extern std::atomic<u32> g_bb_compiles, g_bb_compiles_total;
} // namespace Vulkan

namespace AmdGpu {
void OcclusionDumpCounts(u64& dumps, u64& in_order); // bbport: liverpool.cpp
} // namespace AmdGpu

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    const char* separate = std::getenv("BB_PRESENT_THREAD");
    separate_swap = !(separate && separate[0] == '0');
    if (separate_swap) {
        swap_thread = std::jthread([&](std::stop_token token) { SwapThread(token); });
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

void VideoOutDriver::RunPresenter(std::function<void()> work, bool if_idle) {
    if (!separate_swap) {
        work();
        return;
    }
    {
        std::scoped_lock lock{swap_mutex};
        if (if_idle && (swap_busy || !swap_queue.empty())) {
            return; // a redraw of the last frame is pointless while frames are queued
        }
        swap_queue.push_back(std::move(work));
    }
    swap_cv.notify_one();
}

void VideoOutDriver::SwapThread(std::stop_token token) {
    Common::SetCurrentThreadName("bb:Present");
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock{swap_mutex};
            swap_busy = false;
            if (!swap_cv.wait(lock, token, [this] { return !swap_queue.empty(); })) {
                return;
            }
            work = std::move(swap_queue.front());
            swap_queue.pop_front();
            swap_busy = true;
        }
        work();
    }
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };
        Vulkan::FrameCapture::AddDisplayBuffer(address);

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

/// bbport: memory per statistics window: the kernel's count for the game (VRAM and GTT of its
/// DRM clients, RSS) and the parts we know of. GTT holds the game's direct memory (fixed).
static void PrintMemory() {
#ifndef _WIN32
    u64 vram_kib = 0, gtt_kib = 0;
    std::vector<u64> clients;
    if (DIR* dir = opendir("/proc/self/fdinfo")) {
        while (const dirent* entry = readdir(dir)) {
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%s", entry->d_name);
            FILE* file = std::fopen(path, "r");
            if (!file) {
                continue;
            }
            char line[160];
            bool drm = false;
            unsigned long long client = 0, vram = 0, gtt = 0;
            while (std::fgets(line, sizeof(line), file)) {
                drm |= std::strncmp(line, "drm-driver:", 11) == 0;
                std::sscanf(line, "drm-client-id: %llu", &client);
                std::sscanf(line, "drm-memory-vram: %llu", &vram);
                std::sscanf(line, "drm-memory-gtt: %llu", &gtt);
            }
            std::fclose(file);
            // Duplicated descriptors share a client.
            if (drm && std::find(clients.begin(), clients.end(), client) == clients.end()) {
                clients.push_back(client);
                vram_kib += vram;
                gtt_kib += gtt;
            }
        }
        closedir(dir);
    }
    unsigned long long size_pages = 0, rss_pages = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%llu %llu", &size_pages, &rss_pages) != 2) {
            rss_pages = 0;
        }
        std::fclose(statm);
    }
    const u64 rss = u64(rss_pages) * u64(sysconf(_SC_PAGESIZE));
    const u64 device = BbStats::device_alloc_bytes.load() - BbStats::device_free_bytes.load();
    u64 vma_blocks = 0, vma_used = 0;
    Vulkan::VmaDeviceUsage(vma_blocks, vma_used);
    std::printf("Memory: VRAM %llu MiB, GTT %llu MiB, RSS %llu MiB; Vulkan memory %llu MiB "
                "allocated (VMA blocks %llu MiB, %llu MiB of them used); Vulkan images %llu MiB; "
                "cached images %llu MiB in %llu; guest blocks in VRAM %llu MiB allocated, "
                "%llu MiB of it unused (%llu MiB moved back idle, %llu MiB unmapped, %llu MiB freed; %llu chunks, %llu emptied, blocks kept: %llu GPU-written, %llu not direct memory); texture collector: usage %llu MiB, starts at %llu MiB, %llu images freed, %llu GPU-written kept\n",
                (unsigned long long)(vram_kib >> 10), (unsigned long long)(gtt_kib >> 10),
                (unsigned long long)(rss >> 20), (unsigned long long)(device >> 20),
                (unsigned long long)(vma_blocks >> 20), (unsigned long long)(vma_used >> 20),
                (unsigned long long)(BbStats::vk_image_bytes.load() >> 20),
                (unsigned long long)(BbStats::live_image_bytes.load() >> 20),
                (unsigned long long)BbStats::live_images.load(),
                (unsigned long long)(BbStats::residency_alloc_bytes.load() >> 20),
                (unsigned long long)(BbStats::residency_unused_bytes.load() >> 20),
                (unsigned long long)(BbStats::vram_idle_bytes.exchange(0) >> 20),
                (unsigned long long)(BbStats::vram_unmapped_bytes.exchange(0) >> 20),
                (unsigned long long)(BbStats::vram_chunks_freed_bytes.exchange(0) >> 20),
                (unsigned long long)BbStats::residency_chunk_count.load(),
                (unsigned long long)BbStats::evacuations.exchange(0),
                (unsigned long long)BbStats::evac_kept_gpu.exchange(0),
                (unsigned long long)BbStats::evac_kept_other.exchange(0),
                (unsigned long long)(BbStats::gc_used_bytes.load() >> 20),
                (unsigned long long)(BbStats::gc_trigger_bytes.load() >> 20),
                (unsigned long long)BbStats::gc_freed_images.exchange(0),
                (unsigned long long)BbStats::gc_kept_images.exchange(0));
#endif
}

/// bbport: the game's RAM and the PC's, once a minute in every session: steady growth over a
/// session is a leak, and a crash right after "free" ran out is the PC running out of memory.
static void LogRam() {
    BbPlatform::Memory m;
    if (!BbPlatform::GetMemory(m)) {
        return;
    }
    const auto mib = [](u64 bytes) { return static_cast<unsigned long long>(bytes >> 20); };
    std::printf("Memory: game %llu MiB in RAM (%llu MiB committed, peak %llu MiB); PC %llu of %llu "
                "MiB free, %llu MiB commit left; GPU images %llu MiB, %llu MiB cached in %llu\n",
                mib(m.working_set), mib(m.private_bytes), mib(m.peak_working_set), mib(m.free_phys),
                mib(m.total_phys), mib(m.commit_left), mib(BbStats::vk_image_bytes.load()),
                mib(BbStats::live_image_bytes.load()),
                static_cast<unsigned long long>(BbStats::live_images.load()));
    // Where that RAM is: on the first report, then every five minutes. Listing every resident page
    // takes a while, so not on this (the presenting) thread.
    static int reports = 0;
    if (reports++ % 5 == 0) {
        std::thread([mib] {
            BbPlatform::MemoryBreakdown b;
            if (!BbPlatform::GetMemoryBreakdown(b)) return;
            std::printf("Memory by kind: game memory %llu MiB, other mapped %llu MiB, programs and DLLs %llu MiB, "
                        "engine and driver %llu MiB (shared %llu MiB)\n",
                        mib(b.game_pool), mib(b.mapped), mib(b.images), mib(b.priv), mib(b.shared));
            std::fflush(stdout);
        }).detach();
    }
    std::fflush(stdout);
}

/// bbport: what each frame costs and the counts that could grow with every area load, after the
/// "Memory:" line, without BB_FRAME_STATS: FPS falling over a session (90 to 50 after a few
/// deaths, players 2026-10) shows here as one of them climbing with it. `flips` and
/// `worst_ms`, `slow` (frames over 33 ms) since the last call, `seconds` ago.
static void LogLoad(double seconds, u64 flips, double worst_ms, u64 slow) {
    struct Snapshot {
        u64 draws, dispatches, cpu_us, faults, tracker_faults, protects, protect_pages;
        u64 rw_found, rw_steps, sema_found, sema_steps, thread_found, thread_steps;
        u64 zpass, zpass_in_order;
    };
    static Snapshot last{};
    Snapshot now{};
    now.draws = BbStats::draws.load(std::memory_order_relaxed);
    now.dispatches = BbStats::dispatches.load(std::memory_order_relaxed);
    if (BbPlatform::Usage usage; BbPlatform::GetUsage(false, usage)) {
        now.cpu_us = usage.user_us + usage.sys_us;
        now.faults = usage.minor_faults;
    }
    now.tracker_faults = BbStats::tracker_faults.load(std::memory_order_relaxed);
    now.protects = BbStats::protect_calls.load(std::memory_order_relaxed);
    now.protect_pages = BbStats::protect_pages.load(std::memory_order_relaxed);
    AmdGpu::OcclusionDumpCounts(now.zpass, now.zpass_in_order);
    u64 rwlocks = 0, semas = 0, made = 0, finished = 0;
    sys_rwlock_census(&rwlocks, &now.rw_found, &now.rw_steps);
    sys_sema_census(&semas, &now.sema_found, &now.sema_steps);
    runtime_thread_census(&made, &finished, &now.thread_found, &now.thread_steps);
    const bool first = last.draws == 0 && last.cpu_us == 0;
    const double frames = static_cast<double>(std::max<u64>(flips, 1));
    const double secs = std::max(seconds, 1.0);
    // The frame stats (BB_FRAME_STATS) reset some of these counters every window: a counter
    // below its last value started again (a garbage rate in 9.9's first logs).
    const auto delta = [](u64 a, u64 b) { return a >= b ? a - b : a; };
    const auto per_s = [&](u64 a, u64 b) { return first ? 0.0 : double(delta(a, b)) / secs; };
    const auto steps = [](u64 found, u64 walked) {
        return found ? double(walked) / double(found) : 0.0;
    };
    std::printf("Load: %.1f FPS (%llu over 33 ms, worst %.0f ms), %.0f draws and %.0f dispatches a "
                "frame; CPU %.1f cores, %.0f page faults/s; write faults %.0f/s, protects %.0f/s "
                "(%.0f pages/s), hot pages %lld; threads %llu live of %llu, rwlocks %llu (%.1f "
                "steps, %.0f/s), semaphores %llu (%.1f steps), memory map %llu regions, heap "
                "growths %llu; occlusion dumps %.1f a frame (%.1f in order)\n",
                double(flips) / secs, static_cast<unsigned long long>(slow), worst_ms,
                first ? 0.0 : double(delta(now.draws, last.draws)) / frames,
                first ? 0.0 : double(delta(now.dispatches, last.dispatches)) / frames,
                per_s(now.cpu_us, last.cpu_us) / 1e6, per_s(now.faults, last.faults),
                per_s(now.tracker_faults, last.tracker_faults), per_s(now.protects, last.protects),
                per_s(now.protect_pages, last.protect_pages),
                static_cast<long long>(BbStats::hot_pages.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(made - finished),
                static_cast<unsigned long long>(made), static_cast<unsigned long long>(rwlocks),
                steps(delta(now.rw_found, last.rw_found), delta(now.rw_steps, last.rw_steps)),
                per_s(now.rw_found, last.rw_found), static_cast<unsigned long long>(semas),
                steps(delta(now.sema_found, last.sema_found), delta(now.sema_steps, last.sema_steps)),
                static_cast<unsigned long long>(runtime_memory_regions()),
                static_cast<unsigned long long>(runtime_heap_growths()),
                first ? 0.0 : double(delta(now.zpass, last.zpass)) / frames,
                first ? 0.0 : double(delta(now.zpass_in_order, last.zpass_in_order)) / frames);
    last = now;
}

void VideoOutDriver::Flip(const Request& req) {
    BbFreeze::NoteFlip();
    // Update HDR status before presenting, then present the frame (bbport: on the swap thread).
    // bbport: frame generation drops its generated frame while the next flip waits here.
    presenter->NoteFlipQueued(1);
    RunPresenter([this, frame = req.frame, hdr = req.port->is_hdr] {
        presenter->NoteFlipQueued(-1);
        presenter->SetHDR(hdr);
        presenter->Present(frame);
    });
    Vulkan::FrameCapture::OnFlip(req.index >= 0 ? req.port->buffer_slots[req.index].address_left
                                                : 0);

    {
        static auto last_flip = std::chrono::steady_clock::now();
        const u32 background_fps = Frontend::BackgroundFrameLimit();
        if (background_fps && Frontend::WindowInBackground()) {
            const auto due = last_flip + std::chrono::nanoseconds(1000000000 / background_fps);
            if (std::chrono::steady_clock::now() < due) {
                std::this_thread::sleep_until(due);
            }
        }
        last_flip = std::chrono::steady_clock::now();
    }

    {
        static auto next_ram = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        // bbport: flips since the last report, for LogLoad.
        static auto load_start = std::chrono::steady_clock::now(), load_last = load_start;
        static u64 load_flips = 0, load_slow = 0;
        static double load_worst_ms = 0.0;
        const auto flip_time = std::chrono::steady_clock::now();
        const double flip_ms =
            std::chrono::duration<double, std::milli>(flip_time - load_last).count();
        load_last = flip_time;
        ++load_flips;
        load_slow += flip_ms > 33.0;
        load_worst_ms = std::max(load_worst_ms, flip_ms);
        if (const auto now = flip_time; now >= next_ram) {
            next_ram = now + std::chrono::seconds(60);
            LogRam();
            LogLoad(std::chrono::duration<double>(now - load_start).count(), load_flips,
                    load_worst_ms, load_slow);
            std::fflush(stdout);
            load_start = now;
            load_flips = load_slow = 0;
            load_worst_ms = 0.0;
        }
    }

    // bbport: BB_FRAME_STATS=1 prints flip rate and frame time spread every 5 seconds.
    static const bool frame_stats = EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false);
    if (frame_stats) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point window_start = Clock::now(), last = window_start;
        static u32 frames;
        static double worst_ms;
        const auto now = Clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
        worst_ms = std::max(worst_ms, frame_ms);
        static std::vector<double> intervals;
        intervals.push_back(frame_ms);
        last = now;
        // Stall diagnostics: what happened during a long frame.
        static u64 last_gpu_ns, last_images, last_image_bytes, last_buffer_bytes;
        static u64 last_t[6], last_minflt, last_sigf, last_pc, last_pp, last_rc, last_rp;
        const u64 pc = BbStats::protect_calls.load(), pp = BbStats::protect_pages.load(),
                  rc = BbStats::protect_revoke_calls.load(), rp = BbStats::protect_revoke_pages.load();
        const u64 minflt = BbStats::gpu_minor_faults.load(), sigf = BbStats::gpu_signal_faults.load();
        static u64 last_copy_cpu, last_copy_sys, last_copy_flt;
        const u64 copy_cpu = BbStats::t_copy_cpu.load(), copy_sys = BbStats::copy_sys_us.load(),
                  copy_flt = BbStats::copy_minflt.load();
        static u64 last_proc_flt, last_copy_ns, last_copy_bytes, last_rf, last_trf, last_twf;
        const u64 rf = BbStats::read_faults.load(), trf = BbStats::t_read_faults.load(),
                  twf = BbStats::t_write_faults.load();
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("       fault handlers: %llu read faults %.1f ms, write faults %.1f ms\n",
                        static_cast<unsigned long long>(rf - last_rf), (trf - last_trf) / 1e6,
                        (twf - last_twf) / 1e6);
        }
        last_rf = rf;
        last_trf = trf;
        last_twf = twf;
        const u64 copy_ns = BbStats::t_copy.load(), copy_bytes = BbStats::copy_bytes.load();
        u64 proc_flt = 0;
        if (BbPlatform::Usage usage; BbPlatform::GetUsage(false, usage)) {
            proc_flt = usage.minor_faults;
        }
        const u64 t_now[6] = {BbStats::t_resident.load(), BbStats::t_protect.load(),
                              BbStats::t_image_create.load(), BbStats::t_refresh.load(),
                              BbStats::t_staging.load(), BbStats::t_host_wait.load()};
        static u64 last_draws, last_dispatches, last_subs, last_sys, last_user, last_invol, last_vol;
        const u64 draws = BbStats::draws.load(), dispatches = BbStats::dispatches.load(),
                  subs = BbStats::submissions.load(), sys_us = BbStats::gpu_sys_us.load(),
                  user_us = BbStats::gpu_user_us.load(), invol = BbStats::gpu_invol_switches.load(),
                  vol = BbStats::gpu_vol_switches.load();
        u64 gpu_ns = 0;
        if (const int clock = BbStats::gpu_thread_clock.load(); clock != -1) {
            gpu_ns = BbPlatform::ReadThreadCpuClockNs(clock);
        }
        const u64 images = BbStats::images_registered.load();
        const u64 image_bytes = BbStats::image_upload_bytes.load();
        const u64 buffer_bytes = BbStats::buffer_upload_bytes.load();
        // bbport: whether a long frame built pipelines (a first visit) or loaded (uploads, no
        // compiles): the first thing a hitch report needs (Linux report, 0.9.37 beta 6).
        static u64 last_compile_ns_total;
        static u32 last_compiles_total;
        const u64 compile_ns_total = Vulkan::g_bb_compile_ns_total.load();
        const u32 compiles_total = Vulkan::g_bb_compiles_total.load();
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            const double frame_compile_ms = (compile_ns_total - last_compile_ns_total) / 1e6;
            std::printf("Stall: %.1f ms frame (%s); GPU thread on CPU %.1f ms; %u pipelines "
                        "built, %.1f ms; %llu images registered, %.1f MB image uploads, %.1f MB "
                        "buffer uploads\n",
                        frame_ms,
                        frame_compile_ms >= frame_ms * 0.5 ? "pipelines built"
                        : frame_compile_ms > 0.0           ? "pipelines built, loading"
                                                           : "loading or other work",
                        (gpu_ns - last_gpu_ns) / 1e6, compiles_total - last_compiles_total,
                        frame_compile_ms, static_cast<unsigned long long>(images - last_images),
                        (image_bytes - last_image_bytes) / 1e6,
                        (buffer_bytes - last_buffer_bytes) / 1e6);
            std::printf("       %llu draws, %llu dispatches, %llu submissions; GPU thread user %.1f ms, "
                        "kernel %.1f ms, %llu preempted, %llu waits\n",
                        static_cast<unsigned long long>(draws - last_draws),
                        static_cast<unsigned long long>(dispatches - last_dispatches),
                        static_cast<unsigned long long>(subs - last_subs), (user_us - last_user) / 1e3,
                        (sys_us - last_sys) / 1e3, static_cast<unsigned long long>(invol - last_invol),
                        static_cast<unsigned long long>(vol - last_vol));
        }
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("       ms in: resident %.1f, protect %.1f, image create %.1f, "
                        "image refresh %.1f, staging %.1f, waiting for host copies %.1f\n",
                        (t_now[0] - last_t[0]) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                        (t_now[2] - last_t[2]) / 1e6, (t_now[3] - last_t[3]) / 1e6,
                        (t_now[4] - last_t[4]) / 1e6, (t_now[5] - last_t[5]) / 1e6);
            // bbport: the parts of image create and refresh (texture loading).
            static u64 last_parts[5];
            const u64 parts[5] = {BbStats::t_image_query.load(), BbStats::t_image_alloc.load(),
                                  BbStats::t_refresh_hash.load(), BbStats::t_refresh_obtain.load(),
                                  BbStats::t_refresh_detile.load()};
            std::printf("       of which: format query %.1f, allocation %.1f; hashing %.1f, "
                        "source data %.1f, detiling %.1f\n",
                        (parts[0] - last_parts[0]) / 1e6, (parts[1] - last_parts[1]) / 1e6,
                        (parts[2] - last_parts[2]) / 1e6, (parts[3] - last_parts[3]) / 1e6,
                        (parts[4] - last_parts[4]) / 1e6);
            std::copy(std::begin(parts), std::end(parts), std::begin(last_parts));
            std::printf("       GPU thread page faults %llu (process %llu), protection faults %llu; "
                        "protect calls %llu (%llu pages), of which write-revoking %llu (%llu pages)\n",
                        static_cast<unsigned long long>(minflt - last_minflt),
                        static_cast<unsigned long long>(proc_flt - last_proc_flt),
                        static_cast<unsigned long long>(sigf - last_sigf),
                        static_cast<unsigned long long>(pc - last_pc),
                        static_cast<unsigned long long>(pp - last_pp),
                        static_cast<unsigned long long>(rc - last_rc),
                        static_cast<unsigned long long>(rp - last_rp));
        }
        last_pc = pc;
        last_pp = pp;
        last_rc = rc;
        last_rp = rp;
        last_minflt = minflt;
        if (frame_ms > 40.0 && last_gpu_ns != 0 && copy_ns > last_copy_ns) {
            std::printf("       guest copies %.1f MB in %.1f thread-ms, %.1f ms on CPU (kernel %.1f ms, "
                        "%llu page faults in large copies) (%.2f GB/s per thread)\n",
                        (copy_bytes - last_copy_bytes) / 1e6, (copy_ns - last_copy_ns) / 1e6,
                        (copy_cpu - last_copy_cpu) / 1e6, (copy_sys - last_copy_sys) / 1e3,
                        static_cast<unsigned long long>(copy_flt - last_copy_flt),
                        double(copy_bytes - last_copy_bytes) / double(copy_ns - last_copy_ns));
        }
        // bbport: BB_FRAME_LOG=path (with BB_FRAME_STATS): one line per flip, to find what the
        // long frames (stutter) have in common.
        static FILE* const frame_log = [] () -> FILE* {
            const char* path = std::getenv("BB_FRAME_LOG");
            FILE* file = path && *path ? std::fopen(path, "w") : nullptr;
            if (file) {
                std::fprintf(file, "t_s,frame_ms,present_wait_ms,tick_wait_ms,stage_a_cpu_ms,"
                                   "images,image_mb,buffer_mb,protect_ms,resident_ms,create_ms,"
                                   "refresh_ms,write_fault_ms,read_faults,guest_copy_mb,draws,"
                                   "dispatches,submissions,compiles,preupload_mb,kernel_ms,alloc_mb,"
                                   "free_mb\n");
            }
            return file;
        }();
        if (frame_log) {
            static const auto log_start = now;
            static u64 log_twf, log_tick, log_present, log_compiles, log_rf, log_preupload;
            static u64 log_alloc, log_free;
            const u64 tick = BbStats::tick_wait_ns.load(), present = BbStats::present_wait_ns.load();
            const u64 compiles_now = Vulkan::g_bb_compiles.load();
            // tick_wait_ns and the compile count are reset every stats window.
            const auto delta = [](u64 now_value, u64& last_value) {
                const u64 d = now_value >= last_value ? now_value - last_value : now_value;
                last_value = now_value;
                return d;
            };
            std::fprintf(frame_log,
                         "%.4f,%.2f,%.2f,%.2f,%.2f,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,"
                         "%.2f,%llu,%llu,%llu,%llu,%.2f,%.2f,%.1f,%.1f\n",
                         std::chrono::duration<double>(now - log_start).count(), frame_ms,
                         delta(present, log_present) / 1e6, delta(tick, log_tick) / 1e6,
                         last_gpu_ns ? (gpu_ns - last_gpu_ns) / 1e6 : 0.0,
                         static_cast<unsigned long long>(images - last_images),
                         (image_bytes - last_image_bytes) / 1e6,
                         (buffer_bytes - last_buffer_bytes) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                         (t_now[0] - last_t[0]) / 1e6, (t_now[2] - last_t[2]) / 1e6,
                         (t_now[3] - last_t[3]) / 1e6, delta(twf, log_twf) / 1e6,
                         static_cast<unsigned long long>(delta(rf, log_rf)),
                         (copy_bytes - last_copy_bytes) / 1e6,
                         static_cast<unsigned long long>(draws - last_draws),
                         static_cast<unsigned long long>(dispatches - last_dispatches),
                         static_cast<unsigned long long>(subs - last_subs),
                         static_cast<unsigned long long>(delta(compiles_now, log_compiles)),
                         delta(BbStats::preupload_bytes.load(), log_preupload) / 1e6,
                         last_gpu_ns ? (sys_us - last_sys) / 1e3 : 0.0,
                         delta(BbStats::device_alloc_bytes.load(), log_alloc) / 1e6,
                         delta(BbStats::device_free_bytes.load(), log_free) / 1e6);
            static u32 unflushed = 0;
            if (++unflushed >= 120) {
                std::fflush(frame_log);
                unflushed = 0;
            }
        }
        last_copy_ns = copy_ns;
        last_copy_cpu = copy_cpu;
        last_copy_sys = copy_sys;
        last_copy_flt = copy_flt;
        last_copy_bytes = copy_bytes;
        last_proc_flt = proc_flt;
        last_sigf = sigf;
        std::copy(std::begin(t_now), std::end(t_now), std::begin(last_t));
        last_draws = draws;
        last_dispatches = dispatches;
        last_subs = subs;
        last_sys = sys_us;
        last_user = user_us;
        last_invol = invol;
        last_vol = vol;
        last_gpu_ns = gpu_ns;
        last_images = images;
        last_image_bytes = image_bytes;
        last_buffer_bytes = buffer_bytes;
        last_compile_ns_total = compile_ns_total;
        last_compiles_total = compiles_total;
        ++frames;
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window >= 5.0) {
            const u32 compiles = Vulkan::g_bb_compiles.exchange(0);
            const u64 compile_ns = Vulkan::g_bb_compile_ns.exchange(0);
            const u64 direct = Vulkan::Scheduler::direct_recordings.exchange(0);
            const u64 faults = BbStats::tracker_faults.exchange(0);
            // GPU command thread CPU time per draw: comparable between builds even when the scene
            // (and so the frame rate) differs a little.
            static u64 window_draws = 0, window_gpu_us = 0;
            const u64 all_draws = BbStats::draws.load();
            const u64 gpu_us = BbStats::gpu_user_us.load() + BbStats::gpu_sys_us.load();
            const double us_per_draw = all_draws > window_draws
                                           ? double(gpu_us - window_gpu_us) / (all_draws - window_draws)
                                           : 0.0;
            const double draws_per_frame = frames ? double(all_draws - window_draws) / frames : 0.0;
            window_draws = all_draws;
            window_gpu_us = gpu_us;
            std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                        "%u shader/pipeline compiles, %.1f ms; %llu recorder syncs; "
                        "%.0f write faults/s, %lld hot pages; GPU thread %.2f us/draw, "
                        "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                        "%.1f%% (%.0f/frame, %.0f/frame not needed), copy threads %.1f%%, GPU ticks %.1f%%; "
                        "reduced-size draws %.0f/frame of %.0f in the scene\n",
                        frames / window, worst_ms, EmulatorSettings.GetVblankFrequency(), compiles,
                        compile_ns / 1e6, static_cast<unsigned long long>(direct),
                        faults / window, static_cast<long long>(BbStats::hot_pages.load()),
                        us_per_draw, draws_per_frame,
                        BbStats::gpu_idle_ns.exchange(0) / (window * 1e7),
                        BbStats::sync_recording_ns.exchange(0) / (window * 1e7),
                        BbStats::host_copies_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(BbStats::host_copy_waits.exchange(0)) / frames : 0.0,
                        frames ? double(BbStats::host_copy_waits_skipped.exchange(0)) / frames : 0.0,
                        BbStats::copy_threads_wait_ns.exchange(0) / (window * 1e7),
                        BbStats::tick_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(BbStats::reduced_draws.exchange(0)) / frames : 0.0,
                        frames ? double(BbStats::scene_draws.exchange(0)) / frames : 0.0);
            // bbport: where host copies and deferred fences were waited for (by call site, mostly
            // the draw recording thread's tasks), and the page protection changes of the draw
            // recording thread and of the others (BB_DMA_WAITS, BB_FENCE_ORDER, BB_DEFER_PROTECT).
            {
                const double f = frames ? double(frames) : 1.0;
                const auto per_frame = [&](std::atomic<u64>& counter) {
                    return double(counter.exchange(0)) / f;
                };
                const auto ms_per_frame = [&](std::atomic<u64>& ns) {
                    return double(ns.exchange(0)) / (f * 1e6);
                };
                std::printf("Stage B waits: fences");
                for (u32 s = 0; s < BbStats::WaitSites; ++s) {
                    const double n = per_frame(BbStats::signal_waits[s]);
                    const double ms = ms_per_frame(BbStats::signal_wait_ns[s]);
                    const double skipped = per_frame(BbStats::signal_waits_skipped[s]);
                    if (n + skipped > 0.0) {
                        std::printf(" %s %.1f/frame %.3f ms", BbStats::WaitSiteNames[s], n, ms);
                        if (skipped > 0.0) {
                            std::printf(" (%.1f/frame not needed)", skipped);
                        }
                    }
                }
                std::printf("; copies");
                for (u32 s = 0; s < BbStats::WaitSites; ++s) {
                    const double n = per_frame(BbStats::copy_waits[s]);
                    const double ms = ms_per_frame(BbStats::copy_wait_ns[s]);
                    if (n > 0.0) {
                        std::printf(" %s %.1f/frame %.3f ms", BbStats::WaitSiteNames[s], n, ms);
                    }
                }
                std::printf("; fence barriers %.1f/frame; DMA into memory stored by the CPU "
                            "%.1f/frame, recorded for the GPU %.1f/frame\n",
                            per_frame(BbStats::fence_barriers), per_frame(BbStats::dma_on_cpu),
                            per_frame(BbStats::dma_on_gpu));
                double stage_b_calls = 0.0, stage_b_ms = 0.0;
                std::array<double, BbStats::ProtectKinds> kind_calls{};
                for (u32 k = 0; k < BbStats::ProtectKinds; ++k) {
                    kind_calls[k] = per_frame(BbStats::stage_b_protect_calls[k]);
                    stage_b_calls += kind_calls[k];
                    stage_b_ms += ms_per_frame(BbStats::stage_b_protect_ns[k]);
                }
                const double other_calls = per_frame(BbStats::other_protect_calls);
                const double other_ms = ms_per_frame(BbStats::other_protect_ns);
                const double deferred_pages = per_frame(BbStats::deferred_protect_pages);
                const double deferred_calls = per_frame(BbStats::deferred_protect_calls);
                const double deferred_ms = ms_per_frame(BbStats::deferred_protect_ns);
                std::printf("Stage B protects: %.1f/frame %.3f ms (uploads %.1f, textures %.1f, "
                            "other %.1f); other threads %.1f/frame %.3f ms; deferred %.0f "
                            "pages/frame, applied in %.1f calls/frame %.3f ms (bb:Protect or "
                            "copies), copies that flushed %.1f/frame, cancelled %.0f pages/frame\n",
                            stage_b_calls, stage_b_ms, kind_calls[BbStats::ProtectUpload],
                            kind_calls[BbStats::ProtectTexture], kind_calls[BbStats::ProtectOther],
                            other_calls, other_ms, deferred_pages, deferred_calls, deferred_ms,
                            per_frame(BbStats::copy_flushes),
                            per_frame(BbStats::deferred_cancelled_pages));
            }
            // bbport (docs/pipeline_compiles.md): what draws of pipelines still compiling did.
            {
                auto& p = Vulkan::pipeline_compile_stats;
                const u64 background = p.background.exchange(0), taken = p.taken.exchange(0);
                const u64 skipped = p.skipped_draws.exchange(0);
                const u64 skipped_pipelines = p.skipped_pipelines.exchange(0);
                const u64 late = p.late_binds.exchange(0), waits = p.waits.exchange(0);
                const u64 wait_ns = p.wait_ns.exchange(0), longest = p.longest_wait_ns.exchange(0);
                if (background + skipped + late + waits != 0) {
                    std::printf("Pipelines: %llu compiled in the background (%llu by the thread "
                                "that needed them); %llu draws of %llu pipelines left out while "
                                "compiling; %llu draws/dispatches bound on the recording threads; "
                                "%llu waits, %.1f ms, longest %.1f ms\n",
                                static_cast<unsigned long long>(background),
                                static_cast<unsigned long long>(taken),
                                static_cast<unsigned long long>(skipped),
                                static_cast<unsigned long long>(skipped_pipelines),
                                static_cast<unsigned long long>(late),
                                static_cast<unsigned long long>(waits), wait_ns / 1e6,
                                longest / 1e6);
                }
            }
            VideoCore::PageManager::ReportFaultSites();
            if (const u64 notes = BbStats::cpu_write_notes.exchange(0)) {
                std::printf("Write notices: %.0f/s, %.2f MB/s (libc copies over pages the GPU side "
                            "watches)\n",
                            notes / window, BbStats::cpu_write_note_bytes.exchange(0) / (window * 1e6));
            }
            Vulkan::Breadcrumbs::PrintProgress();
            PrintMemory();
            if (const u64 in_place = BbStats::bound_in_place_bytes.exchange(0)) {
                std::printf("Buffer bindings: %.1f MB/frame in place (%.1f GPU-written), %.1f MB/frame "
                            "VRAM copies\n",
                            in_place / (frames * 1e6),
                            BbStats::bound_in_place_written_bytes.exchange(0) / (frames * 1e6),
                            BbStats::bound_vram_bytes.exchange(0) / (frames * 1e6));
            }
            {
                const u64 updates = BbStats::gpu_data_updates.exchange(0);
                const u64 copied_back = BbStats::copied_back_blocks.exchange(0);
                const u64 readbacks = BbStats::readbacks.exchange(0);
                const u64 late = BbStats::late_vram_writes.exchange(0);
                if (updates + copied_back + readbacks + late != 0) {
                    std::printf("GPU data in VRAM: %.1f command writes/s put into it, %.1f exact writes/s "
                                "(labels, CPU bytes beside GPU data), %.1f blocks/s copied back, "
                                "%.1f readbacks/s\n",
                                updates / window, late / window, copied_back / window,
                                readbacks / window);
                }
            }
            Libraries::Kernel::ReportEqueueWaits(frames);
            if (static int heap_reports = 0; ++heap_reports % 6 == 1) {
                BbHeapSites::Report();
            }
            runtime_wait_report(frames);
            for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
                if (const u64 a = BbStats::range_allocators[i].load()) {
                    std::printf("%s %#llx: %llu live, %.1f MB", i ? ";" : "GPU range allocators:",
                                (unsigned long long)a, (unsigned long long)BbStats::range_live[i].load(),
                                BbStats::range_bytes[i].load() / 1e6);
                }
            }
            if (BbStats::range_allocators[0].load()) {
                std::printf("\n");
            }
            std::printf("Gnm: %.1f sceGnmSubmitDone/frame, %.1f sceGnmAreSubmitsAllowed/frame, %.1f refused/frame; "
                        "guest waited for the previous frame %.1fx %.2f ms/frame; %.1f compute queue submissions/frame\n",
                        BbStats::submit_done_calls.exchange(0) / double(frames),
                        BbStats::submits_allowed_queries.exchange(0) / double(frames),
                        BbStats::submits_refused.exchange(0) / double(frames),
                        BbStats::gnm_frame_waits.exchange(0) / double(frames),
                        BbStats::gnm_frame_wait_ns.exchange(0) / (1e6 * double(frames)),
                        BbStats::asc_submits.exchange(0) / double(frames));
            std::printf("EOP fences: %llu decoded, %llu labels written, %lld pending\n",
                        (unsigned long long)BbStats::eop_decoded.load(),
                        (unsigned long long)BbStats::eop_written.load(),
                        (long long)(BbStats::eop_decoded.load() - BbStats::eop_written.load()));
            if (const u64 early = BbStats::idle_flushes.exchange(0)) {
                std::printf("Honest labels: %.1f submissions/frame sent early (GPU idle)\n",
                            early / double(frames));
            }
            {
                uint64_t sleeps = 0, sleep_ns = 0;
                runtime_sleep_stats(&sleeps, &sleep_ns);
                if (sleeps != 0 && frames != 0) {
                    std::printf("Guest sleeps per frame: %.1f, %.2f ms\n", sleeps / double(frames),
                                sleep_ns / (frames * 1e6));
                }
            }
            if (const u64 copies = BbStats::shadow_copies.exchange(0)) {
                std::printf("Shadow copies: %.0f/frame, %.1f MB/frame (in-place data the CPU writes, "
                            "copied into VRAM for reading)\n",
                            copies / double(frames), BbStats::shadow_bytes.exchange(0) / (frames * 1e6));
            }
            if (const u64 allocs = BbStats::gpu_range_allocs.exchange(0)) {
                std::printf("Guest GPU memory: %.0f ranges/s handed out, %.2f MB/s\n", allocs / window,
                            BbStats::gpu_range_alloc_bytes.exchange(0) / (window * 1e6));
            }
            // bbport: parallel Vulkan recording (vk_scheduler.h): command buffer segments.
            if (const u64 submits = Vulkan::Scheduler::recorded_submissions.exchange(0)) {
                const u64 segments = Vulkan::Scheduler::recorded_segments.exchange(0);
                std::printf("Recording: %.1f submissions/frame, %.2f command buffers each\n",
                            frames ? double(submits) / frames : 0.0, double(segments) / submits);
            }
            BbWaitTrace::Report(window);
            // Frame pacing: spread of the guest flip intervals (judder that the mean hides).
            if (intervals.size() > 2) {
                std::vector<double> sorted = intervals;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2];
                double sum = 0, sq = 0;
                u32 spikes = 0;
                for (const double ms : intervals) {
                    sum += ms;
                    sq += ms * ms;
                    spikes += ms > 1.5 * median;
                }
                const double mean = sum / intervals.size();
                std::printf("Frame pacing: median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                            "%u frames over 1.5x median\n",
                            median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                            sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
            }
            intervals.clear();
            window_start = now;
            frames = 0;
            worst_ms = 0;
        }
    }

    auto* port = req.port;
    // bbport: release the buffer this flip replaced before the game can learn of the flip (flip
    // status, flip events), not after: the game renders into that buffer next and sets its label
    // to 1 again, which a later reset here cleared (the flip IRQ then waited up to 20 ms for a
    // label that never came, video_out.cpp). Frames are prepared at submission, so the buffer
    // is no longer read here.
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    port->prev_index = req.index;

    // Update flip status.
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }
}

void VideoOutDriver::DrawBlankFrame() {
    RunPresenter([this] {
        const auto empty_frame = presenter->PrepareBlankFrame(true);
        presenter->Present(empty_frame, false, false);
    }, true);
}

void VideoOutDriver::DrawLastFrame() {
    RunPresenter([this] {
        const auto frame = presenter->PrepareLastFrame();
        if (frame != nullptr) {
            presenter->Present(frame, true);
        }
    }, true);
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            // bbport: an EOP flip has nobody to return the error to: refusing it ended the game
            // (assertion, exit 23), and dropping it would keep its buffer's label (the game may
            // wait for it). It is queued anyway; frame preparation waits for a free frame.
            if (!is_eop) {
                LOG_ERROR(Lib_VideoOut, "Flip queue is full");
                return false;
            }
            static std::atomic<u32> notes{0};
            if (notes.fetch_add(1, std::memory_order_relaxed) < 4) {
                std::printf("VideoOut: %d flips pending; EOP flip queued anyway\n",
                            port->flip_status.flip_pending_num);
            }
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    {
        std::scoped_lock lock{mutex};
        requests.push({
            .frame = frame,
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
        });
    }
    request_cv.notify_one();
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    // bbport: frame limit (see EmulatorSettings::GetFrameLimit). A request waits in the queue
    // until its slot; slots advance by one period (no drift) but never lag behind by more.
    u32 frame_limit = EmulatorSettings.GetFrameLimit();
    // bbport: uncapped presets never present faster than the display refreshes. With Mailbox
    // (the default) every frame past the refresh rate is rendered in full - scene, upscaler,
    // post process - and then replaced in the mailbox before it is ever shown: GPU work and
    // power for nothing (240 FPS on a 144 Hz screen threw away 40% of it). Fixed presets are
    // left alone (their game speed follows the flip rate), and so is Immediate, chosen for
    // tearing at the lowest latency. BB_CAP_TO_REFRESH=0 turns it off.
    if (EmulatorSettings.IsUncappedVblank() && EmulatorSettings.GetPresentMode() != "Immediate") {
        const char* env = std::getenv("BB_CAP_TO_REFRESH");
        const u32 refresh = Frontend::DisplayRefreshHz();
        if (!(env && env[0] == '0') && refresh >= 30 && (frame_limit == 0 || frame_limit > refresh)) {
            std::printf("VideoOut: frame limit %u FPS lowered to the display's %u Hz "
                        "(BB_CAP_TO_REFRESH=0 keeps it)\n", frame_limit, refresh);
            frame_limit = refresh;
        }
    } else if (const u32 refresh = Frontend::DisplayRefreshHz();
               refresh >= 30 && frame_limit > refresh + 1 &&
               EmulatorSettings.GetPresentMode() == "Mailbox") {
        std::printf("VideoOut: the %u FPS preset is faster than the display's %u Hz: about %u%% "
                    "of the frames are rendered and never shown (a lower FPS preset saves that GPU "
                    "work)\n", frame_limit, refresh, 100 - refresh * 100 / frame_limit);
    }
    auto frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                    : std::chrono::nanoseconds(0);
    // bbport: frame generation's cap (BbSettings::frame_gen_cap, any present mode): the game at
    // half the refresh rate, so its frame and the generated one fill every refresh (with Mailbox
    // a faster game replaces generated frames before they are shown). Applied while generated
    // frames are shown (BbSettings::frame_gen_active), back to the limit above otherwise.
    const u32 base_limit = frame_limit;
    // The frame limit caps the frames shown, generated ones included: the game runs at half of
    // it (a 120 FPS limit is 60 + 60 generated, not 120 + 120). bbport: DLSS Frame Generation
    // shows frame_gen_multiplier frames per game frame (up to 6): the limit and the cap divide by
    // that. Only with the uncapped preset: the others (30, 60, 90, 120, 144) are fixed-timestep
    // patches whose game speed follows the flip rate, and a halved limit ran the game in slow
    // motion with twice the input lag (DLSS-G test, 60 FPS preset at 30).
    static const bool variable_timestep = [] {
        const char* fps = std::getenv("BB_FPS");
        return fps && std::string_view{fps} == "uncap";
    }();
    const auto effective_limit = [&] {
        const auto& s = BbSettings::Get();
        if (!variable_timestep || !s.frame_gen_active.load(std::memory_order_relaxed)) {
            return base_limit;
        }
        const u32 shown = u32(std::clamp(s.frame_gen_multiplier.load(std::memory_order_relaxed), 2,
                                         BbSettings::FrameGenFramesMax + 1));
        u32 limit = base_limit / shown;
        const u32 refresh_share = Frontend::DisplayRefreshHz() / shown;
        if (refresh_share >= 20 && s.frame_gen_cap.load(std::memory_order_relaxed)) {
            limit = limit == 0 ? refresh_share : std::min(limit, refresh_share);
        }
        return limit == 0 ? base_limit : limit;
    };
    auto next_flip = std::chrono::steady_clock::now();
    std::printf("VideoOut: vblank %u Hz, frame limit %u FPS, precise pacing %s\n",
                EmulatorSettings.GetVblankFrequency(), frame_limit,
                LanternPacing::Enabled() ? "on" : "off");

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    // bbport: with a frame limit or the uncapped presets a queued flip is presented as soon as
    // it arrives and its slot allows (no limit: at once), between vblanks, instead of on the
    // next vblank tick.
    const bool immediate_flips = frame_limit != 0 || EmulatorSettings.IsUncappedVblank();

    const auto signal_vblank = [this] {
        auto& vblank_status = main_port.vblank_status;
        // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
        std::scoped_lock lock{main_port.vo_mutex};

        // Trigger flip events for the port
        for (auto event : main_port.vblank_events) {
            auto equeue = Kernel::GetEqueue(event);
            if (equeue != nullptr) {
                equeue->TriggerEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                     Kernel::OrbisKernelEvent::Filter::VideoOut,
                                     reinterpret_cast<void*>(
                                         static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                                         (vblank_status.count << 16)));
            }
        }

        // Update vblank status
        vblank_status.count++;
        vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        main_port.vblank_cv.notify_all();
    };

    // Lantern Engine pacing for immediate flips (lantern_pacing.h; BB_PRECISE_PACING, on by
    // default on Windows). Vblank ticks and flips each follow a cadence of their own: ticks one
    // vblank period apart, flips one pace period apart (the frame limit's, or the vblank period
    // without one). A flip may take its slot a sixteenth of a period early; one that comes more
    // than a quarter period late restarts the flip cadence at itself instead of being followed by
    // a quick catch-up flip, which on a variable refresh display is a visible refresh swing.
    // Flips on vblank ticks without a frame limit keep the AccurateTimer path below.
    if (immediate_flips && LanternPacing::Enabled()) {
        using Clock = LanternPacing::Clock;
        const auto pace_of = [&] { return frame_limit ? frame_period : vblank_period; };
        LanternPacing::Cadence ticks{vblank_period, Clock::now() + vblank_period};
        LanternPacing::Cadence flips{pace_of()};
        // The pace follows frame generation's cap (effective_limit) when it changes.
        const auto update_pace = [&] {
            const u32 limit = effective_limit();
            if (limit == frame_limit) {
                return;
            }
            std::printf("VideoOut: frame limit %u FPS%s\n", limit,
                        limit != base_limit ? " (frame generation: half the frames shown)" : "");
            frame_limit = limit;
            frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                       : std::chrono::nanoseconds(0);
            flips.SetPeriod(pace_of());
        };
        // Flips are timed exactly (a spin of at most an eighth of the vblank period). Ticks only
        // spin without a frame limit: with one they just count time, and a spin before each of
        // 480 ticks a second took a core from the game's threads on 6-8 core CPUs.
        const auto max_spin = std::min<std::chrono::nanoseconds>(std::chrono::microseconds(1500),
                                                                 vblank_period / 8);
        // Flips locked to the display's vblanks (LanternPacing::VblankPacing; BB_VBLANK_PACING=0
        // turns it off): on a fixed refresh display whose refresh is a whole number N of frame
        // limits (60 FPS on 60, 120, 180 or 240 Hz), the flip period is N measured refreshes and
        // each slot sits just past a vblank, so every frame is shown for exactly N refreshes. A
        // cadence of its own drifts against the display's clock and its flips cross vblanks now
        // and then: a frame up a refresh longer, the next one shorter (judder at 60 FPS on 180 Hz).
        // Not with Immediate (tearing, no vblank to wait for) or while frame generation paces the
        // frames it adds; G-Sync and FreeSync refresh on the flips, and the watch reports nothing.
        const bool lock_allowed = LanternPacing::VblankPacing() &&
                                  EmulatorSettings.GetPresentMode() != "Immediate";
        bool locked = false;
        int lock_reports = 0;
        const auto lock_to_vblank = [&] {
            std::optional<LanternPacing::VblankTime> vblank;
            if (lock_allowed && frame_limit &&
                !BbSettings::Get().frame_gen_active.load(std::memory_order_relaxed)) {
                vblank = LanternPacing::Vblank();
            }
            u32 refreshes = 0;
            double hz = 0;
            if (vblank && vblank->period.count() > 0) {
                hz = 1e9 / double(vblank->period.count());
                const double nominal = Frontend::DisplayRefreshHz();
                const double ratio = hz / double(frame_limit);
                const double n = std::round(ratio);
                // The display's own mode (not a variable refresh that happens to sit still), and
                // the limit a whole share of it.
                if (nominal > 0 && std::abs(hz - nominal) < nominal * 0.005 && n >= 1 &&
                    std::abs(ratio - n) < n * 0.01) {
                    refreshes = u32(n);
                }
            }
            if ((refreshes != 0) != locked && lock_reports < 8) {
                ++lock_reports;
                if (refreshes) {
                    std::printf("VideoOut: flips locked to the display's vblanks (%.3f Hz, %u "
                                "refresh%s a frame)\n", hz, refreshes, refreshes == 1 ? "" : "es");
                } else {
                    std::printf("VideoOut: flips keep their own clock (no steady vblanks to lock to)\n");
                }
            }
            if (!refreshes) {
                if (locked) {
                    flips.SetPeriod(pace_of());
                }
                locked = false;
                return;
            }
            locked = true;
            flips.SetPeriod(vblank->period * refreshes);
            // Past the vblank by more than the sixteenth of a period a flip may come early (Ready),
            // so an early flip is not shown a refresh sooner; at most half a refresh.
            const auto offset = std::min<std::chrono::nanoseconds>(
                flips.Period() / 16 + std::chrono::microseconds(300), vblank->period / 2);
            flips.Align(vblank->last, vblank->period, offset);
        };
        const auto take_flip = [&](const Request& request, Clock::time_point now) {
            flips.Taken(now);
            lock_to_vblank();
            Flip(request);
            FRAME_END;
        };

        while (!token.stop_requested()) {
            update_pace();
            const Clock::time_point tick_deadline = ticks.Slot(); // the next tick

            if (DebugState.IsGuestThreadsPaused()) {
                DrawLastFrame();
                LanternPacing::WaitUntil(tick_deadline, !frame_limit, max_spin);
                ticks.Tick(Clock::now());
                continue;
            }

            const auto now = Clock::now();
            if (flips.Ready(now) && main_port.vblank_status.count % (main_port.flip_rate + 1) == 0) {
                const auto request = receive_request();
                if (!request) {
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                } else {
                    take_flip(request, now);
                }
            }

            signal_vblank();

            // Until the next tick: flips as they arrive (a flip rate asks for flips on ticks).
            while (main_port.flip_rate == 0 && !token.stop_requested()) {
                {
                    std::unique_lock lk{mutex};
                    if (!request_cv.wait_until(lk, tick_deadline - max_spin,
                                               [this] { return !requests.empty(); })) {
                        break;
                    }
                }
                if (flips.Slot() - flips.Period() / 16 >= tick_deadline) {
                    break; // its slot is at the next tick or later
                }
                LanternPacing::WaitUntil(flips.Slot() - flips.Period() / 16, true, max_spin);
                if (const auto request = receive_request()) {
                    take_flip(request, Clock::now());
                }
            }
            LanternPacing::WaitUntil(tick_deadline, !frame_limit, max_spin);
            ticks.Tick(Clock::now());
        }
        return;
    }

    while (!token.stop_requested()) {
        timer.Start();
        const auto tick_deadline = std::chrono::steady_clock::now() + vblank_period;
        // bbport: frame generation's cap with VSync (effective_limit).
        if (immediate_flips) {
            if (const u32 limit = effective_limit(); limit != frame_limit) {
                frame_limit = limit;
                frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                           : std::chrono::nanoseconds(0);
            }
        }

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        const auto now = std::chrono::steady_clock::now();
        const bool flip_slot = !frame_limit || now >= next_flip;
        if (flip_slot && vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (request && frame_limit) {
                next_flip = std::max(next_flip + frame_period, now - frame_period);
            }
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        signal_vblank();

        if (!immediate_flips || main_port.flip_rate != 0) {
            timer.End();
            continue;
        }
        while (!token.stop_requested()) {
            {
                std::unique_lock lk{mutex};
                if (!request_cv.wait_until(lk, tick_deadline,
                                           [&] { return !requests.empty(); })) {
                    break; // next vblank
                }
            }
            if (std::chrono::steady_clock::now() < next_flip) {
                if (next_flip >= tick_deadline) {
                    break;
                }
                std::this_thread::sleep_until(next_flip);
            }
            const auto now = std::chrono::steady_clock::now();
            const auto request = receive_request();
            if (request) {
                next_flip = std::max(next_flip + frame_period, now - frame_period);
                Flip(request);
                FRAME_END;
            }
        }
        std::this_thread::sleep_until(tick_deadline);
    }
}

} // namespace Libraries::VideoOut
