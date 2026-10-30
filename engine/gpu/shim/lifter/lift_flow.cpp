// SPDX-License-Identifier: GPL-3.0-or-later
#include "lifter/lift_core.h"

namespace Lifter {

u32 Lift_::TypeOfTy(Ty ty) const {
    switch (ty) {
    case Ty::Bool:
        return t_bool;
    case Ty::F32:
        return t_f32;
    default:
        return t_u32;
    }
}

u32 Lift_::UndefOf(Ty ty) {
    u32& id = undef_ids[u32(ty)];
    if (id == 0) {
        id = m.NewId(ty);
        Module::Word(m.Globals(), u32(spv::Op::OpUndef), 3);
        m.Globals().push_back(TypeOfTy(ty));
        m.Globals().push_back(id);
    }
    return id;
}

void Lift_::Open(u32 label) {
    OutBlock block;
    block.label = label;
    out_blocks.push_back(std::move(block));
    cur = u32(out_blocks.size() - 1);
}

void Lift_::Branch(Sink& sink) {
    sink.edges.push_back({cur, s});
    Terminate(u16(spv::Op::OpBranch), {sink.label});
}

void Lift_::CastInPred(u32 out_block, u32& value, Ty want) {
    if (value == 0 || m.TyOf(value) == want) {
        return;
    }
    if (m.IsConst(value)) {
        if (want == Ty::F32) {
            value = m.Const(Ty::F32, t_f32, m.ConstBits(value));
        } else if (want == Ty::U32) {
            value = m.TyOf(value) == Ty::Bool ? CU(m.ConstBits(value)) : CU(m.ConstBits(value));
        } else {
            value = CB(m.ConstBits(value) != 0);
        }
        return;
    }
    const u32 saved = cur;
    cur = out_block;
    std::vector<u32> tail;
    auto& list = out_blocks[cur].nodes;
    tail.push_back(list.back());
    list.pop_back();
    if (!list.empty() && (nodes[list.back()].op == u16(spv::Op::OpSelectionMerge) ||
                          nodes[list.back()].op == u16(spv::Op::OpLoopMerge))) {
        tail.push_back(list.back());
        list.pop_back();
    }
    if (want == Ty::F32) {
        value = ToF(value);
    } else if (want == Ty::U32) {
        value = ToU(value);
    } else {
        value = ICmp(u16(spv::Op::OpINotEqual), ToU(value), CU(0));
    }
    auto& after = out_blocks[cur].nodes;
    after.insert(after.end(), tail.rbegin(), tail.rend());
    cur = saved;
}

void Lift_::MergeEdges(Sink& sink, u32 cond) {
    auto& edges = sink.edges;
    if (edges.size() == 1) {
        s = edges[0].state;
        return;
    }
    for (u32 reg = 0; reg < RegCount; ++reg) {
        const u32 first = edges[0].state.r[reg];
        bool differ = false;
        for (size_t e = 1; e < edges.size(); ++e) {
            if (edges[e].state.r[reg] != first) {
                differ = true;
                break;
            }
        }
        if (!differ) {
            s.r[reg] = first;
            continue;
        }
        Ty want = Ty::U32;
        bool any_bool = false;
        bool all_bool = true;
        bool any_float = false;
        for (const Edge& edge : edges) {
            const u32 value = edge.state.r[reg];
            if (value == 0) {
                continue;
            }
            const Ty ty = m.TyOf(value);
            any_bool |= ty == Ty::Bool;
            all_bool &= ty == Ty::Bool || (m.IsConst(value) && ty == Ty::U32);
            any_float |= ty == Ty::F32;
        }
        if (any_float) {
            want = Ty::F32;
        } else if (any_bool && all_bool) {
            want = Ty::Bool;
        }
        if (want == Ty::Bool && cond != 0 && edges.size() == 2) {
            u32 a = edges[0].state.r[reg];
            u32 b = edges[1].state.r[reg];
            CastInPred(edges[0].out_block, a, Ty::Bool);
            CastInPred(edges[1].out_block, b, Ty::Bool);
            if (a != 0 && b != 0 && m.IsConst(a) && m.IsConst(b)) {
                s.r[reg] = IsTrue(a) ? (IsTrue(b) ? true_id : cond)
                                     : (IsTrue(b) ? Not(cond) : false_id);
                ++out.stats.phis_folded;
                continue;
            }
        }
        const u32 id = m.NewId(want);
        Node node;
        node.op = u16(spv::Op::OpPhi);
        node.type = TypeOfTy(want);
        node.result = id;
        node.nargs = u16(edges.size() * 2);
        node.first = u32(args.size());
        args.resize(args.size() + edges.size() * 2);
        const u32 first_arg = node.first;
        const u32 node_index = u32(nodes.size());
        nodes.push_back(node);
        m.BindNode(id, node_index);
        for (size_t e = 0; e < edges.size(); ++e) {
            u32 value = edges[e].state.r[reg];
            if (value == 0) {
                value = UndefOf(want);
            } else {
                CastInPred(edges[e].out_block, value, want);
            }
            args[first_arg + e * 2] = value;
            args[first_arg + e * 2 + 1] = out_blocks[edges[e].out_block].label;
        }
        auto& list = out_blocks[cur].nodes;
        u32 at = 0;
        while (at < list.size() && nodes[list[at]].op == u16(spv::Op::OpPhi)) {
            ++at;
        }
        list.insert(list.begin() + at, node_index);
        if (tess_on) {
            for (const Edge& edge : edges) {
                TagFrom(id, {edge.state.r[reg]});
            }
        }
        {
            u32 origin = 0;
            bool same = true;
            for (const Edge& edge : edges) {
                const u32 value = edge.state.r[reg];
                if (value == 0) {
                    continue;
                }
                const u32 other = m.Origin(value);
                if (other == 0 || (origin != 0 && origin != other)) {
                    same = false;
                    break;
                }
                origin = other;
            }
            if (same && origin != 0) {
                m.SetOrigin(id, origin);
            }
        }
        s.r[reg] = id;
        ++out.stats.phis;
    }
}

bool Lift_::BlockIsHeader(s32 at) const {
    for (const u32 p : blocks[at].pred_list) {
        if (blocks[p].order >= blocks[at].order) {
            return true;
        }
    }
    return false;
}

bool Lift_::ReachesDisjoint(s32 a, s32 b) const {
    std::vector<u8> seen(blocks.size(), 0);
    std::vector<s32> stack{a};
    seen[a] = 1;
    const LoopCtx* loop = loops.empty() ? nullptr : loops.back();
    const auto visit = [&](s32 next) {
        if (next < 0 || seen[next]) {
            return;
        }
        if (loop && (next == loop->header || next == loop->merge.block)) {
            return;
        }
        seen[next] = 1;
        stack.push_back(next);
    };
    while (!stack.empty()) {
        const s32 at = stack.back();
        stack.pop_back();
        visit(blocks[at].taken);
        visit(blocks[at].fall);
    }
    std::vector<u8> seen_b(blocks.size(), 0);
    stack.push_back(b);
    if (seen[b]) {
        return false;
    }
    seen_b[b] = 1;
    while (!stack.empty()) {
        const s32 at = stack.back();
        stack.pop_back();
        for (const s32 next : {blocks[at].taken, blocks[at].fall}) {
            if (next < 0 || seen_b[next]) {
                continue;
            }
            if (seen[next]) {
                return false;
            }
            if (loop && (next == loop->header || next == loop->merge.block)) {
                continue;
            }
            seen_b[next] = 1;
            stack.push_back(next);
        }
    }
    return true;
}

s32 Lift_::FindMerge(s32 at, s32 t, s32 f) {
    const LoopCtx* loop = loops.empty() ? nullptr : loops.back();
    s32 merge = blocks[at].ipdom;
    if (merge >= 0 && loop) {
        if (merge == loop->header || !loop->body[merge]) {
            merge = -1;
        }
    }
    const auto reaches = [&](s32 from, s32 to) {
        std::vector<u8> seen(blocks.size(), 0);
        std::vector<s32> stack{from};
        seen[from] = 1;
        while (!stack.empty()) {
            const s32 current = stack.back();
            stack.pop_back();
            if (current == to) {
                return true;
            }
            for (const s32 next : {blocks[current].taken, blocks[current].fall}) {
                if (next < 0 || seen[next] || (loop && (next == loop->header || next == loop->merge.block))) {
                    continue;
                }
                seen[next] = 1;
                stack.push_back(next);
            }
        }
        return false;
    };
    if (merge >= 0 && (!loop || (reaches(t, merge) && reaches(f, merge)))) {
        return merge;
    }
    if (reaches(f, t)) {
        return t;
    }
    if (reaches(t, f)) {
        return f;
    }
    if (ReachesDisjoint(t, f)) {
        u32 size_t_side = 0;
        u32 size_f_side = 0;
        for (s32 walk = t; walk >= 0 && size_t_side < 64; walk = blocks[walk].taken) {
            ++size_t_side;
        }
        for (s32 walk = f; walk >= 0 && size_f_side < 64; walk = blocks[walk].taken) {
            ++size_f_side;
        }
        return size_t_side <= size_f_side ? f : t;
    }
    return -1;
}

void Lift_::Walk(s32 at, Sink* sink) {
    while (true) {
        if (sink && at == sink->block) {
            Branch(*sink);
            return;
        }
        LoopCtx* loop = loops.empty() ? nullptr : loops.back();
        if (loop && at == loop->merge.block) {
            Branch(loop->merge);
            return;
        }
        if (loop && at == loop->header && visited[at]) {
            Branch(loop->cont);
            return;
        }
        if (visited[at]) {
            out.decline_pc = insts[blocks[at].start].pc;
            Fail("side entry into a region");
        }
        if (BlockIsHeader(at) && at != entering_header) {
            at = WalkLoop(at);
            if (at < 0) {
                return;
            }
            continue;
        }
        visited[at] = 1;
        entering_header = -1;
        const Block& block = blocks[at];
        LiftBlock(block);
        if (block.is_exit) {
            Epilogue();
            Terminate(u16(spv::Op::OpReturn), {});
            return;
        }
        if (block.fall < 0) {
            at = block.taken;
            continue;
        }
        u32 cond = 0;
        u32 taken_reg = 0;
        bool taken_value = false;
        switch (block.branch_op) {
        case Op::SOPP_S_CBRANCH_SCC0:
            cond = Not(s.r[RegScc] ? s.r[RegScc] : false_id);
            taken_reg = RegScc;
            taken_value = false;
            break;
        case Op::SOPP_S_CBRANCH_SCC1:
            cond = s.r[RegScc] ? s.r[RegScc] : false_id;
            taken_reg = RegScc;
            taken_value = true;
            break;
        case Op::SOPP_S_CBRANCH_VCCZ:
            cond = Not(Mask(VccReg, insts[block.end - 1]));
            taken_reg = VccReg;
            taken_value = false;
            break;
        case Op::SOPP_S_CBRANCH_VCCNZ:
            cond = Mask(VccReg, insts[block.end - 1]);
            taken_reg = VccReg;
            taken_value = true;
            break;
        case Op::SOPP_S_CBRANCH_EXECZ:
            cond = Not(Exec());
            taken_reg = ExecReg;
            taken_value = false;
            break;
        case Op::SOPP_S_CBRANCH_EXECNZ:
            cond = Exec();
            taken_reg = ExecReg;
            taken_value = true;
            break;
        default:
            Fail("branch kind");
        }
        const auto edge_state = [&](bool taken) {
            const bool value = taken ? taken_value : !taken_value;
            if (taken_reg == VccReg) {
                s.r[VccReg] = CB(value);
                s.r[VccReg + 1] = mask_hi;
            } else {
                s.r[taken_reg] = CB(value);
            }
        };
        if (IsTrue(cond) || IsFalse(cond)) {
            at = IsTrue(cond) ? block.taken : block.fall;
            continue;
        }
        const s32 t = block.taken;
        const s32 f = block.fall;
        const auto loop_edge = [&](s32 target) -> Sink* {
            if (!loop) {
                return nullptr;
            }
            if (target == loop->header) {
                return &loop->cont;
            }
            if (target == loop->merge.block) {
                return &loop->merge;
            }
            return nullptr;
        };
        Sink* t_exit = loop_edge(t);
        Sink* f_exit = loop_edge(f);
        if (t_exit || f_exit) {
            const State saved = s;
            const u32 header_block = cur;
            if (t_exit && f_exit) {
                edge_state(true);
                t_exit->edges.push_back({cur, s});
                s = saved;
                edge_state(false);
                f_exit->edges.push_back({cur, s});
                Terminate(u16(spv::Op::OpBranchConditional), {cond, t_exit->label, f_exit->label});
                return;
            }
            const u32 next_label = m.NewId();
            if (t_exit) {
                edge_state(true);
                t_exit->edges.push_back({header_block, s});
                s = saved;
                Terminate(u16(spv::Op::OpBranchConditional), {cond, t_exit->label, next_label});
                edge_state(false);
                Open(next_label);
                at = f;
            } else {
                edge_state(false);
                f_exit->edges.push_back({header_block, s});
                s = saved;
                Terminate(u16(spv::Op::OpBranchConditional), {cond, next_label, f_exit->label});
                edge_state(true);
                Open(next_label);
                at = t;
            }
            continue;
        }
        const s32 merge = FindMerge(at, t, f);
        Sink inner;
        inner.block = merge;
        inner.label = m.NewId();
        ++out.stats.selections;
        EmitVoid(u16(spv::Op::OpSelectionMerge), {inner.label, 0}, 2);
        const State saved = s;
        const u32 t_label = t == merge ? inner.label : m.NewId();
        const u32 f_label = f == merge ? inner.label : m.NewId();
        const u32 header_block = cur;
        Terminate(u16(spv::Op::OpBranchConditional), {cond, t_label, f_label});
        if (t == merge) {
            edge_state(true);
            inner.edges.push_back({header_block, s});
        } else {
            edge_state(true);
            Open(t_label);
            Walk(t, &inner);
        }
        s = saved;
        if (f == merge) {
            edge_state(false);
            inner.edges.push_back({header_block, s});
        } else {
            edge_state(false);
            Open(f_label);
            Walk(f, &inner);
        }
        Open(inner.label);
        if (inner.edges.empty()) {
            Terminate(u16(spv::Op::OpUnreachable), {});
            return;
        }
        MergeEdges(inner, inner.edges.size() == 2 ? cond : 0);
        if (merge < 0) {
            Fail("arms join without a merge");
        }
        at = merge;
    }
}

s32 Lift_::WalkLoop(s32 header) {
    ++out.stats.loops;
    LoopCtx ctx;
    ctx.header = header;
    ctx.body.assign(blocks.size(), 0);
    std::vector<s32> stack;
    ctx.body[header] = 1;
    for (const u32 p : blocks[header].pred_list) {
        if (blocks[p].order >= blocks[header].order && !ctx.body[p]) {
            ctx.body[p] = 1;
            stack.push_back(s32(p));
        }
    }
    while (!stack.empty()) {
        const s32 at = stack.back();
        stack.pop_back();
        for (const u32 p : blocks[at].pred_list) {
            if (!ctx.body[p]) {
                ctx.body[p] = 1;
                stack.push_back(s32(p));
            }
        }
    }
    s32 exit_block = -1;
    std::array<u8, 128> sgpr_written{};
    for (u32 b = 0; b < blocks.size(); ++b) {
        if (!ctx.body[b]) {
            continue;
        }
        for (const s32 succ : {blocks[b].taken, blocks[b].fall}) {
            if (succ >= 0 && !ctx.body[succ]) {
                if (exit_block >= 0 && exit_block != succ) {
                    Fail("loop with two exits");
                }
                exit_block = succ;
            }
        }
        for (u32 i = blocks[b].start; i < blocks[b].end; ++i) {
            const Inst& inst = insts[i];
            u32 first = 128;
            u32 count = 1;
            switch (inst.enc) {
            case Enc::Sop1:
            case Enc::Sop2:
            case Enc::Sopk:
                first = inst.dst;
                count = Is64BitDst(inst) ? 2 : 1;
                break;
            case Enc::Smrd: {
                static constexpr u32 sizes[5] = {1, 2, 4, 8, 16};
                first = inst.dst;
                count = sizes[inst.op & 7];
                break;
            }
            case Enc::Vop:
                if (inst.op == Op::V_READLANE_B32 || inst.op == Op::V_READFIRSTLANE_B32) {
                    first = inst.dst;
                }
                if (inst.sdst != 0 && inst.sdst < 128) {
                    sgpr_written[inst.sdst] = 1;
                    sgpr_written[inst.sdst + 1] = 1;
                }
                break;
            default:
                break;
            }
            for (u32 r = first; r < first + count && r < 128; ++r) {
                sgpr_written[r] = 1;
            }
        }
    }
    ctx.merge.block = exit_block;
    ctx.merge.label = m.NewId();
    ctx.cont.label = m.NewId();
    const u32 header_label = m.NewId();
    const u32 body_label = m.NewId();
    const u32 pre_block = cur;
    const u32 pre_label = out_blocks[cur].label;
    Terminate(u16(spv::Op::OpBranch), {header_label});
    Open(header_label);
    struct LoopPhi {
        u32 reg;
        u32 phi;
        u32 entry;
        u32 node;
    };
    std::vector<LoopPhi> phis;
    const bool exec_true = IsTrue(Exec()) && !exec_pessimistic;
    const auto add_phi = [&](u32 reg) {
        const u32 entry = s.r[reg];
        if (entry == 0) {
            return;
        }
        const Ty ty = m.TyOf(entry);
        if (ty != Ty::F32 && ty != Ty::U32 && ty != Ty::Bool) {
            Fail("loop register type");
        }
        const u32 id = m.NewId(ty);
        Node node;
        node.op = u16(spv::Op::OpPhi);
        node.type = TypeOfTy(ty);
        node.result = id;
        node.nargs = 4;
        node.first = u32(args.size());
        args.insert(args.end(), {entry, pre_label, 0, 0});
        const u32 index = u32(nodes.size());
        nodes.push_back(node);
        m.BindNode(id, index);
        out_blocks[cur].nodes.push_back(index);
        phis.push_back({reg, id, entry, index});
        s.r[reg] = id;
        if (in.discover && std::find(no_loop_origin.begin(), no_loop_origin.end(), reg) == no_loop_origin.end()) {
            m.SetOrigin(id, m.Origin(entry));
        }
    };
    for (u32 reg = 0; reg < 128; ++reg) {
        if (reg == ExecReg || reg == ExecReg + 1) {
            continue;
        }
        if (sgpr_written[reg] || reg == VccReg || reg == VccReg + 1 || reg == SrcM0) {
            add_phi(reg);
        }
    }
    add_phi(RegScc);
    if (!exec_true) {
        add_phi(ExecReg);
    }
    for (u32 reg = SrcVgpr; reg < RegCount; ++reg) {
        add_phi(reg);
    }
    EmitVoid(u16(spv::Op::OpLoopMerge), {ctx.merge.label, ctx.cont.label, 0}, 4);
    Terminate(u16(spv::Op::OpBranch), {body_label});
    Open(body_label);
    loops.push_back(&ctx);
    entering_header = header;
    Walk(header, nullptr);
    loops.pop_back();
    (void)pre_block;
    Open(ctx.cont.label);
    if (ctx.cont.edges.empty()) {
        Fail("loop without a back edge");
    }
    MergeEdges(ctx.cont, 0);
    if (exec_true && !IsTrue(Exec())) {
        throw RestartPessimistic{};
    }
    std::vector<u32> origin_conflicts;
    for (const LoopPhi& phi : phis) {
        const u32 origin = m.Origin(phi.phi);
        if (origin != 0 && s.r[phi.reg] != phi.phi && (s.r[phi.reg] == 0 || m.Origin(s.r[phi.reg]) != origin)) {
            origin_conflicts.push_back(phi.reg);
        }
    }
    if (!origin_conflicts.empty()) {
        throw RestartOrigins{std::move(origin_conflicts)};
    }
    for (const LoopPhi& phi : phis) {
        u32 back = s.r[phi.reg];
        if (back == 0) {
            back = UndefOf(m.TyOf(phi.phi));
        }
        const Ty ty = m.TyOf(phi.phi);
        if (m.TyOf(back) != ty) {
            if (ty == Ty::F32) {
                back = ToF(back);
            } else if (ty == Ty::U32) {
                back = ToU(back);
            } else {
                back = ICmp(u16(spv::Op::OpINotEqual), ToU(back), CU(0));
            }
        }
        const u32 arg = nodes[phi.node].first;
        args[arg + 2] = back;
        args[arg + 3] = ctx.cont.label;
        if (back == phi.phi) {
            m.SetForward(phi.phi, phi.entry);
        }
    }
    Terminate(u16(spv::Op::OpBranch), {header_label});
    Open(ctx.merge.label);
    if (ctx.merge.edges.empty()) {
        Terminate(u16(spv::Op::OpUnreachable), {});
        return -1;
    }
    MergeEdges(ctx.merge, 0);
    if (exit_block < 0) {
        Fail("loop edges without an exit");
    }
    return exit_block;
}

void Lift_::MarkLive(std::vector<u8>& live) {
    std::vector<u32> stack;
    const auto mark = [&](u32 index) {
        if (!live[index]) {
            live[index] = 1;
            stack.push_back(index);
        }
    };
    for (u32 i = 0; i < nodes.size(); ++i) {
        if (nodes[i].effect) {
            mark(i);
        }
    }
    while (!stack.empty()) {
        const Node node = nodes[stack.back()];
        stack.pop_back();
        for (u32 a = 0; a < node.nargs; ++a) {
            if (a < 32 && ((node.lits >> a) & 1)) {
                continue;
            }
            const u32 id = m.Forward(args[node.first + a]);
            if (id == 0 || m.IsConst(id)) {
                continue;
            }
            const u32 n = m.NodeOf(id);
            if (n < nodes.size() && nodes[n].result == id) {
                mark(n);
            }
        }
    }
}

std::vector<u32> Lift_::Serialize() {
    std::vector<u8> live(nodes.size(), 0);
    MarkLive(live);
    bool exact = !fp_contract();
    std::vector<u32> fn;
    fn.reserve(nodes.size() * 6 + out_blocks.size() * 2);
    const u32 fn_type = m.AddType(u16(spv::Op::OpTypeFunction), {t_void});
    Module::Word(fn, u32(spv::Op::OpFunction), 5);
    fn.push_back(t_void);
    fn.push_back(main_id);
    fn.push_back(0);
    fn.push_back(fn_type);
    u32 live_count = 0;
    for (const OutBlock& block : out_blocks) {
        Module::Word(fn, u32(spv::Op::OpLabel), 2);
        fn.push_back(block.label);
        for (const u32 index : block.nodes) {
            if (!live[index]) {
                continue;
            }
            ++live_count;
            const Node& node = nodes[index];
            const u32 words = 1 + node.nargs + (node.result ? 2 : 0);
            Module::Word(fn, node.op, words);
            if (node.result) {
                fn.push_back(node.type);
                fn.push_back(node.result);
            }
            for (u32 a = 0; a < node.nargs; ++a) {
                const u32 value = args[node.first + a];
                const bool literal = a < 32 && ((node.lits >> a) & 1);
                fn.push_back(literal ? value : m.Forward(value));
            }
            if (node.nocontract && exact) {
                m.Decorate(node.result, u32(spv::Decoration::NoContraction));
            }
        }
    }
    Module::Word(fn, u32(spv::Op::OpFunctionEnd), 1);
    out.stats.nodes = u32(nodes.size());
    out.stats.nodes_live = live_count;
    return fn;
}

} // namespace Lifter
