// SPDX-License-Identifier: GPL-3.0-or-later
#include "lifter/lift_core.h"

namespace Lifter {

namespace {

constexpr u16 O(spv::Op op) {
    return u16(op);
}

} // namespace

bool Lift_::KnownValue(u32 id, u32& value) const {
    if (m.IsConst(id) && m.TyOf(id) == Ty::U32) {
        value = m.ConstBits(id);
        return true;
    }
    if (tess_on && id < tags.size() && tags[id].known) {
        value = tags[id].value;
        return true;
    }
    return false;
}

u32 Lift_::MaskOf(u32 id) const {
    return tess_on && id < tags.size() ? tags[id].mask : 0;
}

u32 Lift_::TaggedConst(u32 value, u32 mask) {
    const u32 id = Emit(O(spv::Op::OpCopyObject), t_u32, Ty::U32, {CU(value)});
    tags.resize(std::max<size_t>(tags.size(), id + 1));
    tags[id] = {true, value, mask};
    return id;
}

void Lift_::TagFrom(u32 result, std::initializer_list<u32> sources) {
    if (!tess_on || result == 0) {
        return;
    }
    u32 count = 0;
    for (const u32 source : sources) {
        count += MaskOf(source);
    }
    if (count != 0) {
        tags.resize(std::max<size_t>(tags.size(), result + 1));
        tags[result].mask += count;
    }
}

u32 Lift_::TessRegionOf(u32 addr) const {
    const u32 count = MaskOf(addr);
    if (count == 0) {
        return 0;
    }
    if (in.stage == Stage::Hull && in.tess.passthrough) {
        return 2;
    }
    return count;
}

bool Lift_::DivisibleBy(u32 id, u32 stride, u32 depth) const {
    id = m.Forward(id);
    u32 value = 0;
    if (KnownValue(id, value)) {
        return value == stride;
    }
    if (depth > 16) {
        return false;
    }
    const Node& node = nodes[m.NodeOf(id)];
    if (node.op == O(spv::Op::OpBitFieldUExtract) || node.op == O(spv::Op::OpBitFieldSExtract)) {
        u32 offset = 1;
        u32 count = 0;
        if (KnownValue(args[node.first + 1], offset) && KnownValue(args[node.first + 2], count) && offset == 0 && count == 24) {
            return DivisibleBy(args[node.first], stride, depth + 1);
        }
        return false;
    }
    if (node.op == O(spv::Op::OpIMul)) {
        return DivisibleBy(args[node.first], stride, depth + 1) || DivisibleBy(args[node.first + 1], stride, depth + 1);
    }
    return false;
}

bool Lift_::CollectAddends(u32 id, u32 stride, std::vector<u32>& kept, u32 depth) const {
    id = m.Forward(id);
    if (!m.IsConst(id) && depth < 16) {
        const Node& node = nodes[m.NodeOf(id)];
        if (node.op == O(spv::Op::OpIAdd)) {
            const bool a = CollectAddends(args[node.first], stride, kept, depth + 1);
            const bool b = CollectAddends(args[node.first + 1], stride, kept, depth + 1);
            return a || b;
        }
    }
    if (DivisibleBy(id, stride, 0)) {
        return true;
    }
    kept.push_back(id);
    return false;
}

u32 Lift_::OffsetInStride(u32 addr, u32 stride) {
    std::vector<u32> kept;
    if (CollectAddends(addr, stride, kept, 0)) {
        addr = CU(0);
        for (const u32 addend : kept) {
            addr = IOp(O(spv::Op::OpIAdd), addr, addend);
        }
    }
    u32 value = 0;
    if (KnownValue(addr, value)) {
        return CU(value % stride);
    }
    return Emit(O(spv::Op::OpUMod), t_u32, Ty::U32, {addr, CU(stride)});
}

u32 Lift_::ControlPointVar(bool output) {
    u32& var = output ? var_cp_out : var_cp_in;
    if (!var) {
        const u32 stride = output || in.stage == Stage::Domain ? in.tess.hs_cp_stride : in.tess.ls_stride;
        const u32 count = ((stride + 15) & ~15u) >> 4;
        const u32 inner = m.AddType(O(spv::Op::OpTypeArray), {t_v4f, CU(count)});
        const u32 outer = m.AddType(O(spv::Op::OpTypeArray), {inner, CU(output ? in.tess.output_vertices : 32u)});
        var = output ? OutputVar(outer, 0u, std::nullopt) : InputVar(outer, 0u, std::nullopt);
    }
    return var;
}

u32 Lift_::ReadControlPoint(u32 addr, u32 stride, bool output) {
    const u32 var = ControlPointVar(output);
    u32 value = 0;
    u32 cp;
    if (KnownValue(addr, value)) {
        cp = CU(value / stride);
    } else {
        cp = Emit(O(spv::Op::OpUDiv), t_u32, Ty::U32, {addr, CU(stride)});
    }
    const u32 offset = OffsetInStride(addr, stride);
    const u32 attr = IOp(O(spv::Op::OpShiftRightLogical), offset, CU(4));
    const u32 comp = IOp(O(spv::Op::OpShiftRightLogical), IOp(O(spv::Op::OpBitwiseAnd), offset, CU(15)), CU(2));
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), output ? ptr_output_f32 : ptr_input_f32, Ty::None, {var, cp, attr, comp});
    return ToU(Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr}));
}

u32 Lift_::PatchVar(u32 index) {
    if (index >= var_patch.size()) {
        Fail("patch constant out of range");
    }
    if (!var_patch[index]) {
        const u32 count = ((in.tess.hs_cp_stride + 15) & ~15u) >> 4;
        const u32 location = count + index;
        const u32 var = in.stage == Stage::Hull ? OutputVar(t_v4f, location, std::nullopt) : InputVar(t_v4f, location, std::nullopt);
        m.Decorate(var, u32(spv::Decoration::Patch));
        var_patch[index] = var;
    }
    return var_patch[index];
}

u32 Lift_::InvocationId() {
    return LoadBuiltinU32(var_invocation, spv::BuiltIn::InvocationId);
}

void Lift_::TessPrologue() {
    u32 v = SrcVgpr;
    if (in.stage == Stage::Hull) {
        s.r[v] = LoadBuiltinU32(var_primitive, spv::BuiltIn::PrimitiveId);
        hull_packed = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
        undefined_reads.push_back(hull_packed);
        s.r[v + 1] = hull_packed;
        if (in.tess.offchip) {
            Fail("off-chip tessellation");
        }
        tess_factor_base = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
        undefined_reads.push_back(tess_factor_base);
        s.r[in.num_user_sgprs] = tess_factor_base;
        if (in.tess.passthrough) {
            const u32 count = ((in.tess.ls_stride + 15) & ~15u) >> 4;
            const u32 invocation = InvocationId();
            const u32 source = ControlPointVar(false);
            const u32 target = ControlPointVar(true);
            for (u32 attr = 0; attr < count; ++attr) {
                for (u32 comp = 0; comp < 4; ++comp) {
                    const u32 from = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {source, invocation, CU(attr), CU(comp)});
                    const u32 to = Emit(O(spv::Op::OpAccessChain), ptr_output_f32, Ty::None, {target, invocation, CU(attr), CU(comp)});
                    EmitVoid(O(spv::Op::OpStore), {to, Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {from})});
                }
            }
        }
        return;
    }
    if (in.stage == Stage::Domain) {
        if (!var_tess_coord) {
            var_tess_coord = InputVar(t_v3f, std::nullopt, spv::BuiltIn::TessCoord);
        }
        const auto coord = [&](u32 comp) {
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_tess_coord, CU(comp)});
            return Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
        };
        s.r[v] = coord(0);
        s.r[v + 1] = coord(1);
        s.r[v + 2] = CU(0);
        s.r[v + 3] = LoadBuiltinU32(var_primitive, spv::BuiltIn::PrimitiveId);
        return;
    }
    if (in.stage == Stage::Geometry) {
        s.r[v] = CU(0);
        if (in.gs.in_vertices >= 2) {
            s.r[v + 1] = CU(1);
        }
        if (in.gs.in_vertices >= 3) {
            s.r[v + 3] = CU(2);
        }
        if (in.gs.in_vertices >= 4) {
            s.r[v + 4] = CU(3);
        }
        if (in.gs.in_vertices >= 6) {
            s.r[v + 5] = CU(4);
            s.r[v + 6] = CU(5);
        }
        s.r[v + 2] = LoadBuiltinU32(var_primitive, spv::BuiltIn::PrimitiveId);
    }
}

bool Lift_::TessBufferLoad(const Inst& inst, u32 count) {
    if (!tess_on || !MatchSharpAt(in.tess.sharp, inst.src[0])) {
        return false;
    }
    if (!inst.imm) {
        Fail("tessellation constant read with a computed index");
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 off = inst.offset + i;
        u32 value;
        switch (off) {
        case 0:
            value = CU(in.tess.ls_stride);
            break;
        case 1:
            value = CU(in.tess.hs_cp_stride);
            break;
        case 2:
        case 3:
        case 5: {
            value = TaggedConst(0, off == 5 ? 2 : 1);
            break;
        }
        default: {
            const s32 slot = FindBuffer({inst.src[0], u32(inst.src[0] + 1), u32(inst.src[0] + 2), u32(inst.src[0] + 3)});
            if (slot < 0) {
                value = 0;
                const u32 undefined = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
                undefined_reads.push_back(undefined);
                value = undefined;
            } else {
                value = BufferDword(u32(slot), CU(off));
            }
            break;
        }
        }
        s.r[inst.dst + i] = value;
    }
    return true;
}

bool Lift_::TessBufferStore(const Inst& inst, u32 count) {
    if (in.stage == Stage::Hull) {
        const u32 soffset = inst.src[3] < 128 ? s.r[inst.src[3]] : 0;
        if (soffset != tess_factor_base || soffset == 0) {
            return false;
        }
        if (inst.idxen) {
            Fail("indexed tessellation factor store");
        }
        u32 voffset = 0;
        if (inst.offen && !KnownValue(ToU(s.r[SrcVgpr + inst.src[0]]), voffset)) {
            Fail("computed tessellation factor address");
        }
        const u32 first = (inst.offset + voffset) >> 2;
        for (u32 i = 0; i < count; ++i) {
            const u32 index = first + i;
            u32 factor = index;
            if (in.tess.domain == 1) {
                if (index >= 4) {
                    Fail("triangle tessellation factor index");
                }
                factor = index == 3 ? 4 : index;
            } else if (in.tess.domain == 0 && index >= 2) {
                Fail("line tessellation factor index");
            }
            if (factor >= 6) {
                Fail("tessellation factor index");
            }
            u32& var = factor < 4 ? var_tess_outer : var_tess_inner;
            if (!var) {
                const u32 array = m.AddType(O(spv::Op::OpTypeArray), {t_f32, CU(factor < 4 ? 4u : 2u)});
                var = OutputVar(array, std::nullopt, factor < 4 ? spv::BuiltIn::TessLevelOuter : spv::BuiltIn::TessLevelInner);
                m.Decorate(var, u32(spv::Decoration::Patch));
            }
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_output_f32, Ty::None, {var, CU(factor < 4 ? factor : factor - 4)});
            EmitVoid(O(spv::Op::OpStore), {ptr, ToF(s.r[SrcVgpr + inst.src[1] + i])});
        }
        return true;
    }
    if (in.stage == Stage::Export && inst.glc && inst.slc) {
        for (u32 i = 0; i < count; ++i) {
            const u32 offset = inst.offset + i * 4;
            const u32 value = ToF(s.r[SrcVgpr + inst.src[1] + i]);
            if (offset < 16) {
                ExportPos(12, (offset / 4) % 4, value);
            } else {
                ExportAttr(32 + offset / 16 - 1, (offset / 4) % 4, value);
            }
        }
        return true;
    }
    if (in.stage == Stage::Geometry && inst.glc && inst.slc) {
        for (u32 i = 0; i < count; ++i) {
            const u32 offset = inst.offset + i * 4;
            const u32 value = ToF(s.r[SrcVgpr + inst.src[1] + i]);
            const u32 comp_ofs = in.gs.copy_output_vertices * 4;
            const u32 output_size = in.gs.output_vertices * in.gs.dwords_per_vertex * 4;
            const u32 read_ofs = (((offset / comp_ofs) * comp_ofs) % output_size) * 16;
            bool found = false;
            for (const auto& entry : in.gs.attr_map) {
                if (entry.offset == read_ofs) {
                    found = true;
                    if (entry.attr >= 12 && entry.attr < 16) {
                        ExportPos(entry.attr, entry.comp, value);
                    } else {
                        ExportAttr(entry.attr, entry.comp, value);
                    }
                    break;
                }
            }
            if (!found) {
                Fail("geometry ring write outside the copy shader");
            }
        }
        return true;
    }
    return false;
}

bool Lift_::TessBufferLoadRing(const Inst& inst, u32 count) {
    if (in.stage != Stage::Geometry || !inst.glc || !inst.slc) {
        return false;
    }
    const u32 raw = ToU(s.r[SrcVgpr + inst.src[0]]);
    u32 address = 0;
    if (!KnownValue(raw, address)) {
        Fail("geometry ring read of a computed vertex");
    }
    const u32 soffset_id = ToU(SrcRaw(inst.src[3], inst));
    u32 soffset = 0;
    if (!KnownValue(soffset_id, soffset)) {
        Fail("geometry ring read of a computed offset");
    }
    if (count != 1) {
        Fail("wide geometry ring read");
    }
    const u32 vertex = address >> 2;
    const u32 bucket = soffset / 256;
    const u32 comp = bucket % 4;
    u32 value;
    if (bucket < 4) {
        if (!var_gl_in) {
            const u32 clip = m.AddType(O(spv::Op::OpTypeArray), {t_f32, CU(1)});
            const std::array<u32, 3> members{t_v4f, t_f32, clip};
            const u32 block = m.AddUniqueType(O(spv::Op::OpTypeStruct), members);
            m.MemberDecorate(block, 0, u32(spv::Decoration::BuiltIn), u32(spv::BuiltIn::Position));
            m.MemberDecorate(block, 1, u32(spv::Decoration::BuiltIn), u32(spv::BuiltIn::PointSize));
            m.MemberDecorate(block, 2, u32(spv::Decoration::BuiltIn), u32(spv::BuiltIn::ClipDistance));
            m.Decorate(block, u32(spv::Decoration::Block));
            m.Capability(u32(spv::Capability::ClipDistance));
            const u32 array = m.AddType(O(spv::Op::OpTypeArray), {block, CU(in.gs.in_vertices)});
            var_gl_in = InputVar(array, std::nullopt, std::nullopt);
        }
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_gl_in, CU(vertex), CU(0), CU(comp)});
        value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
    } else {
        const u32 param = bucket / 4 - 1;
        if (param >= var_param_in.size()) {
            Fail("geometry input out of range");
        }
        if (!var_param_in[param]) {
            const u32 array = m.AddType(O(spv::Op::OpTypeArray), {t_v4f, CU(in.gs.in_vertices)});
            var_param_in[param] = InputVar(array, param, std::nullopt);
        }
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {var_param_in[param], CU(vertex), CU(comp)});
        value = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
    }
    SetVgpr(SrcVgpr + inst.src[1], value);
    return true;
}

void Lift_::LiftLds(const Inst& inst) {
    const u32 addr = ToU(s.r[inst.src[0]]);
    const auto load = [&](u32 address) -> u32 {
        const u32 region = TessRegionOf(address);
        if (in.stage == Stage::Hull) {
            if (region == 0) {
                return ReadControlPoint(address, in.tess.ls_stride, false);
            }
            if (region == 1) {
                return ReadControlPoint(address, in.tess.hs_cp_stride, true);
            }
            Fail("read of a patch constant in a hull shader");
        }
        if (in.stage != Stage::Domain) {
            Fail("local memory read in this stage");
        }
        if (region == 1 || (in.discover && region == 0)) {
            return ReadControlPoint(address, in.tess.hs_cp_stride, false);
        }
        if (region != 2) {
            Fail("domain shader read of region " + std::to_string(region));
        }
        u32 value = 0;
        if (!KnownValue(address, value)) {
            Fail("computed patch constant address");
        }
        const u32 dword = value >> 2;
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_input_f32, Ty::None, {PatchVar(dword / 4), CU(dword % 4)});
        return ToU(Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr}));
    };
    const auto store = [&](u32 address, u32 data) {
        if (in.stage == Stage::Local) {
            u32 value = 0;
            if (!KnownValue(address, value)) {
                if (in.discover) {
                    return;
                }
                Fail("computed local shader output address");
            }
            if (!var_cp_out) {
                const u32 count = ((in.tess.ls_stride + 15) & ~15u) >> 4;
                const u32 array = m.AddType(O(spv::Op::OpTypeArray), {t_v4f, CU(count)});
                var_cp_out = OutputVar(array, 0u, std::nullopt);
            }
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_output_f32, Ty::None, {var_cp_out, CU(value / 16), CU((value / 4) % 4)});
            EmitVoid(O(spv::Op::OpStore), {ptr, ToF(data)});
            return;
        }
        if (in.stage != Stage::Hull) {
            Fail("local memory write in this stage");
        }
        const u32 region = TessRegionOf(address);
        if (region == 2) {
            u32 value = 0;
            if (!KnownValue(address, value)) {
                Fail("computed patch constant address");
            }
            const u32 dword = value >> 2;
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_output_f32, Ty::None, {PatchVar(dword / 4), CU(dword % 4)});
            EmitVoid(O(spv::Op::OpStore), {ptr, ToF(data)});
            return;
        }
        if (region != 1 && !in.discover) {
            Fail("hull shader write of region " + std::to_string(region));
        }
        const u32 offset = OffsetInStride(address, in.tess.hs_cp_stride);
        const u32 attr = IOp(O(spv::Op::OpShiftRightLogical), offset, CU(4));
        const u32 comp = IOp(O(spv::Op::OpShiftRightLogical), IOp(O(spv::Op::OpBitwiseAnd), offset, CU(15)), CU(2));
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_output_f32, Ty::None, {ControlPointVar(true), InvocationId(), attr, comp});
        EmitVoid(O(spv::Op::OpStore), {ptr, ToF(data)});
    };
    const u32 offset = (u32(inst.offset1) << 8) + inst.offset;
    switch (inst.op) {
    case Op::DS_READ_B32:
        return SetVgpr(inst.dst, load(IOp(O(spv::Op::OpIAdd), addr, CU(offset))));
    case Op::DS_READ2_B32:
        SetVgpr(inst.dst, load(IOp(O(spv::Op::OpIAdd), addr, CU(inst.offset * 4))));
        SetVgpr(inst.dst + 1, load(IOp(O(spv::Op::OpIAdd), addr, CU(u32(inst.offset1) * 4))));
        return;
    case Op::DS_WRITE_B32:
        return store(IOp(O(spv::Op::OpIAdd), addr, CU(offset)), s.r[inst.src[1]]);
    case Op::DS_WRITE2_B32:
        store(IOp(O(spv::Op::OpIAdd), addr, CU(inst.offset * 4)), s.r[inst.src[1]]);
        store(IOp(O(spv::Op::OpIAdd), addr, CU(u32(inst.offset1) * 4)), s.r[inst.src[2]]);
        return;
    default:
        Fail("ds " + std::to_string(inst.op));
    }
}

} // namespace Lifter
