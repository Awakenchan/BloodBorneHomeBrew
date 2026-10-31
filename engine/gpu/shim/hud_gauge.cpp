// SPDX-License-Identifier: GPL-3.0-or-later
#include "hud_gauge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <xxhash.h>
#include <zlib.h>

namespace BbHudGauge {
namespace {
constexpr char kPack[] = "dvdroot_ps4/menu/common.tpf.dcx";
constexpr char16_t kAtlas[] = u"MENU_Common_00091";
constexpr int kAtlasWidth = 1024, kAtlasHeight = 512, kBc7 = 98;
constexpr Piece kFrame{4, 4, 648, 17}, kHealth{10, 26, 638, 9}, kTrail{10, 39, 638, 9}, kStamina{4, 122, 638, 9};
constexpr Piece kPortrait{580, 311, 84, 96}, kFiligree{580, 296, 84, 11}, kRust{669, 312, 83, 96}, kTeal{669, 412, 82, 94};
constexpr Piece kHeader{757, 10, 153, 12};
constexpr int kKeyFloor = 12, kKeyFull = 90;
constexpr std::size_t kMostBytes = 64u << 20;

constexpr int kSharpScale = 2;
std::atomic<int> g_state{0};
std::atomic<bool> g_sharp{false};
Art g_art;

using Bytes = std::vector<std::uint8_t>;

std::uint32_t Be32(const Bytes& b, std::size_t at) {
    return std::uint32_t(b[at]) << 24 | std::uint32_t(b[at + 1]) << 16 | std::uint32_t(b[at + 2]) << 8 | b[at + 3];
}

template <typename T> T Le(const Bytes& b, std::size_t at) {
    T v{};
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

bool Inflate(const Bytes& file, Bytes& out) {
    if (file.size() < 0x4c || std::memcmp(file.data(), "DCX\0", 4) != 0 || std::memcmp(file.data() + 0x28, "DFLT", 4) != 0)
        return false;
    const std::size_t start = 0x44 + Be32(file, 0x48), size = Be32(file, 0x1c);
    if (start >= file.size() || size > kMostBytes) return false;
    out.assign(size, 0);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(file.data() + start);
    z.avail_in = uInt(file.size() - start);
    z.next_out = out.data();
    z.avail_out = uInt(size);
    const int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.avail_out == 0;
}

template <std::size_t N>
bool FindTexture(const Bytes& tpf, const char16_t (&atlas)[N], int want_w, int want_h, std::size_t& data, std::size_t& size) {
    if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0) return false;
    const std::uint32_t count = Le<std::uint32_t>(tpf, 8);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 0x10 + std::size_t(i) * 36;
        if (at + 36 > tpf.size()) return false;
        const std::uint32_t offset = Le<std::uint32_t>(tpf, at), name = Le<std::uint32_t>(tpf, at + 24);
        const std::uint32_t format = Le<std::uint32_t>(tpf, at + 32);
        if (name + sizeof atlas > tpf.size()) continue;
        if (std::memcmp(tpf.data() + name, atlas, sizeof atlas) != 0 || format != kBc7) continue;
        const int width = Le<std::uint16_t>(tpf, at + 12), height = Le<std::uint16_t>(tpf, at + 14);
        data = offset;
        size = Le<std::uint32_t>(tpf, at + 4);
        return width == want_w && height == want_h && data + std::size_t(width) * height <= tpf.size();
    }
    return false;
}

constexpr std::uint16_t kPartition2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8,
    0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110,
    0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696,
    0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720,
    0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22};
constexpr const char* kPartition3[64] = {
    "0011001102210222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122",
    "0011001100220022", "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222",
    "0000111122222222", "0012001200120012", "0112011201120112", "0122012201220122", "0011011211221222",
    "0011200122002220", "0001001101120122", "0111001200220002", "0000112211221122", "0222002200220000",
    "0011012201220122", "0022001100110000", "0112011200000000", "0222002200220000", "0002001100110000",
    "0001001100110000", "0112111112222000", "0000000000002222", "0000000000000222", "0222002200220000",
    "0011120122201222", "0000112201102221", "0000120112201222", "0000200012211220", "0011011100010012",
    "0000000011112211", "0002000200221112", "0022200222002220", "0000000012222122", "0011110111221222",
    "0000111001121222", "0022201102222221", "0000001011121112", "0002000222222222", "0012001200120012",
    "0000111121122112", "0122012000120012", "0101010122220022", "0000101012221222", "0012001200112221",
    "0110010022110222", "0022122200122222", "0112001200120012", "0010101212221222", "0022122200112222",
    "0111012201220122", "0220022002220222", "0022002200220000", "0012001111122000", "0222022222222222",
    "0101222222222222", "0101010101010101", "0000101100002222", "0222112200000000"};
constexpr std::uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
                                       8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
                                       2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};
constexpr std::uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8, 3, 15, 15, 8,  8,  6,  6,  6, 5,  3,  3,  3,  3, 8,  15, 3, 3,
                                        6, 10, 5,  8,  8, 6, 8,  5,  15, 15, 8,  15, 3, 5,  6,  10, 8,  15, 15, 3,  15, 5,
                                        15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5,  10, 5, 10, 8,  13, 15, 12, 3,  3};
constexpr std::uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
                                        15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
                                        3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
constexpr std::uint8_t kWeights2[] = {0, 21, 43, 64}, kWeights3[] = {0, 9, 18, 27, 37, 46, 55, 64},
                       kWeights4[] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
struct Mode {
    int subsets, partition_bits, rotation_bits, selector_bits, color_bits, alpha_bits, endpoint_pbits, shared_pbits,
        index_bits, index2_bits;
};
constexpr Mode kModes[8] = {{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
                            {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
                            {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

struct BitReader {
    const std::uint8_t* data;
    int at = 0;
    int Get(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i, ++at) v |= ((data[at >> 3] >> (at & 7)) & 1) << i;
        return v;
    }
};

const std::uint8_t* Weights(int bits) {
    return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4;
}

int Expand(int v, int bits) {
    return bits >= 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8));
}

void DecodeBc7(const std::uint8_t* block, std::uint32_t out[16]) {
    BitReader r{block};
    int mode = 0;
    while (mode < 8 && !r.Get(1)) ++mode;
    if (mode == 8) {
        std::fill(out, out + 16, 0u);
        return;
    }
    const Mode& m = kModes[mode];
    const int partition = r.Get(m.partition_bits), rotation = r.Get(m.rotation_bits), selector = r.Get(m.selector_bits);
    const int ends = m.subsets * 2;
    int ep[6][4] = {};
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < ends; ++e) ep[e][c] = r.Get(m.color_bits);
    if (m.alpha_bits)
        for (int e = 0; e < ends; ++e) ep[e][3] = r.Get(m.alpha_bits);
    const int channels = m.alpha_bits ? 4 : 3;
    if (m.endpoint_pbits)
        for (int e = 0; e < ends; ++e) {
            const int p = r.Get(1);
            for (int c = 0; c < channels; ++c) ep[e][c] = ep[e][c] << 1 | p;
        }
    if (m.shared_pbits)
        for (int s = 0; s < m.subsets; ++s) {
            const int p = r.Get(1);
            for (int e = 2 * s; e < 2 * s + 2; ++e)
                for (int c = 0; c < channels; ++c) ep[e][c] = ep[e][c] << 1 | p;
        }
    const int pbit = m.endpoint_pbits || m.shared_pbits ? 1 : 0;
    for (int e = 0; e < ends; ++e) {
        for (int c = 0; c < 3; ++c) ep[e][c] = Expand(ep[e][c], m.color_bits + pbit);
        ep[e][3] = m.alpha_bits ? Expand(ep[e][3], m.alpha_bits + pbit) : 255;
    }
    const auto subset = [&](int i) {
        if (m.subsets == 2) return (kPartition2[partition] >> i) & 1;
        if (m.subsets == 3) return kPartition3[partition][i] - '0';
        return 0;
    };
    const auto anchor = [&](int i) {
        if (i == 0) return true;
        if (m.subsets == 2) return i == kAnchor2[partition];
        if (m.subsets == 3) return i == kAnchor3a[partition] || i == kAnchor3b[partition];
        return false;
    };
    int index[16], index2[16] = {};
    for (int i = 0; i < 16; ++i) index[i] = r.Get(m.index_bits - (anchor(i) ? 1 : 0));
    if (m.index2_bits)
        for (int i = 0; i < 16; ++i) index2[i] = r.Get(m.index2_bits - (i == 0 ? 1 : 0));
    for (int i = 0; i < 16; ++i) {
        const int s = subset(i);
        const int* a = ep[2 * s];
        const int* b = ep[2 * s + 1];
        int ci = index[i], ai = index[i], cb = m.index_bits, ab = m.index_bits;
        if (m.index2_bits) {
            ci = selector ? index2[i] : index[i];
            ai = selector ? index[i] : index2[i];
            cb = selector ? m.index2_bits : m.index_bits;
            ab = selector ? m.index_bits : m.index2_bits;
        }
        const int cw = Weights(cb)[ci], aw = Weights(ab)[ai];
        int px[4];
        for (int c = 0; c < 3; ++c) px[c] = (a[c] * (64 - cw) + b[c] * cw + 32) >> 6;
        px[3] = (a[3] * (64 - aw) + b[3] * aw + 32) >> 6;
        if (rotation) std::swap(px[rotation - 1], px[3]);
        out[i] = std::uint32_t(px[0]) | std::uint32_t(px[1]) << 8 | std::uint32_t(px[2]) << 16 | std::uint32_t(px[3]) << 24;
    }
}

int Morton(int x, int y) {
    int r = 0;
    for (int i = 0; i < 3; ++i) r |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
    return r;
}

std::vector<std::uint32_t> Decode(const Bytes& tpf, std::size_t data, int width, const Piece& p) {
    const int tiles_wide = width / 4 / 8;
    std::vector<std::uint32_t> out(std::size_t(p.w) * p.h, 0u);
    std::uint32_t texels[16];
    for (int by = p.y / 4; by <= (p.y + p.h - 1) / 4; ++by) {
        for (int bx = p.x / 4; bx <= (p.x + p.w - 1) / 4; ++bx) {
            const std::size_t tile = std::size_t(by / 8) * tiles_wide + bx / 8;
            DecodeBc7(tpf.data() + data + (tile * 64 + Morton(bx % 8, by % 8)) * 16, texels);
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4 - p.x, y = by * 4 + i / 4 - p.y;
                if (x >= 0 && y >= 0 && x < p.w && y < p.h) out[std::size_t(y) * p.w + x] = texels[i];
            }
        }
    }
    return out;
}

bool ReadPack(const char* game, const char* pack, Bytes& tpf) {
    std::ifstream in(std::filesystem::path(game) / pack, std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return !file.empty() && Inflate(file, tpf);
}

Piece Scaled(const Piece& p, int scale) {
    return Piece{p.x * scale, p.y * scale, p.w * scale, p.h * scale};
}

bool LoadSharp(const Bytes& tpf, std::size_t data, std::size_t size, int width, int height, std::vector<std::uint32_t>& pixels) {
    const char* dir = std::getenv("BB_UI_4K");
    if (!dir || !*dir || data + size > tpf.size()) return false;
    char key[96];
    std::snprintf(key, sizeof key, "%dx%d_%zu_%016llx.bbui", width, height, size,
                  static_cast<unsigned long long>(XXH3_64bits(tpf.data() + data, size)));
    std::ifstream in(std::filesystem::path(dir) / key, std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (file.size() < 12 || std::memcmp(file.data(), "BBUI", 4) != 0) return false;
    std::uint32_t w = 0, h = 0;
    std::memcpy(&w, file.data() + 4, 4);
    std::memcpy(&h, file.data() + 8, 4);
    if (!w || w % width != 0 || w / width < kSharpScale || h * width != w * height) return false;
    Bytes rgba(std::size_t(w) * h * 4);
    uLongf unpacked = uLong(rgba.size());
    if (uncompress(rgba.data(), &unpacked, file.data() + 12, uLong(file.size() - 12)) != Z_OK || unpacked != rgba.size()) return false;
    const int box = int(w) / width / kSharpScale;
    if (box * width * kSharpScale != int(w)) return false;
    const int out_w = width * kSharpScale, out_h = height * kSharpScale;
    pixels.assign(std::size_t(out_w) * out_h, 0u);
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            int sum[4] = {};
            for (int by = 0; by < box; ++by) {
                for (int bx = 0; bx < box; ++bx) {
                    const std::uint8_t* c = rgba.data() + (std::size_t(y * box + by) * w + (x * box + bx)) * 4;
                    for (int i = 0; i < 4; ++i) sum[i] += c[i];
                }
            }
            std::uint32_t& out = pixels[std::size_t(y) * out_w + x];
            for (int i = 0; i < 4; ++i) out |= std::uint32_t(sum[i] / (box * box)) << (8 * i);
        }
    }
    return true;
}

void Load() {
    const char* game = std::getenv("BB_GAME_DIR");
    if (!game || !*game) {
        g_state = -1;
        return;
    }
    Bytes tpf;
    std::size_t data = 0, size = 0;
    int width = kAtlasWidth, height = kAtlasHeight;
    if (!ReadPack(game, kPack, tpf) || !FindTexture(tpf, kAtlas, width, height, data, size)) {
        std::printf("Party HUD: the game's gauge art was not found; drawn bars are used\n");
        g_state = -1;
        return;
    }
    g_art.width = width;
    g_art.height = height;
    int scale = 1;
    if (g_sharp.load() && LoadSharp(tpf, data, size, width, height, g_art.pixels)) {
        scale = kSharpScale;
        width *= scale;
        height *= scale;
        g_art.width = width;
        g_art.height = height;
    } else {
        g_art.pixels = Decode(tpf, data, width, Piece{0, 0, width, height});
    }
    for (const Piece& base : {kHeader, kFiligree}) {
        const Piece p = Scaled(base, scale);
        for (int y = p.y; y < p.y + p.h; ++y) {
            for (int x = p.x; x < p.x + p.w; ++x) {
                std::uint32_t& c = g_art.pixels[std::size_t(y) * width + x];
                const int bright = std::max({int(c & 0xff), int(c >> 8 & 0xff), int(c >> 16 & 0xff)});
                const int alpha = std::clamp((bright - kKeyFloor) * 255 / (kKeyFull - kKeyFloor), 0, int(c >> 24));
                c = (c & 0x00ffffffu) | std::uint32_t(alpha) << 24;
            }
        }
    }
    g_art.frame = Scaled(kFrame, scale);
    g_art.health = Scaled(kHealth, scale);
    g_art.trail = Scaled(kTrail, scale);
    g_art.stamina = Scaled(kStamina, scale);
    g_art.portrait = Scaled(kPortrait, scale);
    g_art.filigree = Scaled(kFiligree, scale);
    g_art.rust = Scaled(kRust, scale);
    g_art.teal = Scaled(kTeal, scale);
    g_art.header = Scaled(kHeader, scale);
    std::printf("Party HUD: the game's own gauge, frame and panel art loaded%s\n", scale > 1 ? " from the 4K textures" : "");
    g_state = 1;
}
}

void Start(bool sharp) {
    int idle = 0;
    if (g_state.load() == 0) g_sharp = sharp;
    if (!g_state.compare_exchange_strong(idle, 2)) return;
    std::thread(Load).detach();
}

const Art* Ready() {
    return g_state.load() == 1 ? &g_art : nullptr;
}

bool InflateDcx(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& out) {
    return Inflate(file, out);
}

void DecodeBc7Block(const std::uint8_t* block, std::uint32_t out[16]) {
    DecodeBc7(block, out);
}

int TileIndex(int x, int y) {
    return Morton(x, y);
}
}
