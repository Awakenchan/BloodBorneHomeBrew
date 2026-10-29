// SPDX-License-Identifier: GPL-3.0-or-later
#include "face_shot.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <zlib.h>

namespace BbFaceShot {
namespace {
constexpr float kFace[4] = {0.633f, 0.139f, 0.831f, 0.491f};
constexpr auto kSettle = std::chrono::milliseconds(1500);
constexpr auto kEvery = std::chrono::seconds(2);
constexpr std::uint8_t kPngMagic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
constexpr int kSameFace = 8;
constexpr double kEmptyMean = 40.0, kEmptySpread = 300.0;
constexpr std::size_t kRgbBytes = std::size_t(kSize) * kSize * 3;

struct Other {
    std::vector<std::uint32_t> rgba;
    std::uint32_t version = 0;
};

std::mutex g_mu;
std::vector<std::uint8_t> g_own, g_pending;
std::uint32_t g_own_version = 0;
std::map<std::string, Other> g_others;
std::atomic<bool> g_due{false};
std::atomic<std::int64_t> g_taken_ms{0};
constexpr std::int64_t kToastMs = 6000;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool g_loaded = false;

std::filesystem::path File() {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    return std::filesystem::path(dir && *dir ? dir : ".") / "party_portrait.png";
}

void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(std::uint8_t(v >> shift));
}

std::uint32_t GetBe32(const std::vector<std::uint8_t>& in, std::size_t at) {
    return std::uint32_t(in[at]) << 24 | std::uint32_t(in[at + 1]) << 16 | std::uint32_t(in[at + 2]) << 8 | in[at + 3];
}

void PutChunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& body) {
    PutBe32(out, std::uint32_t(body.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    PutBe32(out, std::uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
}

std::vector<std::uint8_t> EncodePng(const std::vector<std::uint8_t>& rgb) {
    std::vector<std::uint8_t> raw;
    raw.reserve(std::size_t(kSize) * (kSize * 3 + 1));
    for (int y = 0; y < kSize; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + std::ptrdiff_t(y) * kSize * 3, rgb.begin() + std::ptrdiff_t(y + 1) * kSize * 3);
    }
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<std::uint8_t> packed(size);
    if (compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 9) != Z_OK) return {};
    packed.resize(size);
    std::vector<std::uint8_t> out(std::begin(kPngMagic), std::end(kPngMagic));
    std::vector<std::uint8_t> header;
    PutBe32(header, kSize);
    PutBe32(header, kSize);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    PutChunk(out, "IHDR", header);
    PutChunk(out, "IDAT", packed);
    PutChunk(out, "IEND", {});
    return out;
}

bool DecodePng(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& rgb) {
    if (file.size() < 33 || std::memcmp(file.data(), kPngMagic, sizeof kPngMagic) != 0) return false;
    if (GetBe32(file, 16) != std::uint32_t(kSize) || GetBe32(file, 20) != std::uint32_t(kSize) || file[24] != 8 || file[25] != 2)
        return false;
    std::vector<std::uint8_t> packed;
    for (std::size_t at = 8; at + 12 <= file.size();) {
        const std::uint32_t n = GetBe32(file, at);
        if (at + 12 + n > file.size()) return false;
        if (std::memcmp(file.data() + at + 4, "IDAT", 4) == 0)
            packed.insert(packed.end(), file.begin() + std::ptrdiff_t(at + 8), file.begin() + std::ptrdiff_t(at + 8 + n));
        at += 12 + n;
    }
    std::vector<std::uint8_t> raw(std::size_t(kSize) * (kSize * 3 + 1));
    uLongf size = uLongf(raw.size());
    if (uncompress(raw.data(), &size, packed.data(), uLong(packed.size())) != Z_OK || size != raw.size()) return false;
    rgb.assign(kRgbBytes, 0);
    for (int y = 0; y < kSize; ++y) {
        if (raw[std::size_t(y) * (kSize * 3 + 1)] != 0) return false;
        std::memcpy(rgb.data() + std::size_t(y) * kSize * 3, raw.data() + std::size_t(y) * (kSize * 3 + 1) + 1, kSize * 3);
    }
    return true;
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(File(), std::ios::binary);
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!DecodePng(file, g_own)) {
        g_own.clear();
        return;
    }
    g_own_version = 1;
    std::printf("Party HUD: this hunter's portrait loaded from %s\n", File().string().c_str());
}

void SaveLocked() {
    const std::vector<std::uint8_t> png = EncodePng(g_own);
    if (png.empty()) return;
    std::ofstream out(File(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
}

bool SameFace(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) return false;
    std::uint64_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff += std::uint64_t(std::abs(int(a[i]) - int(b[i])));
    return diff / a.size() < std::uint64_t(kSameFace);
}
}

void Tick(bool preview_shown) {
    static std::chrono::steady_clock::time_point opened, asked;
    static bool shown = false;
    const auto now = std::chrono::steady_clock::now();
    if (preview_shown && !shown) opened = now;
    if (preview_shown && now - opened >= kSettle && now - asked >= kEvery) {
        asked = now;
        g_due = true;
    }
    if (!preview_shown && shown) {
        std::lock_guard lock(g_mu);
        LoadLocked();
        if (!g_pending.empty() && SameFace(g_pending, g_own)) {
            std::printf("Party HUD: the appearance did not change; the saved portrait is kept\n");
        } else if (!g_pending.empty()) {
            g_own = std::move(g_pending);
            ++g_own_version;
            SaveLocked();
            g_taken_ms = NowMs();
            std::printf("Party HUD: this hunter's portrait taken from the appearance screen and saved to %s\n", File().string().c_str());
        }
        g_pending.clear();
    }
    shown = preview_shown;
}

bool Pending() {
    return g_due.load();
}

bool Due(float rect[4]) {
    if (!g_due.exchange(false)) return false;
    std::memcpy(rect, kFace, sizeof kFace);
    return true;
}

void Deliver(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride, bool bgra) {
    if (!pixels || !width || !height) return;
    std::vector<std::uint8_t> rgb(kRgbBytes);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const std::uint32_t x0 = x * width / kSize, x1 = std::max(x0 + 1, (x + 1) * width / kSize);
            const std::uint32_t y0 = y * height / kSize, y1 = std::max(y0 + 1, (y + 1) * height / kSize);
            std::uint32_t sum[3] = {}, n = 0;
            for (std::uint32_t sy = y0; sy < y1 && sy < height; ++sy) {
                for (std::uint32_t sx = x0; sx < x1 && sx < width; ++sx) {
                    const std::uint8_t* p = pixels + std::size_t(sy) * stride + std::size_t(sx) * 4;
                    sum[0] += p[bgra ? 2 : 0];
                    sum[1] += p[1];
                    sum[2] += p[bgra ? 0 : 2];
                    ++n;
                }
            }
            for (int c = 0; c < 3; ++c) rgb[(std::size_t(y) * kSize + x) * 3 + c] = std::uint8_t(n ? sum[c] / n : 0);
        }
    }
    std::uint64_t sum = 0, sum2 = 0;
    for (const std::uint8_t v : rgb) {
        sum += v;
        sum2 += std::uint64_t(v) * v;
    }
    const double mean = double(sum) / double(rgb.size()), spread = double(sum2) / double(rgb.size()) - mean * mean;
    if (mean < kEmptyMean || spread < kEmptySpread) {
        std::printf("Party HUD: the appearance preview showed no hunter (mean %.0f, spread %.0f); nothing taken\n", mean, spread);
        return;
    }
    std::lock_guard lock(g_mu);
    g_pending = std::move(rgb);
}

bool JustTaken() {
    const std::int64_t at = g_taken_ms.load();
    return at && NowMs() - at < kToastMs;
}

std::uint32_t OwnVersion() {
    std::lock_guard lock(g_mu);
    LoadLocked();
    return g_own.size() == kRgbBytes ? g_own_version : 0;
}

bool OwnRgba(std::vector<std::uint32_t>& rgba, std::uint32_t& version) {
    std::lock_guard lock(g_mu);
    LoadLocked();
    if (g_own.size() != kRgbBytes) return false;
    if (version != g_own_version) {
        rgba.resize(std::size_t(kSize) * kSize);
        for (std::size_t i = 0; i < rgba.size(); ++i)
            rgba[i] = std::uint32_t(g_own[i * 3]) | std::uint32_t(g_own[i * 3 + 1]) << 8 | std::uint32_t(g_own[i * 3 + 2]) << 16 |
                      0xff000000u;
    }
    version = g_own_version;
    return true;
}

bool OwnPacked(std::vector<std::uint8_t>& out, std::uint32_t& version) {
    std::lock_guard lock(g_mu);
    LoadLocked();
    if (g_own.size() != kRgbBytes) return false;
    uLongf size = compressBound(uLong(g_own.size()));
    out.resize(size);
    if (compress2(out.data(), &size, g_own.data(), uLong(g_own.size()), 6) != Z_OK) return false;
    out.resize(size);
    version = g_own_version;
    return true;
}

void Receive(const std::string& name, const std::vector<std::uint8_t>& packed) {
    std::vector<std::uint8_t> rgb(kRgbBytes);
    uLongf size = uLongf(rgb.size());
    if (name.empty() || uncompress(rgb.data(), &size, packed.data(), uLong(packed.size())) != Z_OK || size != rgb.size()) return;
    std::vector<std::uint32_t> rgba(std::size_t(kSize) * kSize);
    for (std::size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = std::uint32_t(rgb[i * 3]) | std::uint32_t(rgb[i * 3 + 1]) << 8 | std::uint32_t(rgb[i * 3 + 2]) << 16 | 0xff000000u;
    std::lock_guard lock(g_mu);
    Other& other = g_others[name];
    if (other.rgba == rgba) return;
    other.rgba = std::move(rgba);
    ++other.version;
    std::printf("Party HUD: %s's portrait came in\n", name.c_str());
}

bool Get(const std::string& name, std::vector<std::uint32_t>& rgba, std::uint32_t& version) {
    std::lock_guard lock(g_mu);
    const auto it = g_others.find(name);
    if (it == g_others.end()) return false;
    if (it->second.version != version) rgba = it->second.rgba;
    version = it->second.version;
    return true;
}
}
