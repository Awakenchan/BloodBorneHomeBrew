// SPDX-License-Identifier: GPL-3.0-or-later
#include <bit>
#include "lifter/lift_core.h"

namespace Lifter {

const Node* Lift_::ToneNode(u32 id, spv::Op op) const {
    if (id == 0 || m.IsConst(id)) {
        return nullptr;
    }
    const Node& node = nodes[m.NodeOf(id)];
    return node.op == u16(op) ? &node : nullptr;
}

const u32* Lift_::ToneExtArgs(u32 id, GLSLstd450 inst) const {
    const Node* node = ToneNode(id, spv::Op::OpExtInst);
    if (!node || args[node->first + 1] != u32(inst)) {
        return nullptr;
    }
    return &args[node->first + 2];
}

bool Lift_::ToneIs(u32 id, float value) const {
    return id != 0 && m.IsConst(id) && m.ConstBits(id) == std::bit_cast<u32>(value);
}

int64_t Lift_::ToneWord(u32 id) const {
    const Node* cast = ToneNode(id, spv::Op::OpBitcast);
    if (!cast) {
        return -1;
    }
    const auto found = tone_words.find(args[cast->first]);
    return found == tone_words.end() ? -1 : int64_t(found->second);
}

namespace {

constexpr u16 O(spv::Op op) {
    return u16(op);
}

template <class A, class B>
bool Both(const u32* pair, A&& a, B&& b) {
    return (a(pair[0]) && b(pair[1])) || (a(pair[1]) && b(pair[0]));
}

} // namespace

bool Lift_::ToneMatchExponential(u32 product_id, ToneChannel& c) const {
    const Node* product = ToneNode(product_id, spv::Op::OpFMul);
    if (!product) {
        return false;
    }
    u32 a_id = 0;
    u32 b_id = 0;
    for (u32 i = 0; i < 2 && !a_id; ++i) {
        const u32 candidate_b = args[product->first + i];
        const Node* inner = ToneNode(args[product->first + 1 - i], spv::Op::OpFMul);
        if (!ToneNode(candidate_b, spv::Op::OpFAdd) || !inner) {
            continue;
        }
        for (u32 j = 0; j < 2; ++j) {
            if (args[inner->first + j] == candidate_b && ToneNode(args[inner->first + 1 - j], spv::Op::OpFSub)) {
                a_id = args[inner->first + 1 - j];
                b_id = candidate_b;
            }
        }
    }
    const Node* a = a_id ? ToneNode(a_id, spv::Op::OpFSub) : nullptr;
    if (!a || !ToneIs(args[a->first], 1.f)) {
        return false;
    }
    const u32* exp = ToneExtArgs(args[a->first + 1], GLSLstd450Exp2);
    if (!exp) {
        return false;
    }
    const u32 exponent = exp[0];
    const Node* scaled_outer = ToneNode(exponent, spv::Op::OpFMul);
    if (!scaled_outer) {
        return false;
    }
    u32 negated_scaled = 0;
    if (!Both(&args[scaled_outer->first], [&](u32 v) { return ToneIs(v, 1.442695f); },
              [&](u32 v) { return ToneNode(v, spv::Op::OpFMul) && (negated_scaled = v); })) {
        return false;
    }
    u32 scene = 0;
    u32 k = 0;
    if (!Both(&args[ToneNode(negated_scaled, spv::Op::OpFMul)->first],
              [&](u32 v) {
                  const Node* neg = ToneNode(v, spv::Op::OpFNegate);
                  return neg && ToneExtArgs(args[neg->first], GLSLstd450FMax) && (scene = args[neg->first]);
              },
              [&](u32 v) { return ToneWord(v) >= 0 && (k = v); })) {
        return false;
    }
    if (!Both(ToneExtArgs(scene, GLSLstd450FMax), [&](u32 v) { return ToneIs(v, 6.1035156e-05f); },
              [](u32) { return true; })) {
        return false;
    }
    u32 shoulder = 0;
    const bool shaped = Both(
        &args[ToneNode(b_id, spv::Op::OpFAdd)->first], [&](u32 v) { return ToneIs(v, 1.f); },
        [&](u32 v) {
            const Node* mul = ToneNode(v, spv::Op::OpFMul);
            return mul && Both(&args[mul->first], [&](u32 w) { return ToneWord(w) >= 0 && (shoulder = w); },
                               [&](u32 w) {
                                   const Node* add = ToneNode(w, spv::Op::OpFAdd);
                                   return add && Both(&args[add->first], [&](u32 u) { return ToneIs(u, -1.f); },
                                                      [&](u32 u) { return u == a_id; });
                               });
        });
    if (!shaped) {
        return false;
    }
    c = {false, scene, exponent, k, shoulder, u32(ToneWord(k)), u32(ToneWord(shoulder))};
    return true;
}

bool Lift_::ToneMatchReinhard(u32 product_id, ToneChannel& c) const {
    const Node* product = ToneNode(product_id, spv::Op::OpFMul);
    if (!product) {
        return false;
    }
    u32 numerator = 0;
    u32 recip = 0;
    if (!Both(&args[product->first], [&](u32 v) { return ToneNode(v, spv::Op::OpFAdd) && (numerator = v); },
              [&](u32 v) {
                  const Node* div = ToneNode(v, spv::Op::OpFDiv);
                  return div && ToneIs(args[div->first], 1.f) && (recip = v);
              })) {
        return false;
    }
    u32 scene = 0;
    u32 k = 0;
    int64_t k_word = -1;
    const auto kx = [&](u32 v) {
        const Node* mul = ToneNode(v, spv::Op::OpFMul);
        if (!mul) {
            return false;
        }
        u32 scene_here = 0;
        u32 k_here = 0;
        const bool is = Both(&args[mul->first],
                             [&](u32 w) { return ToneExtArgs(w, GLSLstd450FMax) && (scene_here = w); },
                             [&](u32 w) { return ToneWord(w) >= 0 && (k_here = w); });
        if (!is || (scene && scene_here != scene) || (k_word >= 0 && ToneWord(k_here) != k_word)) {
            return false;
        }
        scene = scene_here;
        k = k_here;
        k_word = ToneWord(k_here);
        return true;
    };
    const Node* sum = ToneNode(args[ToneNode(recip, spv::Op::OpFDiv)->first + 1], spv::Op::OpFAdd);
    u32 exponent = 0;
    if (!sum || !Both(&args[sum->first], [&](u32 v) { return kx(v) && (exponent = v); },
                      [&](u32 v) { return ToneIs(v, 1.f); })) {
        return false;
    }
    u32 shoulder = 0;
    const bool shaped =
        Both(&args[ToneNode(numerator, spv::Op::OpFAdd)->first], kx, [&](u32 v) {
            const Node* mul = ToneNode(v, spv::Op::OpFMul);
            return mul && Both(&args[mul->first], kx, [&](u32 u) {
                const Node* inner = ToneNode(u, spv::Op::OpFMul);
                return inner && Both(&args[inner->first],
                                     [&](u32 t) { return ToneWord(t) >= 0 && (shoulder = t); }, kx);
            });
        });
    if (!shaped || !Both(ToneExtArgs(scene, GLSLstd450FMax), [&](u32 v) { return ToneIs(v, 6.1035156e-05f); },
                         [](u32) { return true; })) {
        return false;
    }
    c = {true, scene, exponent, k, shoulder, u32(k_word), u32(ToneWord(shoulder))};
    return true;
}

bool Lift_::ToneComposite() const {
    if (tone_channels.size() != 3 || out_blocks.size() != 1) {
        return false;
    }
    const ToneChannel& a = tone_channels[0];
    for (const ToneChannel& c : tone_channels) {
        if (c.reinhard != a.reinhard || c.k_word != a.k_word || c.shoulder_word != a.shoulder_word) {
            return false;
        }
    }
    return a.scene != tone_channels[1].scene && a.scene != tone_channels[2].scene &&
           tone_channels[1].scene != tone_channels[2].scene;
}

void Lift_::ToneTaint(u32 id, std::span<const u32> ids, u32 lits) {
    u8 mask = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (((lits >> i) & 1) == 0) {
            const auto found = tone_taint.find(ids[i]);
            if (found != tone_taint.end()) {
                mask |= found->second;
            }
        }
    }
    if (mask != 0) {
        tone_taint[id] = mask;
    }
}

u32 Lift_::ToneNegateInput(u32 a) {
    for (const u32 select : tone_selects) {
        if (select == a) {
            return Ext(GLSLstd450FMin, t_f32, Ty::F32, {a, CF(1.f)});
        }
    }
    return a;
}

u32 Lift_::ToneSaturate(u32 a) {
    const auto reached = tone_taint.find(a);
    if (reached != tone_taint.end()) {
        u32 any = false_id;
        for (u32 channel = 0; channel < tone_above.size(); ++channel) {
            if ((reached->second >> channel) & 1) {
                any = any == false_id ? tone_above[channel]
                                      : Emit(O(spv::Op::OpLogicalOr), t_bool, Ty::Bool, {any, tone_above[channel]});
            }
        }
        const u32 upper = Emit(O(spv::Op::OpSelect), t_f32, Ty::F32, {any, CF(3.0e38f), CF(1.f)});
        return Ext(GLSLstd450FClamp, t_f32, Ty::F32, {a, CF(0.f), upper});
    }
    const u32 clamp = Ext(GLSLstd450FClamp, t_f32, Ty::F32, {a, CF(0.f), CF(1.f)});
    ToneChannel channel;
    if (!ToneMatchExponential(a, channel) && !ToneMatchReinhard(a, channel)) {
        return clamp;
    }
    tone_channels.push_back(channel);
    if (!in.tone_apply) {
        return clamp;
    }
    return ToneExtend(clamp, channel);
}

u32 Lift_::ToneExtend(u32 clamp, const ToneChannel& c) {
    const auto add = [&](u32 a, u32 b) { return FAdd(a, b); };
    const auto sub = [&](u32 a, u32 b) { return FSub(a, b); };
    const auto mul = [&](u32 a, u32 b) { return FMul(a, b); };
    const auto div = [&](u32 a, u32 b) { return FDiv(a, b); };
    const auto ext = [&](GLSLstd450 inst, std::initializer_list<u32> a) { return Ext(inst, t_f32, Ty::F32, a); };
    const auto max = [&](u32 a, float b) { return ext(GLSLstd450FMax, {a, CF(b)}); };
    const auto clamped = [&](u32 a, float low, float high) { return ext(GLSLstd450FClamp, {a, CF(low), CF(high)}); };
    const auto log2 = [&](u32 a) { return ext(GLSLstd450Log2, {a}); };
    const auto greater = [&](u32 a, u32 b) { return Emit(O(spv::Op::OpFOrdGreaterThan), t_bool, Ty::Bool, {a, b}); };
    const auto select = [&](u32 cond, u32 a, u32 b) { return Emit(O(spv::Op::OpSelect), t_f32, Ty::F32, {cond, a, b}); };
    const auto param = [&](u32 index) {
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_push_f32, Ty::None, {push_block, CU(13), CU(index)});
        return Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
    };
    if (tone_above.empty()) {
        const u32 knee = clamped(param(0), 0.05f, 0.99f);
        const u32 peak = param(1);
        tone_strength = clamped(param(2), 0.f, 1.f);
        tone_on = Emit(O(spv::Op::OpLogicalAnd), t_bool, Ty::Bool, {greater(param(3), CF(0.5f)), greater(peak, CF(1.f))});
        const u32 top = log2(max(peak, 1.f));
        tone_start = mul(top, CF(0.7f));
        tone_room = max(sub(top, tone_start), 1e-6f);
        if (!c.reinhard) {
            u32 e = sub(CF(1.f), knee);
            for (u32 i = 0; i < 5; ++i) {
                const u32 b = sub(CF(1.f), mul(c.shoulder, e));
                const u32 a = sub(CF(1.f), e);
                const u32 value = sub(mul(a, mul(b, b)), knee);
                const u32 derivative = FNeg(add(mul(b, b), mul(CF(2.f), mul(c.shoulder, mul(a, b)))));
                e = clamped(sub(e, div(value, derivative)), 1e-7f, 1.f - 1e-6f);
            }
            const u32 a = sub(CF(1.f), e);
            const u32 b = sub(CF(1.f), mul(c.shoulder, e));
            const u32 y = mul(a, mul(b, b));
            const u32 u = mul(FNeg(log2(e)), CF(0.6931472f));
            tone_z_knee = FNeg(log2(e));
            tone_log_knee = log2(y);
            tone_slope = div(mul(u, mul(e, mul(b, add(b, mul(CF(2.f), mul(c.shoulder, a)))))), y);
        } else {
            const u32 w = max(c.shoulder, 0.f);
            const u32 one_minus = sub(CF(1.f), knee);
            const u32 root = ext(GLSLstd450Sqrt, {add(mul(one_minus, one_minus), mul(CF(4.f), mul(w, knee)))});
            const u32 quadratic = div(sub(root, one_minus), max(mul(CF(2.f), w), 1e-30f));
            const u32 linear = div(knee, one_minus);
            const u32 v = select(greater(w, CF(1e-12f)), quadratic, linear);
            const u32 one_plus = add(CF(1.f), v);
            const u32 y = div(mul(v, add(CF(1.f), mul(w, v))), one_plus);
            const u32 dy = div(sub(mul(add(CF(1.f), mul(CF(2.f), mul(w, v))), one_plus), mul(v, add(CF(1.f), mul(w, v)))),
                               mul(one_plus, one_plus));
            tone_z_knee = v;
            tone_log_knee = log2(y);
            tone_slope = div(mul(v, dy), y);
        }
    }
    const u32 z = c.reinhard ? c.exponent : FNeg(c.exponent);
    const u32 above = Emit(O(spv::Op::OpLogicalAnd), t_bool, Ty::Bool, {tone_on, greater(z, tone_z_knee)});
    const u32 stops = log2(div(max(z, 1e-30f), tone_z_knee));
    const u32 linear = add(tone_log_knee, mul(tone_slope, stops));
    const u32 over = div(sub(linear, tone_start), tone_room);
    const u32 rolled = add(tone_start, mul(tone_room, sub(CF(1.f), ext(GLSLstd450Exp2, {mul(FNeg(over), CF(1.442695f))}))));
    const u32 target = select(greater(linear, tone_start), rolled, linear);
    const u32 game = log2(max(clamp, 1e-30f));
    const u32 mixed = add(game, mul(tone_strength, sub(target, game)));
    const u32 result = select(above, ext(GLSLstd450Exp2, {mixed}), clamp);
    tone_taint[result] = u8(1u << tone_above.size());
    tone_above.push_back(above);
    tone_selects.push_back(result);
    return result;
}

} // namespace Lifter
