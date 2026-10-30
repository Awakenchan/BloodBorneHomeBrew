// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp11>

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

namespace {

struct Type {
    u32 op{};
    u32 width{};
    u32 is_signed{};
    u32 elem{};
    u32 count{};
    std::vector<u32> members;
    u32 storage{};
    u32 dim{};
    u32 arrayed{};
};

struct Value {
    u32 n{};
    std::array<u32, 8> w{};
};

struct Pointer {
    u32 var{};
    u64 offset{};
    bool array_index{};
};

struct Inst {
    u32 op{};
    u32 type{};
    u32 result{};
    u32 first{};
    u32 count{};
};

struct Var {
    u32 type{};
    u32 storage{};
    s32 location = -1;
    s32 builtin = -1;
    s32 binding = -1;
    s32 set = 0;
    s32 index = 0;
    bool flat{}, noperspective{}, per_vertex{}, centroid{}, sample{};
};

struct Module {
    std::vector<u32> words;
    std::unordered_map<u32, Type> types;
    std::unordered_map<u32, Value> constants;
    std::unordered_map<u32, Var> vars;
    std::unordered_map<u32, u32> stride;
    std::map<std::pair<u32, u32>, u32> member_offset;
    std::unordered_map<u32, s32> member_builtin;
    std::map<u32, std::vector<Inst>> blocks;
    std::vector<u32> args;
    u32 entry{};
    u32 bound{};
    u32 glsl{};
    u32 trinary{};
    u32 cube{};
    u32 explicit_vertex{};

    bool Load(const fs::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return false;
        }
        const size_t size = size_t(file.tellg());
        file.seekg(0);
        words.resize(size / 4);
        file.read(reinterpret_cast<char*>(words.data()), std::streamsize(size));
        bound = words[3];
        size_t at = 5;
        u32 current = 0;
        bool in_function = false;
        while (at < words.size()) {
            const u32 word = words[at];
            const u32 op = word & 0xffff;
            const u32 count = word >> 16;
            const u32* w = words.data() + at;
            switch (spv::Op(op)) {
            case spv::Op::OpExtInstImport:
                if (std::strncmp(reinterpret_cast<const char*>(w + 2), "GLSL", 4) == 0) {
                    glsl = w[1];
                } else if (std::strstr(reinterpret_cast<const char*>(w + 2), "trinary")) {
                    trinary = w[1];
                } else if (std::strstr(reinterpret_cast<const char*>(w + 2), "gcn_shader")) {
                    cube = w[1];
                } else if (std::strstr(reinterpret_cast<const char*>(w + 2), "explicit_vertex")) {
                    explicit_vertex = w[1];
                }
                break;
            case spv::Op::OpDecorate: {
                auto& var = vars[w[1]];
                const auto decoration = spv::Decoration(w[2]);
                if (decoration == spv::Decoration::Location) {
                    var.location = s32(w[3]);
                } else if (decoration == spv::Decoration::BuiltIn) {
                    var.builtin = s32(w[3]);
                } else if (decoration == spv::Decoration::Binding) {
                    var.binding = s32(w[3]);
                } else if (decoration == spv::Decoration::DescriptorSet) {
                    var.set = s32(w[3]);
                } else if (decoration == spv::Decoration::Index) {
                    var.index = s32(w[3]);
                } else if (decoration == spv::Decoration::Flat) {
                    var.flat = true;
                } else if (decoration == spv::Decoration::NoPerspective) {
                    var.noperspective = true;
                } else if (decoration == spv::Decoration::PerVertexKHR || decoration == spv::Decoration::ExplicitInterpAMD) {
                    var.per_vertex = true;
                } else if (decoration == spv::Decoration::Centroid) {
                    var.centroid = true;
                } else if (decoration == spv::Decoration::Sample) {
                    var.sample = true;
                } else if (decoration == spv::Decoration::ArrayStride) {
                    stride[w[1]] = w[3];
                }
                break;
            }
            case spv::Op::OpMemberDecorate:
                if (spv::Decoration(w[3]) == spv::Decoration::Offset) {
                    member_offset[{w[1], w[2]}] = w[4];
                }
                break;
            case spv::Op::OpTypeVoid:
            case spv::Op::OpTypeBool:
            case spv::Op::OpTypeSampler:
                types[w[1]] = Type{op};
                break;
            case spv::Op::OpTypeInt:
                types[w[1]] = Type{op, w[2], w[3]};
                break;
            case spv::Op::OpTypeFloat:
                types[w[1]] = Type{op, w[2]};
                break;
            case spv::Op::OpTypeVector:
                types[w[1]] = Type{op, 0, 0, w[2], w[3]};
                break;
            case spv::Op::OpTypeArray:
                types[w[1]] = Type{op, 0, 0, w[2], constants[w[3]].w[0]};
                break;
            case spv::Op::OpTypeRuntimeArray:
                types[w[1]] = Type{op, 0, 0, w[2], 0};
                break;
            case spv::Op::OpTypeStruct: {
                Type type{op};
                type.members.assign(w + 2, w + count);
                types[w[1]] = type;
                break;
            }
            case spv::Op::OpTypePointer: {
                Type type{op};
                type.storage = w[2];
                type.elem = w[3];
                types[w[1]] = type;
                break;
            }
            case spv::Op::OpTypeImage: {
                Type type{op};
                type.elem = w[2];
                type.dim = w[3];
                type.arrayed = w[5];
                types[w[1]] = type;
                break;
            }
            case spv::Op::OpTypeSampledImage: {
                Type type{op};
                type.elem = w[2];
                types[w[1]] = type;
                break;
            }
            case spv::Op::OpTypeFunction:
                types[w[1]] = Type{op};
                break;
            case spv::Op::OpConstant: {
                Value value;
                value.n = count - 3;
                for (u32 i = 0; i + 3 < count; ++i) {
                    value.w[i] = w[3 + i];
                }
                if (types[w[1]].width == 64) {
                    value.n = 2;
                } else {
                    value.n = 1;
                }
                constants[w[2]] = value;
                break;
            }
            case spv::Op::OpConstantTrue:
                constants[w[2]] = Value{1, {1}};
                break;
            case spv::Op::OpConstantFalse:
                constants[w[2]] = Value{1, {0}};
                break;
            case spv::Op::OpConstantComposite: {
                Value value;
                value.n = count - 3;
                for (u32 i = 0; i + 3 < count && i < 8; ++i) {
                    value.w[i] = constants[w[3 + i]].w[0];
                }
                constants[w[2]] = value;
                break;
            }
            case spv::Op::OpUndef:
                if (!in_function) {
                    constants[w[2]] = Value{1, {0}};
                } else {
                    Inst inst{op, w[1], w[2], u32(args.size()), 0};
                    blocks[current].push_back(inst);
                }
                break;
            case spv::Op::OpVariable: {
                auto& var = vars[w[2]];
                var.type = types[w[1]].elem;
                var.storage = w[3];
                break;
            }
            case spv::Op::OpEntryPoint:
                entry = w[2];
                break;
            case spv::Op::OpFunction:
                if (in_function) {
                    return false;
                }
                in_function = entry == w[2];
                break;
            case spv::Op::OpFunctionEnd:
                in_function = false;
                break;
            case spv::Op::OpLabel:
                if (in_function) {
                    current = w[1];
                    if (first_block == 0) {
                        first_block = current;
                    }
                }
                break;
            default:
                if (in_function) {
                    Inst inst{op, 0, 0, u32(args.size()), 0};
                    const bool has_type = HasResultType(op);
                    const bool has_result = has_type || op == 0;
                    u32 start = 1;
                    if (has_type) {
                        inst.type = w[1];
                        inst.result = w[2];
                        start = 3;
                    }
                    (void)has_result;
                    for (u32 i = start; i < count; ++i) {
                        args.push_back(w[i]);
                    }
                    inst.count = count - start;
                    blocks[current].push_back(inst);
                }
                break;
            }
            at += count ? count : 1;
        }
        return first_block != 0;
    }

    static bool HasResultType(u32 op) {
        switch (spv::Op(op)) {
        case spv::Op::OpStore:
        case spv::Op::OpBranch:
        case spv::Op::OpBranchConditional:
        case spv::Op::OpSelectionMerge:
        case spv::Op::OpLoopMerge:
        case spv::Op::OpReturn:
        case spv::Op::OpUnreachable:
        case spv::Op::OpImageWrite:
        case spv::Op::OpDemoteToHelperInvocation:
        case spv::Op::OpKill:
        case spv::Op::OpMemoryBarrier:
        case spv::Op::OpControlBarrier:
        case spv::Op::OpEmitVertex:
        case spv::Op::OpEndPrimitive:
            return false;
        default:
            return true;
        }
    }

    u32 first_block{};
};

constexpr u32 BdaVar = 0xfffffff0u;

float F(u32 bits) {
    return std::bit_cast<float>(bits);
}
u32 B(float value) {
    return std::bit_cast<u32>(value);
}

u64 Mix(u64 h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

u64 Hash(u64 a, u64 b = 0, u64 c = 0, u64 d = 0) {
    return Mix(Mix(Mix(Mix(a + 0x9e3779b97f4a7c15ull) ^ b) ^ c) ^ d);
}

float Unit(u64 h) {
    return float((h >> 40) & 0xffff) / 65535.f;
}

float RandomFloat(u64 h) {
    const u32 kind = (h >> 60) & 15;
    switch (kind) {
    case 0:
        return 0.f;
    case 1:
        return 1.f;
    case 2:
        return -1.f;
    case 3:
        return 0.5f;
    default:
        return (Unit(h) - 0.5f) * 8.f;
    }
}

struct Run {
    const Module& m;
    u64 seed;
    std::vector<Value> values;
    std::vector<Pointer> pointers;
    std::map<std::pair<s32, u64>, u32> outputs;
    std::map<std::pair<u64, u64>, u32> memory;
    std::map<std::pair<u64, u64>, u32> written;
    std::map<std::pair<u64, u64>, u32> emitted;
    u32 emitted_count{};
    std::map<std::pair<u32, u64>, u32> workgroup;
    std::array<float, 3> bary{};
    std::array<float, 3> bary_np{};
    bool killed{};
    bool timed_out{};
    std::string unsupported;
    u64 steps{};
    std::set<u32>* cover = nullptr;

    Run(const Module& module, u64 s) : m{module}, seed{s}, values(module.bound), pointers(module.bound) {
        const float a = Unit(Hash(seed, 1));
        const float b = Unit(Hash(seed, 2)) * (1.f - a);
        bary = {1.f - a - b, a, b};
        const float c = Unit(Hash(seed, 3));
        const float d = Unit(Hash(seed, 4)) * (1.f - c);
        bary_np = {1.f - c - d, c, d};
    }

    Value Get(u32 id) const {
        const auto it = m.constants.find(id);
        if (it != m.constants.end()) {
            return it->second;
        }
        return values[id];
    }

    const Type& TypeOf(u32 id) const {
        static const Type none{};
        const auto it = m.types.find(id);
        return it == m.types.end() ? none : it->second;
    }

    u32 ScalarSize(u32 type) const {
        const Type& t = TypeOf(type);
        return t.width ? t.width / 8 : 4;
    }

    u32 Comps(u32 type) const {
        const Type& t = TypeOf(type);
        return t.op == u32(spv::Op::OpTypeVector) ? t.count : t.width == 64 ? 2 : 1;
    }

    u64 ElementSize(u32 type) const {
        const auto stride_it = m.stride.find(type);
        if (stride_it != m.stride.end()) {
            return stride_it->second;
        }
        const Type& t = TypeOf(type);
        switch (spv::Op(t.op)) {
        case spv::Op::OpTypeVector:
            return t.count == 2 ? 8 : 16;
        case spv::Op::OpTypeArray:
            return ElementSize(t.elem) * t.count;
        default:
            return t.width ? t.width / 8 : 4;
        }
    }

    float Shape(float value) const {
        switch (seed % 3) {
        case 1:
            return std::fabs(value) * 0.25f;
        case 2:
            return value * 0.05f;
        default:
            return value;
        }
    }

    u32 ReadBuffer(u64 key, u64 index) {
        const u64 mode = Hash(seed, 19, key) & 7;
        const u64 h = Hash(seed, 5, key, index);
        if (mode == 0) {
            return 0;
        }
        if (mode == 1) {
            return u32((h >> 8) & 3);
        }
        if (mode == 2) {
            return B(float(int((h >> 8) & 7)));
        }
        return ((h >> 56) & 7) == 0 ? u32(h & 7) : B(Shape(RandomFloat(h)));
    }

    u32 ReadDword(const Var& var, u32 id, u64 offset) {
        const auto storage = spv::StorageClass(var.storage);
        const u64 index = offset / 4;
        if (storage == spv::StorageClass::PushConstant) {
            if (offset >= 80 && offset < 120) {
                return u32(Hash(seed, 79, offset)) & 0xfcfcfcfcu & 0x3c3c3c3cu;
            }
            if (offset == 124) {
                return 1920 | (1080u << 16);
            }
            if (offset == 120) {
                return 0;
            }
            if (offset < 16) {
                return B(1.f + Unit(Hash(seed, 77, offset)));
            }
            return u32(Hash(seed, 78, offset));
        }
        if (storage == spv::StorageClass::Uniform || storage == spv::StorageClass::StorageBuffer) {
            const u64 key = (u64(u32(var.set)) << 32) | u32(var.binding);
            const auto it = memory.find({key, index});
            if (it != memory.end()) {
                return it->second;
            }
            return ReadBuffer(key, index);
        }
        if (storage == spv::StorageClass::Workgroup) {
            const auto it = workgroup.find({id, index});
            return it == workgroup.end() ? 0 : it->second;
        }
        if (storage == spv::StorageClass::Input) {
            return ReadInput(var, id, offset);
        }
        if (storage == spv::StorageClass::Output) {
            const auto it = outputs.find({OutKey(var), offset});
            return it == outputs.end() ? 0 : it->second;
        }
        unsupported = "read of a storage class " + std::to_string(var.storage);
        return 0;
    }

    float Attr(u32 location, u32 comp, u32 vertex) const {
        const u64 h = Hash(seed, 6, u64(location) * 16 + comp, vertex);
        if (((Hash(seed, 7, location, comp) >> 60) & 3) == 0) {
            return float(int(Hash(seed, 8, location, comp) & 31));
        }
        return Shape(RandomFloat(h));
    }

    u32 ReadInput(const Var& var, u32 id, u64 offset) {
        (void)id;
        if (var.builtin >= 0) {
            const auto builtin = spv::BuiltIn(var.builtin);
            const u32 comp = u32(offset / 4);
            switch (builtin) {
            case spv::BuiltIn::FragCoord:
                if (comp == 3) {
                    return B(0.25f + Unit(Hash(seed, 9, comp)) * 2.f);
                }
                return B(comp < 2 ? std::floor(Unit(Hash(seed, 9, comp)) * 1000.f) + 0.5f : Unit(Hash(seed, 9, comp)));
            case spv::BuiltIn::FrontFacing:
                return u32(Hash(seed, 10) & 1);
            case spv::BuiltIn::HelperInvocation:
                return 0;
            case spv::BuiltIn::SampleId:
                return 0;
            case spv::BuiltIn::VertexIndex:
                return u32(Hash(seed, 11) & 1023) + 16;
            case spv::BuiltIn::InstanceIndex:
                return u32(Hash(seed, 12) & 63) + 8;
            case spv::BuiltIn::BaseVertex:
                return u32(Hash(seed, 13) & 15);
            case spv::BuiltIn::BaseInstance:
                return u32(Hash(seed, 14) & 7);
            case spv::BuiltIn::LocalInvocationId:
                return u32(Hash(seed, 15, comp) & 7);
            case spv::BuiltIn::WorkgroupId:
                return u32(Hash(seed, 16, comp) & 15);
            case spv::BuiltIn::SubgroupLocalInvocationId:
                return u32(Hash(seed, 17) & 3);
            case spv::BuiltIn::InvocationId:
                return u32(Hash(seed, 22) & 3);
            case spv::BuiltIn::PrimitiveId:
                return u32(Hash(seed, 23) & 255);
            case spv::BuiltIn::PatchVertices:
                return 3;
            case spv::BuiltIn::TessCoord:
                return B(Unit(Hash(seed, 24, comp)));
            case spv::BuiltIn::BaryCoordSmoothAMD:
            case spv::BuiltIn::BaryCoordSmoothCentroidAMD:
            case spv::BuiltIn::BaryCoordSmoothSampleAMD:
                return B(bary[comp + 1]);
            case spv::BuiltIn::BaryCoordNoPerspAMD:
            case spv::BuiltIn::BaryCoordNoPerspCentroidAMD:
            case spv::BuiltIn::BaryCoordNoPerspSampleAMD:
                return B(bary_np[comp + 1]);
            case spv::BuiltIn::BaryCoordPullModelAMD:
                return B(bary[comp < 2 ? comp + 1 : 0]);
            case spv::BuiltIn::BaryCoordKHR:
                return B(bary[comp]);
            case spv::BuiltIn::BaryCoordNoPerspKHR:
                return B(bary_np[comp]);
            default:
                unsupported = "builtin " + std::to_string(var.builtin);
                return 0;
            }
        }
        const Type& t = TypeOf(var.type);
        const bool array = t.op == u32(spv::Op::OpTypeArray);
        const u32 vertex = array ? u32(offset / 16) : 0;
        const u32 comp = u32((offset % 16) / 4);
        const u32 location = u32(var.location);
        if (TypeOf(array ? t.elem : var.type).elem) {
            const u32 scalar = TypeOf(array ? t.elem : var.type).elem;
            if (TypeOf(scalar).op == u32(spv::Op::OpTypeInt)) {
                return u32(Hash(seed, 18, location, comp) & 255);
            }
        }
        if (array || var.per_vertex) {
            return B(Attr(location, comp, vertex));
        }
        if (var.flat) {
            return B(Attr(location, comp, 0));
        }
        const auto& weights = var.noperspective ? bary_np : bary;
        const float p0 = Attr(location, comp, 0);
        const float p1 = Attr(location, comp, 1);
        const float p2 = Attr(location, comp, 2);
        return B(p0 + weights[1] * (p1 - p0) + weights[2] * (p2 - p0));
    }

    s32 OutKey(const Var& var) const {
        if (var.builtin >= 0) {
            return 1000 + var.builtin;
        }
        return var.location * 2 + var.index;
    }

    const Var& VarOf(u32 id) const {
        static const Var bda = [] {
            Var var;
            var.storage = u32(spv::StorageClass::StorageBuffer);
            var.binding = -100;
            return var;
        }();
        return id == BdaVar ? bda : m.vars.at(id);
    }

    Value Load(u32 type, const Pointer& ptr) {
        const Var& var = VarOf(ptr.var);
        const Type& t = TypeOf(type);
        Value out;
        if (t.op == u32(spv::Op::OpTypeImage) || t.op == u32(spv::Op::OpTypeSampler)) {
            out.n = 1;
            out.w[0] = ptr.var;
            return out;
        }
        if (t.op == u32(spv::Op::OpTypeBool)) {
            out.n = 1;
            out.w[0] = ReadDword(var, ptr.var, ptr.offset);
            return out;
        }
        const u32 scalar = t.op == u32(spv::Op::OpTypeVector) ? t.elem : type;
        const u32 size = ScalarSize(scalar);
        const u32 comps = t.op == u32(spv::Op::OpTypeVector) ? t.count : 1;
        if (size == 8) {
            out.n = 2;
            out.w[0] = ReadDword(var, ptr.var, ptr.offset);
            out.w[1] = ReadDword(var, ptr.var, ptr.offset + 4);
            return out;
        }
        out.n = comps;
        for (u32 c = 0; c < comps; ++c) {
            const u64 at = ptr.offset + u64(c) * size;
            u32 dword = ReadDword(var, ptr.var, at & ~3ull);
            if (size == 2) {
                dword = (dword >> ((at & 2) * 8)) & 0xffff;
            } else if (size == 1) {
                dword = (dword >> ((at & 3) * 8)) & 0xff;
            }
            out.w[c] = dword;
        }
        return out;
    }

    void Write(const Var& var, u32 id, u64 offset, u32 value) {
        const auto storage = spv::StorageClass(var.storage);
        if (storage == spv::StorageClass::Output) {
            outputs[{OutKey(var), offset}] = value;
        } else if (storage == spv::StorageClass::StorageBuffer) {
            const u64 key = (u64(u32(var.set)) << 32) | u32(var.binding);
            memory[{key, offset / 4}] = value;
            written[{key, offset / 4}] = value;
        } else if (storage == spv::StorageClass::Workgroup) {
            workgroup[{id, offset / 4}] = value;
        } else {
            unsupported = "write to storage class " + std::to_string(var.storage);
        }
    }

    void Store(u32 type_hint, const Pointer& ptr, const Value& value) {
        const Var& var = VarOf(ptr.var);
        (void)type_hint;
        for (u32 c = 0; c < value.n; ++c) {
            Write(var, ptr.var, ptr.offset + u64(c) * 4, value.w[c]);
        }
    }

    float TexelFn(u32 binding, const float* coord, u32 n, float lod, u32 comp, u32 sampler) const {
        float sum = float(binding) * 0.13f + float(comp) * 0.37f + float(sampler) * 0.011f;
        for (u32 i = 0; i < n; ++i) {
            sum += std::sin(coord[i] * (1.3f + float(i)));
        }
        return 0.5f + 0.4f * std::sin(sum + lod * 0.31f);
    }

    std::array<u32, 3> SizeOf(u32 binding) const {
        return {u32(Hash(5, binding) & 511) + 16, u32(Hash(6, binding) & 255) + 16, 6};
    }

    bool Execute() {
        u32 block = m.first_block;
        u32 previous = 0;
        while (true) {
            const auto found = m.blocks.find(block);
            if (found == m.blocks.end()) {
                unsupported = "missing block";
                return false;
            }
            u32 next = 0;
            const auto& list = found->second;
            if (cover) {
                cover->insert(block);
            }
            std::vector<std::pair<u32, Value>> phi_values;
            size_t first_other = 0;
            for (; first_other < list.size(); ++first_other) {
                const Inst& inst = list[first_other];
                if (inst.op != u32(spv::Op::OpPhi)) {
                    break;
                }
                const u32* a = m.args.data() + inst.first;
                bool matched = false;
                for (u32 i = 0; i + 1 < inst.count; i += 2) {
                    if (a[i + 1] == previous) {
                        phi_values.push_back({inst.result, Get(a[i])});
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    unsupported = "phi without this predecessor";
                    return false;
                }
            }
            for (const auto& [id, value] : phi_values) {
                values[id] = value;
            }
            for (size_t i = first_other; i < list.size(); ++i) {
                const Inst& inst = list[i];
                if (++steps > 2000000) {
                    timed_out = true;
                    return true;
                }
                const u32* a = m.args.data() + inst.first;
                switch (spv::Op(inst.op)) {
                case spv::Op::OpBranch:
                    next = a[0];
                    break;
                case spv::Op::OpBranchConditional:
                    next = Get(a[0]).w[0] ? a[1] : a[2];
                    break;
                case spv::Op::OpSelectionMerge:
                case spv::Op::OpLoopMerge:
                case spv::Op::OpControlBarrier:
                case spv::Op::OpMemoryBarrier:
                    break;
                case spv::Op::OpReturn:
                    return true;
                case spv::Op::OpUnreachable:
                    unsupported = "reached OpUnreachable";
                    return false;
                default:
                    Step(inst, a);
                    if (!unsupported.empty()) {
                        return false;
                    }
                    break;
                }
            }
            if (next == 0) {
                unsupported = "block without a branch";
                return false;
            }
            previous = block;
            block = next;
        }
    }

    Pointer Access(const Inst& inst, const u32* a) {
        Pointer ptr = m.vars.count(a[0]) && m.vars.at(a[0]).storage != 0 && !IsPointerValue(a[0]) ? Pointer{a[0], 0} : pointers[a[0]];
        u32 type = PointeeOf(a[0]);
        for (u32 i = 1; i < inst.count; ++i) {
            const Value index = Get(a[i]);
            const Type& t = TypeOf(type);
            switch (spv::Op(t.op)) {
            case spv::Op::OpTypeStruct: {
                const u32 member = index.w[0];
                const auto it = m.member_offset.find({type, member});
                ptr.offset += it == m.member_offset.end() ? 0 : it->second;
                type = t.members[member];
                break;
            }
            case spv::Op::OpTypeVector:
                ptr.offset += u64(index.w[0]) * ScalarSize(t.elem);
                type = t.elem;
                break;
            case spv::Op::OpTypeArray:
            case spv::Op::OpTypeRuntimeArray:
                ptr.offset += u64(index.w[0]) * ElementSize(t.elem);
                type = t.elem;
                break;
            default:
                unsupported = "access chain into a scalar";
                break;
            }
        }
        return ptr;
    }

    bool IsPointerValue(u32 id) const {
        return pointers[id].var != 0;
    }

    u32 PointeeOf(u32 id) const {
        if (pointer_types.size() > id && pointer_types[id]) {
            return pointer_types[id];
        }
        const auto it = m.vars.find(id);
        return it == m.vars.end() ? 0 : it->second.type;
    }

    mutable std::vector<u32> pointer_types;

    void SetPtrType(u32 id, u32 type) {
        if (pointer_types.size() <= id) {
            pointer_types.resize(m.bound, 0);
        }
        pointer_types[id] = TypeOf(type).elem;
    }

    Value Unary(const Value& v, auto&& fn) {
        Value out;
        out.n = v.n;
        for (u32 c = 0; c < v.n; ++c) {
            out.w[c] = fn(v.w[c]);
        }
        return out;
    }
    Value Binary(const Value& x, const Value& y, auto&& fn) {
        Value out;
        out.n = x.n;
        for (u32 c = 0; c < x.n; ++c) {
            out.w[c] = fn(x.w[c], y.w[c]);
        }
        return out;
    }

    void Glsl(const Inst& inst, const u32* a) {
        const u32 fn = a[1];
        const Value x = inst.count > 2 ? Get(a[2]) : Value{};
        const Value y = inst.count > 3 ? Get(a[3]) : Value{};
        const Value z = inst.count > 4 ? Get(a[4]) : Value{};
        Value out;
        out.n = x.n;
        const auto each = [&](auto&& f) {
            for (u32 c = 0; c < x.n; ++c) {
                out.w[c] = B(f(F(x.w[c])));
            }
        };
        switch (fn) {
        case GLSLstd450FAbs:
            each([](float v) { return std::fabs(v); });
            break;
        case GLSLstd450Floor:
            each([](float v) { return std::floor(v); });
            break;
        case GLSLstd450Ceil:
            each([](float v) { return std::ceil(v); });
            break;
        case GLSLstd450Trunc:
            each([](float v) { return std::trunc(v); });
            break;
        case GLSLstd450RoundEven:
            each([](float v) { return std::nearbyint(v); });
            break;
        case GLSLstd450Fract:
            each([](float v) { return v - std::floor(v); });
            break;
        case GLSLstd450Sin:
            each([](float v) { return std::sin(v); });
            break;
        case GLSLstd450Cos:
            each([](float v) { return std::cos(v); });
            break;
        case GLSLstd450Exp2:
            each([](float v) { return std::exp2(v); });
            break;
        case GLSLstd450Log2:
            each([](float v) { return std::log2(v); });
            break;
        case GLSLstd450Sqrt:
            each([](float v) { return std::sqrt(v); });
            break;
        case GLSLstd450InverseSqrt:
            each([](float v) { return 1.f / std::sqrt(v); });
            break;
        case GLSLstd450Pow:
            out.w[0] = B(std::pow(F(x.w[0]), F(y.w[0])));
            break;
        case GLSLstd450FMin:
            out.w[0] = B(std::fmin(F(x.w[0]), F(y.w[0])));
            break;
        case GLSLstd450FMax:
            out.w[0] = B(std::fmax(F(x.w[0]), F(y.w[0])));
            break;
        case GLSLstd450FClamp:
            out.w[0] = B(std::fmin(std::fmax(F(x.w[0]), F(y.w[0])), F(z.w[0])));
            break;
        case GLSLstd450Fma:
            out.w[0] = B(F(x.w[0]) * F(y.w[0]) + F(z.w[0]));
            break;
        case GLSLstd450Ldexp:
            out.w[0] = B(std::ldexp(F(x.w[0]), s32(y.w[0])));
            break;
        case GLSLstd450SMin:
            out.w[0] = u32(std::min(s32(x.w[0]), s32(y.w[0])));
            break;
        case GLSLstd450SMax:
            out.w[0] = u32(std::max(s32(x.w[0]), s32(y.w[0])));
            break;
        case GLSLstd450UMin:
            out.w[0] = std::min(x.w[0], y.w[0]);
            break;
        case GLSLstd450UMax:
            out.w[0] = std::max(x.w[0], y.w[0]);
            break;
        case GLSLstd450PackHalf2x16: {
            const auto half = [](float v) {
                if (std::isnan(v)) {
                    return u32(0x7e00);
                }
                const u32 bits = B(v);
                const u32 sign = (bits >> 16) & 0x8000;
                const s32 exp = s32((bits >> 23) & 255) - 127 + 15;
                const u32 mant = bits & 0x7fffff;
                if (exp >= 31) {
                    return sign | 0x7c00;
                }
                if (exp <= 0) {
                    return sign;
                }
                return sign | (u32(exp) << 10) | (mant >> 13);
            };
            out.n = 1;
            out.w[0] = half(F(x.w[0])) | (half(F(x.w[1])) << 16);
            break;
        }
        case GLSLstd450UnpackHalf2x16: {
            const auto full = [](u32 h) {
                const u32 sign = (h & 0x8000) << 16;
                const u32 exp = (h >> 10) & 31;
                const u32 mant = h & 0x3ff;
                if (exp == 0) {
                    return sign;
                }
                if (exp == 31) {
                    return sign | 0x7f800000 | (mant << 13);
                }
                return sign | ((exp + 112) << 23) | (mant << 13);
            };
            out.n = 2;
            out.w[0] = full(x.w[0] & 0xffff);
            out.w[1] = full(x.w[0] >> 16);
            break;
        }
        case GLSLstd450UnpackUnorm4x8:
            out.n = 4;
            for (u32 i = 0; i < 4; ++i) {
                out.w[i] = B(float((x.w[0] >> (i * 8)) & 255) / 255.f);
            }
            break;
        case GLSLstd450UnpackSnorm4x8:
            out.n = 4;
            for (u32 i = 0; i < 4; ++i) {
                out.w[i] = B(std::max(float(s32(x.w[0] << (24 - i * 8)) >> 24) / 127.f, -1.f));
            }
            break;
        case GLSLstd450PackUnorm4x8: {
            out.n = 1;
            for (u32 i = 0; i < 4; ++i) {
                out.w[0] |= u32(std::nearbyint(std::clamp(F(x.w[i]), 0.f, 1.f) * 255.f)) << (i * 8);
            }
            break;
        }
        case GLSLstd450PackSnorm4x8: {
            out.n = 1;
            for (u32 i = 0; i < 4; ++i) {
                out.w[0] |= (u32(s32(std::nearbyint(std::clamp(F(x.w[i]), -1.f, 1.f) * 127.f))) & 255) << (i * 8);
            }
            break;
        }
        case GLSLstd450PackUnorm2x16: {
            out.n = 1;
            for (u32 i = 0; i < 2; ++i) {
                out.w[0] |= u32(std::nearbyint(std::clamp(F(x.w[i]), 0.f, 1.f) * 65535.f)) << (i * 16);
            }
            break;
        }
        case GLSLstd450PackSnorm2x16: {
            out.n = 1;
            for (u32 i = 0; i < 2; ++i) {
                out.w[0] |= (u32(s32(std::nearbyint(std::clamp(F(x.w[i]), -1.f, 1.f) * 32767.f))) & 65535) << (i * 16);
            }
            break;
        }
        case GLSLstd450UnpackUnorm2x16:
            out.n = 2;
            out.w[0] = B(float(x.w[0] & 0xffff) / 65535.f);
            out.w[1] = B(float(x.w[0] >> 16) / 65535.f);
            break;
        case GLSLstd450UnpackSnorm2x16:
            out.n = 2;
            out.w[0] = B(std::max(float(short(x.w[0] & 0xffff)) / 32767.f, -1.f));
            out.w[1] = B(std::max(float(short(x.w[0] >> 16)) / 32767.f, -1.f));
            break;
        case GLSLstd450InterpolateAtCentroid: {
            const Pointer ptr = IsPointerValue(a[2]) ? pointers[a[2]] : Pointer{a[2], 0};
            out = Load(PointeeOf(a[2]), ptr);
            break;
        }
        case GLSLstd450InterpolateAtSample: {
            const Pointer ptr = IsPointerValue(a[2]) ? pointers[a[2]] : Pointer{a[2], 0};
            out = Load(PointeeOf(a[2]), ptr);
            break;
        }
        default:
            unsupported = "GLSL instruction " + std::to_string(fn);
            break;
        }
        values[inst.result] = out;
    }

    void Sample(const Inst& inst, const u32* a, bool dref, bool gather) {
        const Value handle = Get(a[0]);
        const u32 image = handle.w[0];
        const u32 sampler = handle.w[1];
        const Value coord = Get(a[1]);
        float c[4]{};
        for (u32 i = 0; i < coord.n && i < 4; ++i) {
            c[i] = F(coord.w[i]);
        }
        u32 next = 2;
        float compare = 0;
        u32 gather_comp = 0;
        if (dref) {
            compare = F(Get(a[next++]).w[0]);
        } else if (gather) {
            gather_comp = Get(a[next++]).w[0];
        }
        float lod = 0.25f;
        float offset_mix = 0;
        if (next < inst.count) {
            const u32 mask = a[next++];
            if (mask & u32(spv::ImageOperandsMask::Bias)) {
                lod += 0.5f + F(Get(a[next++]).w[0]);
            }
            if (mask & u32(spv::ImageOperandsMask::Lod)) {
                lod = F(Get(a[next++]).w[0]);
            }
            if (mask & u32(spv::ImageOperandsMask::Grad)) {
                const Value dx = Get(a[next++]);
                const Value dy = Get(a[next++]);
                for (u32 i = 0; i < dx.n; ++i) {
                    lod += F(dx.w[i]) * 0.07f + F(dy.w[i]) * 0.11f;
                }
            }
            if (mask & u32(spv::ImageOperandsMask::ConstOffset)) {
                const Value offset = Get(a[next++]);
                for (u32 i = 0; i < offset.n; ++i) {
                    offset_mix += float(s32(offset.w[i])) * (0.017f + float(i) * 0.003f);
                }
            }
        }
        const u32 binding = u32(m.vars.at(image).binding);
        const u32 sampler_binding = sampler ? u32(m.vars.at(sampler).binding) : 0;
        Value out;
        const Type& result = TypeOf(inst.type);
        const u32 n = result.op == u32(spv::Op::OpTypeVector) ? result.count : 1;
        out.n = n;
        const bool integer = TypeOf(result.op == u32(spv::Op::OpTypeVector) ? result.elem : inst.type).op == u32(spv::Op::OpTypeInt);
        for (u32 i = 0; i < n; ++i) {
            const float value = TexelFn(binding, c, coord.n, lod + offset_mix + compare * 0.21f, gather ? gather_comp * 7 + i : i, sampler_binding);
            out.w[i] = integer ? u32(value * 255.f) : B(value);
        }
        values[inst.result] = out;
    }

    void Step(const Inst& inst, const u32* a) {
        const auto op = spv::Op(inst.op);
        switch (op) {
        case spv::Op::OpUndef:
            values[inst.result] = Value{Comps(inst.type), {0}};
            break;
        case spv::Op::OpLoad: {
            const Pointer ptr = IsPointerValue(a[0]) ? pointers[a[0]] : Pointer{a[0], 0};
            values[inst.result] = Load(inst.type, ptr);
            break;
        }
        case spv::Op::OpStore: {
            const Pointer ptr = IsPointerValue(a[0]) ? pointers[a[0]] : Pointer{a[0], 0};
            Store(0, ptr, Get(a[1]));
            break;
        }
        case spv::Op::OpAccessChain:
            pointers[inst.result] = Access(inst, a);
            SetPtrType(inst.result, inst.type);
            break;
        case spv::Op::OpExtInst:
            if (a[0] == m.glsl) {
                Glsl(inst, a);
            } else if (a[0] == m.trinary) {
                const Value x = Get(a[2]);
                const Value y = Get(a[3]);
                const Value z = Get(a[4]);
                const float fx = F(x.w[0]), fy = F(y.w[0]), fz = F(z.w[0]);
                const s32 sx = s32(x.w[0]), sy = s32(y.w[0]), sz = s32(z.w[0]);
                u32 r = 0;
                switch (a[1]) {
                case 1:
                    r = B(std::fmin(fx, std::fmin(fy, fz)));
                    break;
                case 2:
                    r = std::min({x.w[0], y.w[0], z.w[0]});
                    break;
                case 3:
                    r = u32(std::min({sx, sy, sz}));
                    break;
                case 4:
                    r = B(std::fmax(fx, std::fmax(fy, fz)));
                    break;
                case 5:
                    r = std::max({x.w[0], y.w[0], z.w[0]});
                    break;
                case 6:
                    r = u32(std::max({sx, sy, sz}));
                    break;
                case 7:
                    r = B(std::fmax(std::fmin(fx, fy), std::fmin(std::fmax(fx, fy), fz)));
                    break;
                default:
                    unsupported = "trinary instruction";
                    break;
                }
                values[inst.result] = Value{1, {r}};
            } else if (a[0] == m.cube) {
                const Value v = Get(a[2]);
                const float x = F(v.w[0]), y = F(v.w[1]), z = F(v.w[2]);
                const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
                const float id = az >= ax && az >= ay ? (z < 0 ? 5.f : 4.f) : ay >= ax ? (y < 0 ? 3.f : 2.f) : (x < 0 ? 1.f : 0.f);
                values[inst.result] = Value{1, {B(id)}};
            } else if (a[0] == m.explicit_vertex) {
                const Var& var = m.vars.at(a[2]);
                const u32 vertex = Get(a[3]).w[0];
                Value out;
                out.n = 4;
                for (u32 c = 0; c < 4; ++c) {
                    out.w[c] = B(Attr(u32(var.location), c, vertex));
                }
                values[inst.result] = out;
            } else {
                unsupported = "other extended instruction";
            }
            break;
        case spv::Op::OpBitcast: {
            Value v = Get(a[0]);
            const Type& t = TypeOf(inst.type);
            if (t.op == u32(spv::Op::OpTypeInt) && t.width == 64) {
                v.n = 2;
            } else if (t.width == 16) {
                v.n = 1;
            } else if (t.op == u32(spv::Op::OpTypeVector)) {
                v.n = t.count;
            } else {
                v.n = 1;
            }
            values[inst.result] = v;
            break;
        }
        case spv::Op::OpCompositeConstruct: {
            Value out;
            for (u32 i = 0; i < inst.count; ++i) {
                const Value part = Get(a[i]);
                for (u32 c = 0; c < part.n && out.n < 8; ++c) {
                    out.w[out.n++] = part.w[c];
                }
            }
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpCompositeExtract: {
            const Value v = Get(a[0]);
            Value out;
            out.n = 1;
            out.w[0] = v.w[a[1]];
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpCompositeInsert: {
            Value v = Get(a[1]);
            v.w[a[2]] = Get(a[0]).w[0];
            values[inst.result] = v;
            break;
        }
        case spv::Op::OpVectorShuffle: {
            const Value x = Get(a[0]);
            const Value y = Get(a[1]);
            Value out;
            for (u32 i = 2; i < inst.count; ++i) {
                const u32 pick = a[i];
                out.w[out.n++] = pick < x.n ? x.w[pick] : y.w[pick - x.n];
            }
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpVectorExtractDynamic: {
            const Value v = Get(a[0]);
            Value out;
            out.n = 1;
            out.w[0] = v.w[Get(a[1]).w[0] & 3];
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpCopyObject:
            values[inst.result] = Get(a[0]);
            break;
        case spv::Op::OpSelect: {
            const Value cond = Get(a[0]);
            values[inst.result] = cond.w[0] ? Get(a[1]) : Get(a[2]);
            break;
        }
#define FBIN(NAME, EXPR)                                                                           \
    case spv::Op::NAME:                                                                            \
        values[inst.result] = Binary(Get(a[0]), Get(a[1]), [](u32 p, u32 q) {                      \
            const float x = F(p);                                                                  \
            const float y = F(q);                                                                  \
            return EXPR;                                                                           \
        });                                                                                        \
        break;
            FBIN(OpFAdd, B(x + y))
            FBIN(OpFSub, B(x - y))
            FBIN(OpFMul, B(x * y))
            FBIN(OpFDiv, B(x / y))
            FBIN(OpFOrdEqual, u32(x == y))
            FBIN(OpFUnordEqual, u32(x == y || std::isnan(x) || std::isnan(y)))
            FBIN(OpFOrdNotEqual, u32(x != y && !std::isnan(x) && !std::isnan(y)))
            FBIN(OpFUnordNotEqual, u32(x != y))
            FBIN(OpFOrdLessThan, u32(x < y))
            FBIN(OpFUnordLessThan, u32(!(x >= y)))
            FBIN(OpFOrdGreaterThan, u32(x > y))
            FBIN(OpFUnordGreaterThan, u32(!(x <= y)))
            FBIN(OpFOrdLessThanEqual, u32(x <= y))
            FBIN(OpFUnordLessThanEqual, u32(!(x > y)))
            FBIN(OpFOrdGreaterThanEqual, u32(x >= y))
            FBIN(OpFUnordGreaterThanEqual, u32(!(x < y)))
#undef FBIN
        case spv::Op::OpSNegate:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return 0u - v; });
            break;
        case spv::Op::OpFNegate:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return v ^ 0x80000000u; });
            break;
        case spv::Op::OpIsNan:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return u32(std::isnan(F(v))); });
            break;
        case spv::Op::OpIsInf:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return u32(std::isinf(F(v))); });
            break;
        case spv::Op::OpConvertFToU:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) {
                const float f = F(v);
                return f >= 4294967040.f ? ~0u : f > 0 ? u32(f) : 0u;
            });
            break;
        case spv::Op::OpConvertFToS:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) {
                const float f = F(v);
                return f >= 2147483520.f ? 0x7fffffffu : f <= -2147483648.f ? 0x80000000u : std::isnan(f) ? 0u : u32(s32(f));
            });
            break;
        case spv::Op::OpConvertUToF:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return B(float(v)); });
            break;
        case spv::Op::OpConvertSToF:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return B(float(s32(v))); });
            break;
#define IBIN(NAME, EXPR)                                                                           \
    case spv::Op::NAME: {                                                                          \
        const Value p = Get(a[0]);                                                                 \
        const Value q = Get(a[1]);                                                                 \
        if (TypeOf(inst.type).width == 64) {                                                       \
            const u64 x = u64(p.w[0]) | (u64(p.w[1]) << 32);                                       \
            const u64 y = q.n == 2 ? u64(q.w[0]) | (u64(q.w[1]) << 32) : q.w[0];                   \
            const u64 r = EXPR;                                                                    \
            Value out;                                                                             \
            out.n = 2;                                                                             \
            out.w[0] = u32(r);                                                                     \
            out.w[1] = u32(r >> 32);                                                               \
            values[inst.result] = out;                                                             \
        } else {                                                                                   \
            values[inst.result] = Binary(p, q, [](u32 x, u32 y) { return u32(EXPR); });            \
        }                                                                                          \
        break;                                                                                     \
    }
            IBIN(OpIAdd, x + y)
            IBIN(OpISub, x - y)
            IBIN(OpIMul, x * y)
            IBIN(OpUDiv, y ? x / y : 0)
            IBIN(OpUMod, y ? x % y : 0)
            IBIN(OpBitwiseAnd, x & y)
            IBIN(OpBitwiseOr, x | y)
            IBIN(OpBitwiseXor, x ^ y)
            IBIN(OpShiftLeftLogical, y < 64 ? decltype(x)(x << (y & (sizeof(x) * 8 - 1))) : 0)
            IBIN(OpShiftRightLogical, y < 64 ? decltype(x)(x >> (y & (sizeof(x) * 8 - 1))) : 0)
#undef IBIN
        case spv::Op::OpShiftRightArithmetic:
            values[inst.result] = Binary(Get(a[0]), Get(a[1]), [](u32 x, u32 y) { return u32(s32(x) >> (y & 31)); });
            break;
#define ICMP(NAME, EXPR)                                                                           \
    case spv::Op::NAME:                                                                            \
        values[inst.result] = Binary(Get(a[0]), Get(a[1]), [](u32 x, u32 y) { return u32(EXPR); }); \
        break;
            ICMP(OpIEqual, x == y)
            ICMP(OpINotEqual, x != y)
            ICMP(OpULessThan, x < y)
            ICMP(OpULessThanEqual, x <= y)
            ICMP(OpUGreaterThan, x > y)
            ICMP(OpUGreaterThanEqual, x >= y)
            ICMP(OpSLessThan, s32(x) < s32(y))
            ICMP(OpSLessThanEqual, s32(x) <= s32(y))
            ICMP(OpSGreaterThan, s32(x) > s32(y))
            ICMP(OpSGreaterThanEqual, s32(x) >= s32(y))
            ICMP(OpLogicalAnd, x && y)
            ICMP(OpLogicalOr, x || y)
            ICMP(OpLogicalEqual, bool(x) == bool(y))
            ICMP(OpLogicalNotEqual, bool(x) != bool(y))
#undef ICMP
        case spv::Op::OpLogicalNot:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return u32(!v); });
            break;
        case spv::Op::OpNot:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return ~v; });
            break;
        case spv::Op::OpBitCount:
            values[inst.result] = Unary(Get(a[0]), [](u32 v) { return u32(std::popcount(v)); });
            break;
        case spv::Op::OpBitFieldUExtract: {
            const u32 x = Get(a[0]).w[0];
            const u32 o = Get(a[1]).w[0];
            const u32 c = Get(a[2]).w[0];
            Value out;
            out.n = 1;
            out.w[0] = c == 0 ? 0 : (x >> (o & 31)) & (c >= 32 ? ~0u : (1u << c) - 1);
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpBitFieldSExtract: {
            const u32 x = Get(a[0]).w[0];
            const u32 o = Get(a[1]).w[0];
            const u32 c = Get(a[2]).w[0];
            Value out;
            out.n = 1;
            out.w[0] = c == 0 ? 0 : o + c > 32 ? 0 : u32(s32(x << (32 - o - c)) >> (32 - c));
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpBitFieldInsert: {
            const u32 base = Get(a[0]).w[0];
            const u32 ins = Get(a[1]).w[0];
            const u32 o = Get(a[2]).w[0];
            const u32 c = Get(a[3]).w[0];
            const u32 mask = (c >= 32 ? ~0u : (1u << c) - 1) << (o & 31);
            Value out;
            out.n = 1;
            out.w[0] = (base & ~mask) | ((ins << (o & 31)) & mask);
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpUConvert: {
            Value v = Get(a[0]);
            if (TypeOf(inst.type).width == 64) {
                v.n = 2;
                v.w[1] = 0;
            } else {
                v.n = 1;
                if (TypeOf(inst.type).width == 16) {
                    v.w[0] &= 0xffff;
                }
            }
            values[inst.result] = v;
            break;
        }
        case spv::Op::OpUMulExtended:
        case spv::Op::OpSMulExtended: {
            const u32 x = Get(a[0]).w[0];
            const u32 y = Get(a[1]).w[0];
            const u64 r = op == spv::Op::OpUMulExtended ? u64(x) * y : u64(s64(s32(x)) * s64(s32(y)));
            Value out;
            out.n = 2;
            out.w[0] = u32(r);
            out.w[1] = u32(r >> 32);
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpDemoteToHelperInvocation:
        case spv::Op::OpKill:
            killed = true;
            break;
        case spv::Op::OpEmitVertex:
            for (const auto& [key, value] : outputs) {
                emitted[{u64(emitted_count), (u64(u32(key.first)) << 32) | key.second}] = value;
            }
            ++emitted_count;
            break;
        case spv::Op::OpEndPrimitive:
            emitted[{u64(emitted_count), ~0ull}] = 1;
            break;
        case spv::Op::OpSampledImage: {
            Value out;
            out.n = 2;
            out.w[0] = Get(a[0]).w[0];
            out.w[1] = Get(a[1]).w[0];
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpImageSampleImplicitLod:
        case spv::Op::OpImageSampleExplicitLod:
            Sample(inst, a, false, false);
            break;
        case spv::Op::OpImageSampleDrefImplicitLod:
        case spv::Op::OpImageSampleDrefExplicitLod:
            Sample(inst, a, true, false);
            break;
        case spv::Op::OpImageGather:
            Sample(inst, a, false, true);
            break;
        case spv::Op::OpImageDrefGather:
            Sample(inst, a, true, true);
            break;
        case spv::Op::OpImageFetch:
        case spv::Op::OpImageRead: {
            const u32 image = Get(a[0]).w[0];
            const Value coord = Get(a[1]);
            Value out;
            const Type& result = TypeOf(inst.type);
            const bool integer = TypeOf(result.elem).op == u32(spv::Op::OpTypeInt);
            out.n = 4;
            u64 h = Hash(seed, 21, u32(m.vars.at(image).binding));
            for (u32 i = 0; i < coord.n; ++i) {
                h = Hash(h, coord.w[i]);
            }
            for (u32 i = 2; i < inst.count; ++i) {
                h = Hash(h, i == 2 ? a[i] : Get(a[i]).w[0]);
            }
            for (u32 i = 0; i < 4; ++i) {
                const float value = Unit(Hash(h, i));
                out.w[i] = integer ? u32(value * 255.f) : B(value);
            }
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpImageWrite: {
            const u32 image = Get(a[0]).w[0];
            const Value coord = Get(a[1]);
            const Value texel = Get(a[2]);
            u64 where = 0;
            for (u32 i = 0; i < coord.n; ++i) {
                where = Hash(where, coord.w[i]);
            }
            const u64 key = (u64(0xffffu) << 32) | u32(m.vars.at(image).binding);
            for (u32 i = 0; i < texel.n; ++i) {
                written[{key, where + i}] = texel.w[i];
            }
            break;
        }
        case spv::Op::OpImageQuerySizeLod:
        case spv::Op::OpImageQuerySize: {
            const u32 binding = u32(m.vars.at(Get(a[0]).w[0]).binding);
            const auto size = SizeOf(binding);
            Value out;
            const Type& result = TypeOf(inst.type);
            out.n = result.op == u32(spv::Op::OpTypeVector) ? result.count : 1;
            for (u32 i = 0; i < out.n; ++i) {
                out.w[i] = size[i];
            }
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpImageQueryLevels:
            values[inst.result] = Value{1, {u32(Hash(7, m.vars.at(Get(a[0]).w[0]).binding) & 7) + 1}};
            break;
        case spv::Op::OpImageQuerySamples:
            values[inst.result] = Value{1, {4}};
            break;
        case spv::Op::OpImageQueryLod: {
            const u32 binding = u32(m.vars.at(Get(a[0]).w[0]).binding);
            const Value coord = Get(a[1]);
            float sum = float(binding);
            for (u32 i = 0; i < coord.n; ++i) {
                sum += std::sin(F(coord.w[i]));
            }
            values[inst.result] = Value{2, {B(sum * 0.5f), B(sum * 0.25f)}};
            break;
        }
        case spv::Op::OpGroupNonUniformQuadBroadcast:
        case spv::Op::OpGroupNonUniformShuffle:
        case spv::Op::OpGroupNonUniformShuffleXor:
        case spv::Op::OpGroupNonUniformBroadcastFirst: {
            const u32 bits = Get(a[1]).w[0];
            const u32 index = inst.count > 2 ? Get(a[2]).w[0] : 0;
            const float f = F(bits);
            const u32 mixed = std::isfinite(f) && (bits & 0x7f800000u) ? B(f * (1.f + float(index & 15) / 64.f) + float(index & 15) * 0.015625f) : bits ^ (index * 0x10001u);
            values[inst.result] = Value{1, {mixed}};
            break;
        }
        case spv::Op::OpConvertUToPtr: {
            const Value v = Get(a[0]);
            pointers[inst.result] = Pointer{BdaVar, (u64(v.w[1]) << 32) | v.w[0]};
            SetPtrType(inst.result, inst.type);
            break;
        }
        case spv::Op::OpAtomicExchange: {
            const Pointer ptr = IsPointerValue(a[0]) ? pointers[a[0]] : Pointer{a[0], 0};
            const Var& var = VarOf(ptr.var);
            const u32 before = ReadDword(var, ptr.var, ptr.offset);
            Write(var, ptr.var, ptr.offset, Get(a[3]).w[0]);
            values[inst.result] = Value{1, {before}};
            break;
        }
        case spv::Op::OpFConvert: {
            const Value v = Get(a[0]);
            Value out;
            out.n = 1;
            if (TypeOf(inst.type).width == 32) {
                const u32 h = v.w[0];
                const u32 sign = (h & 0x8000) << 16;
                const u32 exp = (h >> 10) & 31;
                const u32 mant = h & 0x3ff;
                out.w[0] = exp == 0 ? sign : exp == 31 ? sign | 0x7f800000 | (mant << 13) : sign | ((exp + 112) << 23) | (mant << 13);
            } else {
                unsupported = "conversion to 16 bits";
            }
            values[inst.result] = out;
            break;
        }
        case spv::Op::OpAtomicIAdd:
        case spv::Op::OpAtomicISub: {
            const Pointer ptr = IsPointerValue(a[0]) ? pointers[a[0]] : Pointer{a[0], 0};
            const Var& var = VarOf(ptr.var);
            const u32 before = ReadDword(var, ptr.var, ptr.offset);
            const u32 operand = Get(a[3]).w[0];
            Write(var, ptr.var, ptr.offset, op == spv::Op::OpAtomicIAdd ? before + operand : before - operand);
            values[inst.result] = Value{1, {before}};
            break;
        }
        default:
            unsupported = "instruction " + std::to_string(inst.op);
            break;
        }
    }
};

bool Near(u32 a, u32 b) {
    if (a == b) {
        return true;
    }
    const float x = F(a);
    const float y = F(b);
    if (std::isnan(x) && std::isnan(y)) {
        return true;
    }
    if (std::isnan(x) || std::isnan(y)) {
        return false;
    }
    if (std::isinf(x) || std::isinf(y)) {
        return x == y;
    }
    const float scale = std::max({1.f, std::fabs(x), std::fabs(y)});
    return std::fabs(x - y) <= 2e-4f * scale;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "spirv-diff old_dir new_dir [seeds] [report]\n");
        return 2;
    }
    const fs::path old_dir = argv[1];
    const fs::path new_dir = argv[2];
    const int seeds = argc > 3 ? std::atoi(argv[3]) : 32;
    FILE* report = argc > 4 ? std::fopen(argv[4], "w") : nullptr;
    u32 same = 0, different = 0, failed = 0, compared = 0;
    u64 blocks_total = 0, blocks_run = 0;
    std::map<std::string, u32> reasons;
    for (const auto& entry : fs::directory_iterator(new_dir)) {
        if (entry.path().extension() != ".spv") {
            continue;
        }
        Module a;
        Module b;
        if (!a.Load(old_dir / entry.path().filename()) || !b.Load(entry.path())) {
            ++failed;
            continue;
        }
        ++compared;
        bool bad = false;
        std::string why;
        std::set<u32> covered;
        for (int seed = 0; seed < seeds && !bad && why.empty(); ++seed) {
            Run x{a, u64(seed) + 1};
            Run y{b, u64(seed) + 1};
            y.cover = &covered;
            const bool ok_x = x.Execute();
            const bool ok_y = y.Execute();
            if (!ok_x || !ok_y) {
                why = (!ok_x ? "old: " + x.unsupported : "new: " + y.unsupported);
                break;
            }
            if (x.timed_out || y.timed_out) {
                if (x.timed_out != y.timed_out) {
                    bad = true;
                    if (report) {
                        std::fprintf(report, "%s seed %d: one module ran out of steps\n", entry.path().filename().string().c_str(), seed);
                    }
                }
                continue;
            }
            if (x.killed != y.killed) {
                bad = true;
                if (report) {
                    std::fprintf(report, "%s seed %d: killed old %d new %d\n", entry.path().filename().string().c_str(), seed, x.killed, y.killed);
                }
                continue;
            }
            if (x.killed) {
                continue;
            }
            if (x.outputs.size() != y.outputs.size() || x.written.size() != y.written.size()) {
                bad = true;
                if (report) {
                    std::fprintf(report, "%s seed %d: outputs %zu/%zu writes %zu/%zu\n", entry.path().filename().string().c_str(), seed,
                                 x.outputs.size(), y.outputs.size(), x.written.size(), y.written.size());
                    int shown = 0;
                    for (const auto& [key, value] : x.outputs) {
                        if (!y.outputs.count(key) && shown++ < 8) {
                            std::fprintf(report, "   only old %d+%llu\n", key.first, (unsigned long long)key.second);
                        }
                    }
                    shown = 0;
                    for (const auto& [key, value] : y.outputs) {
                        if (!x.outputs.count(key) && shown++ < 8) {
                            std::fprintf(report, "   only new %d+%llu\n", key.first, (unsigned long long)key.second);
                        }
                    }
                }
                continue;
            }
            auto ita = x.outputs.begin();
            auto itb = y.outputs.begin();
            for (; ita != x.outputs.end(); ++ita, ++itb) {
                if (ita->first != itb->first || !Near(ita->second, itb->second)) {
                    bad = true;
                    if (report) {
                        std::fprintf(report, "%s seed %d: output %d+%llu old %08x (%g) new key %d+%llu %08x (%g)\n",
                                     entry.path().filename().string().c_str(), seed, ita->first.first, (unsigned long long)ita->first.second,
                                     ita->second, F(ita->second), itb->first.first, (unsigned long long)itb->first.second, itb->second, F(itb->second));
                    }
                    break;
                }
            }
            if (x.emitted.size() != y.emitted.size()) {
                bad = true;
                if (report) {
                    std::fprintf(report, "%s seed %d: emitted %zu/%zu\n", entry.path().filename().string().c_str(), seed, x.emitted.size(), y.emitted.size());
                }
                continue;
            }
            auto ime = x.emitted.begin();
            auto ime2 = y.emitted.begin();
            for (; !bad && ime != x.emitted.end(); ++ime, ++ime2) {
                if (ime->first != ime2->first || !Near(ime->second, ime2->second)) {
                    bad = true;
                    if (report) {
                        std::fprintf(report, "%s seed %d: emitted vertex differs\n", entry.path().filename().string().c_str(), seed);
                    }
                }
            }
            auto ima = x.written.begin();
            auto imb = y.written.begin();
            for (; !bad && ima != x.written.end(); ++ima, ++imb) {
                if (ima->first != imb->first || !Near(ima->second, imb->second)) {
                    bad = true;
                    if (report) {
                        std::fprintf(report, "%s seed %d: memory differs\n", entry.path().filename().string().c_str(), seed);
                    }
                }
            }
        }
        blocks_total += b.blocks.size();
        blocks_run += covered.size();
        if (!why.empty()) {
            ++reasons[why];
            ++failed;
            if (report) {
                std::fprintf(report, "%s: cannot run: %s\n", entry.path().filename().string().c_str(), why.c_str());
            }
        } else if (bad) {
            ++different;
        } else {
            ++same;
        }
    }
    std::printf("%u compared over %d seeds: %u same, %u different, %u could not run; %llu of %llu blocks ran\n", compared, seeds, same, different, failed,
                (unsigned long long)blocks_run, (unsigned long long)blocks_total);
    for (const auto& [why, count] : reasons) {
        std::printf("%5u %s\n", count, why.c_str());
    }
    if (report) {
        std::fclose(report);
    }
    return different ? 1 : 0;
}
