// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera_timing.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

#define GUEST_ABI __attribute__((sysv_abi))

namespace BbEngine::CameraTiming {
namespace {
enum class Take { Rate, Rates, Step, Swap, Restore };

constexpr std::size_t kFrameTime = 0x535a908;
constexpr float kGameStep = 1.0f / 30.0f;

constexpr std::uint8_t kChase[] = {0xc5, 0xf2, 0x5c, 0xca, 0xc5, 0xf2, 0x59, 0xcc, 0xc5, 0xea, 0x58,
                                   0xc9, 0xc4, 0xc1, 0x7a, 0x11, 0x8d, 0x88, 0x01, 0x00, 0x00};
constexpr std::uint8_t kFollow[] = {0xc5, 0xe8, 0x59, 0xd3, 0xc5, 0xd8, 0x58, 0xe2, 0xc4,
                                    0xc1, 0x78, 0x29, 0xa5, 0x90, 0x00, 0x00, 0x00};
constexpr std::uint8_t kLockLoad[] = {0xc4, 0x41, 0x7a, 0x10, 0x95, 0x40, 0x02, 0x00, 0x00, 0xc4, 0xc1, 0x2a, 0x59, 0xc3};
constexpr std::uint8_t kLockDone[] = {0xc5, 0x92, 0x58, 0xc0, 0xc5, 0xf8, 0x50, 0xc0,
                                      0xc1, 0xe0, 0x1f, 0x0d, 0xdb, 0x0f, 0x49, 0x40};
constexpr std::uint8_t kTurn[] = {0xc5, 0xea, 0x5c, 0xe3, 0xc5, 0xf8, 0x50, 0xcc, 0xc1,
                                  0xe1, 0x1f, 0x81, 0xc9, 0xdb, 0x0f, 0x49, 0x40};
constexpr std::uint8_t kPitchBack[] = {0xc4, 0xc1, 0x7a, 0x59, 0x85, 0xa8, 0x01, 0x00, 0x00,
                                       0xc5, 0xf2, 0x58, 0xc0, 0xc5, 0xf8, 0x50, 0xc0};
constexpr std::uint8_t kFocus[] = {0xc5, 0xf8, 0x59, 0xc1, 0xc5, 0xd8, 0x58, 0xc0,
                                   0xc5, 0xf8, 0x28, 0xa5, 0xb0, 0xfd, 0xff, 0xff};
constexpr std::uint8_t kPitch[] = {0xc4, 0xc1, 0x7a, 0x59, 0x85, 0x3c, 0x02, 0x00, 0x00, 0xc5, 0xda,
                                   0x58, 0xd0, 0xc5, 0xfa, 0x11, 0x95, 0xc8, 0xfd, 0xff, 0xff};
constexpr std::uint8_t kPitchLimit[] = {0xc4, 0xc1, 0x72, 0x59, 0x8d, 0x3c, 0x02, 0x00, 0x00,
                                        0xc5, 0xda, 0x58, 0xc9, 0xc5, 0xf8, 0x2e, 0xc1};

struct Site {
    std::size_t offset;
    std::span<const std::uint8_t> code;
    Take take;
    unsigned xmm;
    std::size_t field;
};

constexpr Site kSites[] = {
    {0x143ae7e, kChase, Take::Rate, 4, 0},
    {0x143baa0, kFollow, Take::Rates, 3, 0},
    {0x143c30f, kLockLoad, Take::Swap, 0, 0x240},
    {0x143c31d, kLockDone, Take::Restore, 0, 0x240},
    {0x143c78d, kTurn, Take::Rate, 5, 0},
    {0x143cef0, kPitchBack, Take::Step, 0, 0x1a8},
    {0x143e7ef, kFocus, Take::Rates, 1, 0},
    {0x143f97e, kPitch, Take::Step, 0, 0x23c},
    {0x143fa9f, kPitchLimit, Take::Step, 1, 0x23c},
};
constexpr std::size_t kCount = std::size(kSites);

std::uint8_t* g_image = nullptr;
float g_kept = 0.0f;

float ForFrame(float rate) {
    if (!(rate > 0.0f) || rate >= 1.0f) return rate;
    float frame;
    std::memcpy(&frame, g_image + kFrameTime, sizeof frame);
    if (!(frame > 0.0f) || frame >= kGameStep) return rate;
    return 1.0f - std::pow(1.0f - rate, frame / kGameStep);
}

template <std::size_t N> GUEST_ABI std::int64_t Apply(std::uint64_t camera, const std::uint64_t* saved) {
    const Site& site = kSites[N];
    auto* xmm = reinterpret_cast<float*>(const_cast<std::uint8_t*>(SavedXmm(saved, site.xmm)));
    auto* field = reinterpret_cast<float*>(camera + site.field);
    switch (site.take) {
    case Take::Rate: xmm[0] = ForFrame(xmm[0]); break;
    case Take::Rates: xmm[0] = xmm[1] = xmm[2] = xmm[3] = ForFrame(xmm[0]); break;
    case Take::Step:
        if (*field > 0.0f) xmm[0] *= ForFrame(*field) / *field;
        break;
    case Take::Swap:
        g_kept = *field;
        *field = ForFrame(g_kept);
        break;
    case Take::Restore: *field = g_kept; break;
    }
    return 0;
}

template <std::size_t N> __attribute__((naked)) GUEST_ABI std::int64_t Entry(std::uint64_t, const std::uint64_t*) {
    asm("pushq %%rbp\n\tmovq %%rsp, %%rbp\n\tpushq %%r10\n\tpushq %%r11\n\tandq $-16, %%rsp\n\tsubq $128, %%rsp\n\t"
        "movups %%xmm8, 0(%%rsp)\n\tmovups %%xmm9, 16(%%rsp)\n\tmovups %%xmm10, 32(%%rsp)\n\tmovups %%xmm11, 48(%%rsp)\n\t"
        "movups %%xmm12, 64(%%rsp)\n\tmovups %%xmm13, 80(%%rsp)\n\tmovups %%xmm14, 96(%%rsp)\n\tmovups %%xmm15, 112(%%rsp)\n\t"
        "movq %%r13, %%rdi\n\tleaq %c0(%%rip), %%rax\n\tcallq *%%rax\n\t"
        "movups 0(%%rsp), %%xmm8\n\tmovups 16(%%rsp), %%xmm9\n\tmovups 32(%%rsp), %%xmm10\n\tmovups 48(%%rsp), %%xmm11\n\t"
        "movups 64(%%rsp), %%xmm12\n\tmovups 80(%%rsp), %%xmm13\n\tmovups 96(%%rsp), %%xmm14\n\tmovups 112(%%rsp), %%xmm15\n\t"
        "leaq -16(%%rbp), %%rsp\n\tpopq %%r11\n\tpopq %%r10\n\tpopq %%rbp\n\tretq" : : "i"(&Apply<N>));
}

template <std::size_t... N> constexpr std::array<Callback, kCount> Entries(std::index_sequence<N...>) {
    return {Entry<N>...};
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_CAMERA_TIMING");
    if (on && on[0] == '0') return false;
    if (!image || size < kFrameTime + 4) return false;
    const auto entries = Entries(std::make_index_sequence<kCount>{});
    std::array<Hook, kCount> hooks;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (!Prepare(hooks[i], image, size, kSites[i].offset, kSites[i].code, entries[i])) {
            for (std::size_t j = 0; j < i; ++j) Discard(hooks[j]);
            host_log("Camera timing off: the camera code differs from 1.09");
            return false;
        }
    }
    g_image = image;
    Commit(hooks);
    host_log("Camera timing: on, the camera's smoothing follows the frame time");
    return true;
}
}
