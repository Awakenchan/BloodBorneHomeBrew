// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace BbNet {

inline std::uint64_t IpMaskSalt() {
    static const std::uint64_t salt = [] {
        std::random_device rd;
        const std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        return r ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }();
    return salt;
}

inline unsigned IpMaskTag(std::uint64_t value) {
    std::uint64_t z = value ^ IpMaskSalt();
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<unsigned>(z & 0xffffffu);
}

inline std::string MaskIpv4Host(std::uint32_t h) {
    const unsigned a = h >> 24, b = (h >> 16) & 0xffu;
    char text[40];
    if (h == 0 || h == 0xffffffffu || a == 127) {
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a, b, (h >> 8) & 0xffu, h & 0xffu);
        return text;
    }
    const bool keep_b = (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) ||
                        (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254);
    if (keep_b) std::snprintf(text, sizeof(text), "%u.%u.x.x#%06x", a, b, IpMaskTag(h));
    else std::snprintf(text, sizeof(text), "%u.x.x.x#%06x", a, IpMaskTag(h));
    return text;
}

inline std::string MaskIpv4(std::uint32_t net) {
    const auto* p = reinterpret_cast<const unsigned char*>(&net);
    return MaskIpv4Host((std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]);
}

inline std::string MaskIpText(const std::string& text) {
    unsigned a, b, c, d;
    char extra;
    const int n = std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra);
    if ((n == 4 || (n == 5 && extra == ':')) && a < 256 && b < 256 && c < 256 && d < 256) {
        const std::string ip = MaskIpv4Host((a << 24) | (b << 16) | (c << 8) | d);
        return n == 4 ? ip : ip + text.substr(text.find(':'));
    }
    const auto colon = text.find(':');
    if (colon == std::string::npos || text.find(':', colon + 1) == std::string::npos) return text;
    std::uint64_t h = 1469598103934665603ull;
    for (const char ch : text) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    char out[24];
    std::snprintf(out, sizeof(out), "v6#%06x", IpMaskTag(h));
    return out;
}

} // namespace BbNet
