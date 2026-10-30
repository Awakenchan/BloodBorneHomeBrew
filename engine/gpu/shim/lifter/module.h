// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "lifter/gcn.h"

namespace Lifter {

enum class Ty : u8 { None, Void, Bool, U32, F32, U64, U16, U8, V2F, V3F, V4F, V2U, V3U, V4U, V4S };

struct Node {
    u16 op{};
    u16 nargs{};
    u32 type{};
    u32 result{};
    u32 first{};
    u32 lits{};
    bool effect{};
    bool nocontract{};
    u8 pseudo{};
};

class Module {
public:
    Module() {
        ids_.reserve(4096);
        ids_.push_back({});
    }

    u32 NewId(Ty ty = Ty::None) {
        const u32 id = u32(ids_.size());
        ids_.push_back({ty, 0, 0, 0, false});
        return id;
    }
    Ty TyOf(u32 id) const {
        return ids_[id].ty;
    }
    bool IsConst(u32 id) const {
        return ids_[id].is_const;
    }
    u32 ConstBits(u32 id) const {
        return ids_[id].bits;
    }
    void SetOrigin(u32 id, u32 origin) {
        ids_[id].origin = origin;
    }
    u32 Origin(u32 id) const {
        return ids_[id].origin;
    }
    u32 NodeOf(u32 id) const {
        return ids_[id].node;
    }
    void BindNode(u32 id, u32 node) {
        ids_[id].node = node;
    }
    u32 Forward(u32 id) const {
        while (forward_.size() > id && forward_[id] != 0) {
            id = forward_[id];
        }
        return id;
    }
    void SetForward(u32 id, u32 to) {
        if (forward_.size() <= id) {
            forward_.resize(ids_.size() + 64, 0);
        }
        forward_[id] = to;
    }

    u32 AddType(u16 op, std::initializer_list<u32> words) {
        std::vector<u32> key(words);
        key.push_back(op);
        const auto it = type_map_.find(key);
        if (it != type_map_.end()) {
            return it->second;
        }
        const u32 id = NewId();
        Word(globals_, op, u32(words.size()) + 2);
        globals_.push_back(id);
        globals_.insert(globals_.end(), words.begin(), words.end());
        type_map_.emplace(std::move(key), id);
        return id;
    }
    u32 AddUniqueType(u16 op, std::span<const u32> words) {
        const u32 id = NewId();
        Word(globals_, op, u32(words.size()) + 2);
        globals_.push_back(id);
        globals_.insert(globals_.end(), words.begin(), words.end());
        return id;
    }

    u32 Const(Ty ty, u32 type_id, u32 bits) {
        const u64 key = (u64(u8(ty)) << 32) | bits;
        const auto it = const_map_.find(key);
        if (it != const_map_.end()) {
            return it->second;
        }
        const u32 id = NewId(ty);
        ids_[id].is_const = true;
        ids_[id].bits = bits;
        Word(globals_, 43, 4);
        globals_.push_back(type_id);
        globals_.push_back(id);
        globals_.push_back(bits);
        const_map_.emplace(key, id);
        return id;
    }
    u32 ConstBool(u32 type_id, bool value) {
        const u64 key = (u64(u8(Ty::Bool)) << 32) | u32(value);
        const auto it = const_map_.find(key);
        if (it != const_map_.end()) {
            return it->second;
        }
        const u32 id = NewId(Ty::Bool);
        ids_[id].is_const = true;
        ids_[id].bits = value;
        Word(globals_, value ? 41 : 42, 3);
        globals_.push_back(type_id);
        globals_.push_back(id);
        const_map_.emplace(key, id);
        return id;
    }
    u32 ConstComposite(u32 type_id, std::span<const u32> parts) {
        const u32 id = NewId();
        Word(globals_, 44, u32(parts.size()) + 3);
        globals_.push_back(type_id);
        globals_.push_back(id);
        globals_.insert(globals_.end(), parts.begin(), parts.end());
        return id;
    }
    u32 ConstU64(u32 type_id, u64 value) {
        const u32 id = NewId(Ty::U64);
        Word(globals_, 43, 5);
        globals_.push_back(type_id);
        globals_.push_back(id);
        globals_.push_back(u32(value));
        globals_.push_back(u32(value >> 32));
        return id;
    }
    u32 Variable(u32 pointer_type, u32 storage, u32 init = 0) {
        const u32 id = NewId();
        Word(globals_, 59, init ? 5 : 4);
        globals_.push_back(pointer_type);
        globals_.push_back(id);
        globals_.push_back(storage);
        if (init) {
            globals_.push_back(init);
        }
        return id;
    }
    void Decorate(u32 target, u32 decoration) {
        Word(annotations_, 71, 3);
        annotations_.push_back(target);
        annotations_.push_back(decoration);
    }
    void Decorate(u32 target, u32 decoration, u32 value) {
        Word(annotations_, 71, 4);
        annotations_.push_back(target);
        annotations_.push_back(decoration);
        annotations_.push_back(value);
    }
    void MemberDecorate(u32 target, u32 member, u32 decoration) {
        Word(annotations_, 72, 4);
        annotations_.push_back(target);
        annotations_.push_back(member);
        annotations_.push_back(decoration);
    }
    void MemberDecorate(u32 target, u32 member, u32 decoration, u32 value) {
        Word(annotations_, 72, 5);
        annotations_.push_back(target);
        annotations_.push_back(member);
        annotations_.push_back(decoration);
        annotations_.push_back(value);
    }
    void Capability(u32 capability) {
        for (const u32 existing : capabilities_) {
            if (existing == capability) {
                return;
            }
        }
        capabilities_.push_back(capability);
    }
    void Extension(std::string_view name) {
        for (const auto& existing : extensions_) {
            if (existing == name) {
                return;
            }
        }
        extensions_.emplace_back(name);
    }
    u32 ExtInst(std::string_view name) {
        for (const auto& [known, id] : ext_sets_) {
            if (known == name) {
                return id;
            }
        }
        const u32 id = NewId();
        ext_sets_.emplace_back(std::string(name), id);
        return id;
    }
    void ExecutionMode(u32 mode, std::initializer_list<u32> args = {}) {
        modes_.push_back(u32(args.size()) | (mode << 8));
        modes_.insert(modes_.end(), args.begin(), args.end());
    }
    void Interface(u32 id) {
        interfaces_.push_back(id);
    }

    std::vector<u32> Assemble(u32 execution_model, u32 main_id, u32 addressing, u32 version,
                              std::span<const u32> function) {
        std::vector<u32> out;
        out.reserve(function.size() + globals_.size() + annotations_.size() + 64);
        out.insert(out.end(), {0x07230203u, version, 0x00560001u, u32(ids_.size()), 0u});
        for (const u32 capability : capabilities_) {
            Word(out, 17, 2);
            out.push_back(capability);
        }
        for (const auto& name : extensions_) {
            Literal(out, 10, 1, name);
        }
        for (const auto& [name, id] : ext_sets_) {
            const u32 words = u32(name.size()) / 4 + 1;
            Word(out, 11, words + 2);
            out.push_back(id);
            PushString(out, name);
        }
        Word(out, 14, 3);
        out.push_back(addressing);
        out.push_back(1);
        Word(out, 15, u32(3 + 2 + interfaces_.size()));
        out.push_back(execution_model);
        out.push_back(main_id);
        PushString(out, "main");
        out.insert(out.end(), interfaces_.begin(), interfaces_.end());
        for (size_t at = 0; at < modes_.size();) {
            const u32 count = modes_[at] & 0xff;
            const u32 mode = modes_[at] >> 8;
            Word(out, 16, 3 + count);
            out.push_back(main_id);
            out.push_back(mode);
            out.insert(out.end(), modes_.begin() + at + 1, modes_.begin() + at + 1 + count);
            at += 1 + count;
        }
        out.insert(out.end(), annotations_.begin(), annotations_.end());
        out.insert(out.end(), globals_.begin(), globals_.end());
        out.insert(out.end(), function.begin(), function.end());
        out[3] = u32(ids_.size());
        return out;
    }

    static void Word(std::vector<u32>& out, u32 op, u32 count) {
        out.push_back(op | (count << 16));
    }

    std::vector<u32>& Globals() {
        return globals_;
    }

private:
    static void PushString(std::vector<u32>& out, std::string_view name) {
        const u32 words = u32(name.size()) / 4 + 1;
        const size_t at = out.size();
        out.resize(at + words, 0);
        std::memcpy(out.data() + at, name.data(), name.size());
    }
    static void Literal(std::vector<u32>& out, u32 op, u32 extra, std::string_view name) {
        const u32 words = u32(name.size()) / 4 + 1;
        Word(out, op, words + extra);
        PushString(out, name);
    }

    struct IdInfo {
        Ty ty{};
        u32 bits{};
        u32 origin{};
        u32 node{};
        bool is_const{};
    };
    struct KeyHash {
        size_t operator()(const std::vector<u32>& key) const {
            size_t hash = 0xcbf29ce484222325ull;
            for (const u32 word : key) {
                hash = (hash ^ word) * 0x100000001b3ull;
            }
            return hash;
        }
    };

    std::vector<IdInfo> ids_;
    std::vector<u32> forward_;
    std::vector<u32> globals_;
    std::vector<u32> annotations_;
    std::vector<u32> capabilities_;
    std::vector<u32> modes_;
    std::vector<u32> interfaces_;
    std::vector<std::string> extensions_;
    std::vector<std::pair<std::string, u32>> ext_sets_;
    std::unordered_map<std::vector<u32>, u32, KeyHash> type_map_;
    std::unordered_map<u64, u32> const_map_;
};

} // namespace Lifter
