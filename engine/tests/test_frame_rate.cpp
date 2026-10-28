// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/frame_rate.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace BbEngine {
void host_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
}
}

using namespace BbEngine::FrameRate;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

float FloatAt(const std::uint8_t* image, std::uint32_t offset) {
    float f;
    std::memcpy(&f, image + offset, 4);
    return f;
}
std::int32_t IntAt(const std::uint8_t* image, std::uint32_t offset) {
    std::int32_t i;
    std::memcpy(&i, image + offset, 4);
    return i;
}

void SetFloat(std::uint8_t* image, std::uint32_t offset, float f) { std::memcpy(image + offset, &f, 4); }

std::uint8_t* Image(std::size_t size) {
#ifdef _WIN32
    auto* image = static_cast<std::uint8_t*>(_aligned_malloc(size, 0x1000));
#else
    auto* image = static_cast<std::uint8_t*>(std::aligned_alloc(0x1000, size));
#endif
    std::memset(image, 0, size);
    std::memcpy(image + kFrameTimeCave, kFrameTimeCaveCode, sizeof kFrameTimeCaveCode);
    for (const Site& s : kSites) {
        if (s.checked) std::memcpy(image + s.offset - 4, s.prefix, 4);
        const std::uint32_t bits = Value(s.kind, 60.0f);
        std::memcpy(image + s.offset, &bits, 4);
    }
    return image;
}

const Site* First(SiteKind kind, bool code) {
    for (const Site& s : kSites)
        if (&s != &kSites[0] && s.kind == kind && s.code == code) return &s;
    return nullptr;
}
}

int main() {
    const std::size_t size = 0x5400000;
    Check(std::fabs(BbEngine::FrameRate::HeldRate()) < 1e-6f, "nothing held before install");
    std::uint8_t* blank = static_cast<std::uint8_t*>(std::calloc(size, 1));
    Check(!BbEngine::FrameRate::Install(blank, size), "no frame-rate list: not installed");
    std::free(blank);

    std::uint8_t* image = Image(size);
    const Site* code_step = First(SiteKind::Step, true);
    const Site* data_step = First(SiteKind::Step, false);
    const Site* sixth = First(SiteKind::CountSixth, true);
    const Site* ease = First(SiteKind::EaseDistance, false);
    Check(code_step && data_step && sixth && ease, "the table has code steps, data steps, a count and an ease");
    std::memset(image + kSites[0].offset - 4, 0xcc, kSites[0].checked ? 4 : 0);

#ifdef _WIN32
    _putenv_s("BB_FPS", "120");
#else
    setenv("BB_FPS", "120", 1);
#endif
    Check(BbEngine::FrameRate::Install(image, size), "installed over a 60 FPS list");
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 120) < 1e-7f, "code step at 1/120");
    Check(std::fabs(FloatAt(image, data_step->offset) - 1.0f / 120) < 1e-7f, "data step at 1/120");
    Check(IntAt(image, sixth->offset) == 20, "a sixth of a second is 20 frames at 120");
    Check(std::fabs(FloatAt(image, ease->offset) - float(1.0 - std::pow(0.9, 0.25))) < 1e-6f, "distance ease for 120");
    if (kSites[0].checked)
        Check(std::fabs(FloatAt(image, kSites[0].offset) - 1.0f / 60) < 1e-7f, "a site whose code differs is left");

    SetFloat(image, kFrameTime, 1.0f / 60);
    BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, data_step->offset) - 1.0f / 60) < 1e-7f, "data step follows the frame time at once");
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 120) < 1e-7f, "code step waits for a held rate");
    for (int i = 0; i < 400; ++i) BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 60) < 1e-6f, "code step follows a held 60");
    Check(IntAt(image, sixth->offset) == 10, "the count follows a held 60");
    Check(std::fabs(BbEngine::FrameRate::HeldRate() - 60.0f) < 1.0f, "held rate is 60");

    SetFloat(image, kFrameTime, 1.0f / 20);
    for (int i = 0; i < 400; ++i) BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, data_step->offset) - 1.0f / 30) < 1e-7f, "no step longer than 1/30");

    SetFloat(image, kFrameTime, 1.0f / 240);
    for (int i = 0; i < 400; ++i) BbEngine::FrameRate::Tick();
    Check(std::fabs(FloatAt(image, code_step->offset) - 1.0f / 120) < 1e-6f, "never past the target");

    std::uint8_t* fresh = static_cast<std::uint8_t*>(std::calloc(size, 1));
    for (const ListWrite& w : kListWrites)
        if (w.list == Ninety || w.list == Sprint) std::memcpy(fresh + w.offset, kListBlob + w.at, w.size);
    g_target = 120.0f;
    g_uncapped = false;
    ApplyLists(fresh, size);
    bool all = true;
    for (const ListWrite& w : kListWrites)
        if ((w.list == Ninety || w.list == Sprint) && std::memcmp(fresh + w.offset, kListBlob + w.at + w.size, w.size) != 0)
            all = false;
    Check(all, "the engine writes the 90 FPS code and the sprint fix over the game's own bytes");
    Check(std::memcmp(fresh + kFrameTimeCave, kFrameTimeCaveCode, sizeof kFrameTimeCaveCode) == 0,
          "the frame-time code is in place after the engine's writes");
    std::free(fresh);

    if (failures) return 1;
    std::puts("frame rate: ok");
    return 0;
}
