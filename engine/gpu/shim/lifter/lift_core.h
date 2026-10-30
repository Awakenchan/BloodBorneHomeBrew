// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <unordered_map>
#include <span>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp11>
#include "lifter/gcn.h"
#include "lifter/lift.h"
#include "lifter/module.h"

namespace Lifter {

constexpr u32 RegScc = 128;
constexpr u32 RegCount = 512 + 256;
constexpr u32 FirstSlot = 512;
constexpr u32 ExecReg = SrcExec;
constexpr u32 VccReg = SrcVcc;

struct State {
    std::array<u32, RegCount> r{};
};

struct Block {
    u32 start{};
    u32 end{};
    s32 taken = -1;
    s32 fall = -1;
    u16 branch_op{};
    bool is_exit{};
    bool in_loop{};
    u32 preds{};
    s32 idom = -1;
    s32 ipdom = -1;
    u32 order{};
    std::vector<u32> pred_list;
};

struct Decline {
    std::string why;
};

struct OutBlock {
    u32 label{};
    std::vector<u32> nodes;
};

struct Edge {
    u32 out_block{};
    State state;
};

struct Sink {
    s32 block = -1;
    u32 label{};
    std::vector<Edge> edges;
};

struct LoopCtx {
    s32 header = -1;
    Sink merge;
    Sink cont;
    std::vector<u8> body;
};

struct RestartPessimistic {};
struct ToneChannel {
    bool reinhard{};
    u32 scene{};
    u32 exponent{};
    u32 k{};
    u32 shoulder{};
    u32 k_word{};
    u32 shoulder_word{};
};
struct RestartOrigins {
    std::vector<u32> regs;
};

struct InterpUse {
    u32 attr{};
    u32 chan{};
    u32 bary{};
    u32 node{};
};

enum Pseudo : u8 { PseudoNone, PseudoInterpP1, PseudoInterpP2, PseudoInterpMov };

class Lift_ {
public:
    explicit Lift_(const Input& input, Input* discovering = nullptr) : in{input}, found{discovering} {}
    Output Run();

private:


    const Input& in;
    Input* found;
    Module m;
    Output out;
    std::vector<Inst> insts;
    std::vector<Block> blocks;
    std::map<u32, u32> block_at;

    std::vector<OutBlock> out_blocks;
    u32 cur = 0;
    std::vector<Node> nodes;
    std::vector<u32> args;
    State s;
    std::vector<u8> visited;
    std::array<u32, 16> undef_ids{};
    std::vector<LoopCtx*> loops;
    std::vector<u32> phi_nodes;
    bool exec_pessimistic = false;
    std::vector<u32> no_loop_origin;
    std::unordered_map<u32, u32> tone_words;
    std::unordered_map<u32, u8> tone_taint;
    std::vector<ToneChannel> tone_channels;
    std::vector<u32> tone_above;
    std::vector<u32> tone_selects;
    u32 tone_on{}, tone_strength{}, tone_start{}, tone_room{}, tone_z_knee{}, tone_log_knee{}, tone_slope{};
    bool pc_valid = false;

    u32 t_void{}, t_bool{}, t_u32{}, t_f32{}, t_s32{}, t_u64{}, t_u16{}, t_u8{};
    u32 t_v2f{}, t_v3f{}, t_v4f{}, t_v2u{}, t_v3u{}, t_v4u{}, t_v2s{}, t_v3s{}, t_v4s{};
    u32 true_id{}, false_id{}, mask_hi{};
    u32 glsl{};
    u32 main_id{};

    std::array<u32, 3> ptr_cache{};
    u32 ptr_input_f32{}, ptr_input_u32{}, ptr_output_f32{}, ptr_output_u32{}, ptr_output_s32{},
        ptr_push_u32{}, ptr_push_f32{};
    u32 push_block{}, push_type{};
    std::vector<u32> interfaces;

    u32 var_frag_coord{}, var_front_face{}, var_sample_id{}, var_helper{}, var_sample_mask_in{},
        var_bary{}, var_bary_nopersp{}, var_position{}, var_clip{}, var_cull{}, var_point_size{},
        var_layer{}, var_viewport{}, var_frag_depth{}, var_sample_mask{}, var_stencil{},
        var_vertex_index{}, var_instance_index{}, var_base_vertex{}, var_base_instance{},
        var_local_id{}, var_work_id{}, var_lane_id{}, var_shared{};
    std::array<u32, 6> var_bary_amd{};
    std::array<u32, 32> var_param_out{};
    std::array<u32, 32> var_param_in{};
    std::array<u32, 8> var_color{};
    std::array<u32, 8> color_comps{};
    std::array<std::array<u32, 3>, 32> amd_vertex{};
    std::array<u32, 40> buffer_u32{}, buffer_f32{}, buffer_u16{}, buffer_u8{}, buffer_uniform{};
    std::array<u32, 40> buffer_off{};
    std::array<u32, 40> buffer_off_id{};
    std::array<u32, 40> buffer_off_dwords{};
    std::array<u32, 64> image_var{}, image_type{}, sampled_type{};
    std::array<u32, 16> sampler_var{};
    u32 t_sampler{}, uniform_block{}, uniform_elem_ptr{};
    u32 var_motion_addr{}, var_motion_cur_out{}, var_motion_prev_out{}, var_motion_frag{},
        var_motion_cur_in{}, var_motion_prev_in{};
    bool uses_quad{}, uses_shuffle{}, uses_lane{}, uses_discard{}, uses_sample_shading{},
        uses_float16{}, uses_storage_image{}, uses_image_query{}, uses_gather{},
        uses_ballot{}, stores_depth{}, stores_stencil{}, stores_mask{}, uses_shared{},
        wrote_clip{}, wrote_cull{}, wrote_viewport{}, wrote_layer{}, uses_int8{}, uses_int16{},
        uses_fp16_ops{}, uses_broadcast_first{};

    std::array<u32, 32> interp_kind{};
    std::array<u8, 32> interp_reads{};
    std::array<u8, 32> interp_decl{};
    std::array<u8, 32> interp_aux{};
    std::array<u8, 256> vgpr_bary{};

    std::array<u32, 128> sgpr_written_scratch{};

    bool fp_contract() const {
        return in.stage == Stage::Fragment && !in.opt.fragment_exact_fp && !stores_depth;
    }

    [[noreturn]] void Fail(std::string why) {
        throw Decline{std::move(why)};
    }

    void BuildCfg();
    void Dominators();
    void DefineBaseTypes();
    void Prologue();
    void Walk(s32 at, Sink* sink);
    s32 WalkLoop(s32 header);
    void Branch(Sink& sink);
    void Open(u32 label);
    void MergeEdges(Sink& sink, u32 cond);
    void CastInPred(u32 out_block, u32& value, Ty want);
    s32 FindMerge(s32 at, s32 t, s32 f);
    u32 UndefOf(Ty ty);
    u32 TypeOfTy(Ty ty) const;
    bool ReachesDisjoint(s32 a, s32 b) const;
    bool BlockIsHeader(s32 at) const;
    void LiftBlock(const Block& block);
    void LiftInst(const Inst& inst);
    void LiftScalar(const Inst& inst);
    void LiftSop1(const Inst& inst);
    void LiftSop2(const Inst& inst);
    void LiftSopc(const Inst& inst);
    void LiftSopk(const Inst& inst);
    void LiftSmrd(const Inst& inst);
    void LiftVop(const Inst& inst);
    void LiftVopc(const Inst& inst);
    void LiftInterp(const Inst& inst);
    void LiftExport(const Inst& inst);
    void LiftDs(const Inst& inst);
    void LiftBuffer(const Inst& inst);
    void LiftImage(const Inst& inst);
    void ImageSample(const Inst& inst, s32 image, s32 sampler);
    void ImageOther(const Inst& inst, s32 image);
    void Epilogue();
    void ExpandInterpolation();
    void Finish();
    void DeclareStageModes();
    void VertexMotion();
    void FragmentMotion();

    std::vector<u32> Serialize();
    void MarkLive(std::vector<u8>& live);

    void Terminate(u16 op, std::initializer_list<u32> a);

    u32 Emit(u16 op, u32 type, Ty ty, std::initializer_list<u32> a, u32 lits = 0);
    void EmitVoid(u16 op, std::initializer_list<u32> a, u32 lits = 0);
    u32 Ext(GLSLstd450 inst, u32 type, Ty ty, std::initializer_list<u32> a);

    u32 CU(u32 v) {
        return m.Const(Ty::U32, t_u32, v);
    }
    u32 CF(float v) {
        return m.Const(Ty::F32, t_f32, std::bit_cast<u32>(v));
    }
    u32 CB(bool v) {
        return v ? true_id : false_id;
    }
    u32 CS(s32 v) {
        return m.Const(Ty::None, t_s32, u32(v));
    }
    bool IsTrue(u32 id) const {
        return id == true_id;
    }
    bool IsFalse(u32 id) const {
        return id == false_id;
    }

    u32 ToF(u32 id);
    u32 ToU(u32 id);
    u32 BoolToU(u32 id);
    u32 FAdd(u32 a, u32 b);
    u32 FSub(u32 a, u32 b);
    u32 FMul(u32 a, u32 b);
    u32 FDiv(u32 a, u32 b);
    u32 FFma(u32 a, u32 b, u32 c);
    u32 FNeg(u32 a);
    u32 FAbs(u32 a);
    u32 FSat(u32 a);
    u32 FloatOp(u16 op, u32 a, u32 b, bool contract_rule = true);
    u32 IOp(u16 op, u32 a, u32 b);
    u32 ICmp(u16 op, u32 a, u32 b);
    u32 FCmp(u16 op, u32 a, u32 b);
    u32 Select(u32 cond, u32 a, u32 b);
    u32 And(u32 a, u32 b);
    u32 Or(u32 a, u32 b);
    u32 Xor(u32 a, u32 b);
    u32 Not(u32 a);
    u32 BfeU(u32 base, u32 offset, u32 count);
    u32 BfeS(u32 base, u32 offset, u32 count);

    u32 ReadReg(u32 reg);
    void WriteReg(u32 reg, u32 value);
    u32 SrcU(const Inst& inst, u32 index);
    u32 SrcF(const Inst& inst, u32 index);
    u32 SrcRaw(u32 code, const Inst& inst);
    u32 SrcRawF(u32 code, const Inst& inst);
    u32 Mask(u32 code, const Inst& inst);
    void SetMask(u32 code, u32 value);
    void SetVgpr(u32 reg, u32 value);
    void SetDstF(const Inst& inst, u32 value);
    void SetDstU(const Inst& inst, u32 value);
    u32 Exec() const {
        return s.r[ExecReg];
    }
    u32 GuardCond();
    bool NarrowsForGood(const Inst& inst);
    std::vector<u32> kill_masks;
    u32 VReadF(u32 reg);
    u32 VReadU(u32 reg);

    u32 PtrType(u32 storage, u32 pointee);
    u32 InputVar(u32 type, std::optional<u32> location, std::optional<spv::BuiltIn> builtin);
    u32 OutputVar(u32 type, std::optional<u32> location, std::optional<spv::BuiltIn> builtin, u32 init = 0);
    u32 LoadBuiltinU32(u32& var, spv::BuiltIn builtin);
    u32 BaryRead(u32 kind, u32 comp);
    u32 LoadAttrComp(u32 attr, u32 comp, s32 vertex);
    u32 FragCoordRead(u32 comp);
    u32 PushU32(u32 member, s32 element);
    u32 PushF32(u32 member);
    u32 ReadUserData(u32 reg);
    u32 FlatLoad(u32 flat);
    u32 FlatIndexOf(u32 parent, u32 offset);
    s32 FindBuffer(const std::array<u32, 4>& regs);
    s32 FindImage(u32 base, bool r128, bool array);
    s32 FindSampler(u32 base, bool depth);
    void NoteResource(ResourceKind kind, u32 slot, u32 base);
    u32 BufferDword(u32 slot, u32 index_dwords);
    u32 BufferDwordRaw(u32 slot, u32 index);
    u32 BufferOffsetDwords(u32 slot, u32 shift);
    u32 BufferVar(u32 slot, Ty element);
    u32 BufferAddress(const Inst& inst, const BufferSlot& slot, u32 shift);
    u32 ImageVar(u32 slot);
    u32 SamplerVar(u32 slot);
    u32 LoadImage(u32 slot);
    u32 SampledImage(u32 image_slot, u32 sampler_slot);
    u32 ComponentsF(std::initializer_list<u32> parts);
    u32 ComponentsU(std::initializer_list<u32> parts);
    u32 Extract(u32 type, Ty ty, u32 vec, u32 comp);
    u32 ApplyConv(u32 value, Conv conv);
    u32 ApplyWriteConv(u32 value, Conv conv);
    void GuardedVoid(u32 cond, auto&& body);
    void Discard(u32 cond);
    void StoreOutput(u32 var, u32 type, u32 ptr_type, s32 comp, u32 value);
    void ExportAttr(u32 target, u32 comp, u32 value);
    void ExportPos(u32 target, u32 comp, u32 value);
    void StoreClipDistance(u32 index, u32 value);
    const Node* ToneNode(u32 id, spv::Op op) const;
    const u32* ToneExtArgs(u32 id, GLSLstd450 inst) const;
    bool ToneIs(u32 id, float value) const;
    int64_t ToneWord(u32 id) const;
    bool ToneMatchExponential(u32 product, ToneChannel& c) const;
    bool ToneMatchReinhard(u32 product, ToneChannel& c) const;
    bool ToneComposite() const;
    void ToneTaint(u32 id, std::span<const u32> ids, u32 lits);
    u32 ToneNegateInput(u32 a);
    u32 ToneSaturate(u32 a);
    u32 ToneExtend(u32 clamp, const ToneChannel& c);
    void UserClipPlanes();
    void ExportMrt(const Inst& inst);
    std::pair<u32, u32> UnpackedHalves(u32 id);
    u32 ExportedValue(u32 id);
    void ExportZ(const Inst& inst);
    std::array<u32, 4> FormatLoad(const Inst& inst, const BufferSlot& slot, u32 slot_index);
    void FormatStore(const Inst& inst, const BufferSlot& slot, u32 slot_index, std::array<u32, 4> comps);
    u32 Uf11ToF(u32 value, u32 mantissa_bits);
    u32 FToUf11(u32 value, u32 mantissa_bits);
    u32 BufferNarrow(u32 slot_index, u32 address, Ty element);
    void BufferNarrowStore(u32 slot_index, u32 address, Ty element, u32 value);
    u32 BeginGuard(u32& merge);
    void EndGuard(u32 merge);
    u32 SlotFor(u32 kind, u32 a, u32 b);
    std::vector<std::array<u32, 3>> slots;
    std::vector<u32> undefined_reads;
    std::vector<std::pair<u32, u32>> ssbo_blocks;
    std::array<u8, 32> interp_other_read{};
    std::array<u32, 7> var_bary_amd_all{};
    bool vs_clip_emulated = false;
    bool user_clip_planes = false;
    bool tess_on = false;
    struct TessTag {
        bool known{};
        u32 value{};
        u32 mask{};
    };
    std::vector<TessTag> tags;
    u32 hull_packed{}, tess_factor_base{}, var_cp_in{}, var_cp_out{}, var_invocation{}, var_primitive{},
        var_tess_coord{}, var_tess_outer{}, var_tess_inner{}, var_gl_in{};
    std::array<u32, 30> var_patch{};
    bool KnownValue(u32 id, u32& value) const;
    u32 MaskOf(u32 id) const;
    u32 TaggedConst(u32 value, u32 mask);
    void TagFrom(u32 result, std::initializer_list<u32> sources);
    u32 TessRegionOf(u32 addr) const;
    bool DivisibleBy(u32 id, u32 stride, u32 depth) const;
    bool CollectAddends(u32 id, u32 stride, std::vector<u32>& kept, u32 depth) const;
    u32 OffsetInStride(u32 addr, u32 stride);
    u32 ControlPointVar(bool output);
    u32 ReadControlPoint(u32 addr, u32 stride, bool output);
    u32 PatchVar(u32 index);
    u32 InvocationId();
    void TessPrologue();
    bool TessBufferLoad(const Inst& inst, u32 count);
    bool TessBufferStore(const Inst& inst, u32 count);
    bool TessBufferLoadRing(const Inst& inst, u32 count);
    void LiftLds(const Inst& inst);
    bool MatchSharpAt(const Sharp& sharp, u32 base) const;
    void SendMsg(const Inst& inst);
    s32 entering_header = -1;
    u32 export_exec = 0;
    bool physical_addressing = false;

public:
    void SetPessimistic() {
        exec_pessimistic = true;
    }
    void SetNoLoopOrigin(const std::vector<u32>& regs) {
        no_loop_origin = regs;
    }

private:

    friend struct ExactWalk;
};

} // namespace Lifter
