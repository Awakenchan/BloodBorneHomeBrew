// SPDX-License-Identifier: GPL-3.0-or-later
#include "lifter/lift_core.h"

namespace Lifter {

namespace {

constexpr u16 O(spv::Op op) {
    return u16(op);
}

} // namespace

u32 Lift_::SlotFor(u32 kind, u32 a, u32 b) {
    for (u32 i = 0; i < slots.size(); ++i) {
        if (slots[i][0] == kind && slots[i][1] == a && slots[i][2] == b) {
            return FirstSlot + i;
        }
    }
    if (FirstSlot + slots.size() >= RegCount) {
        Fail("too many lane slots");
    }
    slots.push_back({kind, a, b});
    return FirstSlot + u32(slots.size()) - 1;
}

u32 Lift_::FlatIndexOf(u32 parent, u32 offset) {
    for (const FlatEntry& entry : in.flat_table) {
        if (entry.parent == parent && entry.offset == offset) {
            return entry.flat;
        }
    }
    if (found) {
        const u16 flat = u16(16 + found->flat_table.size());
        found->flat_table.push_back({u16(parent), offset, flat});
        return flat;
    }
    return NoFlat;
}

u32 Lift_::PtrType(u32 storage, u32 pointee) {
    return m.AddType(O(spv::Op::OpTypePointer), {storage, pointee});
}

u32 Lift_::InputVar(u32 type, std::optional<u32> location, std::optional<spv::BuiltIn> builtin) {
    const u32 var = m.Variable(PtrType(u32(spv::StorageClass::Input), type), u32(spv::StorageClass::Input));
    if (location) {
        m.Decorate(var, u32(spv::Decoration::Location), *location);
    }
    if (builtin) {
        m.Decorate(var, u32(spv::Decoration::BuiltIn), u32(*builtin));
    }
    m.Interface(var);
    return var;
}

u32 Lift_::OutputVar(u32 type, std::optional<u32> location, std::optional<spv::BuiltIn> builtin, u32 init) {
    const u32 var = m.Variable(PtrType(u32(spv::StorageClass::Output), type), u32(spv::StorageClass::Output), init);
    if (location) {
        m.Decorate(var, u32(spv::Decoration::Location), *location);
    }
    if (builtin) {
        m.Decorate(var, u32(spv::Decoration::BuiltIn), u32(*builtin));
    }
    m.Interface(var);
    return var;
}

u32 Lift_::LoadBuiltinU32(u32& var, spv::BuiltIn builtin) {
    if (!var) {
        var = InputVar(t_u32, std::nullopt, builtin);
        if (builtin == spv::BuiltIn::SampleId || builtin == spv::BuiltIn::SubgroupLocalInvocationId) {
            m.Decorate(var, u32(spv::Decoration::Flat));
        }
    }
    return Emit(O(spv::Op::OpLoad), t_u32, Ty::U32, {var});
}

u32 Lift_::PushU32(u32 member, s32 element) {
    const u32 ptr = element < 0
                        ? Emit(O(spv::Op::OpAccessChain), ptr_push_u32, Ty::None, {push_block, CU(member)})
                        : Emit(O(spv::Op::OpAccessChain), ptr_push_u32, Ty::None, {push_block, CU(member), CU(u32(element))});
    return Emit(O(spv::Op::OpLoad), t_u32, Ty::U32, {ptr});
}

u32 Lift_::PushF32(u32 member) {
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), ptr_push_f32, Ty::None, {push_block, CU(member)});
    return Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
}

u32 Lift_::ReadUserData(u32 reg) {
    u32 id;
    if (in.ud_push[reg] >= 0) {
        const u32 index = u32(in.ud_push[reg]);
        id = PushU32(4 + (index >> 2), s32(index & 3));
    } else if (in.flatbuf_slot >= 0) {
        id = FlatLoad(reg);
    } else {
        id = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
        undefined_reads.push_back(id);
    }
    m.SetOrigin(id, reg + 1);
    return id;
}

u32 Lift_::FlatLoad(u32 flat) {
    if (in.flatbuf_slot < 0) {
        const u32 id = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
        undefined_reads.push_back(id);
        return id;
    }
    return BufferDword(u32(in.flatbuf_slot), CU(flat));
}

u32 Lift_::BufferVar(u32 slot, Ty element) {
    const BufferSlot& buffer = in.buffers[slot];
    u32* table;
    u32 scalar;
    u32 stride;
    switch (element) {
    case Ty::U32:
        table = buffer_u32.data();
        scalar = t_u32;
        stride = 4;
        break;
    case Ty::F32:
        table = buffer_f32.data();
        scalar = t_f32;
        stride = 4;
        break;
    case Ty::U16:
        table = buffer_u16.data();
        if (!t_u16) {
            t_u16 = m.AddType(O(spv::Op::OpTypeInt), {16, 0});
        }
        scalar = t_u16;
        stride = 2;
        m.Capability(u32(spv::Capability::Int16));
        m.Capability(u32(spv::Capability::StorageBuffer16BitAccess));
        break;
    default:
        table = buffer_u8.data();
        if (!t_u8) {
            t_u8 = m.AddType(O(spv::Op::OpTypeInt), {8, 0});
        }
        scalar = t_u8;
        stride = 1;
        m.Capability(u32(spv::Capability::Int8));
        m.Capability(u32(spv::Capability::StorageBuffer8BitAccess));
        break;
    }
    if (table[slot]) {
        return table[slot];
    }
    const u32 array = m.AddType(O(spv::Op::OpTypeRuntimeArray), {scalar});
    u32 block = 0;
    for (const auto& [known_array, known_block] : ssbo_blocks) {
        if (known_array == array) {
            block = known_block;
        }
    }
    if (!block) {
        const u32 words[] = {array};
        block = m.AddUniqueType(O(spv::Op::OpTypeStruct), words);
        m.Decorate(array, u32(spv::Decoration::ArrayStride), stride);
        m.Decorate(block, u32(spv::Decoration::Block));
        m.MemberDecorate(block, 0, u32(spv::Decoration::Offset), 0);
        ssbo_blocks.push_back({array, block});
    }
    const u32 var = m.Variable(PtrType(u32(spv::StorageClass::StorageBuffer), block),
                               u32(spv::StorageClass::StorageBuffer));
    m.Decorate(var, u32(spv::Decoration::Binding), buffer.binding);
    m.Decorate(var, u32(spv::Decoration::DescriptorSet), 0);
    if (!buffer.written) {
        m.Decorate(var, u32(spv::Decoration::NonWritable));
    } else if (buffer.coherent) {
        m.Decorate(var, u32(spv::Decoration::Coherent));
    }
    m.Interface(var);
    table[slot] = var;
    return var;
}

u32 Lift_::BufferOffsetDwords(u32 slot, u32 shift) {
    const u32 byte_offset = buffer_off_id[slot];
    if (!byte_offset) {
        return 0;
    }
    if (shift == 2) {
        return buffer_off_dwords[slot];
    }
    return shift == 0 ? byte_offset : IOp(O(spv::Op::OpShiftRightLogical), byte_offset, CU(shift));
}

u32 Lift_::BufferDword(u32 slot, u32 index) {
    if (in.tone_curve && m.IsConst(index)) {
        const u32 word = m.ConstBits(index);
        const u32 loaded = BufferDwordRaw(slot, index);
        tone_words[loaded] = word;
        return loaded;
    }
    return BufferDwordRaw(slot, index);
}

u32 Lift_::BufferDwordRaw(u32 slot, u32 index) {
    const BufferSlot& buffer = in.buffers[slot];
    if (const u32 offset = BufferOffsetDwords(slot, 2)) {
        index = IOp(O(spv::Op::OpIAdd), index, offset);
    }
    if (buffer.uniform) {
        const bool dwords = in.opt.uniform_dword_layout;
        if (!uniform_block) {
            const u32 array = m.AddType(O(spv::Op::OpTypeArray), {dwords ? t_u32 : t_v4u, CU(dwords ? 16384 : 4096)});
            m.Decorate(array, u32(spv::Decoration::ArrayStride), dwords ? 4 : 16);
            const u32 words[] = {array};
            uniform_block = m.AddUniqueType(O(spv::Op::OpTypeStruct), words);
            m.Decorate(uniform_block, u32(spv::Decoration::Block));
            m.MemberDecorate(uniform_block, 0, u32(spv::Decoration::Offset), 0);
            uniform_elem_ptr = PtrType(u32(spv::StorageClass::Uniform), dwords ? t_u32 : t_v4u);
        }
        if (!buffer_uniform[slot]) {
            const u32 var = m.Variable(PtrType(u32(spv::StorageClass::Uniform), uniform_block),
                                       u32(spv::StorageClass::Uniform));
            m.Decorate(var, u32(spv::Decoration::Binding), buffer.binding);
            m.Decorate(var, u32(spv::Decoration::DescriptorSet), 0);
            m.Interface(var);
            buffer_uniform[slot] = var;
        }
        if (!dwords) {
            const u32 vec_index = IOp(O(spv::Op::OpShiftRightLogical), index, CU(2));
            const u32 component = IOp(O(spv::Op::OpBitwiseAnd), index, CU(3));
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), uniform_elem_ptr, Ty::None, {buffer_uniform[slot], CU(0), vec_index});
            const u32 vec = Emit(O(spv::Op::OpLoad), t_v4u, Ty::V4U, {ptr});
            return Emit(O(spv::Op::OpVectorExtractDynamic), t_u32, Ty::U32, {vec, component});
        }
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), uniform_elem_ptr, Ty::None,
                             {buffer_uniform[slot], CU(0), index});
        return Emit(O(spv::Op::OpLoad), t_u32, Ty::U32, {ptr});
    }
    const u32 var = BufferVar(slot, Ty::U32);
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), t_u32),
                         Ty::None, {var, CU(0), index});
    return Emit(O(spv::Op::OpLoad), t_u32, Ty::U32, {ptr});
}

void Lift_::NoteResource(ResourceKind kind, u32 slot, u32 base) {
    for (const UsedResource& used : out.resources) {
        if (used.kind == kind && used.slot == slot) {
            return;
        }
    }
    UsedResource used;
    used.kind = kind;
    used.slot = slot;
    const u32 origin = m.Origin(s.r[base]);
    used.ud_reg = -1;
    used.table_dword = -1;
    if (origin != 0) {
        const u32 flat = origin - 1;
        if (flat < 16) {
            used.ud_reg = s32(flat);
        } else {
            for (const FlatEntry& entry : in.flat_table) {
                if (entry.flat == flat) {
                    used.ud_reg = entry.parent;
                    used.table_dword = s32(entry.offset);
                }
            }
        }
    }
    out.resources.push_back(used);
}

static bool MatchSharp(const Module& m, const State& s, const Sharp& sharp, u32 base, u32 dwords) {
    for (u32 i = 0; i < dwords; ++i) {
        const u32 id = s.r[base + i];
        if (id == 0) {
            return false;
        }
        if (sharp.flat[i] != NoFlat) {
            if (m.Origin(id) != u32(sharp.flat[i]) + 1) {
                return false;
            }
        } else if (!m.IsConst(id) || m.ConstBits(id) != sharp.imm[i]) {
            return false;
        }
    }
    return true;
}

s32 Lift_::FindBuffer(const std::array<u32, 4>& regs) {
    for (u32 i = 0; i < in.buffers.size(); ++i) {
        if (in.buffers[i].kind == BufferKind::Guest &&
            MatchSharp(m, s, in.buffers[i].sharp, regs[0], 4)) {
            NoteResource(ResourceKind::Buffer, i, regs[0]);
            return s32(i);
        }
    }
    for (u32 i = 0; i < in.buffers.size(); ++i) {
        if (in.buffers[i].kind == BufferKind::Guest &&
            MatchSharp(m, s, in.buffers[i].sharp, regs[0], 1)) {
            NoteResource(ResourceKind::Buffer, i, regs[0]);
            return s32(i);
        }
    }
    if (found && found->buffers.size() < 40) {
        BufferSlot slot;
        const u32 origin = s.r[regs[0]] ? m.Origin(s.r[regs[0]]) : 0;
        if (origin != 0) {
            slot.sharp.flat[0] = u16(origin - 1);
        } else if (s.r[regs[0]] && m.IsConst(s.r[regs[0]])) {
            slot.sharp.imm[0] = m.ConstBits(s.r[regs[0]]);
        } else {
            return -1;
        }
        slot.stride = 4;
        slot.data_format = 14;
        slot.num_format = 7;
        slot.binding = u32(found->buffers.size());
        slot.offset_slot = slot.binding;
        found->buffers.push_back(slot);
        NoteResource(ResourceKind::Buffer, u32(found->buffers.size() - 1), regs[0]);
        return s32(found->buffers.size() - 1);
    }
    return -1;
}

s32 Lift_::FindImage(u32 base, bool r128, bool array) {
    for (u32 i = 0; i < in.images.size(); ++i) {
        if (in.images[i].array == array && MatchSharp(m, s, in.images[i].sharp, base, in.discover ? 1 : r128 ? 4 : 8)) {
            NoteResource(ResourceKind::Image, i, base);
            return s32(i);
        }
    }
    if (found && found->images.size() < 64) {
        ImageSlot slot;
        const u32 origin = s.r[base] ? m.Origin(s.r[base]) : 0;
        if (origin == 0) {
            return -1;
        }
        slot.sharp.flat[0] = u16(origin - 1);
        slot.array = array;
        slot.view = array ? View::D2Array : View::D2;
        found->images.push_back(slot);
        NoteResource(ResourceKind::Image, u32(found->images.size() - 1), base);
        return s32(found->images.size() - 1);
    }
    return -1;
}

s32 Lift_::FindSampler(u32 base, bool depth) {
    for (u32 i = 0; i < in.samplers.size(); ++i) {
        if (in.samplers[i].depth == depth && MatchSharp(m, s, in.samplers[i].sharp, base, in.discover ? 1 : 4)) {
            NoteResource(ResourceKind::Sampler, i, base);
            return s32(i);
        }
    }
    for (u32 i = 0; !in.discover && i < in.samplers.size(); ++i) {
        if (MatchSharp(m, s, in.samplers[i].sharp, base, 1)) {
            NoteResource(ResourceKind::Sampler, i, base);
            return s32(i);
        }
    }
    if (found && found->samplers.size() < 16) {
        SamplerSlot slot;
        const u32 origin = s.r[base] ? m.Origin(s.r[base]) : 0;
        if (origin == 0) {
            if (!s.r[base] || !m.IsConst(s.r[base])) {
                return -1;
            }
            slot.sharp.imm[0] = m.ConstBits(s.r[base]);
        } else {
            slot.sharp.flat[0] = u16(origin - 1);
        }
        slot.depth = depth;
        found->samplers.push_back(slot);
        NoteResource(ResourceKind::Sampler, u32(found->samplers.size() - 1), base);
        return s32(found->samplers.size() - 1);
    }
    return -1;
}

void Lift_::LiftSmrd(const Inst& inst) {
    u32 count = 1;
    switch (inst.op) {
    case Op::SMRD_S_LOAD_DWORD:
    case Op::SMRD_S_BUFFER_LOAD_DWORD:
        count = 1;
        break;
    case Op::SMRD_S_LOAD_DWORDX2:
    case Op::SMRD_S_BUFFER_LOAD_DWORDX2:
        count = 2;
        break;
    case Op::SMRD_S_LOAD_DWORDX4:
    case Op::SMRD_S_BUFFER_LOAD_DWORDX4:
        count = 4;
        break;
    case Op::SMRD_S_LOAD_DWORDX8:
    case Op::SMRD_S_BUFFER_LOAD_DWORDX8:
        count = 8;
        break;
    case Op::SMRD_S_LOAD_DWORDX16:
    case Op::SMRD_S_BUFFER_LOAD_DWORDX16:
        count = 16;
        break;
    default:
        Fail("smrd " + std::to_string(inst.op));
    }
    const bool buffer_load = inst.op >= Op::SMRD_S_BUFFER_LOAD_DWORD;
    if (!buffer_load) {
        const u32 base = s.r[inst.src[0]];
        const u32 origin = base ? m.Origin(base) : 0;
        if (origin == 0) {
            Fail(base == 0 ? "s_load through an empty register"
                 : m.IsConst(base) ? "s_load through a constant"
                 : nodes[m.NodeOf(base)].op == O(spv::Op::OpPhi) ? "s_load through a merge of pointers"
                 : "s_load through a pointer that is not in the user data");
        }
        if (!inst.imm && inst.offset != SrcLiteral) {
            Fail("s_load with a computed offset");
        }
        const u32 load_offset = inst.imm ? inst.offset : inst.literal;
        for (u32 i = 0; i < count; ++i) {
            const u32 flat = FlatIndexOf(origin - 1, load_offset + i);
            u32 id;
            if (flat == NoFlat) {
                id = Emit(O(spv::Op::OpUndef), t_u32, Ty::U32, {});
                undefined_reads.push_back(id);
                m.SetOrigin(id, 0x10000 + ((origin - 1) << 16) + load_offset + i);
            } else {
                id = FlatLoad(flat);
                m.SetOrigin(id, flat + 1);
            }
            s.r[inst.dst + i] = id;
        }
        return;
    }
    if (TessBufferLoad(inst, count)) {
        return;
    }
    const s32 slot = FindBuffer({inst.src[0], u32(inst.src[0] + 1), u32(inst.src[0] + 2), u32(inst.src[0] + 3)});
    if (slot < 0) {
        Fail("constant buffer not in the resource list");
    }
    u32 index;
    if (inst.imm) {
        index = CU(inst.offset);
    } else if (inst.offset == SrcLiteral) {
        index = CU(inst.literal);
    } else {
        index = IOp(O(spv::Op::OpShiftRightLogical), ToU(s.r[inst.offset]), CU(2));
    }
    for (u32 i = 0; i < count; ++i) {
        s.r[inst.dst + i] = BufferDword(u32(slot), IOp(O(spv::Op::OpIAdd), index, CU(i)));
    }
}

u32 Lift_::BufferAddress(const Inst& inst, const BufferSlot& slot, u32 shift) {
    const u32 mask = (1u << shift) - 1;
    const u32 soffset = ToU(SrcRaw(inst.src[3], inst));
    const u32 vaddr = SrcVgpr + inst.src[0];
    if (inst.idxen && !inst.offen && m.IsConst(soffset) && !slot.swizzle_enable && !slot.add_tid &&
        (slot.stride & mask) == 0) {
        const u32 total = m.ConstBits(soffset) + inst.offset;
        if ((total & mask) == 0) {
            return IOp(O(spv::Op::OpIAdd), IOp(O(spv::Op::OpIMul), ToU(s.r[vaddr]), CU(slot.stride >> shift)),
                       CU(total >> shift));
        }
    }
    u32 index = CU(0);
    if (inst.idxen) {
        index = ToU(s.r[vaddr]);
    }
    if (slot.add_tid) {
        Fail("buffer with a thread id");
    }
    u32 offset = IOp(O(spv::Op::OpIAdd), CU(inst.offset), soffset);
    if (inst.offen) {
        offset = IOp(O(spv::Op::OpIAdd), offset, ToU(s.r[inst.idxen ? vaddr + 1 : vaddr]));
    }
    u32 address;
    if (slot.swizzle_enable) {
        const u32 index_stride = CU(slot.index_stride);
        const u32 element = CU(slot.element_size);
        const u32 index_msb = Emit(O(spv::Op::OpUDiv), t_u32, Ty::U32, {index, index_stride});
        const u32 index_lsb = Emit(O(spv::Op::OpUMod), t_u32, Ty::U32, {index, index_stride});
        const u32 offset_msb = Emit(O(spv::Op::OpUDiv), t_u32, Ty::U32, {offset, element});
        const u32 offset_lsb = Emit(O(spv::Op::OpUMod), t_u32, Ty::U32, {offset, element});
        const u32 high = IOp(O(spv::Op::OpIMul),
                             IOp(O(spv::Op::OpIAdd), IOp(O(spv::Op::OpIMul), index_msb, CU(slot.stride)),
                                 IOp(O(spv::Op::OpIMul), offset_msb, element)),
                             index_stride);
        address = IOp(O(spv::Op::OpIAdd), high,
                      IOp(O(spv::Op::OpIAdd), IOp(O(spv::Op::OpIMul), index_lsb, element), offset_lsb));
    } else {
        address = IOp(O(spv::Op::OpIAdd), IOp(O(spv::Op::OpIMul), index, CU(slot.stride)), offset);
    }
    if (shift != 0) {
        address = IOp(O(spv::Op::OpShiftRightLogical), address, CU(shift));
    }
    return address;
}

bool Lift_::MatchSharpAt(const Sharp& sharp, u32 base) const {
    return MatchSharp(m, s, sharp, base, 1);
}

void Lift_::LiftBuffer(const Inst& inst) {
    if (in.discover && in.stage == Stage::Geometry && inst.glc && inst.slc) {
        return;
    }
    if (inst.enc == Enc::Mubuf) {
        if (inst.op >= Op::MUBUF_STORE_DWORD && inst.op <= Op::MUBUF_STORE_DWORDX3) {
            static constexpr u32 sizes[4] = {1, 2, 4, 3};
            if (TessBufferStore(inst, sizes[inst.op - Op::MUBUF_STORE_DWORD])) {
                return;
            }
        }
        if (inst.op == Op::MUBUF_LOAD_DWORD && TessBufferLoadRing(inst, 1)) {
            return;
        }
    }
    const u32 base = inst.src[2] * 4u;
    const s32 buffer_index = FindBuffer({base, base + 1, base + 2, base + 3});
    if (buffer_index < 0) {
        Fail("buffer not in the resource list");
    }
    const u32 slot_index = u32(buffer_index);
    const BufferSlot& slot = in.buffers[slot_index];
    if (found) {
        found->buffers[slot_index].not_only_constants = true;
    }
    const u32 vdata = SrcVgpr + inst.src[1];
    u32 count = 0;
    bool store = false;
    bool typed = false;
    bool atomic = false;
    u8 dfmt = 0;
    u8 nfmt = 0;
    if (inst.enc == Enc::Mtbuf) {
        typed = true;
        store = inst.op >= 4;
        count = (inst.op & 3) + 1;
        dfmt = inst.dfmt;
        nfmt = inst.nfmt;
    } else {
        switch (inst.op) {
        case Op::MUBUF_LOAD_FORMAT_X:
        case Op::MUBUF_LOAD_FORMAT_XY:
        case Op::MUBUF_LOAD_FORMAT_XYZ:
        case Op::MUBUF_LOAD_FORMAT_XYZW:
            typed = true;
            count = inst.op + 1;
            dfmt = slot.data_format;
            nfmt = slot.num_format;
            break;
        case Op::MUBUF_STORE_FORMAT_X:
        case Op::MUBUF_STORE_FORMAT_XY:
        case Op::MUBUF_STORE_FORMAT_XYZ:
        case Op::MUBUF_STORE_FORMAT_XYZW:
            typed = true;
            store = true;
            count = inst.op - 3;
            dfmt = slot.data_format;
            nfmt = slot.num_format;
            break;
        case Op::MUBUF_LOAD_DWORD:
            count = 1;
            break;
        case Op::MUBUF_LOAD_DWORDX2:
            count = 2;
            break;
        case Op::MUBUF_LOAD_DWORDX3:
            count = 3;
            break;
        case Op::MUBUF_LOAD_DWORDX4:
            count = 4;
            break;
        case Op::MUBUF_STORE_DWORD:
            count = 1;
            store = true;
            break;
        case Op::MUBUF_STORE_DWORDX2:
            count = 2;
            store = true;
            break;
        case Op::MUBUF_STORE_DWORDX3:
            count = 3;
            store = true;
            break;
        case Op::MUBUF_STORE_DWORDX4:
            count = 4;
            store = true;
            break;
        case Op::MUBUF_ATOMIC_ADD:
            atomic = true;
            count = 1;
            break;
        default:
            Fail("mubuf " + std::to_string(inst.op));
        }
    }
    const u32 u32_ptr = PtrType(u32(spv::StorageClass::StorageBuffer), t_u32);
    const u32 f32_ptr = PtrType(u32(spv::StorageClass::StorageBuffer), t_f32);
    const u32 exec = Exec();
    if (atomic) {
        if (!IsTrue(exec)) {
            Fail("buffer atomic under an unknown exec");
        }
        u32 address = BufferAddress(inst, slot, 2);
        if (const u32 off = BufferOffsetDwords(slot_index, 2)) {
            address = IOp(O(spv::Op::OpIAdd), address, off);
        }
        const u32 var = BufferVar(slot_index, Ty::U32);
        const u32 value = ToU(s.r[vdata]);
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), u32_ptr, Ty::None, {var, CU(0), address});
        const u32 prev = Emit(O(spv::Op::OpAtomicIAdd), t_u32, Ty::U32,
                              {ptr, CU(u32(spv::Scope::Device)), CU(0), value});
        nodes[m.NodeOf(prev)].effect = true;
        if (inst.glc) {
            s.r[vdata] = prev;
        }
        return;
    }
    if (found && store) {
        found->buffers[slot_index].written = true;
    }
    if (typed && inst.enc == Enc::Mubuf) {
        if (!store) {
            const std::array<u32, 4> comps = FormatLoad(inst, slot, slot_index);
            for (u32 i = 0; i < count; ++i) {
                SetVgpr(vdata + i, comps[i]);
            }
            return;
        }
        std::array<u32, 4> comps{CF(0.f), CF(0.f), CF(0.f), CF(0.f)};
        for (u32 i = 0; i < count; ++i) {
            comps[i] = ToF(s.r[vdata + i]);
        }
        FormatStore(inst, slot, slot_index, comps);
        return;
    }
    if (typed && dfmt != 4 && dfmt != 11 && dfmt != 13 && dfmt != 14) {
        Fail("typed buffer instruction of format " + std::to_string(dfmt));
    }
    if (typed && nfmt != 7) {
        Fail("typed buffer instruction of number format " + std::to_string(nfmt));
    }
    const u32 num_components = !typed ? count : dfmt == 4 ? 1 : dfmt == 11 ? 2 : dfmt == 13 ? 3 : 4;
    u32 address = BufferAddress(inst, slot, 2);
    if (const u32 off = BufferOffsetDwords(slot_index, 2)) {
        address = IOp(O(spv::Op::OpIAdd), address, off);
    }
    const auto dword = [&](u32 i) { return i == 0 ? address : IOp(O(spv::Op::OpIAdd), address, CU(i)); };
    if (!store) {
        const Ty element = typed ? Ty::F32 : Ty::U32;
        const u32 var = BufferVar(slot_index, element);
        u32 values[4]{};
        for (u32 i = 0; i < 4; ++i) {
            if (i < num_components && i < count) {
                const u32 ptr = Emit(O(spv::Op::OpAccessChain), typed ? f32_ptr : u32_ptr, Ty::None, {var, CU(0), dword(i)});
                values[i] = Emit(O(spv::Op::OpLoad), typed ? t_f32 : t_u32, element, {ptr});
            } else {
                values[i] = CF(0.f);
            }
        }
        for (u32 i = 0; i < count; ++i) {
            u32 value = values[i];
            if (typed && inst.enc == Enc::Mubuf) {
                const u8 sel = slot.swizzle[i];
                value = sel == 0 ? CF(0.f) : sel == 1 ? CF(1.f) : values[(sel - 4) & 3];
                if (slot.conv != Conv::None) {
                    value = ApplyConv(value, slot.conv);
                }
            }
            SetVgpr(vdata + i, value);
        }
        return;
    }
    const Ty element = typed ? Ty::F32 : Ty::U32;
    const u32 var = BufferVar(slot_index, element);
    u32 merge = 0;
    BeginGuard(merge);
    for (u32 i = 0; i < num_components && (typed ? true : i < count); ++i) {
        u32 value;
        if (typed) {
            value = i < count ? ToF(s.r[vdata + i]) : CF(0.f);
        } else {
            value = ToU(s.r[vdata + i]);
        }
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), typed ? f32_ptr : u32_ptr, Ty::None, {var, CU(0), dword(i)});
        EmitVoid(O(spv::Op::OpStore), {ptr, value});
    }
    EndGuard(merge);
}

u32 Lift_::BeginGuard(u32& merge) {
    const u32 exec = Exec();
    if (IsTrue(exec)) {
        merge = 0;
        return 0;
    }
    merge = m.NewId();
    const u32 body = m.NewId();
    EmitVoid(O(spv::Op::OpSelectionMerge), {merge, 0}, 2);
    EmitVoid(O(spv::Op::OpBranchConditional), {exec, body, merge});
    Open(body);
    ++out.stats.ifs_for_exec;
    return body;
}

void Lift_::EndGuard(u32 merge) {
    if (merge == 0) {
        return;
    }
    EmitVoid(O(spv::Op::OpBranch), {merge});
    Open(merge);
}

u32 Lift_::BufferNarrow(u32 slot_index, u32 address, Ty element) {
    const u32 scalar_var = BufferVar(slot_index, element);
    const u32 scalar = element == Ty::U16 ? t_u16 : t_u8;
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), scalar), Ty::None, {scalar_var, CU(0), address});
    const u32 raw = Emit(O(spv::Op::OpLoad), scalar, element, {ptr});
    return Emit(O(spv::Op::OpUConvert), t_u32, Ty::U32, {raw});
}

void Lift_::BufferNarrowStore(u32 slot_index, u32 address, Ty element, u32 value) {
    const u32 scalar_var = BufferVar(slot_index, element);
    const u32 scalar = element == Ty::U16 ? t_u16 : t_u8;
    const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), scalar), Ty::None, {scalar_var, CU(0), address});
    EmitVoid(O(spv::Op::OpStore), {ptr, Emit(O(spv::Op::OpUConvert), scalar, element, {value})});
}

u32 Lift_::Uf11ToF(u32 value, u32 mantissa_bits) {
    const u32 exponent = BfeU(value, CU(mantissa_bits), CU(5));
    const u32 mantissa = BfeU(value, CU(0), CU(mantissa_bits));
    const u32 mantissa_f = Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {mantissa});
    const u32 a = Select(ICmp(O(spv::Op::OpINotEqual), mantissa, CU(0)),
                         FMul(CF(1.f / float(1u << (14 + mantissa_bits))), mantissa_f), CF(0.f));
    const u32 b = ToF(IOp(O(spv::Op::OpBitwiseOr), mantissa, CU(0x7f800000u)));
    const u32 exponent_c = IOp(O(spv::Op::OpISub), exponent, CU(15));
    const u32 scale_a = FDiv(CF(1.f), Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32,
                                           {IOp(O(spv::Op::OpShiftLeftLogical), CU(1), Emit(O(spv::Op::OpSNegate), t_u32, Ty::U32, {exponent_c}))}));
    const u32 scale_b = Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {IOp(O(spv::Op::OpShiftLeftLogical), CU(1), exponent_c)});
    const u32 scale = Select(ICmp(O(spv::Op::OpSLessThan), exponent_c, CU(0)), scale_a, scale_b);
    const u32 c = FMul(scale, FAdd(CF(1.f), FDiv(mantissa_f, CF(float(1u << mantissa_bits)))));
    return Select(ICmp(O(spv::Op::OpIEqual), exponent, CU(0)), a,
                  Select(ICmp(O(spv::Op::OpIEqual), exponent, CU(31)), b, c));
}

u32 Lift_::FToUf11(u32 value, u32 mantissa_bits) {
    const u32 raw = ToU(value);
    const u32 exponent = BfeS(raw, CU(23), CU(8));
    const u32 sign = BfeU(raw, CU(31), CU(1));
    const u32 is_zero = Or(ICmp(O(spv::Op::OpIEqual), raw, CU(0)), ICmp(O(spv::Op::OpIEqual), sign, CU(1)));
    const u32 is_nan = Emit(O(spv::Op::OpIsNan), t_bool, Ty::Bool, {value});
    const u32 is_inf = Emit(O(spv::Op::OpIsInf), t_bool, Ty::Bool, {value});
    const u32 is_denorm = ICmp(O(spv::Op::OpSLessThanEqual), exponent, CU(u32(-15)));
    const auto round = [&](u32 v) { return Ext(GLSLstd450RoundEven, t_f32, Ty::F32, {v}); };
    const u32 denorm_mantissa = Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {round(FMul(value, CF(float(1u << (mantissa_bits + 14)))))});
    const u32 denorm_overflow = ICmp(O(spv::Op::OpINotEqual), IOp(O(spv::Op::OpShiftRightLogical), denorm_mantissa, CU(mantissa_bits)), CU(0));
    const u32 denorm = Select(denorm_overflow, CU(1u << mantissa_bits), denorm_mantissa);
    const u32 scaled = Ext(GLSLstd450Ldexp, t_f32, Ty::F32, {value, IOp(O(spv::Op::OpISub), CU(mantissa_bits), exponent)});
    const u32 norm_mantissa = Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {round(scaled)});
    const u32 norm_overflow = ICmp(O(spv::Op::OpUGreaterThanEqual), norm_mantissa, CU(2u << mantissa_bits));
    const u32 final_mantissa = IOp(O(spv::Op::OpBitwiseAnd),
                                   Select(norm_overflow, IOp(O(spv::Op::OpShiftRightLogical), norm_mantissa, CU(1)), norm_mantissa),
                                   CU((1u << mantissa_bits) - 1));
    const u32 final_exponent = IOp(O(spv::Op::OpIAdd), Select(norm_overflow, IOp(O(spv::Op::OpIAdd), exponent, CU(1)), exponent), CU(15));
    const u32 norm = IOp(O(spv::Op::OpBitwiseOr), final_mantissa, IOp(O(spv::Op::OpShiftLeftLogical), final_exponent, CU(mantissa_bits)));
    return Select(is_zero, CU(0), Select(is_nan, CU((31u << mantissa_bits) | 1), Select(is_inf, CU(31u << mantissa_bits), Select(is_denorm, denorm, norm))));
}

std::array<u32, 4> Lift_::FormatLoad(const Inst& inst, const BufferSlot& slot, u32 slot_index) {
    const u32 dfmt = slot.data_format;
    const u32 nfmt = slot.num_format;
    const auto shift_of = [&] {
        return dfmt == 1 ? 0u : (dfmt == 3 || dfmt == 2) ? 1u : 2u;
    };
    const u32 shift = shift_of();
    u32 address = BufferAddress(inst, slot, shift);
    if (const u32 off = BufferOffsetDwords(slot_index, shift)) {
        address = IOp(O(spv::Op::OpIAdd), address, off);
    }
    const auto dword = [&](u32 index) {
        const u32 at = index == 0 ? address : IOp(O(spv::Op::OpIAdd), address, CU(index));
        const u32 var = BufferVar(slot_index, Ty::U32);
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), t_u32), Ty::None, {var, CU(0), at});
        return Emit(O(spv::Op::OpLoad), t_u32, Ty::U32, {ptr});
    };
    const auto bytes = [&](u32 raw, std::array<u32, 4>& out_comps, u32 comps) {
        switch (nfmt) {
        case 0:
        case 1: {
            const u32 vec = Ext(nfmt == 0 ? GLSLstd450UnpackUnorm4x8 : GLSLstd450UnpackSnorm4x8, t_v4f, Ty::V4F, {raw});
            for (u32 i = 0; i < comps; ++i) {
                out_comps[i] = Extract(t_f32, Ty::F32, vec, i);
            }
            break;
        }
        case 4:
            for (u32 i = 0; i < comps; ++i) {
                out_comps[i] = ToF(BfeU(raw, CU(i * 8), CU(8)));
            }
            break;
        case 5:
            for (u32 i = 0; i < comps; ++i) {
                out_comps[i] = ToF(BfeS(raw, CU(i * 8), CU(8)));
            }
            break;
        default:
            Fail("8 bit buffer of number format " + std::to_string(nfmt));
        }
    };
    const auto halves = [&](u32 raw, std::array<u32, 4>& out_comps, u32 first, u32 comps) {
        switch (nfmt) {
        case 0:
        case 1:
        case 7: {
            const auto fn = nfmt == 0 ? GLSLstd450UnpackUnorm2x16 : nfmt == 1 ? GLSLstd450UnpackSnorm2x16 : GLSLstd450UnpackHalf2x16;
            const u32 pair = Ext(fn, t_v2f, Ty::V2F, {raw});
            for (u32 i = 0; i < comps; ++i) {
                out_comps[first + i] = Extract(t_f32, Ty::F32, pair, i);
            }
            break;
        }
        case 4:
            for (u32 i = 0; i < comps; ++i) {
                out_comps[first + i] = ToF(BfeU(raw, CU(i * 16), CU(16)));
            }
            break;
        case 5:
            for (u32 i = 0; i < comps; ++i) {
                out_comps[first + i] = ToF(BfeS(raw, CU(i * 16), CU(16)));
            }
            break;
        default:
            Fail("16 bit buffer of number format " + std::to_string(nfmt));
        }
    };
    std::array<u32, 4> raw{CF(0.f), CF(0.f), CF(0.f), CF(0.f)};
    u32 components = 0;
    switch (dfmt) {
    case 0:
        break;
    case 1:
        bytes(BufferNarrow(slot_index, address, Ty::U8), raw, 1);
        components = 1;
        break;
    case 3:
        bytes(BufferNarrow(slot_index, address, Ty::U16), raw, 2);
        components = 2;
        break;
    case 10:
        bytes(dword(0), raw, 4);
        components = 4;
        break;
    case 2:
        halves(BufferNarrow(slot_index, address, Ty::U16), raw, 0, 1);
        components = 1;
        break;
    case 5:
        halves(dword(0), raw, 0, 2);
        components = 2;
        break;
    case 12:
        halves(dword(0), raw, 0, 2);
        halves(dword(1), raw, 2, 2);
        components = 4;
        break;
    case 6: {
        if (nfmt != 7) {
            Fail("10_11_11 buffer of number format " + std::to_string(nfmt));
        }
        const u32 packed = dword(0);
        raw[0] = Uf11ToF(BfeU(packed, CU(0), CU(11)), 6);
        raw[1] = Uf11ToF(BfeU(packed, CU(11), CU(11)), 6);
        raw[2] = Uf11ToF(BfeU(packed, CU(22), CU(10)), 5);
        components = 3;
        break;
    }
    case 4:
    case 11:
    case 13:
    case 14: {
        components = dfmt == 4 ? 1 : dfmt == 11 ? 2 : dfmt == 13 ? 3 : 4;
        const u32 var = BufferVar(slot_index, Ty::F32);
        for (u32 i = 0; i < components; ++i) {
            const u32 at = i == 0 ? address : IOp(O(spv::Op::OpIAdd), address, CU(i));
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), t_f32), Ty::None, {var, CU(0), at});
            raw[i] = Emit(O(spv::Op::OpLoad), t_f32, Ty::F32, {ptr});
        }
        break;
    }
    default:
        Fail("buffer of format " + std::to_string(dfmt));
    }
    for (u32 i = 0; i < components; ++i) {
        raw[i] = ApplyConv(raw[i], slot.conv);
    }
    std::array<u32, 4> result{};
    for (u32 i = 0; i < 4; ++i) {
        const u8 sel = slot.swizzle[i];
        result[i] = sel == 0 ? CF(0.f) : sel == 1 ? CF(1.f) : raw[(sel - 4) & 3];
    }
    return result;
}

void Lift_::FormatStore(const Inst& inst, const BufferSlot& slot, u32 slot_index, std::array<u32, 4> comps) {
    const u32 dfmt = slot.data_format;
    const u32 nfmt = slot.num_format;
    std::array<u32, 4> value{};
    for (u32 c = 0; c < 4; ++c) {
        u32 picked = CF(0.f);
        for (u32 d = 0; d < 4; ++d) {
            if (slot.swizzle[d] == 4 + c) {
                picked = comps[d];
                break;
            }
        }
        value[c] = picked;
    }
    const u32 components = dfmt == 1 || dfmt == 2 || dfmt == 4 ? 1 : dfmt == 3 || dfmt == 5 || dfmt == 11 ? 2 : dfmt == 6 || dfmt == 13 ? 3 : dfmt == 0 ? 0 : 4;
    for (u32 c = 0; c < components; ++c) {
        value[c] = ApplyWriteConv(value[c], slot.conv);
    }
    const u32 shift = dfmt == 1 ? 0u : (dfmt == 3 || dfmt == 2) ? 1u : 2u;
    u32 address = BufferAddress(inst, slot, shift);
    if (const u32 off = BufferOffsetDwords(slot_index, shift)) {
        address = IOp(O(spv::Op::OpIAdd), address, off);
    }
    const auto write_dword = [&](u32 index, u32 raw) {
        const u32 at = index == 0 ? address : IOp(O(spv::Op::OpIAdd), address, CU(index));
        const u32 var = BufferVar(slot_index, Ty::U32);
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), t_u32), Ty::None, {var, CU(0), at});
        EmitVoid(O(spv::Op::OpStore), {ptr, raw});
    };
    const auto pack8 = [&](u32 x, u32 y, u32 z, u32 w) {
        if (nfmt == 0 || nfmt == 1) {
            return Ext(nfmt == 0 ? GLSLstd450PackUnorm4x8 : GLSLstd450PackSnorm4x8, t_u32, Ty::U32, {ComponentsF({x, y, z, w})});
        }
        if (nfmt != 4 && nfmt != 5) {
            Fail("8 bit store of number format " + std::to_string(nfmt));
        }
        const auto byte = [&](u32 v, u32 at) {
            return IOp(O(spv::Op::OpShiftLeftLogical), IOp(O(spv::Op::OpBitwiseAnd), ToU(v), CU(255)), CU(at));
        };
        return IOp(O(spv::Op::OpBitwiseOr), IOp(O(spv::Op::OpBitwiseOr), byte(x, 0), byte(y, 8)),
                   IOp(O(spv::Op::OpBitwiseOr), byte(z, 16), byte(w, 24)));
    };
    const auto pack16 = [&](u32 x, u32 y) {
        if (nfmt == 0 || nfmt == 1 || nfmt == 7) {
            const auto fn = nfmt == 0 ? GLSLstd450PackUnorm2x16 : nfmt == 1 ? GLSLstd450PackSnorm2x16 : GLSLstd450PackHalf2x16;
            return Ext(fn, t_u32, Ty::U32, {ComponentsF({x, y})});
        }
        if (nfmt != 4 && nfmt != 5) {
            Fail("16 bit store of number format " + std::to_string(nfmt));
        }
        const auto half = [&](u32 v, u32 at) {
            return IOp(O(spv::Op::OpShiftLeftLogical), IOp(O(spv::Op::OpBitwiseAnd), ToU(v), CU(65535)), CU(at));
        };
        return IOp(O(spv::Op::OpBitwiseOr), half(x, 0), half(y, 16));
    };
    u32 merge = 0;
    BeginGuard(merge);
    const u32 zero = CF(0.f);
    switch (dfmt) {
    case 0:
        break;
    case 1:
        BufferNarrowStore(slot_index, address, Ty::U8, pack8(value[0], zero, zero, zero));
        break;
    case 3:
        BufferNarrowStore(slot_index, address, Ty::U16, pack8(value[0], value[1], zero, zero));
        break;
    case 10:
        write_dword(0, pack8(value[0], value[1], value[2], value[3]));
        break;
    case 2:
        BufferNarrowStore(slot_index, address, Ty::U16, pack16(value[0], zero));
        break;
    case 5:
        write_dword(0, pack16(value[0], value[1]));
        break;
    case 12:
        write_dword(0, pack16(value[0], value[1]));
        write_dword(1, pack16(value[2], value[3]));
        break;
    case 6: {
        if (nfmt != 7) {
            Fail("10_11_11 store of number format " + std::to_string(nfmt));
        }
        const u32 x = FToUf11(value[0], 6);
        const u32 y = FToUf11(value[1], 6);
        const u32 z = FToUf11(value[2], 5);
        write_dword(0, IOp(O(spv::Op::OpBitwiseOr), x,
                           IOp(O(spv::Op::OpBitwiseOr), IOp(O(spv::Op::OpShiftLeftLogical), y, CU(11)),
                               IOp(O(spv::Op::OpShiftLeftLogical), z, CU(22)))));
        break;
    }
    case 4:
    case 11:
    case 13:
    case 14: {
        const u32 var = BufferVar(slot_index, Ty::F32);
        for (u32 i = 0; i < components; ++i) {
            const u32 at = i == 0 ? address : IOp(O(spv::Op::OpIAdd), address, CU(i));
            const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), t_f32), Ty::None, {var, CU(0), at});
            EmitVoid(O(spv::Op::OpStore), {ptr, value[i]});
        }
        break;
    }
    default:
        Fail("buffer store of format " + std::to_string(dfmt));
    }
    EndGuard(merge);
}

u32 Lift_::Extract(u32 type, Ty ty, u32 vec, u32 comp) {
    return Emit(O(spv::Op::OpCompositeExtract), type, ty, {vec, comp}, 2);
}

u32 Lift_::ComponentsF(std::initializer_list<u32> parts) {
    const u32 type = parts.size() == 2 ? t_v2f : parts.size() == 3 ? t_v3f : t_v4f;
    const Ty ty = parts.size() == 2 ? Ty::V2F : parts.size() == 3 ? Ty::V3F : Ty::V4F;
    return Emit(O(spv::Op::OpCompositeConstruct), type, ty, parts);
}

u32 Lift_::ComponentsU(std::initializer_list<u32> parts) {
    const u32 type = parts.size() == 2 ? t_v2u : parts.size() == 3 ? t_v3u : t_v4u;
    const Ty ty = parts.size() == 2 ? Ty::V2U : parts.size() == 3 ? Ty::V3U : Ty::V4U;
    return Emit(O(spv::Op::OpCompositeConstruct), type, ty, parts);
}

u32 Lift_::ApplyConv(u32 value, Conv conv) {
    switch (conv) {
    case Conv::None:
        return value;
    case Conv::UintToUscaled:
        return Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {ToU(value)});
    case Conv::SintToSscaled:
        return Emit(O(spv::Op::OpConvertSToF), t_f32, Ty::F32, {ToU(value)});
    case Conv::UnormToUbnorm:
        return FSub(FMul(value, CF(2.f)), CF(1.f));
    case Conv::Sint8ToSnormNz:
    case Conv::Sint16ToSnormNz: {
        const u32 twice = IOp(O(spv::Op::OpIAdd), IOp(O(spv::Op::OpIMul), ToU(value), CU(2)), CU(1));
        const u32 left = Emit(O(spv::Op::OpConvertSToF), t_f32, Ty::F32, {twice});
        return FDiv(left, CF(conv == Conv::Sint8ToSnormNz ? 255.f : 65535.f));
    }
    case Conv::Uint32ToUnorm:
        return FDiv(Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {ToU(value)}), CF(4294967295.f));
    case Conv::SrgbToNorm: {
        const u32 a = Ext(GLSLstd450Pow, t_f32, Ty::F32,
                          {FMul(FAdd(value, CF(0.055f)), CF(1.f / 1.055f)), CF(2.4f)});
        const u32 b = FMul(value, CF(1.f / 12.92f));
        return Select(FCmp(O(spv::Op::OpFOrdGreaterThan), value, CF(0.04045f)), a, b);
    }
    default:
        Fail("number conversion");
    }
}

u32 Lift_::ApplyWriteConv(u32 value, Conv conv) {
    switch (conv) {
    case Conv::None:
        return value;
    case Conv::UintToUscaled:
        return ToF(Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {value}));
    case Conv::SintToSscaled:
        return ToF(Emit(O(spv::Op::OpConvertFToS), t_u32, Ty::U32, {value}));
    case Conv::UnormToUbnorm:
        return FDiv(FAdd(value, CF(1.f)), CF(2.f));
    case Conv::Sint8ToSnormNz:
    case Conv::Sint16ToSnormNz: {
        const u32 clamped = Ext(GLSLstd450FClamp, t_f32, Ty::F32, {value, CF(-1.f), CF(1.f)});
        const u32 scaled = FMul(clamped, CF(conv == Conv::Sint8ToSnormNz ? 255.f : 65535.f));
        const u32 left = FSub(scaled, CF(1.f));
        return ToF(Emit(O(spv::Op::OpConvertFToS), t_u32, Ty::U32, {FDiv(left, CF(2.f))}));
    }
    case Conv::Uint32ToUnorm: {
        const u32 clamped = Ext(GLSLstd450FClamp, t_f32, Ty::F32, {value, CF(0.f), CF(1.f)});
        return ToF(Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {FMul(clamped, CF(4294967295.f))}));
    }
    default:
        Fail("write number conversion");
    }
}

u32 Lift_::ImageVar(u32 slot) {
    if (image_var[slot]) {
        return image_var[slot];
    }
    const ImageSlot& image = in.images[slot];
    const u32 sampled_scalar = image.cls == NumClass::Float ? t_f32 : image.cls == NumClass::Uint ? t_u32 : t_s32;
    u32 dim = u32(spv::Dim::Dim2D);
    u32 arrayed = 0;
    u32 ms = 0;
    switch (image.view) {
    case View::D1:
        dim = u32(spv::Dim::Dim1D);
        break;
    case View::D1Array:
        dim = u32(spv::Dim::Dim1D);
        arrayed = 1;
        break;
    case View::D2:
        break;
    case View::D2Array:
        arrayed = 1;
        break;
    case View::D2Msaa:
        ms = 1;
        break;
    case View::D3:
        dim = u32(spv::Dim::Dim3D);
        break;
    default:
        Fail("image view type");
    }
    if (dim == u32(spv::Dim::Dim1D)) {
        m.Capability(image.storage ? u32(spv::Capability::Image1D) : u32(spv::Capability::Sampled1D));
    }
    const u32 format = image.atomic ? image.atomic_format : 0;
    const u32 type = m.AddType(O(spv::Op::OpTypeImage),
                               {sampled_scalar, dim, 0, arrayed, ms, image.storage ? 2u : 1u, format});
    const u32 var = m.Variable(PtrType(u32(spv::StorageClass::UniformConstant), type),
                               u32(spv::StorageClass::UniformConstant));
    m.Decorate(var, u32(spv::Decoration::Binding), image.binding);
    m.Decorate(var, u32(spv::Decoration::DescriptorSet), 0);
    m.Interface(var);
    image_var[slot] = var;
    image_type[slot] = type;
    if (image.storage) {
        uses_storage_image = true;
    } else {
        sampled_type[slot] = m.AddType(O(spv::Op::OpTypeSampledImage), {type});
    }
    return var;
}

u32 Lift_::SamplerVar(u32 slot) {
    if (sampler_var[slot]) {
        return sampler_var[slot];
    }
    if (!t_sampler) {
        t_sampler = m.AddType(O(spv::Op::OpTypeSampler), {});
    }
    const u32 var = m.Variable(PtrType(u32(spv::StorageClass::UniformConstant), t_sampler),
                               u32(spv::StorageClass::UniformConstant));
    m.Decorate(var, u32(spv::Decoration::Binding), in.samplers[slot].binding);
    m.Decorate(var, u32(spv::Decoration::DescriptorSet), 0);
    m.Interface(var);
    sampler_var[slot] = var;
    return var;
}

u32 Lift_::LoadImage(u32 slot) {
    const u32 var = ImageVar(slot);
    return Emit(O(spv::Op::OpLoad), image_type[slot], Ty::None, {var});
}

u32 Lift_::SampledImage(u32 image_slot, u32 sampler_slot) {
    const u32 image = LoadImage(image_slot);
    const u32 var = SamplerVar(sampler_slot);
    const u32 sampler = Emit(O(spv::Op::OpLoad), t_sampler, Ty::None, {var});
    return Emit(O(spv::Op::OpSampledImage), sampled_type[image_slot], Ty::None, {image, sampler});
}

void Lift_::LiftImage(const Inst& inst) {
    const u32 tsharp = inst.src[2] * 4u;
    const u16 op = inst.op;
    const bool sample = (op >= 32 && op < 64) || (op >= 64 && op < 96);
    if (sample) {
        const bool depth_compare = op >= 64 ? ((op - 64) & 15) >= 8 : ((op - 32) & 15) >= 8;
        const s32 image = FindImage(tsharp, inst.r128, inst.da);
        const s32 sampler = FindSampler(inst.src[3] * 4u, depth_compare);
        if (image < 0 || sampler < 0) {
            Fail("sampled image not in the resource list");
        }
        return ImageSample(inst, image, sampler);
    }
    const s32 image = FindImage(tsharp, inst.r128, inst.da);
    if (image < 0) {
        Fail("image not in the resource list");
    }
    if (found && (op == Op::MIMG_STORE || op == Op::MIMG_STORE_MIP)) {
        found->images[image].storage = true;
    }
    ImageOther(inst, image);
}

void Lift_::ImageSample(const Inst& inst, s32 image_slot, s32 sampler_slot) {
    const ImageSlot& image = in.images[image_slot];
    const SamplerSlot& sampler = in.samplers[sampler_slot];
    const u32 addr = SrcVgpr + inst.src[0];
    const u32 vdata = SrcVgpr + inst.src[1];
    const u16 op = inst.op;
    const bool gather = op >= 64;
    const u32 idx = gather ? op - 64 : op - 32;
    const bool has_offset = idx >= 16;
    const bool compare = (idx & 15) >= 8;
    const u32 kind = idx & 7;
    const bool has_clamp = kind == 1 || kind == 3 || kind == 6;
    const bool has_derivs = !gather && (kind == 2 || kind == 3);
    const bool has_lod = kind == 4;
    const bool has_bias = kind == 5 || kind == 6;
    const bool force_level0 = kind == 7;
    if (gather && (kind == 2 || kind == 3)) {
        Fail("gather with derivatives");
    }
    u32 at = 0;
    const auto addr_f = [&](u32 index) { return ToF(s.r[addr + index]); };
    std::array<u32, 3> offset{};
    u32 offset_count = 0;
    if (has_offset) {
        const u32 raw = ToU(s.r[addr + at++]);
        if (!m.IsConst(raw)) {
            Fail("image offset that is not a constant");
        }
        const u32 bits = m.ConstBits(raw);
        const auto comp = [&](u32 shift) { return s32((bits >> shift) << 26) >> 26; };
        offset = {u32(comp(0)), u32(comp(8)), u32(comp(16))};
        offset_count = image.view == View::D3 ? 3 : (image.view == View::D1 || image.view == View::D1Array) ? 1 : 2;
    }
    const u32 bias = has_bias ? addr_f(at++) : 0;
    const u32 dref = compare ? addr_f(at++) : 0;
    u32 grad_x = 0;
    u32 grad_y = 0;
    if (has_derivs) {
        switch (image.view) {
        case View::D1:
        case View::D1Array:
            grad_x = addr_f(at);
            grad_y = addr_f(at + 1);
            at += 2;
            break;
        case View::D2:
        case View::D2Msaa:
        case View::D2Array:
            grad_x = ComponentsF({addr_f(at), addr_f(at + 1)});
            grad_y = ComponentsF({addr_f(at + 2), addr_f(at + 3)});
            at += 4;
            break;
        case View::D3:
            grad_x = ComponentsF({addr_f(at), addr_f(at + 1), addr_f(at + 2)});
            grad_y = ComponentsF({addr_f(at + 3), addr_f(at + 4), addr_f(at + 5)});
            at += 6;
            break;
        default:
            Fail("derivatives of this image");
        }
    }
    const bool is_msaa = image.view == View::D2Msaa;
    const bool unnormalized = sampler.force_unnormalized || inst.unorm;
    u32 dims = 0;
    const auto dim_of = [&](u32 comp, u32 total) {
        if (!dims) {
            const u32 loaded = LoadImage(u32(image_slot));
            uses_image_query = true;
            const u32 vec = total == 1 ? t_u32 : total == 2 ? t_v2u : t_v3u;
            const Ty ty = total == 1 ? Ty::U32 : total == 2 ? Ty::V2U : Ty::V3U;
            dims = is_msaa ? Emit(O(spv::Op::OpImageQuerySize), vec, ty, {loaded})
                           : Emit(O(spv::Op::OpImageQuerySizeLod), vec, ty, {loaded, CU(0)});
        }
        const u32 value = total == 1 ? dims : Extract(t_u32, Ty::U32, dims, comp);
        return Emit(O(spv::Op::OpConvertUToF), t_f32, Ty::F32, {value});
    };
    const auto coord = [&](u32 index, u32 dim_comp, u32 total) -> u32 {
        const u32 value = addr_f(index);
        if (is_msaa) {
            if (unnormalized) {
                return Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {value});
            }
            return Emit(O(spv::Op::OpConvertFToU), t_u32, Ty::U32, {FMul(value, dim_of(dim_comp, total))});
        }
        if (unnormalized) {
            return FDiv(value, dim_of(dim_comp, total));
        }
        return value;
    };
    u32 coords = 0;
    switch (image.view) {
    case View::D1:
        coords = coord(at, 0, 1);
        at += 1;
        break;
    case View::D1Array:
        coords = ComponentsF({coord(at, 0, 2), coord(at + 1, 1, 2)});
        at += 2;
        break;
    case View::D2:
        coords = ComponentsF({coord(at, 0, 2), coord(at + 1, 1, 2)});
        at += 2;
        break;
    case View::D2Msaa:
        coords = ComponentsU({coord(at, 0, 2), coord(at + 1, 1, 2)});
        at += 2;
        break;
    case View::D2Array: {
        const u32 x = coord(at, 0, 3);
        const u32 y = coord(at + 1, 1, 3);
        const u32 face = addr_f(at + 2);
        if (image.cube) {
            const u32 fixed_x = FSub(x, CF(1.f));
            const u32 fixed_y = FSub(y, CF(1.f));
            const u32 quotient = Ext(GLSLstd450Floor, t_f32, Ty::F32, {FDiv(face, CF(8.f))});
            const u32 fixed_face = FFma(quotient, CF(-2.f), face);
            coords = ComponentsF({fixed_x, fixed_y, fixed_face});
        } else {
            coords = ComponentsF({x, y, face});
        }
        at += 3;
        break;
    }
    case View::D3:
        coords = ComponentsF({coord(at, 0, 3), coord(at + 1, 1, 3), coord(at + 2, 2, 3)});
        at += 3;
        break;
    default:
        Fail("sampled image view");
    }
    const u32 lod = has_lod ? addr_f(at++) : force_level0 ? CF(0.f) : 0;
    (void)has_clamp;
    const bool explicit_lod = has_lod || force_level0;
    const u32 result_type = image.cls == NumClass::Float ? t_v4f : image.cls == NumClass::Uint ? t_v4u : t_v4s;
    const Ty result_ty = image.cls == NumClass::Float ? Ty::V4F : Ty::None;
    const auto to_vec4f = [&](u32 texel) {
        return image.cls == NumClass::Float ? texel : Emit(O(spv::Op::OpBitcast), t_v4f, Ty::V4F, {texel});
    };
    u32 offset_id = 0;
    if (has_offset) {
        if (offset_count == 1) {
            offset_id = CS(s32(offset[0]));
        } else {
            std::array<u32, 3> parts{CS(s32(offset[0])), CS(s32(offset[1])), CS(s32(offset[2]))};
            offset_id = m.ConstComposite(offset_count == 2 ? t_v2s : t_v3s, std::span<const u32>(parts.data(), offset_count));
        }
    }
    u32 texel = 0;
    if (is_msaa) {
        const u32 loaded = LoadImage(u32(image_slot));
        const u32 samples = Emit(O(spv::Op::OpImageQuerySamples), t_u32, Ty::U32, {loaded});
        const u32 wrapped = Emit(O(spv::Op::OpUMod), t_u32, Ty::U32, {CU(0), samples});
        uses_image_query = true;
        const u32 raw = Emit(O(spv::Op::OpImageFetch), result_type, result_ty,
                             {loaded, coords, u32(spv::ImageOperandsMask::Sample), wrapped}, 1u << 2);
        texel = to_vec4f(raw);
    } else {
        const u32 sampled = SampledImage(u32(image_slot), u32(sampler_slot));
        const auto with_offset = [&](u32 mask) {
            return has_offset ? mask | u32(spv::ImageOperandsMask::ConstOffset) : mask;
        };
        if (gather) {
            uses_gather = true;
            u32 raw;
            const u32 comp = u32(std::bit_width(u32(inst.dmask)) - 1);
            const u32 mask = with_offset(0);
            if (compare) {
                if (mask) {
                    raw = Emit(O(spv::Op::OpImageDrefGather), result_type, result_ty, {sampled, coords, dref, mask, offset_id}, 1u << 3);
                } else {
                    raw = Emit(O(spv::Op::OpImageDrefGather), result_type, result_ty, {sampled, coords, dref});
                }
            } else if (mask) {
                raw = Emit(O(spv::Op::OpImageGather), result_type, result_ty, {sampled, coords, CU(comp), mask, offset_id}, 1u << 3);
            } else {
                raw = Emit(O(spv::Op::OpImageGather), result_type, result_ty, {sampled, coords, CU(comp)});
            }
            texel = to_vec4f(raw);
        } else if (has_derivs) {
            const u32 mask = with_offset(u32(spv::ImageOperandsMask::Grad));
            const u32 raw = has_offset
                                ? Emit(O(spv::Op::OpImageSampleExplicitLod), result_type, result_ty, {sampled, coords, mask, grad_x, grad_y, offset_id}, 1u << 2)
                                : Emit(O(spv::Op::OpImageSampleExplicitLod), result_type, result_ty, {sampled, coords, mask, grad_x, grad_y}, 1u << 2);
            texel = to_vec4f(raw);
        } else if (compare) {
            const u32 scalar_type = image.cls == NumClass::Float ? t_f32 : image.cls == NumClass::Uint ? t_u32 : t_s32;
            const Ty scalar_ty = image.cls == NumClass::Float ? Ty::F32 : Ty::None;
            u32 raw;
            if (explicit_lod) {
                const u32 mask = with_offset(u32(spv::ImageOperandsMask::Lod));
                raw = has_offset
                          ? Emit(O(spv::Op::OpImageSampleDrefExplicitLod), scalar_type, scalar_ty, {sampled, coords, dref, mask, lod, offset_id}, 1u << 3)
                          : Emit(O(spv::Op::OpImageSampleDrefExplicitLod), scalar_type, scalar_ty, {sampled, coords, dref, mask, lod}, 1u << 3);
            } else if (has_bias) {
                const u32 mask = with_offset(u32(spv::ImageOperandsMask::Bias));
                raw = has_offset
                          ? Emit(O(spv::Op::OpImageSampleDrefImplicitLod), scalar_type, scalar_ty, {sampled, coords, dref, mask, bias, offset_id}, 1u << 3)
                          : Emit(O(spv::Op::OpImageSampleDrefImplicitLod), scalar_type, scalar_ty, {sampled, coords, dref, mask, bias}, 1u << 3);
            } else if (has_offset) {
                raw = Emit(O(spv::Op::OpImageSampleDrefImplicitLod), scalar_type, scalar_ty, {sampled, coords, dref, u32(spv::ImageOperandsMask::ConstOffset), offset_id}, 1u << 3);
            } else {
                raw = Emit(O(spv::Op::OpImageSampleDrefImplicitLod), scalar_type, scalar_ty, {sampled, coords, dref});
            }
            const u32 typed = image.cls == NumClass::Float ? raw : Emit(O(spv::Op::OpBitcast), t_f32, Ty::F32, {raw});
            texel = ComponentsF({typed, CF(0.f), CF(0.f), CF(0.f)});
        } else if (explicit_lod) {
            const u32 mask = with_offset(u32(spv::ImageOperandsMask::Lod));
            const u32 raw = has_offset
                                ? Emit(O(spv::Op::OpImageSampleExplicitLod), result_type, result_ty, {sampled, coords, mask, lod, offset_id}, 1u << 2)
                                : Emit(O(spv::Op::OpImageSampleExplicitLod), result_type, result_ty, {sampled, coords, mask, lod}, 1u << 2);
            texel = to_vec4f(raw);
        } else {
            u32 raw;
            if (has_bias) {
                const u32 mask = with_offset(u32(spv::ImageOperandsMask::Bias));
                raw = has_offset
                          ? Emit(O(spv::Op::OpImageSampleImplicitLod), result_type, result_ty, {sampled, coords, mask, bias, offset_id}, 1u << 2)
                          : Emit(O(spv::Op::OpImageSampleImplicitLod), result_type, result_ty, {sampled, coords, mask, bias}, 1u << 2);
            } else if (has_offset) {
                raw = Emit(O(spv::Op::OpImageSampleImplicitLod), result_type, result_ty, {sampled, coords, u32(spv::ImageOperandsMask::ConstOffset), offset_id}, 1u << 2);
            } else {
                raw = Emit(O(spv::Op::OpImageSampleImplicitLod), result_type, result_ty, {sampled, coords});
            }
            texel = to_vec4f(raw);
        }
    }
    std::array<u32, 4> comps{};
    for (u32 c = 0; c < 4; ++c) {
        comps[c] = Extract(t_f32, Ty::F32, texel, c);
    }
    if (image.conv != Conv::None) {
        for (u32 c = 0; c < 4; ++c) {
            comps[c] = ApplyConv(comps[c], image.conv);
        }
    }
    if (sampler.force_degamma && !image.srgb) {
        for (u32 c = 0; c < 3; ++c) {
            comps[c] = ApplyConv(comps[c], Conv::SrgbToNorm);
        }
    }
    u32 dest = vdata;
    if (gather) {
        for (u32 c = 0; c < 4; ++c) {
            SetVgpr(dest++, comps[c]);
        }
        return;
    }
    for (u32 c = 0; c < 4; ++c) {
        if (!((inst.dmask >> c) & 1)) {
            continue;
        }
        const u32 value = compare ? (c < 3 ? comps[0] : CF(1.f)) : comps[c];
        SetVgpr(dest++, value);
    }
}

void Lift_::ImageOther(const Inst& inst, s32 image_slot) {
    const ImageSlot& image = in.images[image_slot];
    const u32 addr = SrcVgpr + inst.src[0];
    const u32 vdata = SrcVgpr + inst.src[1];
    const u16 op = inst.op;
    const auto body = [&](u32 index) { return ToU(s.r[addr + index]); };
    if (op == Op::MIMG_GET_RESINFO) {
        uses_image_query = true;
        const u32 lod = ToU(s.r[addr]);
        const u32 loaded = LoadImage(u32(image_slot));
        const bool uses_lod = image.view != View::D2Msaa && !image.storage;
        const auto query = [&](u32 type, Ty ty) {
            return uses_lod ? Emit(O(spv::Op::OpImageQuerySizeLod), type, ty, {loaded, lod})
                            : Emit(O(spv::Op::OpImageQuerySize), type, ty, {loaded});
        };
        u32 values[4]{CU(0), CU(0), CU(0), CU(0)};
        switch (image.view) {
        case View::D1:
            values[0] = query(t_u32, Ty::U32);
            break;
        case View::D1Array:
        case View::D2:
        case View::D2Msaa: {
            const u32 size = query(t_v2u, Ty::V2U);
            values[0] = Extract(t_u32, Ty::U32, size, 0);
            values[1] = Extract(t_u32, Ty::U32, size, 1);
            break;
        }
        case View::D2Array:
        case View::D3: {
            const u32 size = query(t_v3u, Ty::V3U);
            values[0] = Extract(t_u32, Ty::U32, size, 0);
            values[1] = Extract(t_u32, Ty::U32, size, 1);
            values[2] = Extract(t_u32, Ty::U32, size, 2);
            break;
        }
        default:
            Fail("query of this image");
        }
        if (inst.dmask & 8) {
            values[3] = Emit(O(spv::Op::OpImageQueryLevels), t_u32, Ty::U32, {loaded});
        }
        u32 dest = vdata;
        for (u32 c = 0; c < 4; ++c) {
            if ((inst.dmask >> c) & 1) {
                SetVgpr(dest++, values[c]);
            }
        }
        return;
    }
    if (op == Op::MIMG_GET_LOD) {
        uses_image_query = true;
        s32 sampler_slot = FindSampler(inst.src[3] * 4u, false);
        if (sampler_slot < 0) {
            Fail("lod query without a sampler");
        }
        const auto body_f = [&](u32 index) { return ToF(s.r[addr + index]); };
        u32 coords;
        switch (image.view) {
        case View::D1:
            coords = body_f(0);
            break;
        case View::D1Array:
        case View::D2:
            coords = ComponentsF({body_f(0), body_f(1)});
            break;
        case View::D2Array:
        case View::D3:
            coords = ComponentsF({body_f(0), body_f(1), body_f(2)});
            break;
        default:
            Fail("lod query of this image");
        }
        const u32 sampled = SampledImage(u32(image_slot), u32(sampler_slot));
        const u32 result = Emit(O(spv::Op::OpImageQueryLod), t_v2f, Ty::V2F, {sampled, coords});
        u32 dest = vdata;
        if (inst.dmask & 1) {
            SetVgpr(dest++, Extract(t_f32, Ty::F32, result, 0));
        }
        if (inst.dmask & 2) {
            SetVgpr(dest++, Extract(t_f32, Ty::F32, result, 1));
        }
        return;
    }
    const bool has_lod_op = op == Op::MIMG_LOAD_MIP || op == Op::MIMG_STORE_MIP;
    u32 coords = 0;
    u32 extra = 0;
    switch (image.view) {
    case View::D1:
        coords = body(0);
        extra = body(1);
        break;
    case View::D1Array:
    case View::D2:
        coords = ComponentsU({body(0), body(1)});
        extra = body(2);
        break;
    case View::D2Msaa:
        coords = ComponentsU({body(0), body(1)});
        extra = has_lod_op ? body(3) : body(2);
        if (has_lod_op && !(m.IsConst(body(2)) && m.ConstBits(body(2)) == 0)) {
            Fail("mip of an msaa image");
        }
        break;
    case View::D2Array:
    case View::D3:
        coords = ComponentsU({body(0), body(1), body(2)});
        extra = body(3);
        break;
    default:
        Fail("image view");
    }
    const bool is_msaa = image.view == View::D2Msaa;
    const u32 color_type = image.cls == NumClass::Float ? t_v4f : image.cls == NumClass::Uint ? t_v4u : t_v4s;
    const Ty color_ty = image.cls == NumClass::Float ? Ty::V4F : Ty::None;
    if (op == Op::MIMG_LOAD || op == Op::MIMG_LOAD_MIP) {
        const u32 loaded = LoadImage(u32(image_slot));
        u32 texel;
        if (!image.storage) {
            if (is_msaa) {
                uses_image_query = true;
                const u32 samples = Emit(O(spv::Op::OpImageQuerySamples), t_u32, Ty::U32, {loaded});
                const u32 wrapped = Emit(O(spv::Op::OpUMod), t_u32, Ty::U32, {extra, samples});
                texel = Emit(O(spv::Op::OpImageFetch), color_type, color_ty, {loaded, coords, u32(spv::ImageOperandsMask::Sample), wrapped}, 1u << 2);
            } else if (has_lod_op) {
                texel = Emit(O(spv::Op::OpImageFetch), color_type, color_ty, {loaded, coords, u32(spv::ImageOperandsMask::Lod), extra}, 1u << 2);
            } else {
                texel = Emit(O(spv::Op::OpImageFetch), color_type, color_ty, {loaded, coords});
            }
        } else {
            if (has_lod_op && !(in.opt.image_lod_rw)) {
                Fail("storage image read with a mip");
            }
            if (has_lod_op) {
                texel = Emit(O(spv::Op::OpImageRead), color_type, color_ty, {loaded, coords, u32(spv::ImageOperandsMask::Lod), extra}, 1u << 2);
            } else {
                texel = Emit(O(spv::Op::OpImageRead), color_type, color_ty, {loaded, coords});
            }
        }
        const u32 as_float = image.cls == NumClass::Float ? texel : Emit(O(spv::Op::OpBitcast), t_v4f, Ty::V4F, {texel});
        std::array<u32, 4> comps{};
        for (u32 c = 0; c < 4; ++c) {
            comps[c] = Extract(t_f32, Ty::F32, as_float, c);
        }
        if (image.storage) {
            const std::array<u32, 4> raw = comps;
            for (u32 c = 0; c < 4; ++c) {
                const u8 sel = image.swizzle[c];
                comps[c] = sel == 0 ? CF(0.f) : sel == 1 ? CF(1.f) : raw[(sel - 4) & 3];
            }
        }
        u32 dest = vdata;
        for (u32 c = 0; c < 4; ++c) {
            if ((inst.dmask >> c) & 1) {
                SetVgpr(dest++, ApplyConv(comps[c], image.conv));
            }
        }
        return;
    }
    if (op == Op::MIMG_STORE || op == Op::MIMG_STORE_MIP) {
        std::array<u32, 4> comps{};
        u32 src = vdata;
        for (u32 c = 0; c < 4; ++c) {
            comps[c] = ((inst.dmask >> c) & 1) ? ToF(s.r[src++]) : CF(0.f);
        }
        std::array<u32, 4> swizzled{};
        for (u32 c = 0; c < 4; ++c) {
            u32 inverse = 0;
            bool found = false;
            for (u32 d = 0; d < 4; ++d) {
                if (image.swizzle[d] == 4 + c) {
                    inverse = d;
                    found = true;
                    break;
                }
            }
            swizzled[c] = found ? comps[inverse] : CF(0.f);
            swizzled[c] = ApplyWriteConv(swizzled[c], image.conv);
        }
        const u32 color = ComponentsF({swizzled[0], swizzled[1], swizzled[2], swizzled[3]});
        const u32 loaded = LoadImage(u32(image_slot));
        const u32 typed = image.cls == NumClass::Float ? color : Emit(O(spv::Op::OpBitcast), color_type, Ty::None, {color});
        u32 merge = 0;
        BeginGuard(merge);
        if (is_msaa) {
            EmitVoid(O(spv::Op::OpImageWrite), {loaded, coords, typed, u32(spv::ImageOperandsMask::Sample), extra}, 1u << 3);
        } else if (has_lod_op) {
            if (!in.opt.image_lod_rw) {
                Fail("storage image write with a mip");
            }
            EmitVoid(O(spv::Op::OpImageWrite), {loaded, coords, typed, u32(spv::ImageOperandsMask::Lod), extra}, 1u << 3);
        } else {
            EmitVoid(O(spv::Op::OpImageWrite), {loaded, coords, typed});
        }
        EndGuard(merge);
        return;
    }
    Fail("mimg " + std::to_string(op));
}

void Lift_::LiftDs(const Inst& inst) {
    if (inst.op == Op::DS_SWIZZLE_B32) {
        const u8 offset0 = u8(inst.offset);
        const u8 offset1 = inst.offset1;
        const u32 src = ToU(s.r[inst.src[0]]);
        const u32 scope = CU(u32(spv::Scope::Subgroup));
        if (offset1 & 0x80) {
            if (offset0 == 0x00 || offset0 == 0x55 || offset0 == 0xaa || offset0 == 0xff) {
                uses_quad = true;
                return SetVgpr(inst.dst, Emit(O(spv::Op::OpGroupNonUniformQuadBroadcast), t_u32, Ty::U32, {scope, src, CU(offset0 & 3u)}));
            }
            uses_shuffle = true;
            uses_lane = true;
            const u32 lane = LoadBuiltinU32(var_lane_id, spv::BuiltIn::SubgroupLocalInvocationId);
            const u32 id_in_quad = IOp(O(spv::Op::OpBitwiseAnd), lane, CU(3));
            const u32 base = IOp(O(spv::Op::OpShiftLeftLogical), id_in_quad, CU(1));
            const u32 sel = BfeU(CU(offset0), base, CU(2));
            const u32 quad_base = IOp(O(spv::Op::OpBitwiseAnd), lane, CU(~3u));
            return SetVgpr(inst.dst, Emit(O(spv::Op::OpGroupNonUniformShuffle), t_u32, Ty::U32, {scope, src, IOp(O(spv::Op::OpBitwiseOr), quad_base, sel)}));
        }
        const u8 and_mask = u8((offset0 & 0x1f) | u8(~u8(0) << 5));
        const u8 or_mask = u8((offset0 >> 5) | ((offset1 & 0x3) << 3));
        const u8 xor_mask = u8(offset1 >> 2);
        uses_shuffle = true;
        if (or_mask == 0 && and_mask == 255) {
            return SetVgpr(inst.dst, Emit(O(spv::Op::OpGroupNonUniformShuffleXor), t_u32, Ty::U32, {scope, src, CU(xor_mask)}));
        }
        uses_lane = true;
        const u32 lane = LoadBuiltinU32(var_lane_id, spv::BuiltIn::SubgroupLocalInvocationId);
        const u32 local = IOp(O(spv::Op::OpBitwiseAnd), lane, CU(31));
        const u32 half = IOp(O(spv::Op::OpBitwiseAnd), lane, CU(~31u));
        const u32 index = IOp(O(spv::Op::OpBitwiseXor),
                              IOp(O(spv::Op::OpBitwiseOr), IOp(O(spv::Op::OpBitwiseAnd), local, CU(and_mask)), CU(or_mask)),
                              CU(xor_mask));
        return SetVgpr(inst.dst, Emit(O(spv::Op::OpGroupNonUniformShuffle), t_u32, Ty::U32, {scope, src, IOp(O(spv::Op::OpBitwiseOr), index, half)}));
    }
    if (inst.op == Op::DS_APPEND || inst.op == Op::DS_CONSUME) {
        s32 gds = -1;
        for (u32 i = 0; i < in.buffers.size(); ++i) {
            if (in.buffers[i].kind == BufferKind::Gds) {
                gds = s32(i);
            }
        }
        if (gds < 0 && found) {
            BufferSlot slot;
            slot.kind = BufferKind::Gds;
            slot.written = true;
            slot.not_only_constants = true;
            gds = s32(found->buffers.size());
            found->buffers.push_back(slot);
        }
        if (gds < 0) {
            Fail("gds counter without a gds buffer");
        }
        const u32 inst_offset = (u32(inst.offset1) << 8) + inst.offset;
        const u32 base = BfeU(ToU(s.r[SrcM0]), CU(16), CU(16));
        const u32 index = IOp(O(spv::Op::OpShiftRightLogical), IOp(O(spv::Op::OpIAdd), base, CU(inst_offset)), CU(2));
        const u32 var = BufferVar(u32(gds), Ty::U32);
        const u32 ptr = Emit(O(spv::Op::OpAccessChain), PtrType(u32(spv::StorageClass::StorageBuffer), t_u32), Ty::None, {var, CU(0), index});
        if (!IsTrue(Exec())) {
            Fail("gds counter under an unknown exec");
        }
        const u32 prev = Emit(inst.op == Op::DS_APPEND ? O(spv::Op::OpAtomicIAdd) : O(spv::Op::OpAtomicISub), t_u32, Ty::U32,
                              {ptr, CU(u32(spv::Scope::Device)), CU(0), CU(1)});
        nodes[m.NodeOf(prev)].effect = true;
        return SetVgpr(inst.dst, prev);
    }
    if (in.stage == Stage::Fragment && (inst.op == Op::DS_WRITE_B32 || inst.op == Op::DS_READ_B32)) {
        const u32 offset = (u32(inst.offset1) << 8) + inst.offset;
        const u32 slot = SlotFor(2, offset, 0);
        if (inst.op == Op::DS_WRITE_B32) {
            s.r[slot] = s.r[inst.src[1]];
        } else {
            SetVgpr(inst.dst, s.r[slot]);
        }
        return;
    }
    LiftLds(inst);
}

} // namespace Lifter
