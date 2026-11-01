// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace Vulkan::Ggx {

constexpr uint64_t DirectionalHash = 0xd92266b9;
enum class Result { Disabled, Unrelated, Unsupported, Applied };

struct Builder {
    uint32_t next, type, glsl;
    std::vector<uint32_t> words;

    uint32_t Id() { return next++; }
    void Emit(uint32_t op, std::initializer_list<uint32_t> operands) {
        words.push_back((uint32_t(operands.size() + 1) << 16) | op);
        words.insert(words.end(), operands);
    }
    uint32_t Op(uint32_t op, uint32_t a, uint32_t b) {
        const auto id = Id();
        Emit(op, {type, id, a, b});
        return id;
    }
    uint32_t Ext(uint32_t op, std::initializer_list<uint32_t> args) {
        const auto id = Id();
        words.push_back((uint32_t(args.size() + 5) << 16) | 12);
        words.insert(words.end(), {type, id, glsl, op});
        words.insert(words.end(), args);
        return id;
    }
    uint32_t Dot(const std::array<uint32_t, 3>& a, const std::array<uint32_t, 3>& b) {
        const auto x = Op(133, a[0], b[0]), y = Op(133, a[1], b[1]), z = Op(133, a[2], b[2]);
        return Op(129, x, Op(129, y, z));
    }
};

// Only scalar instructions authored for this feature are emitted; no captured module is embedded.
inline Result Patch(uint64_t hash, bool fragment, bool enabled, std::vector<uint32_t>& spv) {
    if (!enabled) return Result::Disabled;
    if (!fragment || hash != DirectionalHash) return Result::Unrelated;
    uint32_t checksum = 2166136261u;
    for (const auto word : spv) checksum = (checksum ^ word) * 16777619u;
    const bool legacy = spv.size() == 7981 && checksum == 0x4c359939;
    const bool current = spv.size() == 9664 && checksum == 0xf9b58c68;
    if ((!legacy && !current) || spv[0] != 0x07230203) return Result::Unsupported;
    const uint32_t offset = current ? 14 : 0;
    uint32_t type = 0, boolean = 0, glsl = 0, zero = 0, one = 0, two = 0, half = 0, sixteen = 0;
    size_t first_function = 0;
    for (size_t i = 5; i < spv.size();) {
        const uint32_t size = spv[i] >> 16, op = spv[i] & 0xffff;
        if (!size || size > spv.size() - i) return Result::Unsupported;
        if (op == 22 && size == 3 && spv[i + 2] == 32) type = spv[i + 1];
        if (op == 20 && size == 2) boolean = spv[i + 1];
        if (op == 11 && size == 6) glsl = spv[i + 1];
        if (op == 43 && size == 4 && spv[i + 1] == type) {
            switch (spv[i + 3]) {
            case 0: zero = spv[i + 2]; break;
            case 0x3f000000: half = spv[i + 2]; break;
            case 0x3f800000: one = spv[i + 2]; break;
            case 0x40000000: two = spv[i + 2]; break;
            case 0x41800000: sixteen = spv[i + 2]; break;
            }
        }
        if (op == 54 && !first_function) first_function = i;
        i += size;
    }
    if (!type || !boolean || !glsl || !zero || !one || !two || !half || !sixteen || !first_function)
        return Result::Unsupported;
    Builder b{spv[3], type, glsl, {}};
    const auto function_type = b.Id(), function = b.Id();
    b.Emit(33, {function_type, type, type, type, type, type, type, type});
    const auto constant = [&](float value) {
        const auto id = b.Id();
        b.Emit(43, {type, id, std::bit_cast<uint32_t>(value)});
        return id;
    };
    const auto minus_two = constant(-2), sqrt_two = constant(1.4142135623730951f);
    const auto maximum_n = constant(8192), minimum_den = constant(1e-20f);
    auto declarations = std::move(b.words);
    b.words.clear();
    b.Emit(54, {type, function, 0, function_type});
    const auto parameter = [&]() {
        const auto id = b.Id();
        b.Emit(55, {type, id});
        return id;
    };
    const auto exponent = parameter(), view = parameter(), light = parameter(), hdot = parameter();
    const auto view_half = parameter(), primary = parameter();
    const auto entry = b.Id(), dark = b.Id(), visible = b.Id(), merge = b.Id();
    b.Emit(248, {entry});
    const auto greater = [&](uint32_t value) {
        const auto id = b.Id();
        b.Emit(186, {boolean, id, value, zero});
        return id;
    };
    const auto front_v = greater(view), front_l = greater(light), front_h = greater(hdot);
    const auto front_vl = b.Id(), front = b.Id();
    b.Emit(167, {boolean, front_vl, front_v, front_l});
    b.Emit(167, {boolean, front, front_vl, front_h});
    b.Emit(247, {merge, 0});
    b.Emit(250, {front, visible, dark});
    b.Emit(248, {dark});
    b.Emit(249, {merge});
    b.Emit(248, {visible});
    const auto n = b.Ext(43, {exponent, one, maximum_n});
    const auto v = b.Ext(37, {view, one}), l = b.Ext(37, {light, one}), h = b.Ext(37, {hdot, one});
    const auto t = b.Op(136, minus_two, n), c2 = b.Ext(29, {t});
    const auto numerator = b.Op(131, one, c2), map_den = b.Op(131, sqrt_two, c2);
    const auto a2 = b.Op(136, numerator, map_den);
    const auto h2 = b.Op(133, h, h), one_h2 = b.Op(131, one, h2), ah2 = b.Op(133, a2, h2);
    const auto dden = b.Op(129, one_h2, ah2), dden2 = b.Op(133, dden, dden);
    const auto d = b.Op(136, a2, dden2), one_a2 = b.Op(131, one, a2);
    const auto v2 = b.Op(133, v, v), l2 = b.Op(133, l, l);
    const auto vpart = b.Op(133, one_a2, v2), lpart = b.Op(133, one_a2, l2);
    const auto vsum = b.Op(129, vpart, a2), lsum = b.Op(129, lpart, a2);
    const auto vroot = b.Ext(31, {vsum}), lroot = b.Ext(31, {lsum});
    const auto lambda_v = b.Op(133, l, vroot), lambda_l = b.Op(133, v, lroot);
    const auto vis_den = b.Op(129, lambda_v, lambda_l);
    const auto vis_safe = b.Ext(40, {vis_den, minimum_den}), vis = b.Op(136, half, vis_safe);
    const auto brdf = b.Op(133, d, vis), raw_lit = b.Op(133, brdf, l);
    const auto c = b.Ext(43, {view_half, zero, one}), c_squared = b.Op(133, c, c);
    const auto reference_part = b.Op(133, one_a2, c_squared);
    const auto reference_sum = b.Op(129, reference_part, a2);
    const auto reference_root = b.Ext(31, {reference_sum});
    const auto reference_n = b.Op(129, n, two), reference_width = b.Op(133, reference_n, a2);
    const auto reference_product = b.Op(133, reference_width, reference_root);
    const auto reference_scale = b.Op(133, reference_product, half);
    const auto gate_wide = b.Op(133, c, sixteen), gate_primary = b.Ext(37, {gate_wide, one});
    const auto is_primary = greater(primary), gate = b.Id();
    b.Emit(169, {type, gate, is_primary, gate_primary, one});
    const auto gain = b.Op(133, reference_scale, gate), lit = b.Op(133, raw_lit, gain);
    b.Emit(249, {merge});
    b.Emit(248, {merge});
    const auto result = b.Id();
    b.Emit(245, {type, result, zero, dark, lit, visible});
    b.Emit(254, {result});
    b.Emit(56, {});
    auto helper = std::move(b.words);
    b.words.clear();
    const auto shifted = [&](std::array<uint32_t, 3> ids) {
        for (auto& id : ids) id += offset;
        return ids;
    };
    const auto normal = shifted({463, 468, 473}), view_vector = shifted({700, 705, 710});
    constexpr std::array<uint32_t, 4> results{964, 965, 1179, 1365};
    constexpr std::array<uint32_t, 4> exponents{952, 953, 1176, 1362};
    constexpr std::array<uint32_t, 4> half_dots{788, 794, 1170, 1356};
    constexpr std::array<std::array<uint32_t, 3>, 4> lights{{
        {512, 513, 514}, {536, 537, 538}, {1066, 1067, 1068}, {1252, 1253, 1254}}};
    constexpr std::array<std::array<uint32_t, 3>, 4> halves{{
        {741, 746, 751}, {772, 777, 782}, {1154, 1159, 1164}, {1340, 1345, 1350}}};
    constexpr std::array<uint32_t, 6> bypass{957, 959, 989, 991, 1206, 1392};
    uint32_t seen = 0, view_dot = 0;
    std::vector<uint32_t> patched(spv.begin(), spv.begin() + 5);
    for (size_t i = 5; i < spv.size(); i += spv[i] >> 16) {
        if (i == first_function)
            patched.insert(patched.end(), declarations.begin(), declarations.end());
        const auto size = spv[i] >> 16;
        const auto op = spv[i] & 0xffff;
        const auto id = size >= 3 ? spv[i + 2] : 0;
        bool replaced = false;
        for (size_t j = 0; j < results.size(); ++j) {
            if (id != results[j] + offset || op != 12 || spv[i + 1] != type || size != 6) continue;
            if (seen & (1u << j)) return Result::Unsupported;
            if (!j) view_dot = b.Dot(normal, view_vector);
            if (!view_dot) return Result::Unsupported;
            const auto light_dot = b.Dot(normal, shifted(lights[j]));
            const auto vh = b.Dot(view_vector, shifted(halves[j]));
            b.Emit(57, {type, id, function, exponents[j] + offset, view_dot, light_dot,
                        half_dots[j] + offset, vh, j < 2 ? one : zero});
            seen |= 1u << j;
            replaced = true;
        }
        for (size_t j = 0; j < bypass.size(); ++j) {
            if (id != bypass[j] + offset || spv[i + 1] != type ||
                (op != 129 && op != 12)) continue;
            if (seen & (1u << (j + 4))) return Result::Unsupported;
            b.Emit(83, {type, id, one});
            seen |= 1u << (j + 4);
            replaced = true;
        }
        if (replaced) {
            patched.insert(patched.end(), b.words.begin(), b.words.end());
            b.words.clear();
        } else {
            patched.insert(patched.end(), spv.begin() + i, spv.begin() + i + size);
        }
    }
    if (seen != 0x3ff) return Result::Unsupported;
    patched.insert(patched.end(), helper.begin(), helper.end());
    patched[3] = b.next;
    spv = std::move(patched);
    return Result::Applied;
}
} // namespace Vulkan::Ggx
