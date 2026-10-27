// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Vulkan::Pom {

// Per-shader ids of the game's parallax occlusion mapping loop (found by disassembling the
// original modules). Each uv phi is paired with the view-ray offset it steps along.
struct PomShader {
    uint64_t hash;
    size_t words;
    uint32_t checksum;
    uint32_t layer_phi;
    std::array<uint32_t, 4> uv_phi;
    std::array<uint32_t, 4> uv_dir;
    uint32_t step_id;
    uint32_t steps_convert;
    uint32_t preheader;
};

inline constexpr std::array<PomShader, 3> kShaders{{
    {0x053bbf3b, 8006, 0xa573c2dc, 741, {739, 740, 0, 0}, {642, 644, 0, 0}, 734, 732, 118},
    {0x505acc58, 8252, 0x95faa9a2, 718, {716, 717, 0, 0}, {645, 646, 0, 0}, 712, 710, 120},
    {0x08ccd4c7, 10337, 0x64e132cc, 993, {989, 990, 991, 992}, {904, 905, 907, 908}, 985, 983,
     126},
}};

constexpr uint32_t kOpEntryPoint = 15, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23,
                   kOpTypePointer = 32, kOpConstant = 43, kOpFunction = 54, kOpVariable = 59,
                   kOpLoad = 61, kOpDecorate = 71, kOpCompositeExtract = 81, kOpBitcast = 124,
                   kOpFAdd = 129, kOpFSub = 131, kOpFMul = 133, kOpPhi = 245, kOpLabel = 248,
                   kOpBranch = 249, kOpConvertFToS = 110, kOpExtInst = 12, kOpExtInstImport = 11;

inline uint32_t Word(uint32_t op, uint32_t len) {
    return (len << 16) | op;
}

// Ids for darkening the albedo of pixels that sit deep inside the parallax relief. The depth
// is the ray fraction of the refined hit (0 on the surface, 1 at full depth).
struct Cavity {
    uint32_t hash;
    uint32_t depth;          // refined hit depth
    uint32_t pred_hit;       // block that carries the depth into the merge block
    uint32_t pred_skip;      // block that bypasses the POM
    uint32_t merge_a;        // block with the phis fed by pred_hit/pred_skip
    uint32_t merge_b;        // block after merge_a, with a second pair of phis
    uint32_t skip_b;         // predecessor of merge_b other than merge_a
    std::array<uint32_t, 6> values; // stored colour channels to scale
};

inline constexpr std::array<Cavity, 1> kCavities{{
    {0x053bbf3b, 800, 131, 117, 132, 134, 133, {1348, 1350, 1352, 1359, 1360, 1361}},
}};

inline void PatchCavity(const PomShader& shader, std::vector<uint32_t>& spv) {
    const Cavity* cavity = nullptr;
    for (const auto& c : kCavities) {
        if (c.hash == shader.hash) {
            cavity = &c;
        }
    }
    const char* env = std::getenv("BB_POM_CAVITY");
    const float strength = env ? static_cast<float>(std::atof(env)) : 0.5f;
    if (!cavity || strength <= 0.0f) {
        return;
    }
    uint32_t f32 = 0, glsl = 0, zero = 0;
    size_t first_function = 0, first_store = 0, phi_a_end = 0, phi_b_end = 0;
    bool in_a = false, in_b = false;
    for (size_t i = 5; i < spv.size(); i += spv[i] >> 16) {
        const uint32_t op = spv[i] & 0xffff;
        if (op == kOpTypeFloat && spv[i + 2] == 32) {
            f32 = spv[i + 1];
        } else if (op == kOpExtInstImport) {
            glsl = spv[i + 1];
        } else if (op == kOpConstant && f32 && spv[i + 1] == f32 && spv[i + 3] == 0 && !zero) {
            zero = spv[i + 2];
        } else if (op == kOpFunction && !first_function) {
            first_function = i;
        } else if (op == kOpLabel) {
            in_a = spv[i + 1] == cavity->merge_a;
            in_b = spv[i + 1] == cavity->merge_b;
        } else if (op == kOpPhi) {
            if (in_a) {
                phi_a_end = i + (spv[i] >> 16);
            } else if (in_b) {
                phi_b_end = i + (spv[i] >> 16);
            }
        } else if (op == 62 && !first_store && first_function) { // OpStore
            first_store = i;
        }
    }
    if (!f32 || !glsl || !zero || !first_function || !first_store || !phi_a_end || !phi_b_end ||
        !(phi_a_end < phi_b_end && phi_b_end < first_store)) {
        return;
    }
    // Rewrite the colour stores first; they sit after every insertion point.
    uint32_t next_id = spv[3];
    const uint32_t c_k = next_id++, c_one = next_id++, phi_a = next_id++, phi_b = next_id++,
                   clamped = next_id++, scaled = next_id++, factor = next_id++;
    std::vector<uint32_t> stores;
    for (size_t i = first_store; i < spv.size(); i += spv[i] >> 16) {
        if ((spv[i] & 0xffff) != 62) {
            continue;
        }
        for (const uint32_t value : cavity->values) {
            if (spv[i + 2] == value) {
                stores.push_back(static_cast<uint32_t>(i));
            }
        }
    }
    if (stores.size() != cavity->values.size()) {
        return;
    }
    std::vector<uint32_t> ins;
    ins.insert(ins.end(), {Word(kOpExtInst, 8), f32, clamped, glsl, 43, phi_b, zero, c_one});
    ins.insert(ins.end(), {Word(kOpFMul, 5), f32, scaled, clamped, c_k});
    ins.insert(ins.end(), {Word(kOpFSub, 5), f32, factor, c_one, scaled});
    for (size_t k = 0; k < stores.size(); k++) {
        const uint32_t id = next_id++;
        ins.insert(ins.end(), {Word(kOpFMul, 5), f32, id, spv[stores[k] + 2], factor});
        spv[stores[k] + 2] = id;
    }
    const std::array<uint32_t, 7> phi_b_ins{Word(kOpPhi, 7), f32, phi_b, zero, cavity->skip_b,
                                            phi_a, cavity->merge_a};
    const std::array<uint32_t, 7> phi_a_ins{Word(kOpPhi, 7), f32, phi_a, cavity->depth,
                                            cavity->pred_hit, zero, cavity->pred_skip};
    spv.insert(spv.begin() + first_store, ins.begin(), ins.end());
    spv.insert(spv.begin() + phi_b_end, phi_b_ins.begin(), phi_b_ins.end());
    spv.insert(spv.begin() + phi_a_end, phi_a_ins.begin(), phi_a_ins.end());
    std::vector<uint32_t> globals{Word(kOpConstant, 4), f32, c_k, std::bit_cast<uint32_t>(strength),
                                  Word(kOpConstant, 4), f32, c_one, std::bit_cast<uint32_t>(1.0f)};
    spv.insert(spv.begin() + first_function, globals.begin(), globals.end());
    spv[3] = next_id;
}

// Replaces the fixed start of the POM ray march with a per-pixel dithered start, so the
// stair-step banding of the discrete layers turns into noise that the temporal upscaler
// averages out, and takes 1.5x as many steps. Only the exact original modules are patched.
inline void PatchPom(uint64_t pgm_hash, bool is_fragment, std::vector<uint32_t>& spv) {
    if (!is_fragment || std::getenv("BB_POM_ORIGINAL")) {
        return;
    }
    const PomShader* shader = nullptr;
    for (const auto& s : kShaders) {
        if (s.hash == pgm_hash && s.words == spv.size()) {
            shader = &s;
        }
    }
    if (!shader) {
        return;
    }
    uint32_t checksum = 2166136261u;
    for (const uint32_t word : spv) {
        checksum = (checksum ^ word) * 16777619u;
    }
    if (checksum != shader->checksum) {
        return;
    }

    uint32_t f32 = 0, u32 = 0, vec4 = 0, ptr_vec4 = 0, glsl = 0;
    size_t entry = 0, first_decorate = 0, first_function = 0, convert = 0, terminator = 0;
    size_t layer_phi = 0;
    std::array<size_t, 4> uv_phi{};
    bool in_preheader = false;
    for (size_t i = 5; i < spv.size(); i += spv[i] >> 16) {
        const uint32_t op = spv[i] & 0xffff;
        switch (op) {
        case kOpEntryPoint:
            entry = i;
            break;
        case kOpTypeFloat:
            if (spv[i + 2] == 32) {
                f32 = spv[i + 1];
            }
            break;
        case kOpTypeInt:
            if (spv[i + 2] == 32 && spv[i + 3] == 0) {
                u32 = spv[i + 1];
            }
            break;
        case kOpTypeVector:
            if (f32 && spv[i + 2] == f32 && spv[i + 3] == 4 && !vec4) {
                vec4 = spv[i + 1];
            }
            break;
        case kOpTypePointer:
            if (vec4 && spv[i + 2] == 1 && spv[i + 3] == vec4 && !ptr_vec4) {
                ptr_vec4 = spv[i + 1];
            }
            break;
        case kOpExtInstImport:
            glsl = spv[i + 1];
            break;
        case kOpDecorate:
            if (!first_decorate) {
                first_decorate = i;
            }
            break;
        case kOpFunction:
            if (!first_function) {
                first_function = i;
            }
            break;
        case kOpConvertFToS:
            if (spv[i + 2] == shader->steps_convert) {
                convert = i;
            }
            break;
        case kOpPhi:
            if (spv[i + 2] == shader->layer_phi) {
                layer_phi = i;
            }
            for (size_t k = 0; k < 4; k++) {
                if (shader->uv_phi[k] && spv[i + 2] == shader->uv_phi[k]) {
                    uv_phi[k] = i;
                }
            }
            break;
        case kOpLabel:
            in_preheader = spv[i + 1] == shader->preheader;
            break;
        case kOpBranch:
            if (in_preheader && !terminator) {
                terminator = i;
            }
            in_preheader = false;
            break;
        default:
            break;
        }
    }
    size_t uv_count = 0;
    for (size_t k = 0; k < 4; k++) {
        if (shader->uv_phi[k]) {
            if (!uv_phi[k]) {
                return;
            }
            uv_count++;
        }
    }
    if (!f32 || !u32 || !vec4 || !ptr_vec4 || !glsl || !entry || !first_decorate ||
        !first_function || !convert || !terminator || !layer_phi || !uv_count ||
        !(convert < terminator && terminator < layer_phi) || spv[layer_phi + 3] == 0) {
        return;
    }

    uint32_t next_id = spv[3];
    const auto alloc = [&] { return next_id++; };
    const uint32_t c_steps = alloc(), c_a = alloc(), c_b = alloc(), c_c = alloc(), c_one = alloc(),
                   frag_coord = alloc();
    const std::array<std::pair<uint32_t, float>, 5> constants{{{c_steps, 1.5f},
                                                               {c_a, 0.06711056f},
                                                               {c_b, 0.00583715f},
                                                               {c_c, 52.9829189f},
                                                               {c_one, 1.0f}}};

    // Instructions that run before the step count is converted to an integer.
    const uint32_t steps_f = spv[convert + 3];
    const uint32_t steps_scaled = alloc();
    const std::array<uint32_t, 5> steps_ins{Word(kOpFMul, 5), f32, steps_scaled, steps_f, c_steps};

    // Instructions at the end of the preheader block.
    std::vector<uint32_t> ins;
    const auto emit = [&](std::initializer_list<uint32_t> words) {
        ins.push_back(Word(static_cast<uint32_t>(*words.begin()), static_cast<uint32_t>(words.size())));
        ins.insert(ins.end(), words.begin() + 1, words.end());
    };
    const uint32_t fc = alloc(), x = alloc(), y = alloc(), ax = alloc(), ay = alloc(), s = alloc(),
                   f1 = alloc(), m = alloc(), jitter = alloc(), off = alloc(), layer_f = alloc(),
                   layer_u = alloc();
    emit({kOpLoad, vec4, fc, frag_coord});
    emit({kOpCompositeExtract, f32, x, fc, 0});
    emit({kOpCompositeExtract, f32, y, fc, 1});
    emit({kOpFMul, f32, ax, x, c_a});
    emit({kOpFMul, f32, ay, y, c_b});
    emit({kOpFAdd, f32, s, ax, ay});
    emit({kOpExtInst, f32, f1, glsl, 10, s});
    emit({kOpFMul, f32, m, f1, c_c});
    emit({kOpExtInst, f32, jitter, glsl, 10, m});
    emit({kOpFMul, f32, off, jitter, shader->step_id});
    emit({kOpFSub, f32, layer_f, c_one, off});
    emit({kOpBitcast, u32, layer_u, layer_f});
    std::array<uint32_t, 4> uv_new{};
    for (size_t k = 0; k < uv_count; k++) {
        const uint32_t moved = alloc(), start = alloc();
        uv_new[k] = start;
        emit({kOpFMul, f32, moved, off, shader->uv_dir[k]});
        emit({kOpFSub, f32, start, spv[uv_phi[k] + 3], moved});
    }

    // The phi operand pairs are (value, parent); the initial value comes from the preheader.
    const auto set_initial = [&](size_t phi, uint32_t value) {
        const size_t end = phi + (spv[phi] >> 16);
        for (size_t p = phi + 3; p + 1 < end; p += 2) {
            if (spv[p + 1] == shader->preheader) {
                spv[p] = value;
                return true;
            }
        }
        return false;
    };
    bool ok = set_initial(layer_phi, layer_u);
    for (size_t k = 0; k < uv_count; k++) {
        ok = set_initial(uv_phi[k], uv_new[k]) && ok;
    }
    if (!ok) {
        return;
    }
    spv[convert + 3] = steps_scaled;

    // Insert from the back so earlier positions stay valid.
    spv.insert(spv.begin() + terminator, ins.begin(), ins.end());
    spv.insert(spv.begin() + convert, steps_ins.begin(), steps_ins.end());
    std::vector<uint32_t> globals;
    for (const auto& [id, value] : constants) {
        globals.insert(globals.end(), {Word(kOpConstant, 4), f32, id, std::bit_cast<uint32_t>(value)});
    }
    globals.insert(globals.end(), {Word(kOpVariable, 4), ptr_vec4, frag_coord, 1u});
    spv.insert(spv.begin() + first_function, globals.begin(), globals.end());
    const std::array<uint32_t, 4> decorate{Word(kOpDecorate, 4), frag_coord, 11u, 15u};
    spv.insert(spv.begin() + first_decorate, decorate.begin(), decorate.end());
    const size_t entry_end = entry + (spv[entry] >> 16);
    spv[entry] += 1u << 16;
    spv.insert(spv.begin() + entry_end, frag_coord);
    spv[3] = next_id;
    PatchCavity(*shader, spv);
}

} // namespace Vulkan::Pom
