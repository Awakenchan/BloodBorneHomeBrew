// SPDX-License-Identifier: GPL-3.0-or-later
#include "lifter/lift_core.h"

namespace Lifter {

namespace {

bool IsBranch(u16 op) {
    return op == Op::SOPP_S_BRANCH || op == Op::SOPP_S_CBRANCH_SCC0 ||
           op == Op::SOPP_S_CBRANCH_SCC1 || op == Op::SOPP_S_CBRANCH_VCCZ ||
           op == Op::SOPP_S_CBRANCH_VCCNZ || op == Op::SOPP_S_CBRANCH_EXECZ ||
           op == Op::SOPP_S_CBRANCH_EXECNZ;
}

} // namespace

void Lift_::BuildCfg() {
    std::vector<u32> labels{0};
    for (size_t i = 0; i < insts.size(); ++i) {
        const Inst& inst = insts[i];
        if (inst.enc == Enc::Sopp && (IsBranch(inst.op) || inst.op == Op::SOPP_S_ENDPGM)) {
            labels.push_back(inst.next);
            if (IsBranch(inst.op)) {
                labels.push_back(u32(s32(inst.next) + inst.simm * 4));
            }
        }
    }
    std::sort(labels.begin(), labels.end());
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    std::map<u32, u32> inst_at;
    for (size_t i = 0; i < insts.size(); ++i) {
        inst_at[insts[i].pc] = u32(i);
    }
    const u32 end_pc = insts.empty() ? 0 : insts.back().next;
    for (size_t l = 0; l < labels.size(); ++l) {
        const u32 pc = labels[l];
        if (pc >= end_pc) {
            continue;
        }
        const auto it = inst_at.find(pc);
        if (it == inst_at.end()) {
            Fail("branch into an instruction");
        }
        Block block;
        block.start = it->second;
        const u32 stop = l + 1 < labels.size() ? labels[l + 1] : end_pc;
        const auto stop_it = inst_at.find(stop);
        block.end = stop_it == inst_at.end() ? u32(insts.size()) : stop_it->second;
        block_at[pc] = u32(blocks.size());
        blocks.push_back(std::move(block));
    }
    for (Block& block : blocks) {
        const Inst& last = insts[block.end - 1];
        if (last.enc == Enc::Sopp && last.op == Op::SOPP_S_ENDPGM) {
            block.is_exit = true;
            continue;
        }
        if (last.enc == Enc::Sopp && IsBranch(last.op)) {
            block.branch_op = last.op;
            const u32 target = u32(s32(last.next) + last.simm * 4);
            const auto it = block_at.find(target);
            if (it == block_at.end()) {
                Fail("branch outside the program");
            }
            block.taken = s32(it->second);
            if (last.op == Op::SOPP_S_BRANCH) {
                continue;
            }
            const auto fall = block_at.find(last.next);
            if (fall == block_at.end()) {
                Fail("conditional branch at the end");
            }
            block.fall = s32(fall->second);
            if (block.fall == block.taken) {
                block.fall = -1;
                block.branch_op = Op::SOPP_S_BRANCH;
            }
            continue;
        }
        const auto fall = block_at.find(last.next);
        if (fall == block_at.end()) {
            block.is_exit = true;
        } else {
            block.taken = s32(fall->second);
        }
    }
    out.stats.blocks = u32(blocks.size());
}

void Lift_::Dominators() {
    const u32 n = u32(blocks.size());
    std::vector<u32> post;
    std::vector<u8> seen(n, 0);
    std::vector<std::pair<u32, u8>> stack{{0, 0}};
    seen[0] = 1;
    while (!stack.empty()) {
        auto& [b, step] = stack.back();
        const s32 next = step == 0 ? blocks[b].taken : step == 1 ? blocks[b].fall : -2;
        if (next == -2) {
            post.push_back(b);
            stack.pop_back();
            continue;
        }
        ++step;
        if (next >= 0 && !seen[next]) {
            seen[next] = 1;
            stack.push_back({u32(next), 0});
        }
    }
    std::vector<u32> rpo(post.rbegin(), post.rend());
    for (u32 i = 0; i < rpo.size(); ++i) {
        blocks[rpo[i]].order = i;
    }
    for (u32 b : rpo) {
        for (const s32 succ : {blocks[b].taken, blocks[b].fall}) {
            if (succ >= 0) {
                blocks[succ].pred_list.push_back(b);
            }
        }
    }
    blocks[0].idom = 0;
    const auto intersect = [&](s32 a, s32 b) {
        while (a != b) {
            while (blocks[a].order > blocks[b].order) {
                a = blocks[a].idom;
            }
            while (blocks[b].order > blocks[a].order) {
                b = blocks[b].idom;
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 1; i < rpo.size(); ++i) {
            const u32 b = rpo[i];
            s32 idom = -1;
            for (const u32 p : blocks[b].pred_list) {
                if (blocks[p].idom >= 0) {
                    idom = idom < 0 ? s32(p) : intersect(idom, s32(p));
                }
            }
            if (idom != blocks[b].idom) {
                blocks[b].idom = idom;
                changed = true;
            }
        }
    }
    const s32 exit_node = s32(n);
    std::vector<s32> ipdom(n + 1, -1);
    std::vector<u32> post_order(n + 1, 0);
    std::vector<std::vector<u32>> rpreds(n + 1);
    for (u32 b = 0; b < n; ++b) {
        if (!seen[b]) {
            continue;
        }
        if (blocks[b].is_exit) {
            rpreds[n].push_back(b);
        }
        for (const s32 succ : {blocks[b].taken, blocks[b].fall}) {
            if (succ >= 0) {
                rpreds[succ].push_back(b);
            }
        }
    }
    std::vector<u32> rpost;
    std::vector<u8> rseen(n + 1, 0);
    std::vector<std::pair<u32, u32>> rstack{{n, 0}};
    rseen[n] = 1;
    while (!rstack.empty()) {
        auto& [b, step] = rstack.back();
        if (step >= rpreds[b].size()) {
            rpost.push_back(b);
            rstack.pop_back();
            continue;
        }
        const u32 next = rpreds[b][step++];
        if (!rseen[next]) {
            rseen[next] = 1;
            rstack.push_back({next, 0});
        }
    }
    std::vector<u32> rrpo(rpost.rbegin(), rpost.rend());
    for (u32 i = 0; i < rrpo.size(); ++i) {
        post_order[rrpo[i]] = i;
    }
    ipdom[n] = exit_node;
    const auto pintersect = [&](s32 a, s32 b) {
        while (a != b) {
            while (post_order[a] > post_order[b]) {
                a = ipdom[a];
            }
            while (post_order[b] > post_order[a]) {
                b = ipdom[b];
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 1; i < rrpo.size(); ++i) {
            const u32 b = rrpo[i];
            s32 best = -1;
            const auto consider = [&](s32 succ) {
                if (succ >= 0 && rseen[succ] && ipdom[succ] >= 0) {
                    best = best < 0 ? succ : pintersect(best, succ);
                }
            };
            if (blocks[b].is_exit) {
                consider(exit_node);
            }
            consider(blocks[b].taken);
            consider(blocks[b].fall);
            if (best != ipdom[b]) {
                ipdom[b] = best;
                changed = true;
            }
        }
    }
    for (u32 b = 0; b < n; ++b) {
        blocks[b].ipdom = ipdom[b] == exit_node ? -1 : ipdom[b];
    }
    for (u32 b = 0; b < n; ++b) {
        for (const s32 succ : {blocks[b].taken, blocks[b].fall}) {
            if (succ < 0 || !seen[b]) {
                continue;
            }
            s32 walk = s32(b);
            while (walk != succ && blocks[walk].idom != walk && blocks[walk].idom >= 0) {
                walk = blocks[walk].idom;
            }
            if (blocks[succ].order <= blocks[b].order && walk != succ) {
                Fail("irreducible control flow");
            }
        }
    }
}

u32 Lift_::Emit(u16 op, u32 type, Ty ty, std::initializer_list<u32> a, u32 lits) {
    const u32 id = m.NewId(ty);
    Node node;
    node.op = op;
    node.type = type;
    node.result = id;
    node.nargs = u16(a.size());
    node.first = u32(args.size());
    node.lits = lits;
    args.insert(args.end(), a.begin(), a.end());
    const u32 index = u32(nodes.size());
    nodes.push_back(node);
    m.BindNode(id, index);
    out_blocks[cur].nodes.push_back(index);
    if (tess_on) {
        TagFrom(id, a);
    }
    if (!tone_taint.empty()) {
        ToneTaint(id, std::span<const u32>(a.begin(), a.size()), lits);
    }
    return id;
}

void Lift_::EmitVoid(u16 op, std::initializer_list<u32> a, u32 lits) {
    Node node;
    node.op = op;
    node.nargs = u16(a.size());
    node.first = u32(args.size());
    node.lits = lits;
    node.effect = true;
    args.insert(args.end(), a.begin(), a.end());
    const u32 index = u32(nodes.size());
    nodes.push_back(node);
    out_blocks[cur].nodes.push_back(index);
}

void Lift_::Terminate(u16 op, std::initializer_list<u32> a) {
    EmitVoid(op, a);
}

u32 Lift_::Ext(GLSLstd450 inst, u32 type, Ty ty, std::initializer_list<u32> a) {
    const u32 id = m.NewId(ty);
    Node node;
    node.op = u16(spv::Op::OpExtInst);
    node.type = type;
    node.result = id;
    node.nargs = u16(a.size() + 2);
    node.first = u32(args.size());
    node.lits = 2;
    args.push_back(glsl);
    args.push_back(inst);
    args.insert(args.end(), a.begin(), a.end());
    const u32 index = u32(nodes.size());
    nodes.push_back(node);
    m.BindNode(id, index);
    out_blocks[cur].nodes.push_back(index);
    if (!tone_taint.empty()) {
        ToneTaint(id, std::span<const u32>(a.begin(), a.size()), 0);
    }
    return id;
}

u32 Lift_::ToF(u32 id) {
    if (id == 0) {
        return CF(0.f);
    }
    switch (m.TyOf(id)) {
    case Ty::F32:
        return id;
    case Ty::U32: {
        if (m.IsConst(id)) {
            return m.Const(Ty::F32, t_f32, m.ConstBits(id));
        }
        const Node& node = nodes[m.NodeOf(id)];
        if (node.op == u16(spv::Op::OpBitcast) && m.TyOf(args[node.first]) == Ty::F32) {
            return args[node.first];
        }
        return Emit(u16(spv::Op::OpBitcast), t_f32, Ty::F32, {id});
    }
    case Ty::Bool:
        return ToF(BoolToU(id));
    default:
        Fail("value of another type read as a float");
    }
}

u32 Lift_::ToU(u32 id) {
    if (id == 0) {
        return CU(0);
    }
    switch (m.TyOf(id)) {
    case Ty::U32:
        return id;
    case Ty::F32: {
        if (m.IsConst(id)) {
            return CU(m.ConstBits(id));
        }
        const Node& node = nodes[m.NodeOf(id)];
        if (node.op == u16(spv::Op::OpBitcast) && m.TyOf(args[node.first]) == Ty::U32) {
            return args[node.first];
        }
        return Emit(u16(spv::Op::OpBitcast), t_u32, Ty::U32, {id});
    }
    case Ty::Bool:
        return BoolToU(id);
    default:
        Fail("value of another type read as an integer");
    }
}

u32 Lift_::BoolToU(u32 id) {
    if (IsTrue(id)) {
        return CU(1);
    }
    if (IsFalse(id)) {
        return CU(0);
    }
    return Emit(u16(spv::Op::OpSelect), t_u32, Ty::U32, {id, CU(1), CU(0)});
}

u32 Lift_::FloatOp(u16 op, u32 a, u32 b, bool contract_rule) {
    const u32 id = Emit(op, t_f32, Ty::F32, {a, b});
    if (contract_rule) {
        nodes[m.NodeOf(id)].nocontract = true;
    }
    return id;
}

u32 Lift_::FAdd(u32 a, u32 b) {
    return FloatOp(u16(spv::Op::OpFAdd), a, b);
}
u32 Lift_::FSub(u32 a, u32 b) {
    return FloatOp(u16(spv::Op::OpFSub), a, b);
}
u32 Lift_::FMul(u32 a, u32 b) {
    return FloatOp(u16(spv::Op::OpFMul), a, b);
}
u32 Lift_::FDiv(u32 a, u32 b) {
    return FloatOp(u16(spv::Op::OpFDiv), a, b);
}
u32 Lift_::FFma(u32 a, u32 b, u32 c) {
    const u32 id = Ext(GLSLstd450Fma, t_f32, Ty::F32, {a, b, c});
    nodes[m.NodeOf(id)].nocontract = true;
    return id;
}
u32 Lift_::FNeg(u32 a) {
    if (!tone_selects.empty()) {
        a = ToneNegateInput(a);
    }
    if (m.IsConst(a)) {
        return m.Const(Ty::F32, t_f32, m.ConstBits(a) ^ 0x80000000u);
    }
    const Node& node = nodes[m.NodeOf(a)];
    if (node.op == u16(spv::Op::OpFNegate)) {
        return args[node.first];
    }
    return Emit(u16(spv::Op::OpFNegate), t_f32, Ty::F32, {a});
}
u32 Lift_::FAbs(u32 a) {
    if (m.IsConst(a)) {
        return m.Const(Ty::F32, t_f32, m.ConstBits(a) & 0x7fffffffu);
    }
    return Ext(GLSLstd450FAbs, t_f32, Ty::F32, {a});
}
u32 Lift_::FSat(u32 a) {
    if (in.tone_curve) {
        return ToneSaturate(a);
    }
    return Ext(GLSLstd450FClamp, t_f32, Ty::F32, {a, CF(0.f), CF(1.f)});
}

u32 Lift_::IOp(u16 op, u32 a, u32 b) {
    {
        u32 x = 1;
        u32 y = 1;
        const bool known_a = KnownValue(a, x);
        const bool known_b = KnownValue(b, y);
        if (op == u16(spv::Op::OpIMul) && ((known_a && x == 0) || (known_b && y == 0))) {
            return MaskOf(a) || MaskOf(b) ? TaggedConst(0, MaskOf(a) + MaskOf(b)) : CU(0);
        }
        if (op == u16(spv::Op::OpIAdd)) {
            if (known_a && x == 0 && !MaskOf(a)) {
                return b;
            }
            if (known_b && y == 0 && !MaskOf(b)) {
                return a;
            }
        }
    }
    if (tess_on && (MaskOf(a) || MaskOf(b))) {
        u32 x = 0;
        u32 y = 0;
        if (KnownValue(a, x) && KnownValue(b, y)) {
            u32 r = 0;
            switch (spv::Op(op)) {
            case spv::Op::OpIAdd:
                r = x + y;
                break;
            case spv::Op::OpISub:
                r = x - y;
                break;
            case spv::Op::OpIMul:
                r = x * y;
                break;
            case spv::Op::OpBitwiseAnd:
                r = x & y;
                break;
            case spv::Op::OpBitwiseOr:
                r = x | y;
                break;
            case spv::Op::OpBitwiseXor:
                r = x ^ y;
                break;
            case spv::Op::OpShiftLeftLogical:
                r = y < 32 ? x << y : 0;
                break;
            case spv::Op::OpShiftRightLogical:
                r = y < 32 ? x >> y : 0;
                break;
            default:
                return Emit(op, t_u32, Ty::U32, {a, b});
            }
            return TaggedConst(r, MaskOf(a) + MaskOf(b));
        }
    }
    if (m.IsConst(a) && m.IsConst(b)) {
        const u32 x = m.ConstBits(a);
        const u32 y = m.ConstBits(b);
        switch (spv::Op(op)) {
        case spv::Op::OpIAdd:
            return CU(x + y);
        case spv::Op::OpISub:
            return CU(x - y);
        case spv::Op::OpIMul:
            return CU(x * y);
        case spv::Op::OpBitwiseAnd:
            return CU(x & y);
        case spv::Op::OpBitwiseOr:
            return CU(x | y);
        case spv::Op::OpBitwiseXor:
            return CU(x ^ y);
        case spv::Op::OpShiftLeftLogical:
            return CU(y < 32 ? x << y : 0);
        case spv::Op::OpShiftRightLogical:
            return CU(y < 32 ? x >> y : 0);
        default:
            break;
        }
    }
    return Emit(op, t_u32, Ty::U32, {a, b});
}

u32 Lift_::ICmp(u16 op, u32 a, u32 b) {
    return Emit(op, t_bool, Ty::Bool, {a, b});
}

u32 Lift_::FCmp(u16 op, u32 a, u32 b) {
    return Emit(op, t_bool, Ty::Bool, {a, b});
}

u32 Lift_::Select(u32 cond, u32 a, u32 b) {
    if (IsTrue(cond)) {
        return a;
    }
    if (IsFalse(cond)) {
        return b;
    }
    if (a == b) {
        return a;
    }
    const Ty ty = m.TyOf(a);
    if (ty == Ty::Bool) {
        if (IsTrue(a) && IsFalse(b)) {
            return cond;
        }
        if (IsFalse(a) && IsTrue(b)) {
            return Not(cond);
        }
        return Emit(u16(spv::Op::OpSelect), t_bool, ty, {cond, a, b});
    }
    if (ty == Ty::F32) {
        return Emit(u16(spv::Op::OpSelect), t_f32, ty, {cond, a, ToF(b)});
    }
    if (ty == Ty::V4F) {
        return Emit(u16(spv::Op::OpSelect), t_v4f, ty, {cond, a, b});
    }
    return Emit(u16(spv::Op::OpSelect), t_u32, Ty::U32, {cond, ToU(a), ToU(b)});
}

u32 Lift_::Not(u32 a) {
    if (IsTrue(a)) {
        return false_id;
    }
    if (IsFalse(a)) {
        return true_id;
    }
    const Node& node = nodes[m.NodeOf(a)];
    if (node.op == u16(spv::Op::OpLogicalNot)) {
        return args[node.first];
    }
    return Emit(u16(spv::Op::OpLogicalNot), t_bool, Ty::Bool, {a});
}

u32 Lift_::And(u32 a, u32 b) {
    if (IsFalse(a) || IsFalse(b)) {
        return false_id;
    }
    if (IsTrue(a) || a == b) {
        return b;
    }
    if (IsTrue(b)) {
        return a;
    }
    if (Not(a) == b && !m.IsConst(a)) {
        return false_id;
    }
    return Emit(u16(spv::Op::OpLogicalAnd), t_bool, Ty::Bool, {a, b});
}

u32 Lift_::Or(u32 a, u32 b) {
    if (IsTrue(a) || IsTrue(b)) {
        return true_id;
    }
    if (IsFalse(a) || a == b) {
        return b;
    }
    if (IsFalse(b)) {
        return a;
    }
    if (!m.IsConst(a)) {
        const Node& node = nodes[m.NodeOf(a)];
        if (node.op == u16(spv::Op::OpLogicalNot) && args[node.first] == b) {
            return true_id;
        }
    }
    if (!m.IsConst(b)) {
        const Node& node = nodes[m.NodeOf(b)];
        if (node.op == u16(spv::Op::OpLogicalNot) && args[node.first] == a) {
            return true_id;
        }
    }
    return Emit(u16(spv::Op::OpLogicalOr), t_bool, Ty::Bool, {a, b});
}

u32 Lift_::Xor(u32 a, u32 b) {
    if (IsFalse(a)) {
        return b;
    }
    if (IsFalse(b)) {
        return a;
    }
    if (IsTrue(a)) {
        return Not(b);
    }
    if (IsTrue(b)) {
        return Not(a);
    }
    if (a == b) {
        return false_id;
    }
    return Emit(u16(spv::Op::OpLogicalNotEqual), t_bool, Ty::Bool, {a, b});
}

u32 Lift_::BfeU(u32 base, u32 offset, u32 count) {
    if (hull_packed != 0 && base == hull_packed && m.IsConst(offset) && m.IsConst(count)) {
        const u32 o = m.ConstBits(offset);
        const u32 c = m.ConstBits(count);
        if (o == 0 && c == 8) {
            return CU(0);
        }
        if (o == 8 && c == 5) {
            return in.tess.passthrough ? CU(0) : InvocationId();
        }
        Fail("other use of the hull invocation register");
    }
    if (tess_on && MaskOf(base)) {
        u32 x = 0;
        if (KnownValue(base, x) && m.IsConst(offset) && m.IsConst(count)) {
            const u32 o = m.ConstBits(offset);
            const u32 c = m.ConstBits(count);
            return TaggedConst(c == 0 ? 0 : (x >> o) & (c >= 32 ? ~0u : (1u << c) - 1), MaskOf(base));
        }
    }
    if (m.IsConst(base) && m.IsConst(offset) && m.IsConst(count)) {
        const u32 o = m.ConstBits(offset);
        const u32 c = m.ConstBits(count);
        const u32 mask = c >= 32 ? ~0u : (1u << c) - 1;
        return CU(c == 0 ? 0 : (m.ConstBits(base) >> o) & mask);
    }
    return Emit(u16(spv::Op::OpBitFieldUExtract), t_u32, Ty::U32, {base, offset, count});
}

u32 Lift_::BfeS(u32 base, u32 offset, u32 count) {
    if (tess_on && MaskOf(base)) {
        u32 x = 0;
        if (KnownValue(base, x) && m.IsConst(offset) && m.IsConst(count)) {
            const u32 o = m.ConstBits(offset);
            const u32 c = m.ConstBits(count);
            return TaggedConst(c == 0 ? 0 : u32(s32(x << (32 - o - c)) >> (32 - c)), MaskOf(base));
        }
    }
    if (m.IsConst(base) && m.IsConst(offset) && m.IsConst(count)) {
        const u32 o = m.ConstBits(offset);
        const u32 c = m.ConstBits(count);
        if (c == 0) {
            return CU(0);
        }
        return CU(u32(s32(m.ConstBits(base) << (32 - o - c)) >> (32 - c)));
    }
    return Emit(u16(spv::Op::OpBitFieldSExtract), t_u32, Ty::U32, {base, offset, count});
}

u32 Lift_::ReadReg(u32 reg) {
    return s.r[reg];
}

void Lift_::WriteReg(u32 reg, u32 value) {
    s.r[reg] = value;
}

u32 Lift_::SrcRaw(u32 code, const Inst& inst) {
    if (code < 128) {
        if (code == ExecReg) {
            return BoolToU(Exec());
        }
        if (code == ExecReg + 1) {
            return CU(0);
        }
        const u32 value = s.r[code];
        if (value == 0) {
            return CU(0);
        }
        return value;
    }
    if (code >= SrcVgpr) {
        return s.r[code];
    }
    if (code == SrcZero) {
        return CU(0);
    }
    if (code >= 129 && code <= 192) {
        return CU(code - 128);
    }
    if (code >= 193 && code <= 208) {
        return CU(u32(192 - s32(code)));
    }
    switch (code) {
    case 240:
        return CF(0.5f);
    case 241:
        return CF(-0.5f);
    case 242:
        return CF(1.f);
    case 243:
        return CF(-1.f);
    case 244:
        return CF(2.f);
    case 245:
        return CF(-2.f);
    case 246:
        return CF(4.f);
    case 247:
        return CF(-4.f);
    case SrcLiteral:
        return CU(inst.literal);
    case SrcScc:
        return BoolToU(s.r[RegScc]);
    case 251:
        return BoolToU(Not(Mask(SrcVcc, inst)));
    case 252:
        return BoolToU(Not(Exec()));
    default:
        Fail("source operand " + std::to_string(code));
    }
}

u32 Lift_::SrcRawF(u32 code, const Inst& inst) {
    return ToF(SrcRaw(code, inst));
}

u32 Lift_::SrcF(const Inst& inst, u32 index) {
    u32 value = ToF(SrcRaw(inst.src[index], inst));
    if (inst.abs & (1 << index)) {
        value = FAbs(value);
    }
    if (inst.neg & (1 << index)) {
        value = FNeg(value);
    }
    return value;
}

u32 Lift_::SrcU(const Inst& inst, u32 index) {
    u32 value = ToU(SrcRaw(inst.src[index], inst));
    if (inst.abs & (1 << index)) {
        value = IOp(u16(spv::Op::OpBitwiseAnd), value, CU(0x7fffffffu));
    }
    if (inst.neg & (1 << index)) {
        value = IOp(u16(spv::Op::OpBitwiseXor), value, CU(0x80000000u));
    }
    return value;
}

u32 Lift_::Mask(u32 code, const Inst& inst) {
    if (code == ExecReg) {
        return Exec();
    }
    if (code == SrcZero) {
        return false_id;
    }
    if (code >= 193 && code <= 208) {
        return true_id;
    }
    if (code >= 129 && code <= 192) {
        return true_id;
    }
    if (code == SrcLiteral) {
        return CB(inst.literal != 0);
    }
    if (code >= 128) {
        Fail("mask source " + std::to_string(code));
    }
    const u32 lo = s.r[code];
    if (lo == 0) {
        return false_id;
    }
    if (m.TyOf(lo) == Ty::Bool) {
        return lo;
    }
    const u32 hi = s.r[code + 1];
    const u32 value = IOp(u16(spv::Op::OpBitwiseOr), ToU(lo), hi ? ToU(hi) : CU(0));
    if (m.IsConst(value)) {
        return CB(m.ConstBits(value) != 0);
    }
    return ICmp(u16(spv::Op::OpINotEqual), value, CU(0));
}

void Lift_::SetMask(u32 code, u32 value) {
    if (code == ExecReg) {
        s.r[ExecReg] = value;
        return;
    }
    if (code >= 128) {
        Fail("mask destination");
    }
    s.r[code] = value;
    s.r[code + 1] = mask_hi;
}

u32 Lift_::GuardCond() {
    const u32 exec = Exec();
    if (IsTrue(exec) || kill_masks.empty() || m.IsConst(exec)) {
        return exec;
    }
    const auto killed = [&](u32 id) { return std::find(kill_masks.begin(), kill_masks.end(), id) != kill_masks.end(); };
    if (killed(exec)) {
        return true_id;
    }
    const Node& node = nodes[m.NodeOf(exec)];
    if (node.op == u16(spv::Op::OpLogicalAnd)) {
        if (killed(args[node.first])) {
            return args[node.first + 1];
        }
        if (killed(args[node.first + 1])) {
            return args[node.first];
        }
    }
    return exec;
}

bool Lift_::NarrowsForGood(const Inst& inst) {
    if (in.stage != Stage::Fragment || !loops.empty() || inst.dst != ExecReg || inst.src[0] != ExecReg) {
        return false;
    }
    const u32 prior = Exec();
    std::array<bool, 128> holds{};
    for (u32 reg = 0; reg < 126; ++reg) {
        holds[reg] = s.r[reg] == prior;
    }
    const size_t here = size_t(&inst - insts.data());
    for (size_t i = here + 1; i < insts.size(); ++i) {
        const Inst& next = insts[i];
        if (next.enc == Enc::Sopp && next.op != Op::SOPP_S_ENDPGM && next.op != Op::SOPP_S_NOP && next.op != Op::SOPP_S_WAITCNT &&
            next.op != Op::SOPP_S_BRANCH && next.op != Op::SOPP_S_CBRANCH_SCC0 && next.op != Op::SOPP_S_CBRANCH_SCC1 &&
            next.op != Op::SOPP_S_CBRANCH_VCCZ && next.op != Op::SOPP_S_CBRANCH_VCCNZ && next.op != Op::SOPP_S_CBRANCH_EXECZ &&
            next.op != Op::SOPP_S_CBRANCH_EXECNZ) {
            return false;
        }
        if (next.enc == Enc::Sop1 || next.enc == Enc::Sop2 || next.enc == Enc::Sopc) {
            const u32 sources = next.enc == Enc::Sop1 ? 1 : 2;
            for (u32 k = 0; k < sources; ++k) {
                if (next.src[k] < 126 && holds[next.src[k]]) {
                    return false;
                }
            }
        }
        if (next.enc == Enc::Vop && (next.op == Op::V_CNDMASK_B32) && next.src[2] < 126 && holds[next.src[2]]) {
            return false;
        }
        if ((next.enc == Enc::Sop1 || next.enc == Enc::Sop2) && next.dst < 126) {
            holds[next.dst] = false;
            if (Is64BitDst(next)) {
                holds[next.dst + 1] = false;
            }
        }
        if (next.enc == Enc::Vop && next.sdst != 0 && next.sdst < 126) {
            holds[next.sdst] = false;
            holds[next.sdst + 1] = false;
        }
        if (next.enc == Enc::Smrd && next.dst < 126) {
            static constexpr u32 sizes[5] = {1, 2, 4, 8, 16};
            for (u32 r = 0; r < sizes[next.op & 7] && next.dst + r < 126; ++r) {
                holds[next.dst + r] = false;
            }
        }
    }
    return true;
}

void Lift_::SetVgpr(u32 reg, u32 value) {
    const u32 exec = GuardCond();
    if (!IsTrue(exec)) {
        const u32 old = s.r[reg];
        if (old == 0) {
            s.r[reg] = value;
            return;
        }
        ++out.stats.selects_for_exec;
        value = Select(exec, value, old);
    }
    s.r[reg] = value;
}

void Lift_::SetDstF(const Inst& inst, u32 value) {
    switch (inst.omod) {
    case 1:
        value = FMul(value, CF(2.f));
        break;
    case 2:
        value = FMul(value, CF(4.f));
        break;
    case 3:
        value = FMul(value, CF(0.5f));
        break;
    default:
        break;
    }
    if (inst.clamp) {
        value = FSat(value);
    }
    if (inst.dst >= SrcVgpr) {
        SetVgpr(inst.dst, value);
        return;
    }
    s.r[inst.dst] = value;
}

void Lift_::SetDstU(const Inst& inst, u32 value) {
    if (inst.dst >= SrcVgpr) {
        SetVgpr(inst.dst, value);
        return;
    }
    s.r[inst.dst] = value;
}

u32 Lift_::VReadF(u32 reg) {
    const u32 value = ToF(s.r[SrcVgpr + reg]);
    return value;
}

u32 Lift_::VReadU(u32 reg) {
    return ToU(s.r[SrcVgpr + reg]);
}

void Lift_::LiftBlock(const Block& block) {
    for (u32 i = block.start; i < block.end; ++i) {
        LiftInst(insts[i]);
    }
}

void Lift_::LiftInst(const Inst& inst) {
    out.last_pc = inst.pc;
    switch (inst.enc) {
    case Enc::Sop1:
        return LiftSop1(inst);
    case Enc::Sop2:
        return LiftSop2(inst);
    case Enc::Sopk:
        return LiftSopk(inst);
    case Enc::Sopc:
        return LiftSopc(inst);
    case Enc::Sopp:
        if (inst.op == Op::SOPP_S_ENDPGM || inst.op == Op::SOPP_S_NOP ||
            inst.op == Op::SOPP_S_WAITCNT || IsBranch(inst.op)) {
            return;
        }
        if (inst.op == Op::SOPP_S_BARRIER) {
            const u32 memory = in.stage == Stage::Hull ? u32(spv::Scope::Invocation) : u32(spv::Scope::Workgroup);
            const u32 semantics = in.stage == Stage::Hull
                                      ? 0u
                                      : u32(spv::MemorySemanticsMask::WorkgroupMemory) | u32(spv::MemorySemanticsMask::AcquireRelease);
            EmitVoid(u16(spv::Op::OpControlBarrier), {CU(u32(spv::Scope::Workgroup)), CU(memory), CU(semantics)});
            return;
        }
        if (inst.op == Op::SOPP_S_SENDMSG) {
            return SendMsg(inst);
        }
        Fail("sopp " + std::to_string(inst.op));
    case Enc::Smrd:
        return LiftSmrd(inst);
    case Enc::Vop:
        return LiftVop(inst);
    case Enc::Vintrp:
        return LiftInterp(inst);
    case Enc::Exp:
        return LiftExport(inst);
    case Enc::Ds:
        return LiftDs(inst);
    case Enc::Mubuf:
    case Enc::Mtbuf:
        return LiftBuffer(inst);
    case Enc::Mimg:
        return LiftImage(inst);
    }
}

} // namespace Lifter
